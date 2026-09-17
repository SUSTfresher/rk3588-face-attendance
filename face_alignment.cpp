/*
 * OpenCV implementation of the recognition alignment contract.
 *
 * The five detector landmarks are fitted to the fixed ArcFace template using a
 * similarity-like partial affine transform. The RMS check guards against bad
 * detections before a distorted crop reaches MobileFaceNet. This is a quality
 * gate, not an anti-spoofing or identity-confidence mechanism.
 */
#include "face_alignment.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <limits>
#include <vector>

bool alignFace112(
    const QImage& source,
    const std::array<QPointF, 5>& landmarks,
    QImage& aligned,
    double& rms,
    QString& error)
{
    aligned = QImage();
    rms = std::numeric_limits<double>::quiet_NaN();
    error.clear();
    if (source.isNull()) {
        error = QStringLiteral("对齐输入图片为空");
        return false;
    }

    try {
        // Validate before passing data to OpenCV: invalid points otherwise tend
        // to create a plausible-looking but meaningless affine matrix.
        std::vector<cv::Point2f> points;
        for (const QPointF& point : landmarks) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
                point.x() < 0 || point.y() < 0 ||
                point.x() >= source.width() || point.y() >= source.height()) {
                error = QStringLiteral("关键点无效或超出输入图片范围");
                return false;
            }
            points.emplace_back(static_cast<float>(point.x()),
                                static_cast<float>(point.y()));
        }
        if (cv::norm(points[0] - points[1]) < 5.0) {
            error = QStringLiteral("双眼关键点距离过小");
            return false;
        }

        // ArcFace's 112x112 five-point template. Order is image-left eye,
        // image-right eye, nose, image-left mouth corner, image-right mouth.
        const std::vector<cv::Point2f> target = {
            {38.2946f, 51.6963f}, {73.5318f, 51.5014f},
            {56.0252f, 71.7366f}, {41.5493f, 92.3655f},
            {70.7299f, 92.2041f}
        };
        cv::Mat inliers;
        // Partial affine excludes shear/projective warping, preserving the face
        // geometry expected by the embedding model.
        const cv::Mat transform = cv::estimateAffinePartial2D(
            points, target, inliers, cv::LMEDS);
        if (transform.empty() || !cv::checkRange(transform)) {
            error = QStringLiteral("无法计算人脸对齐变换");
            return false;
        }

        std::vector<cv::Point2f> mapped;
        cv::transform(points, mapped, transform);
        double squaredError = 0.0;
        for (size_t i = 0; i < mapped.size(); ++i) {
            const cv::Point2f delta = mapped[i] - target[i];
            squaredError += delta.dot(delta);
        }
        rms = std::sqrt(squaredError / mapped.size());
        // Development quality gate. It should be calibrated with representative
        // capture data before any formal deployment decision.
        if (!std::isfinite(rms) || rms > 6.0) {
            error = QStringLiteral("对齐误差过大，请正对摄像头");
            return false;
        }

        const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
        if (rgb.isNull()) {
            error = QStringLiteral("无法转换输入图片格式");
            return false;
        }
        // cv::Mat borrows QImage pixels only during this scope. warpAffine does
        // not mutate input and preserves the RGB channel ordering.
        cv::Mat input(rgb.height(), rgb.width(), CV_8UC3,
                      const_cast<uchar*>(rgb.constBits()), rgb.bytesPerLine());
        cv::Mat output;
        cv::warpAffine(input, output, transform, cv::Size(112, 112),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        // Deep-copy because output is a local cv::Mat. Returning a QImage that
        // merely wraps output.data would leave a dangling pixel pointer.
        aligned = QImage(output.data, output.cols, output.rows,
                         static_cast<int>(output.step),
                         QImage::Format_RGB888).copy();
        if (aligned.isNull()) {
            error = QStringLiteral("无法分配对齐图片");
            return false;
        }
        return true;
    } catch (const cv::Exception& exception) {
        error = QStringLiteral("OpenCV 对齐失败：%1")
                    .arg(QString::fromUtf8(exception.what()));
        return false;
    }
}
