#include <opencv2/opencv.hpp>
#include "openbr_internal.h"
#include "openbr/core/qtutils.h"
#include "openbr/core/opencvutils.h"
#include "openbr/core/eigenutils.h"
#include <QString>
#include <Eigen/SVD>

using namespace std;
using namespace cv;

namespace br
{

/*!
 * \ingroup transforms
 * \brief Procrustes alignment of points
 * \author Scott Klum \cite sklum
 */
class ProcrustesTransform : public Transform
{
    Q_OBJECT

    Q_PROPERTY(bool warp READ get_warp WRITE set_warp RESET reset_warp STORED false)
    BR_PROPERTY(bool, warp, true)

    Eigen::MatrixXf meanShape;

    void train(const TemplateList &data)
    {
        QList< QList<QPointF> > normalizedPoints;

        // Normalize all sets of points
        foreach (br::Template datum, data) {
            QList<QPointF> points = datum.file.points();
            QList<QRectF> rects = datum.file.rects();

            if (points.empty() || rects.empty()) continue;

            // Assume rect appended last was bounding box
            points.append(rects.last().topLeft());
            points.append(rects.last().topRight());
            points.append(rects.last().bottomLeft());
            points.append(rects.last().bottomRight());

            // Center shape at origin
            Scalar mean = cv::mean(OpenCVUtils::toPoints(points).toVector().toStdVector());
            for (int i = 0; i < points.size(); i++) points[i] -= QPointF(mean[0],mean[1]);

            // Remove scale component
            float norm = cv::norm(OpenCVUtils::toPoints(points).toVector().toStdVector());
            for (int i = 0; i < points.size(); i++) points[i] /= norm;

            normalizedPoints.append(points);
        }

        if (normalizedPoints.empty()) qFatal("Unable to calculate normalized points");

        // Determine mean shape, assuming all shapes contain the same number of points
        meanShape = Eigen::MatrixXf(normalizedPoints[0].size(), 2);

        for (int i = 0; i < normalizedPoints[0].size(); i++) {
            double x = 0;
            double y = 0;

            for (int j = 0; j < normalizedPoints.size(); j++) {
                x += normalizedPoints[j][i].x();
                y += normalizedPoints[j][i].y();
            }

            x /= (double)normalizedPoints.size();
            y /= (double)normalizedPoints.size();

            meanShape(i,0) = x;
            meanShape(i,1) = y;
        }
    }

    void project(const Template &src, Template &dst) const
    {
        QList<QPointF> points = src.file.points();
        QList<QRectF> rects = src.file.rects();

        if (points.empty() || rects.empty()) {
            dst = src;
            qWarning("Procrustes alignment failed because points or rects are empty.");
            return;
        }

        // Assume rect appended last was bounding box
        points.append(rects.last().topLeft());
        points.append(rects.last().topRight());
        points.append(rects.last().bottomLeft());
        points.append(rects.last().bottomRight());

        Scalar mean = cv::mean(OpenCVUtils::toPoints(points).toVector().toStdVector());
        for (int i = 0; i < points.size(); i++) points[i] -= QPointF(mean[0],mean[1]);

        Eigen::MatrixXf srcMat(points.size(), 2);
        float norm = cv::norm(OpenCVUtils::toPoints(points).toVector().toStdVector());
        for (int i = 0; i < points.size(); i++) {
            points[i] /= norm;
            srcMat(i,0) = points[i].x();
            srcMat(i,1) = points[i].y();
        }

        Eigen::JacobiSVD<Eigen::MatrixXf> svd(srcMat.transpose()*meanShape, Eigen::ComputeThinU | Eigen::ComputeThinV);
        Eigen::MatrixXf R = svd.matrixU()*svd.matrixV().transpose();

        dst = src;

        // Store procrustes stats in the order:
        // R(0,0), R(1,0), R(1,1), R(0,1), mean_x, mean_y, norm
        QList<float> procrustesStats;
        procrustesStats << R(0,0) << R(1,0) << R(1,1) << R(0,1) << mean[0] << mean[1] << norm;
        dst.file.setList<float>("ProcrustesStats",procrustesStats);

        if (warp) {
            Eigen::MatrixXf dstMat = srcMat*R;
            for (int i = 0; i < dstMat.rows(); i++) {
                dst.file.appendPoint(QPointF(dstMat(i,0),dstMat(i,1)));
            }
        }
    }

    void store(QDataStream &stream) const
    {
        stream << meanShape;
    }

    void load(QDataStream &stream)
    {
        stream >> meanShape;
    }

};

BR_REGISTER(Transform, ProcrustesTransform)

/*!
 * \ingroup transforms
 * \brief Creates a Delaunay triangulation based on a set of points
 * \author Scott Klum \cite sklum
 */
class DelaunayTransform : public UntrainableTransform
{
    Q_OBJECT

    Q_PROPERTY(float scaleFactor READ get_scaleFactor WRITE set_scaleFactor RESET reset_scaleFactor STORED false)
    Q_PROPERTY(bool warp READ get_warp WRITE set_warp RESET reset_warp STORED false)
    BR_PROPERTY(float, scaleFactor, 1)
    BR_PROPERTY(bool, warp, true)

