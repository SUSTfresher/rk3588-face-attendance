#pragma once

/*
 * MobileFaceNet embedding extractor backed by RKNN.
 *
 * Input is an ArcFace-style five-point aligned 112x112 face. Output is a
 * finite, non-zero, L2-normalized 512-element embedding suitable for cosine
 * ranking. This object is deliberately non-copyable and inference-thread-only.
 */

#include <QImage>
#include <QString>
#include <array>

#include "rknn_api.h"

class FaceRecognizer {
public:
    using Feature = std::array<float, 512>;

    explicit FaceRecognizer(const QString& modelPath);
    ~FaceRecognizer();

    // One RKNN context has one owner; copying could double-destroy or permit
    // accidental concurrent access.
    FaceRecognizer(const FaceRecognizer&) = delete;
    FaceRecognizer& operator=(const FaceRecognizer&) = delete;

    bool isReady() const;

    // Input must already be five-point aligned at 112x112. On success feature is
    // L2 normalized; on failure it is zeroed and error explains the rejection.
    bool extract(
        const QImage& alignedFace,
        Feature& feature,
        QString& error);

    // Computes cosine similarity in [-1, 1]. Invalid vectors return false so a
    // failed calculation is never interpreted as a valid match score.
    static bool cosineSimilarity(
        const Feature& a,
        const Feature& b,
        double& score);

private:
    rknn_context context_ = 0;
    bool ready_ = false;
};
