/*
 * RetinaFace RKNN implementation.
 *
 * Pipeline per frame:
 *   QImage -> RGB888 -> aspect-preserving 320x320 letterbox -> BGR bytes
 *   -> rknn_inputs_set/run/outputs_get -> 4200-prior decode -> NMS.
 * Boxes and landmarks are mapped back to the original camera coordinate space.
 *
 * This implementation intentionally uses normal host memory. It measures each
 * stage once per second so profiling can identify CPU copy/preprocess costs
 * without printing per-frame model tensors during normal operation.
 */
#include "face_detector.h"
#include "rknn_box_priors.h"

#include <QFile>
#include <QPainter>
#include <QDebug>
#include <functional>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <iostream>

namespace {

// Model contract. BOX_PRIORS_320 contains exactly 4200 priors for this size.
constexpr int INPUT_SIZE = 320;
constexpr float SCORE_THRESHOLD = 0.50f;
constexpr float NMS_THRESHOLD = 0.40f;

struct Prior {
    float cx;
    float cy;
    float w;
    float h;
};

std::vector<Prior> makePriors()
{
    const int steps[] = {8, 16, 32};
    const int min_sizes[][2] = {
        {16, 32},
        {64, 128},
        {256, 512}
    };

    std::vector<Prior> priors;

    for (int level = 0; level < 3; ++level) {
        const int step = steps[level];
        const int feature_size = INPUT_SIZE / step;

        for (int y = 0; y < feature_size; ++y) {
            for (int x = 0; x < feature_size; ++x) {
                for (int k = 0; k < 2; ++k) {
                    const float size =
                        static_cast<float>(min_sizes[level][k]);

                    priors.push_back({
                        (x + 0.5f) * step / INPUT_SIZE,
                        (y + 0.5f) * step / INPUT_SIZE,
                        size / INPUT_SIZE,
                        size / INPUT_SIZE
                    });
                }
            }
        }
    }

    return priors;
}

// Intersection-over-union is only used after decoding to suppress overlapping
// boxes. It is intentionally independent of RKNN tensor types.
float calculateIoU(const QRectF& a, const QRectF& b)
{
    const QRectF inter = a.intersected(b);

    if (inter.isEmpty()) {
        return 0.0f;
    }

    const double inter_area =
        inter.width() * inter.height();

    const double union_area =
        a.width() * a.height() +
        b.width() * b.height() -
        inter_area;

    return union_area > 0.0
        ? static_cast<float>(inter_area / union_area)
        : 0.0f;
}

float confidenceValue(const float* values)
{
    const float a = values[0];
    const float b = values[1];

    if (a >= 0.0f && a <= 1.0f &&
        b >= 0.0f && b <= 1.0f) {
        return b;
    }

    const float max_value = std::max(a, b);
    const float ea = std::exp(a - max_value);
    const float eb = std::exp(b - max_value);

    return eb / (ea + eb);
}

float clampFloat(float value, float low, float high)
{
    return std::max(low, std::min(value, high));
}

} // namespace

FaceDetector::FaceDetector(const QString& model_path)
{
    // Diagnostics are opt-in because terminal output per frame changes timing.
    verboseScores_ = qEnvironmentVariableIntValue("ATTENDANCE_DEBUG_SCORES") == 1;
    legacyResize_ = qEnvironmentVariableIntValue("ATTENDANCE_LEGACY_RESIZE") == 1;
    qInfo() << "Detector profiling: debug_scores=" << verboseScores_
            << "legacy_resize=" << legacyResize_;
    ready_ = loadModel(model_path);
}

FaceDetector::~FaceDetector()
{
    release();
}

bool FaceDetector::isReady() const
{
    return ready_;
}

bool FaceDetector::loadModel(const QString& model_path)
{
    // Keep model bytes alive until rknn_destroy(). Some runtime versions retain
    // references to the passed buffer rather than copying it during rknn_init.
    QFile file(model_path);

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray bytes = file.readAll();

    model_data_.assign(bytes.begin(), bytes.end());

    if (model_data_.empty()) {
        return false;
    }

    const int ret = rknn_init(
        &context_,
        model_data_.data(),
        static_cast<uint32_t>(model_data_.size()),
        0,
        nullptr
    );

    if (ret < 0) {
        context_ = 0;
        return false;
    }

    // Reject an unexpected model rather than decoding unrelated tensors with the
    // RetinaFace layout below.
    rknn_input_output_num io_num {};

    if (rknn_query(
            context_,
            RKNN_QUERY_IN_OUT_NUM,
            &io_num,
            sizeof(io_num)) < 0 ||
        io_num.n_input != 1 ||
        io_num.n_output != 3) {
        release();
        return false;
    }

    return true;
}

void FaceDetector::release()
{
    // Destruction order matters: destroy the RKNN context before its model bytes.
    if (context_ != 0) {
        rknn_destroy(context_);
        context_ = 0;
    }

    model_data_.clear();
    ready_ = false;
}

