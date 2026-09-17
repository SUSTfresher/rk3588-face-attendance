/*
 * PortraitShell
 *
 * linuxfb exposes this board's physical framebuffer as landscape (720x480),
 * while this appliance has a portrait display. This QGraphicsView hosts one
 * fixed 480x720 Qt page, rotates both rendering and input coordinates, and
 * provides a safe modal-panel replacement for QDialog/QMessageBox.
 *
 * Standard QDialog::exec() cannot reliably centre or receive input after the
 * graphics transform. execDialog() temporarily reparents its layout into one
 * overlay, runs a local event loop for the original accept/reject signals, then
 * restores widget ownership before destruction. It supports nested confirmation
 * panels without disabling the root graphics proxy.
 */
#pragma once

#include <QDialog>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMessageBox>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTransform>
#include <QDebug>
#include <QPainter>
#include <QAbstractButton>
#include <QEventLoop>
#include <QCoreApplication>
#include <QVBoxLayout>
#include <QLayout>
#include <QTimer>
#include <QPointer>

// linuxfb 仍输出原生 720x480；应用内部使用 480x720 竖屏坐标。
// QGraphicsView 同时转换绘制和输入坐标，禁止另给触摸设备叠加旋转。
// 摄像头图像与识别坐标不旋转，只旋转最终界面。
class PortraitShell : public QGraphicsView {
public:
    // The page is owned by main()'s stack. The scene owns only the proxy wrapper.
    explicit PortraitShell(QWidget& page) : page_(page) {
        instance = this;
        setScene(&scene_);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setBackgroundBrush(QColor("#0b1220"));
        setRenderHint(QPainter::SmoothPixmapTransform);
        setSceneRect(0, 0, 480, 720);
        page_.setFixedSize(480, 720);
        root_ = scene_.addWidget(&page_);
        // 部署时如竖屏方向反了，脚本改为 270，不改识别算法。
        const int requested = qEnvironmentVariableIntValue("ATTENDANCE_ROTATION");
        angle_ = requested == 270 ? 270 : 90;
        qInfo() << "Portrait UI: logical=480x720 rotation=" << angle_;
    }

    ~PortraitShell() override {
        // 主页面属于 main 的栈，不能让 scene 删除它。
        root_->setWidget(nullptr);
        delete root_;
        instance = nullptr;
    }

    // Hide the logical page without stopping camera/inference; the next face or
    // management panel makes it visible again.
    void setSleeping(bool sleeping) {
        root_->setVisible(!sleeping);
        setBackgroundBrush(sleeping ? Qt::black : QColor("#0b1220"));
    }

    bool dialogOpen() const { return depth_ > 0; }

