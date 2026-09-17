/*
 * Read-only RKNN model-inspection tool. It prints tensor shapes, strides and
 * quantization metadata without running a camera or touching SQLite. Use it to
 * verify a candidate model before considering tensor-memory/zero-copy work.
 */
#include "rknn_api.h"

#include <QCoreApplication>
#include <QFile>
#include <QDebug>

#include <vector>

namespace {
void printAttr(const char* kind, rknn_tensor_attr attr)
{
    qInfo().noquote() << QStringLiteral("%1[%2] name=%3 n_dims=%4 dims=%5x%6x%7x%8 "
        "n_elems=%9 size=%10 size_with_stride=%11 fmt=%12 type=%13 qnt=%14 zp=%15 scale=%16")
        .arg(QString::fromLatin1(kind)).arg(attr.index)
        .arg(QString::fromLatin1(attr.name))
        .arg(attr.n_dims).arg(attr.dims[0]).arg(attr.dims[1])
        .arg(attr.dims[2]).arg(attr.dims[3]).arg(attr.n_elems)
        .arg(attr.size).arg(attr.size_with_stride).arg(attr.fmt)
        .arg(attr.type).arg(attr.qnt_type).arg(attr.zp).arg(attr.scale, 0, 'g', 8);
}
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        qCritical() << "Usage:" << argv[0] << "model.rknn";
        return 2;
    }
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "Cannot read model:" << file.errorString();
        return 1;
    }
    const QByteArray bytes = file.readAll();
    rknn_context context = 0;
    if (rknn_init(&context, const_cast<char*>(bytes.constData()), bytes.size(), 0, nullptr) < 0) {
        qCritical() << "rknn_init failed";
        return 1;
    }
    rknn_input_output_num count{};
    const int queryResult = rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &count, sizeof(count));
    if (queryResult < 0) {
        qCritical() << "RKNN_QUERY_IN_OUT_NUM failed:" << queryResult;
        rknn_destroy(context);
        return 1;
    }
    qInfo() << "RKNN tensors: inputs=" << count.n_input << "outputs=" << count.n_output;
    for (uint32_t index = 0; index < count.n_input; ++index) {
        rknn_tensor_attr attr{};
        attr.index = index;
        if (rknn_query(context, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr)) < 0) {
            qCritical() << "Input attribute query failed:" << index;
            rknn_destroy(context);
            return 1;
        }
        printAttr("input", attr);
    }
    for (uint32_t index = 0; index < count.n_output; ++index) {
        rknn_tensor_attr attr{};
        attr.index = index;
        if (rknn_query(context, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) < 0) {
            qCritical() << "Output attribute query failed:" << index;
            rknn_destroy(context);
            return 1;
        }
        printAttr("output", attr);
    }
    rknn_destroy(context);
    return 0;
}