std::vector<FaceDetection>
FaceDetector::detect(const QImage& image)
{
    std::vector<FaceDetection> detections;

    if (!ready_ || image.isNull()) {
        return detections;
    }

    using Timer = std::chrono::steady_clock;
    const auto callStart = Timer::now();
    auto boundary = callStart;
    // Each mark charges elapsed time to the preceding stage. The final RAII
    // reporter runs even on an RKNN error return, so profiler failures are not
    // silently omitted from a time window.
    auto mark = [&](size_t stage) {
        const auto now = Timer::now();
        const double ms = std::chrono::duration<double, std::milli>(now - boundary).count();
        timingSum_[stage] += ms;
        timingMax_[stage] = std::max(timingMax_[stage], ms);
        ++timingCount_[stage];
        boundary = now;
    };
    bool success = false;
    // 所有提前返回也汇总计时和失败次数；输出日志本身不计入阶段。
    struct OnExit { std::function<void()> action; ~OnExit() { action(); } } report{[&] {
        const auto now = Timer::now();
        const double ms = std::chrono::duration<double, std::milli>(now - callStart).count();
        timingSum_[6] += ms; timingMax_[6] = std::max(timingMax_[6], ms); ++timingCount_[6];
        if (!success) ++timingFailures_;
        if (now - timingStart_ < std::chrono::seconds(1)) return;
        const char* names[] = {"preprocess", "inputs_set", "run", "outputs_get", "decode_debug", "release_nms", "total"};
        QString message = QStringLiteral("detector timing ms: failures=%1").arg(timingFailures_);
        for (size_t i = 0; i < 7; ++i) {
            message += QStringLiteral(" %1_n=%2 %1_avg=%3 %1_max=%4")
                .arg(QString::fromLatin1(names[i])).arg(timingCount_[i])
                .arg(timingCount_[i] ? timingSum_[i]/timingCount_[i] : 0, 0, 'f', 3)
                .arg(timingMax_[i], 0, 'f', 3);
        }
        qInfo().noquote() << message;
        timingSum_.fill(0); timingMax_.fill(0); timingCount_.fill(0);
        timingFailures_ = 0; timingStart_ = Timer::now();
    }};

    // Retained only to compare against an earlier accidental unused resize. It
    // is disabled by default and must never be enabled for baseline profiling.
    if (legacyResize_) {
        image.convertToFormat(QImage::Format_RGB888)
         .scaled(INPUT_SIZE,
                 INPUT_SIZE,
                 Qt::IgnoreAspectRatio,
                 Qt::SmoothTransformation);
    }

    // Letterbox rather than stretch the camera frame. scale/pad are reused when
    // mapping decoded 320x320 coordinates back into the original image.
    const QImage source = image.convertToFormat(QImage::Format_RGB888);

const float scale = std::min(
    static_cast<float>(INPUT_SIZE) / source.width(),
    static_cast<float>(INPUT_SIZE) / source.height());

const int resized_width =
    static_cast<int>(std::round(source.width() * scale));

const int resized_height =
    static_cast<int>(std::round(source.height() * scale));

const int pad_x = (INPUT_SIZE - resized_width) / 2;
const int pad_y = (INPUT_SIZE - resized_height) / 2;

const QImage scaled =
    source.scaled(resized_width,
                  resized_height,
                  Qt::IgnoreAspectRatio,
                  Qt::SmoothTransformation);

QImage resized(INPUT_SIZE,
               INPUT_SIZE,
               QImage::Format_RGB888);

resized.fill(qRgb(114, 114, 114));

{
    QPainter painter(&resized);
    painter.drawImage(pad_x, pad_y, scaled);
}

    // RKNN accepts an uint8 NHWC image here and performs the model-specific
    // conversion internally. This allocation/copy path is measured because it
    // is a known candidate for a future RGA/DMA-BUF experiment.
    std::vector<unsigned char> input_data(INPUT_SIZE * INPUT_SIZE * 3);

    // RetinaFace 模型使用 BGR，均值已在转换时配置为
    // [104, 117, 123]。
    for (int y = 0; y < INPUT_SIZE; ++y) {
        const unsigned char* src =
            resized.constScanLine(y);

        unsigned char* dst =
            input_data.data() + y * INPUT_SIZE * 3;

        for (int x = 0; x < INPUT_SIZE; ++x) {
            const unsigned char r = src[x * 3 + 0];
            const unsigned char g = src[x * 3 + 1];
            const unsigned char b = src[x * 3 + 2];

            dst[x * 3 + 0] = b;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = r;
        }
    }

    rknn_input input {};
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.size = static_cast<uint32_t>(input_data.size());
    input.fmt = RKNN_TENSOR_NHWC;
    input.buf = input_data.data();

    mark(0);
    const int inputResult = rknn_inputs_set(context_, 1, &input);
    mark(1);
    if (inputResult < 0) return detections;
    const int runResult = rknn_run(context_, nullptr);
    mark(2);
    if (runResult < 0) {
        return detections;
    }

    rknn_output outputs[3] {};

    for (auto& output : outputs) {
        output.want_float = 1;
    }

    // want_float asks RKNN to return host-readable float values even though the
    // model's native tensors may use a different representation.
    const int outputResult = rknn_outputs_get(context_, 3, outputs, nullptr);
    mark(3);
    if (outputResult < 0) {
        return detections;
    }

    const float* locations =
        static_cast<const float*>(outputs[0].buf);

    const float* confidences =
        static_cast<const float*>(outputs[1].buf);

    const float* landmarks =
        static_cast<const float*>(outputs[2].buf);


    // Raw confidence logging is useful while validating tensor order, but is
    // deliberately disabled in normal runs to avoid terminal-I/O jitter.
if (verboseScores_) {
    float max_score = 0.0f;
int max_index = -1;

for (int i = 0; i < 4200; ++i) {
    const float raw0 = confidences[i * 2 + 0];
    const float raw1 = confidences[i * 2 + 1];

    const float score = confidences[i * 2 + 1];

    if (score > max_score) {
        max_score = score;
        max_index = i;
    }

    if (i < 5) {
        std::cerr << "conf[" << i << "] = "
                  << raw0 << ", " << raw1
                  << " score=" << score << "\n";
    }
}

std::cerr << "max face score="
          << max_score
          << " index=" << max_index
          << "\n";
}

    const float inv_scale = 1.0f / scale;

for (int i = 0; i < 4200; ++i) {
    const float score = confidences[i * 2 + 1];

    if (score < SCORE_THRESHOLD) {
        continue;
    }

    const float prior_cx = BOX_PRIORS_320[i][0];
    const float prior_cy = BOX_PRIORS_320[i][1];
    const float prior_w  = BOX_PRIORS_320[i][2];
    const float prior_h  = BOX_PRIORS_320[i][3];

    const float* loc = locations + i * 4;
    const float* land = landmarks + i * 10;

    const float cx =
        prior_cx + loc[0] * 0.1f * prior_w;
    const float cy =
        prior_cy + loc[1] * 0.1f * prior_h;
    const float width =
        prior_w * std::exp(loc[2] * 0.2f);
    const float height =
        prior_h * std::exp(loc[3] * 0.2f);

    const float x1_320 = (cx - width * 0.5f) * INPUT_SIZE;
    const float y1_320 = (cy - height * 0.5f) * INPUT_SIZE;
    const float x2_320 = (cx + width * 0.5f) * INPUT_SIZE;
    const float y2_320 = (cy + height * 0.5f) * INPUT_SIZE;

    const float x1 = (x1_320 - pad_x) * inv_scale;
    const float y1 = (y1_320 - pad_y) * inv_scale;
    const float x2 = (x2_320 - pad_x) * inv_scale;
    const float y2 = (y2_320 - pad_y) * inv_scale;

    // Decode one candidate from prior-relative offsets, then clamp it to the
    // camera frame. Clamping prevents downstream crop/alignment code receiving
    // coordinates just outside an image edge.
    FaceDetection detection;
    detection.score = score;
    detection.box = QRectF(
        clampFloat(x1, 0.0f, image.width()),
        clampFloat(y1, 0.0f, image.height()),
        clampFloat(x2 - x1, 0.0f, image.width()),
        clampFloat(y2 - y1, 0.0f, image.height())
    );

    for (int point = 0; point < 5; ++point) {
        const float px_320 =
            (prior_cx + land[point * 2] * 0.1f * prior_w) *
            INPUT_SIZE;
        const float py_320 =
            (prior_cy + land[point * 2 + 1] * 0.1f * prior_h) *
            INPUT_SIZE;

        const float px = (px_320 - pad_x) * inv_scale;
        const float py = (py_320 - pad_y) * inv_scale;

        detection.landmarks[point] = QPointF(
            clampFloat(px, 0.0f, image.width()),
            clampFloat(py, 0.0f, image.height())
        );
    }

    detections.push_back(detection);
}

mark(4);
    // From this point output.buf is no longer valid. All decoded Qt values were
    // copied before release, so NMS does not depend on RKNN-owned memory.
    const int releaseResult = rknn_outputs_release(context_, 3, outputs);

std::sort(
    detections.begin(),
    detections.end(),
    [](const FaceDetection& a, const FaceDetection& b) {
        return a.score > b.score;
    }
);

    // NMS keeps every distinct face, not merely the top-scoring detection. The
    // main workflow later requires exactly one face for enrollment/recognition.
std::vector<FaceDetection> filtered;

for (const FaceDetection& candidate : detections) {
    bool suppressed = false;

    for (const FaceDetection& kept : filtered) {
        if (calculateIoU(candidate.box, kept.box) > NMS_THRESHOLD) {
            suppressed = true;
            break;
        }
    }

    if (!suppressed) {
        filtered.push_back(candidate);
    }
}

mark(5);
success = releaseResult >= 0;
return filtered;

}
