#!/bin/bash
# Production launcher for the framebuffer appliance. It only selects Qt's
# linuxfb backend and logical screen direction; it does not create data or
# alter camera/NPU frequency settings.
# 原生 framebuffer 为 720x480，应用内部旋转为竖屏。
# 若物理摆放方向相反，启动时设置 ATTENDANCE_ROTATION=270。
# 不设置额外触摸旋转，QGraphicsView 会将触摸/鼠标映射到竖屏坐标。
export QT_QPA_PLATFORM=linuxfb
export ATTENDANCE_ROTATION="${ATTENDANCE_ROTATION:-90}"
exec "$(dirname "$0")/face_attendance"