    void project(const Template &src, Template &dst) const
    {
        QList<QPointF> points = src.file.points();
        QList<QRectF> rects = src.file.rects();

        if (points.empty() || rects.empty()) {
            dst = src;
            qWarning("Delauney triangulation failed because points or rects are empty.");
            return;
        }

        int cols = src.m().cols;
        int rows = src.m().rows;

        // Assume rect appended last was bounding box
        points.append(rects.last().topLeft());
        points.append(rects.last().topRight());
        points.append(rects.last().bottomLeft());
        points.append(rects.last().bottomRight());

        Subdiv2D subdiv(Rect(0,0,cols,rows));
        // Make sure points are valid for Subdiv2D
        // TODO: Modify points to make them valid
        for (int i = 0; i < points.size(); i++) {
            if (points[i].x() < 0 || points[i].y() < 0 || points[i].y() >= rows || points[i].x() >= cols) {
                dst = src;
                qWarning("Delauney triangulation failed because points lie on boundary.");
                return;
            }
            subdiv.insert(OpenCVUtils::toPoint(points[i]));
        }

        vector<Vec6f> triangleList;
        subdiv.getTriangleList(triangleList);

        QList<QPointF> validTriangles;

        for (size_t i = 0; i < triangleList.size(); i++) {
            // Check the triangle to make sure it's falls within the matrix
            bool valid = true;

            QList<QPointF> vertices;
            vertices.append(QPointF(triangleList[i][0],triangleList[i][1]));
            vertices.append(QPointF(triangleList[i][2],triangleList[i][3]));
            vertices.append(QPointF(triangleList[i][4],triangleList[i][5]));
            for (int j = 0; j < 3; j++) if (vertices[j].x() > cols || vertices[j].y() > rows || vertices[j].x() < 0 || vertices[j].y() < 0) valid = false;

            if (valid) validTriangles.append(vertices);
        }

        if (warp) {
            dst.m() = Mat::zeros(rows,cols,src.m().type());

            QList<float> procrustesStats = src.file.getList<float>("ProcrustesStats");

            Eigen::MatrixXf R(2,2);
            R(0,0) = procrustesStats.at(0);
            R(1,0) = procrustesStats.at(1);
            R(1,1) = procrustesStats.at(2);
            R(0,1) = procrustesStats.at(3);

            cv::Scalar mean(2);
            mean[0] = procrustesStats.at(4);
            mean[1] = procrustesStats.at(5);

            float norm = procrustesStats.at(6);

            QList<Point2f> mappedPoints;

            for (int i = 0; i < validTriangles.size(); i+=3) {
                // Matrix to store original (pre-transformed) triangle vertices
                Eigen::MatrixXf srcMat(3, 2);

                for (int j = 0; j < 3; j++) {
                    srcMat(j,0) = (validTriangles[i+j].x()-mean[0])/norm;
                    srcMat(j,1) = (validTriangles[i+j].y()-mean[1])/norm;
                }

                Eigen::MatrixXf dstMat = srcMat*R;

                Point2f srcPoints[3];
                for (int j = 0; j < 3; j++) srcPoints[j] = OpenCVUtils::toPoint(validTriangles[i+j]);

                Point2f dstPoints[3];
                for (int j = 0; j < 3; j++) {
                    // Scale and shift destination points
                    Point2f warpedPoint = Point2f(dstMat(j,0)*scaleFactor+cols/2,dstMat(j,1)*scaleFactor+rows/2);
                    dstPoints[j] = warpedPoint;
                    mappedPoints.append(warpedPoint);
                }

                Mat buffer(rows,cols,src.m().type());

                warpAffine(src.m(), buffer, getAffineTransform(srcPoints, dstPoints), Size(cols,rows));

                Mat mask = Mat::zeros(rows, cols, CV_8UC1);
                Point maskPoints[1][3];
                maskPoints[0][0] = dstPoints[0];
                maskPoints[0][1] = dstPoints[1];
                maskPoints[0][2] = dstPoints[2];
                const Point* ppt = { maskPoints[0] };

                fillConvexPoly(mask, ppt, 3, Scalar(255,255,255), 8);

                Mat output(rows,cols,src.m().type());

                if (i > 0) {
                    Mat overlap;
                    bitwise_and(dst.m(),mask,overlap);
                    mask.setTo(0, overlap!=0);
                }

                bitwise_and(buffer,mask,output);

                dst.m() += output;
            }

            // Overwrite any rects
            Rect boundingBox = boundingRect(mappedPoints.toVector().toStdVector());
            dst.file.setRects(QList<QRectF>() << OpenCVUtils::fromRect(boundingBox));
        } else dst = src;

        dst.file.setList<QPointF>("DelaunayTriangles", validTriangles);
    }
};

BR_REGISTER(Transform, DelaunayTransform)

/*!
 * \ingroup transforms
 * \brief Creates a Delaunay triangulation based on a set of points
 * \author Scott Klum \cite sklum
 */
class DrawDelaunayTransform : public UntrainableTransform
{
    Q_OBJECT

    void project(const Template &src, Template &dst) const
    {
        QList<Point2f> validTriangles = OpenCVUtils::toPoints(src.file.getList<QPointF>("DelaunayTriangles"));

        // Clone the matrix do draw on it
        dst.m() = src.m().clone();

        for (int i = 0; i < validTriangles.size(); i+=3) {
            line(dst.m(), validTriangles[i], validTriangles[i+1], Scalar(0,0,0), 1);
            line(dst.m(), validTriangles[i+1], validTriangles[i+2], Scalar(0,0,0), 1);
            line(dst.m(), validTriangles[i+2], validTriangles[i], Scalar(0,0,0), 1);
        }
    }
};

BR_REGISTER(Transform, DrawDelaunayTransform)

} // namespace br

#include "landmarks.moc"
