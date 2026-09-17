/*
 * Edge Face Attendance - application entry point
 *
 * This file composes the device-facing V4L2 camera, Qt portrait user interface,
 * SQLite persistence, RetinaFace detection, five-point alignment and RKNN face
 * embedding into one offline attendance terminal.  The intended target is an
 * Orange Pi 5/RK3588 running Qt5 with the linuxfb platform plugin.
 *
 * Ownership boundary: the Qt GUI thread exclusively owns AttendanceDatabase;
 * capture and inference use in-memory snapshots only.  Capture keeps just the
 * latest frame, deliberately favouring real-time display over processing every
 * camera frame.  The inference thread owns both RKNN model objects.
 */
#include "face_alignment.h"
#include "face_recognizer.h"
#include "recognition_state.h"
#include "person_ranking.h"
#include "portrait_shell.h"
#include <set>
#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <sys/timex.h>
#include "face_detector.h"
#include "database.h"
#include <QListWidget>
#include <QDir>
#include <QUuid>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMessageBox>
#include <QDialog>
#include <QGridLayout>
#include <QLineEdit>
#include <QInputDialog>
#include <QElapsedTimer>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// V4L2 calls may be interrupted by a signal.  Retrying EINTR here keeps each
// camera operation concise while still letting real driver errors propagate.
static int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do { r = ioctl(fd, request, arg); } while (r < 0 && errno == EINTR);
    return r;
}

/*
 * Minimal V4L2 multi-planar NV12 camera wrapper.
 *
 * Four mmap buffers are queued to the driver. grabFrame() dequeues one buffer,
 * validates its reported payload, converts it into an owned RGB QImage, then
 * requeues it on every path after a successful dequeue.  The QImage copy is
 * intentional: V4L2 owns mmap memory and may overwrite it after QBUF.
 */
class Camera {
    struct Buffer { void* data = nullptr; size_t length = 0; };
    int fd_ = -1;
    bool streaming_ = false;
    unsigned width_ = 720, height_ = 480, stride_ = 720;
    std::vector<Buffer> buffers_;

    // These are board/camera-module tuning values, not recognition thresholds.
    // A deployment with another sensor must verify that /dev/v4l-subdev2 and
    // its control ranges exist before retaining them.
    bool setControls() {
        int sensor = open("/dev/v4l-subdev2", O_RDWR);
        if (sensor < 0) { perror("open sensor"); return false; }
        bool ok = true;
        for (const auto& setting : std::vector<std::pair<__u32, int>>{
                 {V4L2_CID_EXPOSURE, 3210}, {V4L2_CID_ANALOGUE_GAIN, 1024}}) {
            v4l2_control control {};
            control.id = setting.first;
            control.value = setting.second;
            if (xioctl(sensor, VIDIOC_S_CTRL, &control) < 0) {
                perror("set sensor control"); ok = false;
            }
        }
        close(sensor);
        return ok;
    }
public:
    // Stop streaming before unmapping buffers.  V4L2 uses this order to avoid
    // leaving the driver with outstanding mappings during process shutdown.
    ~Camera() {
        if (streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
        for (auto& b : buffers_) if (b.data) munmap(b.data, b.length);
        if (fd_ >= 0) close(fd_);
    }
    // Negotiate the only capture format currently supported by this prototype:
    // even-sized, single-plane NV12. bytesperline is honoured during conversion
    // because it need not equal the negotiated width on every camera driver.
    bool openCamera() {
        fd_ = open("/dev/video-camera0", O_RDWR | O_NONBLOCK);
        if (fd_ < 0) { perror("open camera"); return false; }
        v4l2_format fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width = width_;
        fmt.fmt.pix_mp.height = height_;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes = 1;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return false; }
        const auto& p = fmt.fmt.pix_mp;
        if (p.pixelformat != V4L2_PIX_FMT_NV12 || p.num_planes != 1 ||
            !p.width || !p.height || p.width % 2 || p.height % 2) {
            std::cerr << "Unsupported camera format\n"; return false;
        }
        width_ = p.width; height_ = p.height;
        stride_ = p.plane_fmt[0].bytesperline;
        if (stride_ < width_) return false;
        if (!setControls()) return false;
        v4l2_requestbuffers req {};
        req.count = 4; req.type = fmt.type; req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) return false;
        buffers_.resize(req.count);
        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_buffer buf {}; v4l2_plane plane {};
            buf.type = req.type; buf.memory = req.memory; buf.index = i;
            buf.length = 1; buf.m.planes = &plane;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) return false;
            void* ptr = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd_, plane.m.mem_offset);
            if (ptr == MAP_FAILED) return false;
            buffers_[i] = {ptr, plane.length};
            if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) return false;
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return false;
        streaming_ = true;
        qInfo() << "Camera NV12:" << width_ << height_ << "stride:" << stride_;
        return true;
    }
    // Return 0 when a non-blocking DQBUF has no completed frame, 1 on success,
    // and -1 for a malformed buffer or camera error.
    int grabFrame(QImage& image) {
        v4l2_buffer buf {}; v4l2_plane plane {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP; buf.length = 1; buf.m.planes = &plane;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return 0;
            perror("DQBUF"); return -1;
        }
        const size_t needed = size_t(stride_) * height_ * 3 / 2;
        bool valid = buf.index < buffers_.size();
        if (valid) valid = plane.data_offset <= buffers_[buf.index].length &&
            needed <= buffers_[buf.index].length - plane.data_offset &&
            plane.bytesused >= plane.data_offset &&
            needed <= plane.bytesused - plane.data_offset && !(buf.flags & V4L2_BUF_FLAG_ERROR);
        if (valid) {
            const auto* src = static_cast<const unsigned char*>(buffers_[buf.index].data) + plane.data_offset;
            const auto* uv = src + size_t(stride_) * height_;
            // CPU NV12 -> RGB conversion.  It is currently a measured hot path;
            // no RGA or camera-to-NPU zero-copy path is implemented here.
            image = QImage(width_, height_, QImage::Format_RGB888);
            if (image.isNull()) valid = false;
            else for (unsigned y = 0; y < height_; ++y) {
                auto* dst = image.scanLine(y);
                for (unsigned x = 0; x < width_; ++x) {
                    int yy = src[size_t(y) * stride_ + x];
                    size_t k = size_t(y / 2) * stride_ + (x / 2) * 2;
                    int u = int(uv[k]) - 128, v = int(uv[k + 1]) - 128;
                    dst[x * 3] = std::clamp(yy + 1436 * v / 1000, 0, 255);
                    dst[x * 3 + 1] = std::clamp(yy - (352 * u + 731 * v) / 1000, 0, 255);
                    dst[x * 3 + 2] = std::clamp(yy + 1815 * u / 1000, 0, 255);
                }
            }
        }
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return -1; }
        return valid ? 1 : -1;
    }
};

