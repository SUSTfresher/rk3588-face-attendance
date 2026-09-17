#pragma once

/*
 * RetinaFace detector facade backed by one RKNN context.
 *
 * The object is constructed and used only by the inference thread. detect()
 * accepts a normal QImage and returns camera-coordinate boxes plus five points;
 * it does not own camera buffers and has no database or UI dependencies.
 */

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <array>
#include <vector>
#include <chrono>

#include "rknn_api.h"

struct FaceDetection {
    // QRectF and landmarks share the original input image coordinate system.
    QRectF box;
    std::array<QPointF, 5> landmarks;
    float score = 0.0f;
};

class FaceDetector {
public:
    explicit FaceDetector(const QString& model_path);
    ~FaceDetector();

    // Model-load state only. A true value does not guarantee a future inference
    // call will succeed, so callers still handle an empty result safely.
    bool isReady() const;
    std::vector<FaceDetection> detect(const QImage& image);

private:
    // Initializes/releases the RKNN model. loadModel validates the expected
    // single input and three RetinaFace outputs before marking ready_.
    bool loadModel(const QString& model_path);
    void release();

    rknn_context context_ = 0;
    std::vector<unsigned char> model_data_;
    bool ready_ = false;
    // Per-second profiler counters; safe because FaceDetector is thread-confined.
    std::array<double, 7> timingSum_{};
    std::array<double, 7> timingMax_{};
    std::array<unsigned, 7> timingCount_{};
    unsigned timingFailures_ = 0;
    std::chrono::steady_clock::time_point timingStart_ = std::chrono::steady_clock::now();
    bool verboseScores_ = false;
    bool legacyResize_ = false;
};
