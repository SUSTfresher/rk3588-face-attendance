/*
 * Gallery consistency diagnostic for two explicit enrollment sample IDs.
 *
 * It is read-only: stored PNG/landmarks are converted through the production
 * alignment and embedding components and the two normalized embeddings are
 * compared. Use synthetic/authorized test data only; neither input database nor
 * derived feature output belongs in source control.
 */
#include "database.h"
#include "face_alignment.h"
#include "face_recognizer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <array>
#include <cmath>
#include <iostream>

// Only sample-load APIs are used; no schema, enrollment, deletion, or check-in
// call is made. Usage: gallery_test DATABASE MODEL SAMPLE_ID_A SAMPLE_ID_B
static bool makeReference(AttendanceDatabase& database,
                          FaceRecognizer& recognizer,
                          qint64 id,
                          FaceRecognizer::Feature& feature)
{
    feature.fill(0.0f);
    EnrollmentSample sample;
    if (!database.loadEnrollmentSample(id, sample)) {
        std::cerr << "Cannot read sample: " << id << '\n';
        return false;
    }

    const QImage crop = QImage::fromData(sample.facePng, "PNG");
    const QRect sourceRect(QPoint(0, 0), sample.sourceSize);
    if (sample.name.trimmed().isEmpty() || crop.isNull() ||
        sample.sourceSize.isEmpty() || sample.crop.isEmpty() ||
        !sourceRect.contains(sample.crop) || crop.size() != sample.crop.size()) {
        std::cerr << "Invalid image or crop metadata: " << id << '\n';
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        sample.landmarksJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isArray() || document.array().size() != 5) {
        std::cerr << "Expected five landmark pairs: " << id << '\n';
        return false;
    }

    std::array<QPointF, 5> landmarks;
    const QJsonArray points = document.array();
    for (int i = 0; i < 5; ++i) {
        const QJsonArray pair = points[i].toArray();
        if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()) {
            std::cerr << "Invalid landmark JSON: " << id << '\n';
            return false;
        }
        const double x = pair[0].toDouble();
        const double y = pair[1].toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) {
            std::cerr << "Non-finite landmark: " << id << '\n';
            return false;
        }
        // 数据库保存的是整帧坐标，而图片是裁剪图；必须平移坐标。
        // 扩大裁剪后仍使用数据库的 crop 起点，不能使用人脸检测框起点。
        landmarks[i] = QPointF(x - sample.crop.x(), y - sample.crop.y());
    }

    QImage aligned;
    double rms = 0.0;
    QString error;
    if (!alignFace112(crop, landmarks, aligned, rms, error) ||
        !recognizer.extract(aligned, feature, error)) {
        std::cerr << "Sample " << id << " failed: "
                  << error.toStdString() << '\n';
        return false;
    }
    std::cout << "Reference OK: id=" << id
              << " name=" << sample.name.toStdString()
              << " dimensions=" << feature.size()
              << " alignment RMS=" << rms << '\n';
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 5) {
        std::cerr << "Usage: gallery_test DATABASE MODEL SAMPLE_ID_A SAMPLE_ID_B\n";
        return 1;
    }
    bool validA = false;
    bool validB = false;
    const qint64 idA = args[3].toLongLong(&validA);
    const qint64 idB = args[4].toLongLong(&validB);
    if (!validA || !validB || idA <= 0 || idB <= 0) {
        std::cerr << "Sample IDs must be positive integers\n";
        return 1;
    }

    AttendanceDatabase database;
    if (!database.open(args[1])) {
        std::cerr << "Cannot open database\n";
        return 1;
    }
    FaceRecognizer recognizer(args[2]);
    if (!recognizer.isReady()) {
        std::cerr << "Recognition model unavailable\n";
        return 1;
    }
    FaceRecognizer::Feature a{}, b{};
    if (!makeReference(database, recognizer, idA, a) ||
        !makeReference(database, recognizer, idB, b)) {
        return 1;
    }
    double score = 0.0;
    if (!FaceRecognizer::cosineSimilarity(a, b, score)) {
        std::cerr << "Invalid reference features\n";
        return 1;
    }
    // 这是参考样本之间的相似度，不表示实时识别成功，也不设置阈值。
    std::cout << "Reference cosine similarity: " << score << '\n';
    return 0;
}