// A gallery entry contains one sample embedding and its owning person. sampleId
// identifies a particular enrollment image; personId is the stable identity
// used for ranking and daily attendance.
struct ReferenceFeature {
    qint64 sampleId = -1;
    qint64 personId = -1;
    QString name;
    QString employeeNo;
    FaceRecognizer::Feature feature{};
};

// Decode one stored enrollment sample into a normalized, comparable embedding.
// Stored landmarks use original-frame coordinates, whereas facePng is the crop;
// subtracting crop origin is essential before affine alignment.
static bool buildReference(const EnrollmentSample& sample,
                           FaceRecognizer& recognizer,
                           ReferenceFeature& reference,
                           QString& error)
{
    const QImage crop = QImage::fromData(sample.facePng, "PNG");
    if (sample.personId <= 0 || sample.personName.trimmed().isEmpty() ||
        sample.employeeNo.trimmed().isEmpty() || crop.isNull() ||
        sample.sourceSize.isEmpty() || sample.crop.isEmpty() ||
        !QRect(QPoint(0, 0), sample.sourceSize).contains(sample.crop) ||
        crop.size() != sample.crop.size()) {
        error = QStringLiteral("参考样本图片或裁剪元数据无效");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        sample.landmarksJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isArray() || document.array().size() != 5) {
        error = QStringLiteral("参考样本需要五个关键点");
        return false;
    }
    std::array<QPointF, 5> landmarks;
    const QJsonArray points = document.array();
    for (int i = 0; i < 5; ++i) {
        const QJsonArray pair = points[i].toArray();
        if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()) {
            error = QStringLiteral("参考关键点格式错误");
            return false;
        }
        // PNG 是裁剪图，关键点则以原图为坐标系，必须减去裁剪起点。
        landmarks[i] = QPointF(pair[0].toDouble() - sample.crop.x(),
                               pair[1].toDouble() - sample.crop.y());
    }
    QImage aligned;
    double rms = 0.0;
    if (!alignFace112(crop, landmarks, aligned, rms, error) ||
        !recognizer.extract(aligned, reference.feature, error)) return false;
    reference.sampleId = sample.id;
    reference.personId = sample.personId;
    reference.name = sample.personName;
    reference.employeeNo = sample.employeeNo;
    qInfo() << "Reference ready: sample_id=" << sample.id
            << "person_id=" << sample.personId << "name=" << sample.personName
            << "employee_no=" << sample.employeeNo
            << "alignment RMS=" << rms;
    return true;
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Runtime state stays next to the executable for simple device backup. It
    // contains biometric/personal data and is intentionally excluded by .gitignore.
const QString databasePath =
    QCoreApplication::applicationDirPath()
    + QStringLiteral("/data/attendance.db");

AttendanceDatabase database;

if (!database.open(databasePath)) {
    qCritical() << "Cannot open attendance database:"
                << databasePath;
    return 1;
}

if (!database.initialize()) {
    qCritical() << "Cannot initialize attendance database";
    return 1;
}

    /*
     * Gallery hand-off protocol.
     *
     * The GUI reloads sample metadata from SQLite, marks the old gallery invalid,
     * and increments galleryVersion.  The inference thread later builds RKNN
     * embeddings from that immutable copy. Results built for an old version are
     * never allowed to confirm or check in a person.
     */
    std::vector<EnrollmentSample> referenceSamples;
    const bool initialGalleryLoaded = database.loadBoundEnrollmentSamples(referenceSamples);
    if (!initialGalleryLoaded) {
        qWarning() << "Cannot load bound references; detection remains active";
    }
    std::atomic<bool> referencesInvalidated{true};
    std::mutex galleryMutex;
    std::atomic<quint64> galleryVersion{1};
    quint64 snapshotVersion = initialGalleryLoaded ? 1 : 0;
    std::atomic<int> galleryPeople{0};
    // SQLite 只在界面线程访问；推理线程只接收完整快照。
    auto reloadGallery = [&]() {
        {
            std::lock_guard<std::mutex> lock(galleryMutex);
            referencesInvalidated = true;
            ++galleryVersion;
        }
        std::vector<EnrollmentSample> fresh;
        if (!database.loadBoundEnrollmentSamples(fresh)) return false;
        std::lock_guard<std::mutex> lock(galleryMutex);
        referenceSamples = std::move(fresh);
        snapshotVersion = galleryVersion.load();
        return true;
    };
    QTimer galleryRetry;
    QObject::connect(&galleryRetry, &QTimer::timeout, [&] {
        if (referencesInvalidated.load()) {
            std::lock_guard<std::mutex> lock(galleryMutex);
            if (snapshotVersion == galleryVersion.load()) return;
        } else return;
        reloadGallery();
    });
    galleryRetry.start(5000);

    using Clock = std::chrono::steady_clock;
    FaceDetector detector(QCoreApplication::applicationDirPath() +
                          "/models/RetinaFace_mobile320_rk3588.rknn");
    if (!detector.isReady()) { qCritical() << "Face model load failed"; return 1; }
    Camera camera;
    if (!camera.openCamera()) { qCritical() << "Camera initialization failed"; return 1; }

    // The logical page is 480x720 portrait. PortraitShell maps this Qt widget to
    // the board's landscape framebuffer and transforms touch/mouse input too.
    QWidget window;
    window.setStyleSheet(
        "QWidget { background:#0b1220; color:#eef4ff; font-size:18px; }"
        "QPushButton { background:#1a2940; border:1px solid #30445f;"
        "border-radius:10px; padding:8px; min-height:32px; }"
        "QPushButton:pressed { background:#245887; }"
        "QPushButton:disabled { color:#718096; background:#131e2d; }"
        "QLineEdit { background:#14233a; border:1px solid #3c5778; padding:8px; }"
        "QListWidget { background:#111e30; border:1px solid #30445f; }"
        "QListWidget::item:selected { background:#245887; }"
        "QScrollBar:vertical { width:20px; }");
    PortraitShell shell(window);

    QVBoxLayout layout(&window);
    layout.setContentsMargins(16, 18, 16, 14);
    layout.setSpacing(10);
    QLabel heading(QStringLiteral("人脸考勤"));
    heading.setStyleSheet("font-size:28px; font-weight:bold;");
    QLabel clockLabel;
    clockLabel.setStyleSheet("color:#a9bbd2; font-size:16px;");
    layout.addWidget(&heading);
    layout.addWidget(&clockLabel);

    QLabel preview;
    QLabel status(QStringLiteral("正在等待摄像头画面…"));

    preview.setAlignment(Qt::AlignCenter);
    preview.setMinimumSize(1, 1);
    preview.setSizePolicy(
        QSizePolicy::Ignored,
        QSizePolicy::Ignored
    );

    status.setAlignment(Qt::AlignCenter);
    status.setFixedHeight(100);
    status.setWordWrap(true);
    status.setStyleSheet(
        "font-size:20px; background:#142b40; border-radius:12px; padding:8px;"
    );

    QPushButton enrollButton(
        QStringLiteral("录入人员")
    );
    enrollButton.setMinimumHeight(44);
    enrollButton.setEnabled(false);

    QPushButton exitButton(
        QStringLiteral("退出")
    );
    exitButton.setMinimumHeight(44);

    layout.addWidget(&preview, 1);
    layout.addWidget(&status);
    preview.setStyleSheet("background:#050b13; border-radius:12px;");

    QPushButton samplesButton(QStringLiteral("已录入样本"));