    // Present a dialog layout inside the rotated coordinate system. The caller
    // retains the dialog object and its signal handlers; only the layout moves.
    int execDialog(QDialog& dialog) {
        // 只显示普通 QWidget 覆盖层，全应用始终只有一个场景代理。
        // QDialog 仅作为 accept/reject/done 信号对象，绝不 show/exec。
        // 将它的布局临时移入面板，关闭后还回去，保留现有业务回调。
        if (!dialog.layout()) {
            qWarning() << "Portrait panel has no layout";
            return QDialog::Rejected;
        }
        dialog.hide();
        dialog.ensurePolished();
        QWidget overlay(&page_);
        overlay.setObjectName(QStringLiteral("portraitOverlay"));
        overlay.setGeometry(page_.rect());
        overlay.setStyleSheet("QWidget#portraitOverlay { background:#080f1c; }");
        // 覆盖层挡住背景鼠标事件；禁用背景按钮而不是禁用根代理。
        // 根代理被禁用会连同其自动代理/子控件一起失去输入。
        const auto backgroundButtons = page_.findChildren<QAbstractButton*>();
        QList<QPair<QPointer<QAbstractButton>, bool>> enabledStates;
        for (auto* button : backgroundButtons) {
            if (dialog.isAncestorOf(button)) continue;
            enabledStates.append(qMakePair(QPointer<QAbstractButton>(button), button->isEnabled()));
            button->setEnabled(false);
        }
        QWidget panel(&overlay);
        panel.setObjectName(QStringLiteral("portraitPanel"));
        panel.setStyleSheet(page_.styleSheet());
        panel.setLayout(dialog.layout());
        panel.setFixedWidth(440);
        panel.ensurePolished();
        panel.setFixedHeight(qBound(180,
            qMax(dialog.minimumHeight(), panel.sizeHint().height()), 680));
        panel.move((480 - panel.width()) / 2, (720 - panel.height()) / 2);
        ++depth_;

        QEventLoop loop;
        int result = QDialog::Rejected;
        bool finished = false;
        // 原来的 accept/reject/done(10...) 回调全部保留，包括 QMessageBox。
        QObject::connect(&dialog, &QDialog::finished, &loop, [&](int code) {
            result = code;
            finished = true;
            loop.quit();
        });
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                         &loop, &QEventLoop::quit);
        overlay.show();
        overlay.raise();
        panel.show();
        panel.layout()->activate();
        panel.move((480 - panel.width()) / 2, (720 - panel.height()) / 2);
        root_->setFocus();
        // 焦点进入面板，键盘不会留在底下的管理/录入按钮上。
        const auto panelButtons = panel.findChildren<QAbstractButton*>();
        if (!panelButtons.isEmpty()) panelButtons.front()->setFocus();
        qInfo() << "Portrait panel v3:" << panel.geometry()
                << "root_enabled=" << root_->isEnabled();
        if (!finished) loop.exec();

        // 还原布局和控件归属后再销毁覆盖层，栈上控件不会被重复删除。
        QObject::disconnect(&dialog, nullptr, &loop, nullptr);
        overlay.hide();
        dialog.setLayout(panel.layout());
        for (const auto& saved : enabledStates) {
            if (saved.first) saved.first->setEnabled(saved.second);
        }
        --depth_;
        root_->setFocus();
        page_.update();
        return result;
    }

    inline static PortraitShell* instance = nullptr;

protected:
    // Scale to fit without changing logical layout dimensions. QGraphicsView's
    // transform maps pointer events back to the unrotated 480x720 page.
    void resizeEvent(QResizeEvent* event) override {
        QGraphicsView::resizeEvent(event);
        QTransform rotation;
        rotation.rotate(angle_);
        const QRectF bounds = rotation.mapRect(sceneRect());
        const qreal scale = qMin(viewport()->width() / bounds.width(),
                                 viewport()->height() / bounds.height());
        rotation.scale(scale, scale);
        setTransform(rotation);
        centerOn(sceneRect().center());
    }

private:
    QWidget& page_;
    QGraphicsScene scene_;
    QGraphicsProxyWidget* root_ = nullptr;
    int angle_ = 90;
    int depth_ = 0;
};

// Preserve normal QMessageBox result codes while routing visual presentation
// through the portrait overlay. Use these helpers instead of QMessageBox::exec.
namespace PortraitMessages {
inline int show(QMessageBox::Icon icon, const QString& title, const QString& text,
                QMessageBox::StandardButtons buttons) {
    QMessageBox box(icon, title, text, buttons);
    if (buttons.testFlag(QMessageBox::Yes)) {
        box.button(QMessageBox::Yes)->setText(QStringLiteral("确认"));
        box.button(QMessageBox::No)->setText(QStringLiteral("取消"));
        box.setDefaultButton(QMessageBox::No);
    } else box.button(QMessageBox::Ok)->setText(QStringLiteral("知道了"));
    return PortraitShell::instance->execDialog(box);
}
inline int warning(QWidget*, const QString& title, const QString& text) {
    return show(QMessageBox::Warning, title, text, QMessageBox::Ok);
}
inline int information(QWidget*, const QString& title, const QString& text) {
    return show(QMessageBox::Information, title, text, QMessageBox::Ok);
}
inline int question(QWidget*, const QString& title, const QString& text) {
    return show(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No);
}
}
