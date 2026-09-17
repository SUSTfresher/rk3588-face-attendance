/*
 * Read-only alignment diagnostic.
 *
 * Given an explicit enrollment sample ID, it decodes the stored crop and
 * landmarks, applies the same five-point affine alignment concept as production,
 * and saves a diagnostic image. It opens SQLite with SQLITE_OPEN_READONLY and
 * never creates, changes, or enumerates a business database.
 *
 * Usage: alignment_test DATABASE SAMPLE_ID OUTPUT.png
 */
#include <QCoreApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>
#include <sqlite3.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

struct Database {
    sqlite3* handle = nullptr;
    ~Database() { if (handle) sqlite3_close(handle); }
};

struct Statement {
    sqlite3_stmt* handle = nullptr;
    ~Statement() { if (handle) sqlite3_finalize(handle); }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 4) {
        std::cerr << "Usage: alignment_test DATABASE SAMPLE_ID OUTPUT.png\n";
        return 1;
    }
    try {
        bool validId = false;
        const qint64 id = args[2].toLongLong(&validId);
        if (!validId || id <= 0)
            throw std::runtime_error("Sample ID must be a positive integer");

        Database db;
        const QByteArray path = args[1].toUtf8();
        // READONLY also prevents accidentally creating an empty database.
        if (sqlite3_open_v2(path.constData(), &db.handle,
                            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot open database read-only");

        Statement stmt;
        const char* sql =
            "SELECT name,face_png,landmarks_json,crop_x,crop_y,"
            "crop_width,crop_height FROM enrollment_samples WHERE id=?";
        if (sqlite3_prepare_v2(db.handle, sql, -1, &stmt.handle, nullptr)
                != SQLITE_OK ||
            sqlite3_bind_int64(stmt.handle, 1, id) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db.handle));
        const int rc = sqlite3_step(stmt.handle);
        if (rc == SQLITE_DONE) throw std::runtime_error("Sample ID not found");
        if (rc != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db.handle));

        const QString name = QString::fromUtf8(reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.handle, 0)));
        const QImage decoded = QImage::fromData(
            static_cast<const uchar*>(sqlite3_column_blob(stmt.handle, 1)),
            sqlite3_column_bytes(stmt.handle, 1), "PNG");
        if (decoded.isNull()) throw std::runtime_error("Cannot decode sample PNG");
        const QImage crop = decoded.convertToFormat(QImage::Format_RGB888);
        if (crop.width() != sqlite3_column_int(stmt.handle, 5) ||
            crop.height() != sqlite3_column_int(stmt.handle, 6))
            throw std::runtime_error("PNG dimensions disagree with crop metadata");

        const QByteArray json(reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.handle, 2)),
            sqlite3_column_bytes(stmt.handle, 2));
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(json, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray() ||
            document.array().size() != 5)
            throw std::runtime_error("Expected five landmark coordinate pairs");

        const double cropX = sqlite3_column_int(stmt.handle, 3);
        const double cropY = sqlite3_column_int(stmt.handle, 4);
        std::vector<cv::Point2f> source;
        for (const QJsonValue& value : document.array()) {
            const QJsonArray pair = value.toArray();
            if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
                throw std::runtime_error("Invalid landmark coordinate pair");
            // Stored landmarks use ORIGINAL image coordinates, but PNG is a crop.
            const double x = pair[0].toDouble() - cropX;
            const double y = pair[1].toDouble() - cropY;
            if (!std::isfinite(x) || !std::isfinite(y) ||
                x < 0 || y < 0 || x >= crop.width() || y >= crop.height())
                throw std::runtime_error("Landmark is outside the saved crop");
            source.emplace_back(static_cast<float>(x), static_cast<float>(y));
        }

        // Common ArcFace 112x112 template: image-left eye, image-right eye,
        // nose, image-left mouth corner, image-right mouth corner.
        // A future feature model must be checked for its own alignment convention.
        const std::vector<cv::Point2f> target = {
            {38.2946f, 51.6963f}, {73.5318f, 51.5014f},
            {56.0252f, 71.7366f}, {41.5493f, 92.3655f},
            {70.7299f, 92.2041f}
        };
        if (cv::norm(source[0] - source[1]) < 5.0)
            throw std::runtime_error("Eye landmarks are too close together");

        // Partial affine permits translation, rotation and uniform scale,
        // without arbitrary shear or independent horizontal/vertical stretching.
        cv::Mat inliers;
        const cv::Mat transform = cv::estimateAffinePartial2D(
            source, target, inliers, cv::LMEDS);
        if (transform.empty() || !cv::checkRange(transform))
            throw std::runtime_error("Cannot estimate alignment transform");
        std::vector<cv::Point2f> mapped;
        cv::transform(source, mapped, transform);
        double squaredError = 0;
        for (size_t i = 0; i < mapped.size(); ++i) {
            const cv::Point2f delta = mapped[i] - target[i];
            squaredError += delta.dot(delta);
        }
        const double rms = std::sqrt(squaredError / mapped.size());
        std::cout << "Alignment RMS (pixels): " << rms << '\n';
        // This is a provisional geometric check, not an identity-match score.
        if (rms > 6.0)
            throw std::runtime_error("Poor alignment fit; collect a frontal sample");

        // Qt already decoded RGB. OpenCV warpAffine preserves channel order.
        cv::Mat input(crop.height(), crop.width(), CV_8UC3,
                      const_cast<uchar*>(crop.constBits()), crop.bytesPerLine());
        cv::Mat aligned;
        cv::warpAffine(input, aligned, transform, cv::Size(112, 112),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        const QImage output(aligned.data, aligned.cols, aligned.rows,
                            static_cast<int>(aligned.step), QImage::Format_RGB888);
        if (!output.save(args[3], "PNG"))
            throw std::runtime_error("Cannot save output PNG");
        std::cout << "Sample: " << id << " name: " << name.toStdString()
                  << "\nSaved: " << args[3].toStdString() << " (112x112)\n";
        // Black edges are possible because pixels outside the saved face crop
        // cannot be recovered. Production alignment should use the original frame.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Alignment failed: " << error.what() << '\n';
        return 1;
    }
}
