#pragma once

/*
 * Five-landmark similarity alignment for the recognition model.
 *
 * This is intentionally a free function: it owns no RKNN or UI state and is
 * shared by live frames and enrolled PNG samples. The source image and points
 * must always use the same coordinate system.
 */

#include <QImage>
#include <QPointF>
#include <QString>
#include <array>

// Live calls use the full camera frame and original points. Stored crops must
// subtract crop_x/crop_y first. Success returns RGB888 112x112; failure clears
// aligned and explains why. rms is landmark-fit error in pixels, not a match
// probability or identity confidence.
bool alignFace112(
    const QImage& source,
    const std::array<QPointF, 5>& landmarks,
    QImage& aligned,
    double& rms,
    QString& error);
