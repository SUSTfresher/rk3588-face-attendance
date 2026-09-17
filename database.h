#pragma once

/*
 * SQLite persistence boundary for the face-attendance application.
 *
 * Ownership: one AttendanceDatabase instance is owned by the Qt GUI thread.
 * The capture/inference threads never use this SQLite connection; they consume
 * immutable gallery snapshots published by attendance_main.cpp. This avoids
 * accidental cross-thread SQLite access while recognition continues running.
 *
 * Schema policy: initialize() creates missing tables and performs additive,
 * idempotent migrations for legacy databases. Enrollment writes create/bind a
 * person and sample in one transaction so no orphan identity is persisted.
 */

#include <QStringList>
#include <QByteArray>
#include <QRect>
#include <QSize>
#include <QString>
#include <sqlite3.h>
#include <vector>
#include <QDateTime>

// Distinguishes a successful first check-in, expected daily de-duplication,
// and a real database failure. Callers must not treat Failed as "already here".
enum class CheckInResult { Inserted, AlreadyRecorded, Failed };

// Original enrollment evidence. Landmarks remain in full-camera coordinates;
// callers must subtract crop.topLeft() before aligning facePng.
struct EnrollmentSample {
    qint64 id = -1;
    QString name;
    QByteArray facePng;
    QString landmarksJson;
    QSize sourceSize;
    QRect crop;
    // Populated only by joined gallery queries. A stored sample label is kept
    // separately from the authoritative person name for legacy compatibility.
    qint64 personId = -1;
    QString personName;
    QString employeeNo;
};

class AttendanceDatabase {
public:
    ~AttendanceDatabase();

    // Lookup by the business identity key. A successful query with id == -1
    // means "not found"; false means the query itself failed. employee is text
    // by design, so values such as "001" retain leading zeroes.
    bool findEmployee(const QString& employee, qint64& id, QString& name);
    // Atomically creates a person or appends to the explicitly confirmed person.
    // expectedPerson is revalidated inside BEGIN IMMEDIATE to prevent a stale UI
    // confirmation from binding a sample to a changed employee number. Returns
    // the new sample id, or -1 with a human-readable error.
    qint64 savePersonEnrollment(const QString& name, const QString& employee,
        qint64 expectedPerson, const QByteArray& png, const QString& landmarks,
        const QSize& size, const QRect& crop, QString& error);

    // Reads one raw sample for diagnostics/tools. Failure or a missing id clears
    // sample and returns false.
    bool loadEnrollmentSample(qint64 id, EnrollmentSample& sample);

    // Produces the recognition gallery: only samples bound to a non-empty person
    // name and employee number. Failure clears samples rather than returning a
    // stale gallery to the caller.
    bool loadBoundEnrollmentSamples(std::vector<EnrollmentSample>& samples);

    // Legacy name query retained for old tools; new enrollment uses findEmployee
    // because name is not the identity key.
    bool nameExists(const QString& name);

    // Deletes only the selected sample. It deliberately does not delete its
    // person or historical attendance records.
    bool deleteEnrollmentSample(qint64 id);

    // Returns metadata-only rows for the management dialog. Face PNG blobs are
    // not loaded while browsing, and duplicate names are not merged.
    bool listEnrollmentSamples(QStringList& rows);

    // Management UI view: most recent 100 daily records in reverse date/id order.
    bool listDailyAttendance(QStringList& rows);

    // Exports every daily record using UTF-8 BOM and CRLF. QSaveFile commits only
    // a complete file; count is valid only when this method returns true.
    bool exportDailyAttendanceCsv(const QString& path, qint64& count, QString& error);

    // Low-level sample insert used by the enclosing enrollment transaction.
    // facePng is the expanded crop, while landmarks are full-frame coordinates.
    qint64 saveEnrollmentSample(
    const QString& name,
    const QByteArray& facePng,
    const QString& landmarksJson,
    const QSize& sourceSize,
    const QRect& crop);

    // open() enables foreign keys for this connection; initialize() is safe to
    // call repeatedly and preserves existing operational data.
    bool open(const QString& path);
    bool initialize();
    // Legacy APIs retained for the original exercise schema. New UI code uses
    // savePersonEnrollment() and recordDailyAttendance().
    bool addPerson(const QString& name, const QString& feature);
    bool recordAttendance(int personId);
    // Uses an Asia/Shanghai calendar day as the unique key. The stored ISO time
    // contains its UTC offset. is_test remains 1 in this development prototype.
    CheckInResult recordDailyAttendance(qint64 personId, double similarity,
                                        const QDateTime& time, QString& error);
    bool recordUnknownDetection(int faceCount);
    QString findPersonByFeature(const QString& feature);

private:
    // SQLite handle is non-owning outside this class and closed by the destructor.
    sqlite3* db_ = nullptr;
};
