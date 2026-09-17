/*
 * MobileFaceNet RKNN implementation.
 *
 * The model owns a context for the life of the inference thread. extract()
 * validates model layout, copies tightly packed RGB input, asks RKNN for a
 * float output, validates it, and only then exposes a normalized embedding.
 * No match decision is made in this file; ranking and confirmation live in
 * person_ranking.h and recognition_state.h.
 */
#include "face_recognizer.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

FaceRecognizer::FaceRecognizer(const QString& modelPath)
{
    // Fail closed: ready_ is set only after both tensor shapes are verified.
    QFile file(modelPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot open recognition model:" << modelPath;
        return;
    }

    QByteArray data = file.readAll();
    if (data.isEmpty()) {
        qWarning() << "Recognition model is empty";
        return;
    }

    int ret = rknn_init(
        &context_, data.data(),
        static_cast<uint32_t>(data.size()), 0, nullptr);

    if (ret != RKNN_SUCC) {
        qWarning() << "Recognition rknn_init failed:" << ret;
        return;
    }

    rknn_input_output_num count{};
    ret = rknn_query(
        context_, RKNN_QUERY_IN_OUT_NUM, &count, sizeof(count));

    if (ret != RKNN_SUCC ||
        count.n_input != 1 || count.n_output != 1) {
        qWarning() << "Unexpected recognition tensor count";
        return;
    }

    rknn_tensor_attr input{};
    rknn_tensor_attr output{};

    const int inputRet = rknn_query(
        context_, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input));
    const int outputRet = rknn_query(
        context_, RKNN_QUERY_OUTPUT_ATTR, &output, sizeof(output));

    if (inputRet != RKNN_SUCC || outputRet != RKNN_SUCC) {
        qWarning() << "Cannot query recognition tensor attributes";
        return;
    }

    // The exported model may describe NCHW or NHWC. The current runtime accepts
    // the explicit NHWC uint8 submission below; dimensions are still checked to
    // prevent a 112x112 image from being sent to an unrelated model.
    const bool nchw =
        input.n_dims == 4 &&
        input.fmt == RKNN_TENSOR_NCHW &&
        input.dims[0] == 1 && input.dims[1] == 3 &&
        input.dims[2] == 112 && input.dims[3] == 112;

    const bool nhwc =
        input.n_dims == 4 &&
        input.fmt == RKNN_TENSOR_NHWC &&
        input.dims[0] == 1 && input.dims[1] == 112 &&
        input.dims[2] == 112 && input.dims[3] == 3;

    if ((!nchw && !nhwc) || output.n_elems != 512) {
        qWarning() << "Unexpected recognition model dimensions";
        return;
    }

    ready_ = true;
}

FaceRecognizer::~FaceRecognizer()
{
    // The context owns any runtime state allocated by rknn_init.
    if (context_) {
        rknn_destroy(context_);
    }
}

bool FaceRecognizer::isReady() const
{
    return ready_;
}

bool FaceRecognizer::extract(
    const QImage& alignedFace,
    Feature& feature,
    QString& error)
{
    feature.fill(0.0f);
    error.clear();

    if (!ready_) {
        error = QStringLiteral("特征模型未就绪");
        return false;
    }

    if (alignedFace.isNull() ||
        alignedFace.width() != 112 ||
        alignedFace.height() != 112) {
        error = QStringLiteral("特征输入必须是对齐后的 112×112 图片");
        return false;
    }

    // Convert before reading scan lines so channel order is deterministic even
    // if callers supplied another QImage format.
    const QImage rgb = alignedFace.convertToFormat(QImage::Format_RGB888);

    // 去除 QImage 行末填充，构造连续的 RGB 像素数组。
    std::vector<unsigned char> pixels(112 * 112 * 3);
    for (int y = 0; y < 112; ++y) {
        std::memcpy(
            pixels.data() + y * 112 * 3,
            rgb.constScanLine(y), 112 * 3);
    }

    // Mean/std normalization is configured inside the RKNN model. Reapplying it
    // here would alter embeddings and invalidate enrolled gallery features.
    rknn_input input{};
    input.index = 0;
    input.buf = pixels.data();
    input.size = static_cast<uint32_t>(pixels.size());
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.pass_through = 0;

    int ret = rknn_inputs_set(context_, 1, &input);
    if (ret != RKNN_SUCC) {
        error = QStringLiteral("设置特征输入失败：%1").arg(ret);
        return false;
    }

    ret = rknn_run(context_, nullptr);
    if (ret != RKNN_SUCC) {
        error = QStringLiteral("特征推理失败：%1").arg(ret);
        return false;
    }

    rknn_output output{};
    output.index = 0;
    output.want_float = 1;

    ret = rknn_outputs_get(context_, 1, &output, nullptr);
    if (ret != RKNN_SUCC) {
        error = QStringLiteral("读取特征输出失败：%1").arg(ret);
        return false;
    }

    // Copy before release because output.buf is allocated/owned by RKNN. The
    // size check also catches incompatible runtime/model combinations safely.
    Feature values{};
    const bool valid =
        output.buf != nullptr &&
        output.size >= values.size() * sizeof(float);

    if (valid) {
        std::memcpy(
            values.data(), output.buf,
            values.size() * sizeof(float));
    }

    ret = rknn_outputs_release(context_, 1, &output);
    if (!valid || ret != RKNN_SUCC) {
        error = QStringLiteral("特征输出无效或释放失败");
        return false;
    }

    double squaredNorm = 0.0;
    for (float value : values) {
        if (!std::isfinite(value)) {
            error = QStringLiteral("特征包含非有限数值");
            return false;
        }
        squaredNorm += static_cast<double>(value) * value;
    }

    const double norm = std::sqrt(squaredNorm);
    if (norm <= 1e-12) {
        error = QStringLiteral("模型输出零向量");
        return false;
    }

    // Publish only after every element and the norm are valid. Callers receive
    // an all-zero feature on every failure path from the function prologue.
    for (size_t i = 0; i < values.size(); ++i) {
        feature[i] = static_cast<float>(values[i] / norm);
    }
    return true;
}

bool FaceRecognizer::cosineSimilarity(
    const Feature& a,
    const Feature& b,
    double& score)
{
    score = std::numeric_limits<double>::quiet_NaN();

    // Recompute norms instead of assuming callers normalized their vectors; this
    // keeps the helper valid for diagnostics and rejects malformed inputs.
    double dot = 0.0;
    double normA = 0.0;
    double normB = 0.0;

    for (size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) {
            return false;
        }

        dot += static_cast<double>(a[i]) * b[i];
        normA += static_cast<double>(a[i]) * a[i];
        normB += static_cast<double>(b[i]) * b[i];
    }

    if (normA <= 1e-24 || normB <= 1e-24) {
        return false;
    }

    score = std::clamp(
        dot / (std::sqrt(normA) * std::sqrt(normB)),
        -1.0, 1.0);
    return true;
}
