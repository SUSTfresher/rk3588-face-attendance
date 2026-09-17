/*
 * Isolated portrait-overlay interaction test. It creates no camera, model, or
 * database and verifies that a pointer mapped through the rotated QGraphicsView
 * can operate and close a centred modal panel.
 */
#include "portrait_shell.h"
#include <QApplication>
#include <QPushButton>
#include <QMouseEvent>
#include <iostream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget page;
    PortraitShell shell(page);
    QVBoxLayout layout(&page);
    QPushButton background(QStringLiteral("背景按钮"));
    layout.addWidget(&background);
    shell.showFullScreen();
    bool passed = true;
    QTimer::singleShot(100, &app, [&]() {
        for (int attempt = 0; attempt < 3; ++attempt) {
            QDialog dialog(&page);
            QVBoxLayout content(&dialog);
            QPushButton back(QStringLiteral("返回"));
            content.addWidget(&back);
            QObject::connect(&back, &QPushButton::clicked, &dialog, &QDialog::accept);
            // 超时自动退出，测试失败也不会把终端卡住。
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &dialog, &QDialog::reject);
            timeout.start(2000);
            QTimer::singleShot(100, &dialog, [&]() {
                auto* panel = page.findChild<QWidget*>(QStringLiteral("portraitPanel"));
                passed = passed && panel && back.isEnabled();
                if (!panel || !back.isEnabled()) { dialog.reject(); return; }
                const QPoint center = panel->geometry().center();
                passed = passed && qAbs(center.x() - 239) <= 1 && qAbs(center.y() - 359) <= 1;
                // 真实走 viewport -> scene -> 根代理 -> 子控件的鼠标路径。
                const QPoint logical = back.mapTo(&page, back.rect().center());
                const QPoint pixel = shell.mapFromScene(logical);
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(pixel),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pixel),
                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(shell.viewport(), &press);
                QApplication::sendEvent(shell.viewport(), &release);
            });
            const int result = shell.execDialog(dialog);
            timeout.stop();
            passed = passed && result == QDialog::Accepted && background.isEnabled()
                            && !shell.dialogOpen();
        }
        std::cout << (passed ? "Portrait panel tests PASSED" : "Portrait panel tests FAILED") << std::endl;
        app.exit(passed ? 0 : 1);
    });
    return app.exec();
}