samplesButton.setMinimumHeight(44);
    QPushButton attendanceButton(QStringLiteral("考勤记录"));
    attendanceButton.setMinimumHeight(44);
    QPushButton manageButton(QStringLiteral("管理"));
    QHBoxLayout bottomActions;
    bottomActions.addWidget(&enrollButton, 1);
    bottomActions.addWidget(&manageButton, 1);
    layout.addLayout(&bottomActions);
    QLabel footer(QStringLiteral("本地离线运行 · 开发测试版"));
    footer.setAlignment(Qt::AlignCenter);
    footer.setStyleSheet("font-size:14px; color:#8195b0;");
    layout.addWidget(&footer);
    // The management menu dispatches to these existing action entry points. The
    // buttons stay hidden on the home page, avoiding duplicate UI implementations.
    samplesButton.hide();
    attendanceButton.hide();
    exitButton.hide();
    QObject::connect(&manageButton, &QPushButton::clicked, [&]() {
        QDialog menu(&window);
        QVBoxLayout menuLayout(&menu);
        QLabel menuTitle(QStringLiteral("终端管理"));
        menuTitle.setStyleSheet("font-size:24px; font-weight:bold;");
        menuLayout.addWidget(&menuTitle);
        QPushButton samples(QStringLiteral("已录入样本"));
        QPushButton attendance(QStringLiteral("考勤记录"));
        QPushButton exit(QStringLiteral("退出程序"));
        QPushButton back(QStringLiteral("返回"));
        for (auto* button : {&samples, &attendance, &exit, &back}) {
            button->setMinimumHeight(48);
            menuLayout.addWidget(button);
        }
        QObject::connect(&samples, &QPushButton::clicked, [&]() { menu.done(10); });
        QObject::connect(&attendance, &QPushButton::clicked, [&]() { menu.done(11); });
        QObject::connect(&exit, &QPushButton::clicked, [&]() { menu.done(12); });
        QObject::connect(&back, &QPushButton::clicked, &menu, &QDialog::reject);
        const int choice = shell.execDialog(menu);
        if (choice == 10) samplesButton.click();
        if (choice == 11) attendanceButton.click();
        if (choice == 12) exitButton.click();
    });

    QObject::connect(&attendanceButton, &QPushButton::clicked, [&]() {
        // This lambda executes on the GUI thread, the database owner's thread;
        // capture and inference continue independently while the panel is open.
        QDialog dialog(&window);
        dialog.setWindowTitle(QStringLiteral("考勤记录"));
        dialog.setMinimumHeight(560);
        QVBoxLayout dialogLayout(&dialog);
        QLabel title;
        title.setWordWrap(true);
        dialogLayout.addWidget(&title);
        QListWidget list;
        list.setWordWrap(true);
        list.setStyleSheet("QListWidget { font-size:18px; }"
                           "QListWidget::item { padding:10px; }");
        dialogLayout.addWidget(&list, 1);
        QPushButton refreshButton(QStringLiteral("刷新"));
        QPushButton exportButton(QStringLiteral("导出全部 CSV"));
        QPushButton closeButton(QStringLiteral("关闭"));
        refreshButton.setMinimumHeight(48);
        exportButton.setMinimumHeight(48);
        closeButton.setMinimumHeight(48);
        dialogLayout.addWidget(&refreshButton);
        dialogLayout.addWidget(&exportButton);
        dialogLayout.addWidget(&closeButton);

        auto refresh = [&]() {
            QStringList rows;
            // 失败时清掉旧列表，避免误认为仍是最新结果。
            list.clear();
            if (!database.listDailyAttendance(rows)) {
                title.setText(QStringLiteral("读取失败，请查看终端日志后重试"));
                return;
            }
            list.addItems(rows);
            title.setText(rows.isEmpty() ? QStringLiteral("暂无考勤记录")
                : QStringLiteral("最近 %1 条考勤（最多显示 100 条）").arg(rows.size()));
        };
        QObject::connect(&refreshButton, &QPushButton::clicked, &dialog, refresh);
        QObject::connect(&closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
        QObject::connect(&exportButton, &QPushButton::clicked, &dialog, [&] { dialog.done(20); });
        refresh();
        // Close the list panel before showing the result. The portrait host owns
        // one overlay at a time, so nested modal Qt dialogs are intentionally avoided.
        if (shell.execDialog(dialog) == 20) {
            const QDir directory(QCoreApplication::applicationDirPath() + "/exports");
            if (!QDir().mkpath(directory.absolutePath())) {
                PortraitMessages::warning(&window, QStringLiteral("导出失败"),
                    QStringLiteral("无法创建 exports 目录，请检查磁盘和写入权限"));
                return;
            }
            const QString filename = QStringLiteral("attendance_%1_%2.csv")
                .arg(QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Shanghai"))
                    .toString("yyyyMMdd_HHmmss"))
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            const QString path = directory.filePath(filename);
            qint64 count = 0;
            QString error;
            if (!database.exportDailyAttendanceCsv(path, count, error)) {
                qWarning() << "CSV export failed:" << error;
                PortraitMessages::warning(&window, QStringLiteral("导出失败"), error);
                return;
            }
            qInfo() << "CSV exported:" << path << "records=" << count;
            PortraitMessages::information(&window, QStringLiteral("导出完成"),
                QStringLiteral("已导出 %1 条记录\n保存在项目 exports 文件夹\n包含测试标记，可复制到电脑查看。")
                    .arg(count));
        }
    });

    QObject::connect(
        &samplesButton,
    &QPushButton::clicked,
    [&]() {
        QStringList rows;

        if (!database.listEnrollmentSamples(rows)) {
            PortraitMessages::warning(
                &window,
                QStringLiteral("读取失败"),
                QStringLiteral("无法读取录入样本"));
            return;
        }

        QDialog dialog(&window);
        dialog.setWindowTitle(QStringLiteral("已录入样本"));
        dialog.setMinimumHeight(560);

        QVBoxLayout dialogLayout(&dialog);

        QLabel title(
            QStringLiteral("共 %1 条样本").arg(rows.size()));
        dialogLayout.addWidget(&title);

        QListWidget list;
        list.setWordWrap(true);
        list.setStyleSheet(
            "QListWidget { font-size:18px; }"
            "QListWidget::item { padding:12px; }");
        list.addItems(rows);
        dialogLayout.addWidget(&list, 1);

        if (rows.isEmpty()) {
            title.setText(QStringLiteral("尚无录入样本"));
        }

        QPushButton deleteButton(
            QStringLiteral("删除选中样本"));
        deleteButton.setMinimumHeight(48);
        deleteButton.setEnabled(false);
        dialogLayout.addWidget(&deleteButton);

        QPushButton closeButton(QStringLiteral("关闭"));
        closeButton.setMinimumHeight(48);
        dialogLayout.addWidget(&closeButton);

        QObject::connect(
            &list,
            &QListWidget::itemSelectionChanged,
            [&]() {
                deleteButton.setEnabled(
                    list.currentRow() >= 0);
            });

        QObject::connect(
            &deleteButton,
            &QPushButton::clicked,
            [&]() {
                const int row = list.currentRow();
                if (row < 0 || row >= rows.size()) {
                    return;
                }

                const QString selected = rows.at(row);
                const int space = selected.indexOf(' ');
                const qint64 id =
                    selected.mid(1, space - 1).toLongLong();

                if (PortraitMessages::question(
                        &dialog,
                        QStringLiteral("确认删除"),
                        QStringLiteral("确定删除选中的样本吗？"))
                    != QMessageBox::Yes) {
                    return;
                }

                if (!database.deleteEnrollmentSample(id)) {
                    PortraitMessages::warning(
                        &dialog,
                        QStringLiteral("删除失败"),
                        QStringLiteral("数据库删除失败"));
                    return;
                }

                reloadGallery();
                rows.removeAt(row);
                delete list.takeItem(row);
                title.setText(
                    QStringLiteral("共 %1 条样本")
                        .arg(list.count()));
                deleteButton.setEnabled(false);

                if (list.count() == 0) {
                    title.setText(QStringLiteral("尚无录入样本"));
                }
            });

        QObject::connect(
            &closeButton,
            &QPushButton::clicked,
            &dialog,
            &QDialog::accept);

        shell.execDialog(dialog);
    });

    /*
     * Enrollment hand-off: these three values must describe the same inference
     * result. The GUI copies them under enrollmentMutex before opening input, so
     * a person cannot type a name while a later frame changes the saved face.
     */
    bool enrollmentFaceAvailable = false;
    std::mutex enrollmentMutex;

    // 以下数据由 enrollmentMutex 一起保护。
// 原图、检测结果和时间戳必须来自同一次推理，避免样本错配。
QImage enrollmentFrame;
std::vector<FaceDetection> enrollmentDetections;
std::chrono::steady_clock::time_point enrollmentFrameTime {};

    QObject::connect(
        &exitButton,
        &QPushButton::clicked,
        &app,
        &QApplication::quit
    );

    QObject::connect(
        &enrollButton,
        &QPushButton::clicked,
        [&]() {
            // Freeze one fresh, single-face frame before opening the on-screen
            // keyboard. Capture continues, but the eventual write uses this copy.
            QImage sampleFrame;
            FaceDetection sampleFace;

            {
                std::lock_guard<std::mutex> lock(enrollmentMutex);

                if (!enrollmentFaceAvailable ||
                    enrollmentFrame.isNull() ||
                    enrollmentDetections.size() != 1 ||
                    std::chrono::steady_clock::now() -
                            enrollmentFrameTime >=
                        std::chrono::seconds(1)) {
                    status.setText(
                        QStringLiteral("请正对摄像头后重新录入")
                    );
                    return;
                }

                sampleFrame = enrollmentFrame.copy();
                sampleFace = enrollmentDetections.front();
            }

            QDialog dialog(&window);

dialog.setWindowTitle(QStringLiteral("录入人员"));
dialog.setModal(true);
dialog.setMinimumHeight(520);

QVBoxLayout dialogLayout(&dialog);
dialogLayout.setContentsMargins(12, 12, 12, 12);
dialogLayout.setSpacing(6);

QLabel enrollmentTitle(QStringLiteral("录入人员"));
enrollmentTitle.setStyleSheet("font-size:24px; font-weight:bold;");
dialogLayout.addWidget(&enrollmentTitle);

QLineEdit nameEdit;
nameEdit.setPlaceholderText(
    QStringLiteral("请输入英文名或拼音")
);
nameEdit.setMinimumHeight(48);
nameEdit.setReadOnly(true);
dialogLayout.addWidget(&nameEdit);
nameEdit.setMaxLength(40);
QLineEdit employeeEdit;
employeeEdit.setReadOnly(true);
employeeEdit.setMaxLength(20);
employeeEdit.setPlaceholderText(QStringLiteral("工号（数字，保留前导零）"));
employeeEdit.setMinimumHeight(42);
dialogLayout.addWidget(&employeeEdit);
QLineEdit* activeEdit = &nameEdit;
QHBoxLayout fieldLayout;
QPushButton editName(QStringLiteral("编辑姓名")), editEmployee(QStringLiteral("编辑工号"));
fieldLayout.addWidget(&editName); fieldLayout.addWidget(&editEmployee);
dialogLayout.addLayout(&fieldLayout);
auto selectField = [&](QLineEdit* field) {
    activeEdit = field;
    nameEdit.setStyleSheet(field == &nameEdit ? "border:2px solid #39c9aa;" : "");
    employeeEdit.setStyleSheet(field == &employeeEdit ? "border:2px solid #39c9aa;" : "");
};
QObject::connect(&editName, &QPushButton::clicked, [&] { selectField(&nameEdit); });
QObject::connect(&editEmployee, &QPushButton::clicked, [&] { selectField(&employeeEdit); });
selectField(&nameEdit);

QGridLayout keyLayout;
const QString keys =
    QStringLiteral("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");

for (int i = 0; i < keys.size(); ++i) {
    QPushButton* key =
        new QPushButton(QString(keys[i]), &dialog);

    key->setMinimumSize(42, 42);

    QObject::connect(
        key,
        &QPushButton::clicked,
        [&, key]() {
            if (activeEdit == &employeeEdit && !key->text().at(0).isDigit()) return;
            activeEdit->setText((activeEdit->text() + key->text().toLower()).left(activeEdit->maxLength()));
        }
    );

    keyLayout.addWidget(
        key,
        i / 7,
        i % 7
    );
}

dialogLayout.addLayout(&keyLayout);

QHBoxLayout actionLayout;

QPushButton backspaceButton(
    QStringLiteral("退格")
);
QPushButton clearButton(
    QStringLiteral("清空")
);
QPushButton okButton(
    QStringLiteral("确定")
);
QPushButton cancelButton(
    QStringLiteral("取消")
);

for (QPushButton* button : {
         &backspaceButton,
         &clearButton,
         &okButton,
         &cancelButton
     }) {
    button->setMinimumHeight(48);
    actionLayout.addWidget(button);
}

dialogLayout.addLayout(&actionLayout);

QObject::connect(
    &backspaceButton,
    &QPushButton::clicked,
    [&]() {
        QString text = activeEdit->text();
        text.chop(1);
        activeEdit->setText(text);
    }
);

QObject::connect(
    &clearButton,
    &QPushButton::clicked,
    [&]() {
        activeEdit->clear();
    }
);

QObject::connect(
    &okButton,
    &QPushButton::clicked,
    [&]() {
        if (!employeeEdit.text().isEmpty()) {
            dialog.accept();
        }
    }
);

QObject::connect(
    &cancelButton,
    &QPushButton::clicked,
    &dialog,
    &QDialog::reject
);

if (shell.execDialog(dialog) == QDialog::Accepted) {
    QString name = nameEdit.text().trimmed();
    const QString employee = employeeEdit.text();
    qint64 existingPerson = -1;
    QString existingName;
    if (!database.findEmployee(employee, existingPerson, existingName)) {
        PortraitMessages::warning(&window, QStringLiteral("录入失败"), QStringLiteral("人员查询失败，请重试"));
        return;
    }
    if (existingPerson > 0) {
        if (PortraitMessages::question(&window, QStringLiteral("追加样本"),
            QStringLiteral("工号 %1 已属于 %2\n是否为此人追加本次人脸样本？\n不会更改原姓名。")
                .arg(employee).arg(existingName)) != QMessageBox::Yes) return;
        name = existingName;
    } else if (name.isEmpty()) {
        PortraitMessages::warning(&window, QStringLiteral("录入失败"), QStringLiteral("新人员必须填写姓名"));
        return;
    }

    // Store an unannotated crop. Adding margin around the detector box preserves
    // contextual pixels for affine rotation and reduces black corners afterwards.
const QRect faceRect = sampleFace.box.toAlignedRect();
const int marginX = static_cast<int>(faceRect.width() * 0.30);
const int marginY = static_cast<int>(faceRect.height() * 0.30);

const QRect expandedRect(
    faceRect.left() - marginX,
    faceRect.top() - marginY,
    faceRect.width() + marginX * 2,
    faceRect.height() + marginY * 2
);

const QRect crop =
    expandedRect.intersected(sampleFrame.rect());

    constexpr int MIN_FACE_WIDTH = 80;
    constexpr int MIN_FACE_HEIGHT = 80;

// 最小尺寸检查使用原始人脸框，避免扩展边距让小人脸通过。
if (crop.isEmpty() ||
    faceRect.width() < MIN_FACE_WIDTH ||
    faceRect.height() < MIN_FACE_HEIGHT) {
        PortraitMessages::warning(
            &window,
            QStringLiteral("录入失败"),
            QStringLiteral("人脸区域太小或无效，请靠近摄像头后重试"));
        return;
    }

    // 关键点必须在原图范围内。
    for (const QPointF& point : sampleFace.landmarks) {
        if (!sampleFrame.rect().contains(point.toPoint())) {
            PortraitMessages::warning(
                &window,
                QStringLiteral("录入失败"),
                QStringLiteral("人脸关键点越界，请重新采集"));
            return;
        }
    }

    // Reuse the exact alignment path used for gallery construction. Rejecting a
    // bad sample before SQLite insertion prevents a reference that can never be used.
    auto localPoints = sampleFace.landmarks;
    for (auto& point : localPoints) point -= QPointF(crop.topLeft());
    QImage checkAligned;
    double checkRms = 0;
    QString saveError;
    if (!alignFace112(sampleFrame.copy(crop), localPoints, checkAligned, checkRms, saveError)) {
        PortraitMessages::warning(&window, QStringLiteral("请重新采集"), saveError);
        return;
    }
    QByteArray png;
    QBuffer buffer(&png);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !sampleFrame.copy(crop).save(&buffer, "PNG")) {
        PortraitMessages::warning(
            &window,
            QStringLiteral("录入失败"),
            QStringLiteral("人脸图片编码失败"));
        return;
    }

    buffer.close();

    QJsonArray points;
    for (const QPointF& point : sampleFace.landmarks) {
        QJsonArray xy;
        xy.append(point.x());
        xy.append(point.y());
        points.append(xy);
    }

    const QString landmarks = QString::fromUtf8(
        QJsonDocument(points).toJson(QJsonDocument::Compact));

    const qint64 id = database.savePersonEnrollment(
        name, employee, existingPerson,
        png,
        landmarks,
        sampleFrame.size(),
        crop, saveError);

    if (id < 0) {
        PortraitMessages::warning(
            &window,
            QStringLiteral("录入失败"),
            saveError);
    } else {
        const bool refreshed = reloadGallery();
        PortraitMessages::information(
            &window,
            QStringLiteral("样本已保存"),
            QStringLiteral("姓名：%1\n样本编号：%2\n工号：%3\n%4")
            .arg(name)
            .arg(id).arg(employee).arg(refreshed ? QStringLiteral("正在自动更新识别库")
                : QStringLiteral("样本已保存，读取识别库失败，将自动重试")));
    }  // 结束保存结果的 if/else
}      // 结束 dialog.exec() 的判断
});    // 结束录入按钮的 lambda 和 QObject::connect

    /*
     * Thread topology:
     *   capture thread   -> latestFrame (one-slot mailbox) -> inference thread
     *   inference thread -> resultFrame / enrollment snapshot -> GUI timer
     *
     * A single frame slot intentionally drops stale frames under load. This bounds
     * latency and prevents an accumulating V4L2/CPU queue from freezing the UI.
     */
    std::atomic<bool> running{true};

    std::atomic<bool> captureFailed{false};
    std::atomic<unsigned> captured{0};

    std::mutex frameMutex;
    std::mutex resultMutex;
    std::condition_variable frameReady;

    QImage latestFrame;
    QImage resultFrame;

    quint64 frameId = 0;
    quint64 resultId = 0;
    quint64 resultGalleryVersion = 0;

    Clock::time_point frameTime;
    Clock::time_point resultTime;

    size_t resultFaces = 0;
    double resultMs = 0.0;
    // 与 resultFrame 一起由 resultMutex 保护，界面只读取快照。
    QString resultRecognition;
    Clock::time_point resultRecognitionTime{};
    qint64 resultPersonId = -1;
    double resultSimilarity = 0;

    constexpr qint64 SCREEN_SLEEP_TIMEOUT_MS = 30'000;

    QElapsedTimer lastFaceTimer;
    lastFaceTimer.start();

    bool screenSleeping = false;

    // Capture never waits for inference. It replaces the one-slot mailbox with
    // the newest owned QImage and signals the consumer once per captured frame.
    std::thread captureThread([&] {
        while (running) {
            QImage frame;
            int r = camera.grabFrame(frame);
            if (r < 0) { captureFailed = true; running = false; frameReady.notify_all(); break; }
            if (!r) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                latestFrame = std::move(frame); frameTime = Clock::now(); ++frameId;
            }
            ++captured;
            frameReady.notify_one();
        }
    });
    // All RKNN contexts live in this thread. Gallery rebuilds and live extraction
    // therefore cannot concurrently call a model context from the GUI thread.
    std::thread inferenceThread([&] {
    // 模型由推理线程创建、使用和销毁，避免跨线程并发访问。
FaceRecognizer recognizer(
    QCoreApplication::applicationDirPath() +
    "/models/w600k_mbf_rk3588.rknn");

if (!recognizer.isReady()) {
    qWarning() << "Recognition model unavailable; detection remains active";
}

// Reference and live RKNN embeddings are both generated here, so no model is
// shared across threads. A new gallery is committed only after it is complete.
std::vector<ReferenceFeature> references;
std::set<qint64> referencePeople;
quint64 builtVersion = 0;

// Recognition is sampled at most once per second. Detection and preview still
// run for every available frame, while embedding cost stays bounded for this
// development prototype.
auto lastFeatureTest = Clock::now() - std::chrono::seconds(1);
RecognitionState recognition;
QString recognitionName;

        quint64 lastId = 0;
        unsigned count = 0;
        double total = 0, peak = 0;
        auto statsStart = Clock::now();
        double detectTotal = 0, alignTotal = 0, featureTotal = 0;
        double detectPeak = 0, alignPeak = 0, featurePeak = 0;
        unsigned alignCount = 0, featureCount = 0;
        while (running) {
            if (builtVersion != galleryVersion.load()) {
                std::vector<EnrollmentSample> pending;
                quint64 version;
                {
                    std::lock_guard<std::mutex> lock(galleryMutex);
                    version = snapshotVersion;
                    pending = referenceSamples;
                }
                if (version == galleryVersion.load()) {
                    recognition.reset();
                    recognitionName.clear();
                    references.clear();
                    referencePeople.clear();
                    if (recognizer.isReady()) for (const auto& sample : pending) {
                        ReferenceFeature reference;
                        QString error;
                        if (buildReference(sample, recognizer, reference, error)) {
                            referencePeople.insert(reference.personId);
                            references.push_back(std::move(reference));
                        } else qWarning() << "Reference rejected:" << sample.id << error;
                    }
                    builtVersion = version;
                    // GUI 递增版本前先置 invalidated；此锁保证不能覆盖新请求。
                    std::lock_guard<std::mutex> lock(galleryMutex);
                    if (builtVersion == galleryVersion.load()) {
                        galleryPeople = static_cast<int>(referencePeople.size());
                        referencesInvalidated = false;
                        qInfo() << "Gallery ready: samples=" << references.size()
                                << "people=" << referencePeople.size() << "version=" << builtVersion;
                    }
                }
            }
            QImage frame; quint64 id; Clock::time_point timestamp;
            {
                std::unique_lock<std::mutex> lock(frameMutex);
                frameReady.wait(lock, [&] { return !running || frameId != lastId; });
                if (!running) break;
                frame = latestFrame; id = frameId; timestamp = frameTime; lastId = id;
            }
            auto begin = Clock::now();
            const auto detectBegin = Clock::now();
            const auto faces = detector.detect(frame);
            const double detectMs = std::chrono::duration<double, std::milli>(Clock::now() - detectBegin).count();
            detectTotal += detectMs; detectPeak = std::max(detectPeak, detectMs);

        // A recognition attempt is allowed only for one recent, sufficiently
        // large face and a stable gallery. Any failed prerequisite resets the
        // confirmation state instead of showing an old person's name.
        const auto featureNow = Clock::now();
recognition.expire(featureNow);
const bool eligible = recognizer.isReady() && referencePeople.size() >= 2 &&
    !referencesInvalidated.load() && !captureFailed.load() &&
    faces.size() == 1 && faces.front().box.width() >= 80 &&
    faces.front().box.height() >= 80 &&
    featureNow - timestamp < std::chrono::seconds(1);
if (!eligible) recognition.reset();
if (eligible && featureNow - lastFeatureTest >= std::chrono::seconds(1)) {
    lastFeatureTest = featureNow;
    bool acceptedComparison = false;

    // frame 尚未绘制标记；检测关键点也使用整帧坐标。
    const FaceDetection& face = faces.front();
    if (face.box.width() >= 80 && face.box.height() >= 80) {
        QImage aligned;
        double rms = 0.0;
        QString error;
        FaceRecognizer::Feature feature{};

        const auto alignBegin = Clock::now();
        const bool alignOk = alignFace112(frame, face.landmarks, aligned, rms, error);
        const double alignMs = std::chrono::duration<double, std::milli>(Clock::now() - alignBegin).count();
        alignTotal += alignMs; alignPeak = std::max(alignPeak, alignMs); ++alignCount;
        if (!alignOk) {
            qWarning() << "Live alignment rejected:" << error;
        } else {
            const auto featureBegin = Clock::now();
            const bool featureOk = recognizer.extract(aligned, feature, error);
            const double featureMs = std::chrono::duration<double, std::milli>(Clock::now() - featureBegin).count();
            featureTotal += featureMs; featurePeak = std::max(featurePeak, featureMs); ++featureCount;
            if (!featureOk) {
            qWarning() << "Live feature extraction failed:" << error;
            } else {
            // Multiple samples for one person collapse to that person's highest
            // score; second place is necessarily another person. This prevents a
            // heavily enrolled person from occupying both rank slots.
            if (!referencesInvalidated.load()) {
                std::vector<PersonScore> sampleScores;
                bool valid = true;
                for (size_t i = 0; i < references.size(); ++i) {
                    double score = 0;
                    if (!FaceRecognizer::cosineSimilarity(
                            feature, references[i].feature, score)) {
                        valid = false;
                        break;
                    }
                    sampleScores.push_back({references[i].personId, i, score});
                }
                const auto ranked = rankPeople(sampleScores);
                // 对齐和推理耗时后再次检查新鲜度，避免打印过期结果。
                if (valid && ranked.size() >= 2 && !captureFailed.load() &&
                    !referencesInvalidated.load() &&
                    Clock::now() - timestamp < std::chrono::seconds(1)) {
                    const auto& best = references[ranked[0].referenceIndex];
                    const auto& second = references[ranked[1].referenceIndex];
                    acceptedComparison = recognition.observe(
                        best.personId, ranked[0].score, ranked[1].score,
                        Clock::now());
                    if (acceptedComparison)
                        recognitionName = best.name + QStringLiteral("（%1）").arg(best.employeeNo);
                    qInfo() << "Candidate only: name=" << best.name
                            << "person_id=" << best.personId
                            << "employee_no=" << best.employeeNo
                            << "sample_id=" << best.sampleId
                            << "score=" << ranked[0].score
                            << "second=" << second.name
                            << "second_person_id=" << second.personId
                            << "second_score=" << ranked[1].score
                            << "gap=" << ranked[0].score - ranked[1].score
                            << "RMS=" << rms
                            << "hits=" << recognition.hits
                            << "test_confirmed=" << recognition.confirmed();
                }
            }
            }
        }
    }
    // 本次已尝试但未通过（含对齐或推理失败），不能保留上次确认。
    if (!acceptedComparison) recognition.reset();
}

// Publish the unpainted frame and its detections as a single enrollment snapshot.
// Qt's implicit sharing is safe here because later annotation occurs on a copy.
{
    std::lock_guard<std::mutex> lock(enrollmentMutex);

    // frame 尚未绘制绿色框和红点；QImage 共享像素数据，
    // 后续复制或绘制到其他图像时会按需分离。
    enrollmentFrame = frame;
    enrollmentDetections = faces;
    enrollmentFrameTime = timestamp;
}
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            QImage annotated = frame.convertToFormat(QImage::Format_RGB32);

{
    std::lock_guard<std::mutex> lock(enrollmentMutex);

    // 只允许录入最近一秒内的单人脸样本。
    // 摄像头失败或结果过期时，禁止使用残留画面录入。
    enrollmentFaceAvailable =
        !captureFailed.load() &&
        !enrollmentFrame.isNull() &&
        enrollmentDetections.size() == 1 &&
        std::chrono::steady_clock::now() - enrollmentFrameTime <
            std::chrono::seconds(1);

}


{
                QPainter painter(&annotated);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setBrush(Qt::NoBrush);
                for (const auto& face : faces) {
                    painter.setPen(QPen(Qt::green, 3)); painter.drawRect(face.box);
                    painter.setPen(QPen(Qt::red, 3));
                    for (const auto& point : face.landmarks) painter.drawEllipse(point, 3.0, 3.0);
                }
            }
            // The UI never touches detector/recognizer objects. It receives a
            // complete annotated frame and concise recognition/check-in metadata.
            {
                std::lock_guard<std::mutex> lock(resultMutex);
                resultFrame = std::move(annotated); resultId = id; resultTime = timestamp;
                resultFaces = faces.size(); resultMs = ms;
                resultGalleryVersion = builtVersion;
                recognition.expire(Clock::now());
                if (referencesInvalidated.load() || captureFailed.load())
                    recognition.reset();
                resultRecognition.clear();
                resultPersonId = -1;
                resultSimilarity = 0;
                resultRecognitionTime = recognition.lastAccepted;
                if (recognition.confirmed()) {
                    resultPersonId = recognition.id;
                    resultSimilarity = recognition.score;
                    resultRecognition = QStringLiteral("测试匹配：%1｜相似度 %2")
                        .arg(recognitionName).arg(recognition.score, 0, 'f', 3);
                } else if (recognition.hits > 0) {
                    // 累计阶段不显示姓名，避免把候选误认为确认身份。
                    resultRecognition = QStringLiteral("身份确认中 %1/%2")
                        .arg(recognition.hits).arg(RecognitionState::RequiredHits);
                }
            }
            ++count; total += ms; peak = std::max(peak, ms);
            double seconds = std::chrono::duration<double>(Clock::now() - statsStart).count();
            if (seconds >= 1.0) {
                qInfo() << "inference FPS:" << count / seconds << "avg ms:" << total / count
                        << "max ms:" << peak << "faces:" << faces.size();
                qInfo() << "stage timing ms: detect_avg=" << detectTotal / count
                        << "detect_max=" << detectPeak
                        << "align_n=" << alignCount
                        << "align_avg=" << (alignCount ? alignTotal / alignCount : 0)
                        << "align_max=" << alignPeak
                        << "feature_n=" << featureCount
                        << "feature_avg=" << (featureCount ? featureTotal / featureCount : 0)
                        << "feature_max=" << featurePeak;
                statsStart = Clock::now(); count = 0; total = 0; peak = 0;
                detectTotal = alignTotal = featureTotal = 0;
                detectPeak = alignPeak = featurePeak = 0;
                alignCount = featureCount = 0;
            }
        }
    });

    /*
     * GUI-side policy timer (roughly 33 Hz): paints the latest result, gates the
     * enrollment button, writes daily attendance after confirmed recognition, and
     * controls the 30-second display sleep/wake behavior. SQLite remains here.
     */
    QTimer displayTimer;
    // 主线程维护一次在场期间的提示；离开或确认失效后清空。
    // 重复打卡最终由数据库唯一约束保证，不依赖这个内存缓存。
    qint64 checkInPerson = -1;
    QDate checkInDay;
    QString checkInText;
    bool checkInFinished = false;
    auto lastCheckInAttempt = Clock::now() - std::chrono::seconds(5);
    auto lastClockCheck = Clock::now() - std::chrono::seconds(1);
    bool clockSynchronized = false;
    const QTimeZone attendanceZone("Asia/Shanghai");
    auto fpsStart = Clock::now(); unsigned previousCount = 0;
    QObject::connect(&displayTimer, &QTimer::timeout, [&] {
        // 在界面线程更新按钮；锁内只读取共享状态，不操作控件。
bool canEnroll = false;
{
    std::lock_guard<std::mutex> lock(enrollmentMutex);
    canEnroll =
        enrollmentFaceAvailable &&
        !captureFailed.load() &&
        !enrollmentFrame.isNull() &&
        enrollmentDetections.size() == 1 &&
        Clock::now() - enrollmentFrameTime <
            std::chrono::seconds(1);
}
enrollButton.setEnabled(canEnroll);

        QImage frame; quint64 id; size_t faces; double ms; Clock::time_point timestamp;
        QString recognitionText;
        Clock::time_point recognitionTime;
        qint64 confirmedPerson = -1;
        double confirmedScore = 0;
        quint64 shownGalleryVersion = 0;
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            frame = resultFrame; id = resultId; timestamp = resultTime; faces = resultFaces; ms = resultMs;
            recognitionText = resultRecognition;
            recognitionTime = resultRecognitionTime;
            confirmedPerson = resultPersonId;
            confirmedScore = resultSimilarity;
            shownGalleryVersion = resultGalleryVersion;
        }
        auto now = Clock::now();
        // Read only the kernel synchronization flag. Recognition can continue
        // while time is unhealthy, but a wrong clock must not create attendance.
        if (now - lastClockCheck >= std::chrono::seconds(1)) {
            struct timex clockInfo {};
            const int state = adjtimex(&clockInfo);
            clockSynchronized = state >= 0 && state != TIME_ERROR &&
                                !(clockInfo.status & STA_UNSYNC);
            lastClockCheck = now;
        }
        const QDateTime attendanceTime = QDateTime::currentDateTimeUtc().toTimeZone(attendanceZone);
        clockLabel.setText(attendanceTime.toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss")));
        const bool freshConfirmation = confirmedPerson > 0 && faces == 1 && id &&
            shownGalleryVersion == galleryVersion.load() &&
            !frame.isNull() && !captureFailed.load() && !referencesInvalidated.load() &&
            now - timestamp < std::chrono::seconds(1) &&
            now - recognitionTime < RecognitionState::Timeout;
        if (!freshConfirmation || confirmedPerson != checkInPerson ||
            attendanceTime.date() != checkInDay) {
            checkInPerson = freshConfirmation ? confirmedPerson : -1;
            checkInDay = attendanceTime.date();
            checkInText.clear();
            checkInFinished = false;
            lastCheckInAttempt = now - std::chrono::seconds(5);
        }
        if (freshConfirmation && !checkInFinished) {
            if (!clockSynchronized || !attendanceZone.isValid()) {
                checkInText = QStringLiteral("时间未同步，暂停打卡");
            } else if (now - lastCheckInAttempt >= std::chrono::seconds(5)) {
                lastCheckInAttempt = now;
                QString error;
                const auto outcome = database.recordDailyAttendance(
                    confirmedPerson, confirmedScore, attendanceTime, error);
                if (outcome == CheckInResult::Failed) {
                    checkInText = QStringLiteral("考勤写入失败，将重试");
                    qWarning() << "Check-in failed:" << error;
                } else {
                    checkInFinished = true;
                    checkInText = outcome == CheckInResult::Inserted
                        ? QStringLiteral("测试打卡成功") : QStringLiteral("今日已打卡（测试）");
                    qInfo() << "Check-in:" << checkInText << "person_id=" << confirmedPerson
                            << "date=" << checkInDay.toString(Qt::ISODate);
                }
            }
        }
        if (captureFailed) { preview.clear(); status.setText(QStringLiteral("摄像头采集失败，请退出后检查设备")); }
        else if (id && now - timestamp > std::chrono::seconds(1)) {
            preview.clear(); status.setText(QStringLiteral("画面更新超时，请检查摄像头或推理"));
        } else if (!frame.isNull()) {
            preview.setPixmap(QPixmap::fromImage(frame).scaled(preview.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

            // This is a throttled presence diagnostic, not an identity decision:
            // a detected face is not proven to be an unknown person. Cooldown
            // avoids an SQLite write on every preview frame.
static auto lastUnknownRecord =
    std::chrono::steady_clock::now()
    - std::chrono::seconds(10);

const auto nowForRecord =
    std::chrono::steady_clock::now();

if (faces > 0 &&
    nowForRecord - lastUnknownRecord >=
        std::chrono::seconds(10)) {
    if (!database.recordUnknownDetection(
            static_cast<int>(faces))) {
        qWarning() << "Failed to record unknown detection";
    }

    lastUnknownRecord = nowForRecord;
}

// 按钮仅使用回调开头的 canEnroll，新鲜度检查不能被 faces 覆盖。

           // 即使推理线程停止发布，界面也独立检查确认状态是否过期。
           if (faces == 0) {
               status.setText(QStringLiteral("未检测到人脸，请正对摄像头"));
           } else if (faces > 1) {
               status.setText(QStringLiteral("检测到多人，请单人面对摄像头"));
           } else if (referencesInvalidated.load()) {
               status.setText(QStringLiteral("正在更新识别库，请稍候"));
           } else if (galleryPeople.load() < 2) {
               status.setText(QStringLiteral("有效人员不足两人，暂不确认身份"));
           } else if (shownGalleryVersion == galleryVersion.load() && !recognitionText.isEmpty() &&
                      now - recognitionTime < RecognitionState::Timeout) {
               status.setText(recognitionText +
                   (freshConfirmation && !checkInText.isEmpty()
                        ? QStringLiteral("\n") + checkInText : QString()));
           } else {
               status.setText(QStringLiteral("检测到人脸｜身份未确认"));
           }
        }

        const bool freshFace = !captureFailed.load() && id && faces > 0 &&
            now - timestamp < std::chrono::seconds(1);
        if (freshFace || shell.dialogOpen()) {
    // 检测到人脸后立即唤醒，并重新开始无人计时。
    lastFaceTimer.restart();

    if (screenSleeping) {
        screenSleeping = false;
        shell.setSleeping(false);
    }
} else if (!screenSleeping &&
           lastFaceTimer.elapsed() >=
               SCREEN_SLEEP_TIMEOUT_MS) {
    // 超过设定时间没有检测到人脸，进入屏幕休眠状态。
    screenSleeping = true;
    preview.clear();
    shell.setSleeping(true);
}

        double seconds = std::chrono::duration<double>(now - fpsStart).count();
        if (seconds >= 1.0) {
            unsigned current = captured.load();
            qInfo() << "capture FPS:" << (current - previousCount) / seconds;
            previousCount = current; fpsStart = now;
        }
    });
    // Signal both workers before joining; notify_all releases inference if it is
    // waiting for a frame. Calling stop twice is harmless because joinable() gates
    // each join (aboutToQuit and the normal return path may both invoke it).
    auto stop = [&] {
        running = false; frameReady.notify_all();
        if (captureThread.joinable()) captureThread.join();
        if (inferenceThread.joinable()) inferenceThread.join();
    };
    QObject::connect(&app, &QApplication::aboutToQuit, stop);
    shell.showFullScreen(); displayTimer.start(30);
    int result = app.exec(); stop(); return result;
}
