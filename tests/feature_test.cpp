/*
 * RKNN embedding-model diagnostic.
 *
 * This standalone tool validates the model contract for one supplied 112x112
 * aligned image: tensor attributes, finite 512-dimensional output and L2
 * normalization. It does not open the attendance database or a camera. Its
 * output feature file is a sensitive derivative of a face image and is ignored.
 */
#include <QCoreApplication>
#include <QImage>
#include <QFile>
#include <QByteArray>
#include <QStringList>
#include "rknn_api.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

// RAII keeps the RKNN context balanced on every exception/early exit.
struct Model {
    rknn_context context = 0;
    ~Model() {
        if (context) rknn_destroy(context);
    }
};

static void check(int ret, const char* operation)
{
    if (ret != RKNN_SUCC) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + std::to_string(ret));
    }
}

static void printAttr(const rknn_tensor_attr& attr)
{
    std::cout << "tensor[" << attr.index << "] name=" << attr.name
              << " shape=";
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        std::cout << (i ? "x" : "") << attr.dims[i];
    }
    std::cout << " elements=" << attr.n_elems
              << " format=" << attr.fmt
              << " type=" << attr.type << "\n";
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    if (args.size() != 3) {
        std::cerr << "Usage: feature_test MODEL.rknn ALIGNED.png\n";
        return 1;
    }

    try {
        QFile file(args[1]);
        if (!file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("Cannot open model");
        }
        QByteArray modelData = file.readAll();
        if (modelData.isEmpty()) {
            throw std::runtime_error("Model file is empty");
        }

        Model model;
        check(rknn_init(&model.context, modelData.data(),
                        static_cast<uint32_t>(modelData.size()),
                        0, nullptr), "rknn_init");

        rknn_input_output_num count{};
        check(rknn_query(model.context, RKNN_QUERY_IN_OUT_NUM,
                         &count, sizeof(count)), "query tensor count");

        if (count.n_input != 1 || count.n_output != 1) {
            throw std::runtime_error("Expected one input and one output");
        }

        rknn_tensor_attr inputAttr{};
        rknn_tensor_attr outputAttr{};
        check(rknn_query(model.context, RKNN_QUERY_INPUT_ATTR,
                         &inputAttr, sizeof(inputAttr)), "query input");
        check(rknn_query(model.context, RKNN_QUERY_OUTPUT_ATTR,
                         &outputAttr, sizeof(outputAttr)), "query output");
        printAttr(inputAttr);
        printAttr(outputAttr);

        const bool nchw =
            inputAttr.fmt == RKNN_TENSOR_NCHW &&
            inputAttr.n_dims == 4 &&
            inputAttr.dims[0] == 1 && inputAttr.dims[1] == 3 &&
            inputAttr.dims[2] == 112 && inputAttr.dims[3] == 112;
        const bool nhwc =
            inputAttr.fmt == RKNN_TENSOR_NHWC &&
            inputAttr.n_dims == 4 &&
            inputAttr.dims[0] == 1 && inputAttr.dims[1] == 112 &&
            inputAttr.dims[2] == 112 && inputAttr.dims[3] == 3;

        if ((!nchw && !nhwc) || outputAttr.n_elems != 512) {
            throw std::runtime_error("Unexpected model dimensions");
        }

        QImage image(args[2]);
        if (image.isNull() || image.width() != 112 ||
            image.height() != 112) {
            throw std::runtime_error("Expected an aligned 112x112 image");
        }
        image = image.convertToFormat(QImage::Format_RGB888);

        // 去除 QImage 行末可能存在的填充，生成连续 RGB 数据。
        std::vector<unsigned char> pixels(112 * 112 * 3);
        for (int y = 0; y < 112; ++y) {
            std::memcpy(pixels.data() + y * 112 * 3,
                        image.constScanLine(y), 112 * 3);
        }

        // 模型转换时已配置均值和标准差，此处直接输入 RGB uint8。
        // pass_through=0 允许运行时完成类型和布局转换。
        rknn_input input{};
        input.index = 0;
        input.buf = pixels.data();
        input.size = static_cast<uint32_t>(pixels.size());
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;

        check(rknn_inputs_set(model.context, 1, &input), "set input");
        check(rknn_run(model.context, nullptr), "run");

        rknn_output output{};
        output.index = 0;
        output.want_float = 1;
        check(rknn_outputs_get(model.context, 1, &output, nullptr),
              "get output");

        // 先复制结果再释放 RKNN 输出内存。
        std::vector<float> feature(512);
        const bool validBuffer =
            output.buf && output.size >= feature.size() * sizeof(float);
        if (validBuffer) {
            std::memcpy(feature.data(), output.buf,
                        feature.size() * sizeof(float));
        }
        check(rknn_outputs_release(model.context, 1, &output),
              "release output");
        if (!validBuffer) {
            throw std::runtime_error("Invalid output buffer");
        }

        double squaredNorm = 0;
        for (float value : feature) {
            if (!std::isfinite(value)) {
                throw std::runtime_error("Output contains NaN or infinity");
            }
            squaredNorm += static_cast<double>(value) * value;
        }
        const double norm = std::sqrt(squaredNorm);
        if (norm < 1e-12) {
            throw std::runtime_error("Feature vector is zero");
        }

        for (float& value : feature) {
            value = static_cast<float>(value / norm);
        }

        std::cout << "Feature dimensions: 512\n"
                  << "Raw L2 norm: " << norm << "\n"
                  << "Normalized first 8:";
        for (int i = 0; i < 8; ++i) {
            std::cout << " " << feature[i];
        }

        // 保存完整的 L2 归一化特征，每行一个数，供相似度测试使用。
// The optional output is placed next to the caller-supplied input image; keep
// it outside the repository because it is a biometric-derived artifact.
QByteArray featureText;
for (float value : feature) {
    featureText += QByteArray::number(
        static_cast<double>(value), 'g', 9);
    featureText += '\n';
}

QFile featureFile(args[2] + ".feature.txt");
if (!featureFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
    featureFile.write(featureText) != featureText.size()) {
    throw std::runtime_error("Cannot save feature file");
}
featureFile.close();

        std::cout << "\nFeature extraction OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
