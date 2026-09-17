# Qt5 application project for the Orange Pi 5/RK3588 deployment target.
# The vendor RKNN header/runtime and the .rknn models are deliberately external:
# see docs/MODELS.md before building a fresh clone.
QT += widgets gui core

CONFIG += c++17 thread
TEMPLATE = app
TARGET = face_attendance

# Production application units. Stand-alone diagnostics live under tests/ and
# are compiled explicitly, not linked into the attendance executable.
SOURCES += \
    attendance_main.cpp \
    face_detector.cpp \
    database.cpp \
    face_recognizer.cpp \
    face_alignment.cpp

HEADERS += \
    face_detector.h \
    database.h \
    face_recognizer.h \
    face_alignment.h \
    rknn_box_priors.h

# `include/` is expected to contain the RKNN Runtime SDK header supplied by the
# board image or vendor package; it is ignored from public source control.
INCLUDEPATH += \
    $$PWD/include \
    /usr/include/opencv4

# `lib/librknnrt.so` is a vendor runtime copied during deployment and excluded
# from the repository. OpenCV is only used for face affine alignment.
LIBS += \
    -L$$PWD/lib \
    -lrknnrt \
    -lsqlite3 \
    -lopencv_calib3d \
    -lopencv_imgproc \
    -lopencv_core

# Let the deployed executable resolve the colocated vendor runtime without a
# global ldconfig installation.
QMAKE_RPATHDIR += $$PWD/lib
