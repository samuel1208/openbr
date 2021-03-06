#include "openbr_internal.h"
#include "openbr/core/opencvutils.h"
#include "openbr/core/common.h"
#include "openbr/core/qtutils.h"
#include <opencv2/objdetect/objdetect.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

using namespace cv;

namespace br
{

// Find avg aspect ratio
static float getAspectRatio(const TemplateList &data)
{
    double tempRatio = 0;
    int ratioCnt = 0;

    foreach (const Template &tmpl, data) {
        QList<Rect> posRects = OpenCVUtils::toRects(tmpl.file.rects());
        foreach (const Rect &posRect, posRects) {
            if (posRect.x + posRect.width >= tmpl.m().cols || posRect.y + posRect.height >= tmpl.m().rows || posRect.x < 0 || posRect.y < 0) {
                continue;
            }
            tempRatio += (float)posRect.width / (float)posRect.height;
            ratioCnt += 1;
        }
    }
    return tempRatio / (double)ratioCnt;
}

/*!
 * \ingroup transforms
 * \brief Applies a transform to a sliding window.
 *        Discards negative detections.
 * \author Austin Blanton \cite imaus10
 */
class SlidingWindowTransform : public Transform
{
    Q_OBJECT
    Q_PROPERTY(br::Transform *transform READ get_transform WRITE set_transform RESET reset_transform STORED false)
    Q_PROPERTY(int stepSize READ get_stepSize WRITE set_stepSize RESET reset_stepSize STORED false)
    Q_PROPERTY(bool takeFirst READ get_takeFirst WRITE set_takeFirst RESET reset_takeFirst STORED false)
    Q_PROPERTY(int windowWidth READ get_windowWidth WRITE set_windowWidth RESET reset_windowWidth STORED false)
    Q_PROPERTY(float threshold READ get_threshold WRITE set_threshold RESET reset_threshold STORED false)
    BR_PROPERTY(br::Transform *, transform, NULL)
    BR_PROPERTY(int, stepSize, 1)
    BR_PROPERTY(bool, takeFirst, false)
    BR_PROPERTY(int, windowWidth, 24)
    BR_PROPERTY(float, threshold, 0)

public:
    SlidingWindowTransform() : Transform(false, true) {}
private:
    int windowHeight;

    void train(const TemplateList &data)
    {
        float aspectRatio = data.first().file.get<float>("aspectRatio", -1);
        if (aspectRatio == -1)
            aspectRatio = getAspectRatio(data);
        windowHeight = (int) qRound((float) windowWidth / aspectRatio);
        if (transform->trainable) {
            transform->train(data);
        }
    }

    void project(const Template &src, Template &dst) const
    {
        dst = src;
        // no need to slide a window over ground truth data
        if (src.file.getBool("Train", false)) return;

        dst.file.clearRects();
        float scale = src.file.get<float>("scale", 1);
        Template windowTemplate(src.file, src);
        QList<float> confidences = dst.file.getList<float>("Confidences", QList<float>());
        for (double y = 0; y + windowHeight < src.m().rows; y += stepSize) {
            for (double x = 0; x + windowWidth < src.m().cols; x += stepSize) {
                Mat windowMat(src, Rect(x, y, windowWidth, windowHeight));
                windowTemplate.replace(0,windowMat);
                Template detect;
                transform->project(windowTemplate, detect);
                float conf = detect.m().at<float>(0);

                // the result will be in the Label
                if (conf > threshold) {
                    dst.file.appendRect(QRectF((float) x * scale, (float) y * scale, (float) windowWidth * scale, (float) windowHeight * scale));
                    confidences.append(conf);
                    if (takeFirst)
                        return;
                }
            }
        }
        dst.file.setList<float>("Confidences", confidences);
    }
};

BR_REGISTER(Transform, SlidingWindowTransform)

/*!
 * \ingroup transforms
 * \brief .
 * \author Austin Blanton \cite imaus10
 */
class BuildScalesTransform : public Transform
{
    Q_OBJECT
    Q_PROPERTY(br::Transform *transform READ get_transform WRITE set_transform RESET reset_transform STORED false)
    Q_PROPERTY(double scaleFactor READ get_scaleFactor WRITE set_scaleFactor RESET reset_scaleFactor STORED false)
    Q_PROPERTY(bool takeLargestScale READ get_takeLargestScale WRITE set_takeLargestScale RESET reset_takeLargestScale STORED false)
    Q_PROPERTY(int windowWidth READ get_windowWidth WRITE set_windowWidth RESET reset_windowWidth STORED false)
    Q_PROPERTY(int negToPosRatio READ get_negToPosRatio WRITE set_negToPosRatio RESET reset_negToPosRatio STORED false)
    Q_PROPERTY(int minSize READ get_minSize WRITE set_minSize RESET reset_minSize STORED false)
    Q_PROPERTY(double maxOverlap READ get_maxOverlap WRITE set_maxOverlap RESET reset_maxOverlap STORED false)
    Q_PROPERTY(float minScale READ get_minScale WRITE set_minScale RESET reset_minScale STORED false)
    Q_PROPERTY(bool negSamples READ get_negSamples WRITE set_negSamples RESET reset_negSamples STORED false)
    BR_PROPERTY(br::Transform *, transform, NULL)
    BR_PROPERTY(double, scaleFactor, 0.75)
    BR_PROPERTY(bool, takeLargestScale, false)
    BR_PROPERTY(int, windowWidth, 24)
    BR_PROPERTY(int, negToPosRatio, 1)
    BR_PROPERTY(int, minSize, 8)
    BR_PROPERTY(double, maxOverlap, 0)
    BR_PROPERTY(float, minScale, 1.0)
    BR_PROPERTY(bool, negSamples, true)

public:
    BuildScalesTransform() : Transform(false, true) {}
private:
    int windowHeight;

    void train(const TemplateList &_data)
    {
        TemplateList data(_data);  // have to make a copy b/c data is const
        aspectRatio = getAspectRatio(data);
        data.first().file.set("aspectRatio", aspectRatio);
        windowHeight = (int) qRound((float) windowWidth / aspectRatio);

        if (transform->trainable) {
            TemplateList full;
            foreach (const Template &tmpl, data) {
                QList<Rect> posRects = OpenCVUtils::toRects(tmpl.file.rects());
                QList<Rect> negRects;
                foreach (Rect posRect, posRects) {

                    //Adjust for training samples that have different aspect ratios
                    int diff = posRect.width - (int)((float) posRect.height * aspectRatio);
                    posRect.x += diff / 2;
                    posRect.width += diff;

                    if (posRect.x + posRect.width >= tmpl.m().cols || posRect.y + posRect.height >= tmpl.m().rows || posRect.x < 0 || posRect.y < 0) {
                        continue;
                    }

                    Mat scaledImg;
                    resize(Mat(tmpl, posRect), scaledImg, Size(windowWidth,qRound(windowWidth / aspectRatio)));
                    Template pos(tmpl.file, scaledImg);
                    full += pos;

                    // add random negative samples
                    if (negSamples) {
                        Mat m = tmpl.m();
                        int sample = 0;
                        while (sample < negToPosRatio) {
                            int x = Common::RandSample(1, m.cols)[0];
                            int y = Common::RandSample(1, m.rows)[0];
                            int maxWidth = m.cols - x;
                            int maxHeight = m.rows - y;
                            if (maxWidth <= minSize || maxHeight <= minSize)
                                continue;
                            int height;
                            int width;
                            if (aspectRatio > (float) maxWidth / (float) maxHeight) {
                                width = Common::RandSample(1,maxWidth,minSize)[0];
                                height = (int) qRound(width / aspectRatio);
                            } else {
                                height = Common::RandSample(1,maxHeight,minSize)[0];
                                width = (int) qRound(height * aspectRatio);
                            }
                            Rect negRect(x, y, width, height);
                            // the negative samples cannot overlap the positive at all
                            // but they may overlap with other negatives
                            if (overlaps(posRects, negRect, 0) || overlaps(negRects, negRect, maxOverlap))
                                continue;
                            negRects.append(negRect);
                            Template neg(tmpl.file, Mat());
                            resize(Mat(tmpl, negRect), neg, Size(windowWidth, windowHeight));
                            neg.file.set("Label", QString("neg"));
                            full += neg;
                            sample++;
                        }
                    }
                }
            }
            transform->train(full);
        }
    }

    bool overlaps(QList<Rect> posRects, Rect negRect, double overlap)
    {
        foreach (const Rect posRect, posRects) {
            Rect intersect = negRect & posRect;
            if (intersect.area() > overlap*posRect.area())
                return true;
        }
        return false;
    }


    void project(const Template &src, Template &dst) const
    {
        dst = src;
        // do not scale images during training
        if (src.file.getBool("Train", false)) return;

        int rows = src.m().rows;
        int cols = src.m().cols;
        int windowHeight = (int) qRound((float) windowWidth / aspectRatio);
        float startScale;
        if ((cols / rows) > aspectRatio)
            startScale = qRound((float) rows / (float) windowHeight);
        else
            startScale = qRound((float) cols / (float) windowWidth);
        for (float scale = startScale; scale >= minScale; scale -= (1.0 - scaleFactor)) {
            Template scaleImg(src.file, Mat());
            scaleImg.file.set("scale", scale);
            resize(src, scaleImg, Size(qRound(cols / scale), qRound(rows / scale)));
            transform->project(scaleImg, dst);
            if (takeLargestScale && !dst.file.rects().empty())
                return;
        }
    }

    float aspectRatio;
};

BR_REGISTER(Transform, BuildScalesTransform)

/*!
 * \ingroup transforms
 * \brief Detects objects with OpenCV's built-in HOG detection.
 * \author Austin Blanton \cite imaus10
 */
class HOGDetectTransform : public UntrainableTransform
{
    Q_OBJECT

    HOGDescriptor hog;

    void init()
    {
        hog.setSVMDetector(HOGDescriptor::getDefaultPeopleDetector());
    }

    void project(const Template &src, Template &dst) const
    {
        dst = src;
        std::vector<Rect> objLocs;
        QList<Rect> rects;
        hog.detectMultiScale(src, objLocs);
        foreach (const Rect &obj, objLocs)
            rects.append(obj);
        dst.file.setRects(rects);
    }
};

BR_REGISTER(Transform, HOGDetectTransform)

} // namespace br

#include "slidingwindow.moc"
