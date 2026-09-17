/*
 * SQLite implementation for enrollment, gallery reads, daily test attendance,
 * and CSV export.
 *
 * This file is called by the GUI thread only. Every value read from a SQLite
 * statement is copied before sqlite3_finalize(), then optional gallery data is
 * handed to the inference thread as an immutable in-memory snapshot.
 */
#include "database.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QTimeZone>
#include <cmath>
#include <QSaveFile>

bool AttendanceDatabase::exportDailyAttendanceCsv(const QString& path, qint64& count, QString& error)
{
    // Export is a read-only operation. QSaveFile gives all-or-nothing visibility
    // to users opening the CSV while avoiding a partially written attendance file.
    count = 0;
    error.clear();
    if (!db_) { error = QStringLiteral("数据库未打开"); return false; }
    // 一条 SELECT 保证本次导出使用一致的快照；不受列表100条限制。
    const char* sql =
        "SELECT d.id,d.person_id,p.name,p.employee_no,d.attendance_date,"
        "d.check_time,d.similarity,d.is_test FROM daily_attendance d "
        "LEFT JOIN persons p ON p.id=d.person_id "
        "ORDER BY d.attendance_date,d.id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = QString::fromUtf8(sqlite3_errmsg(db_)); return false;
    }
    QSaveFile file(path);
    // 禁止直接写入回退：失败时不留下看似完整的半个CSV文件。
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString(); sqlite3_finalize(stmt); return false;
    }
    auto write = [&](const QByteArray& bytes) {
        if (file.write(bytes) == bytes.size()) return true;
        error = file.errorString(); return false;
    };
    auto field = [](QString value, bool protectText) {
        // Quoting alone does not stop spreadsheet formula interpretation. Prefix
        // user-controlled text that starts with an operator before CSV escaping.
        const QString trimmed = value.trimmed();
        if (protectText && !trimmed.isEmpty() && QStringLiteral("=+-@").contains(trimmed.at(0)))
            value.prepend(QLatin1Char('\''));
        value.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        return QStringLiteral("\"") + value + QStringLiteral("\"");
    };
    auto text = [&](int column) {
        const auto* data = sqlite3_column_text(stmt, column);
        return data ? QString::fromUtf8(reinterpret_cast<const char*>(data),
            sqlite3_column_bytes(stmt, column)) : QString();
    };
    // BOM helps common Windows spreadsheet tools detect UTF-8 Chinese headers.
    bool ok = write(QByteArray::fromHex("efbbbf")) && write(QStringLiteral(
        "记录编号,人员编号,姓名,工号,考勤日期,首次打卡时间,相似度,是否测试(1是0否)\r\n").toUtf8());
    qint64 rows = 0;
    int rc = SQLITE_DONE;
    while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        QStringList fields;
        for (int col = 0; col < 8; ++col)
            fields.append(field(text(col), col >= 2 && col <= 5));
        ok = write((fields.join(QLatin1Char(',')) + QStringLiteral("\r\n")).toUtf8());
        if (ok) ++rows;
    }
    if (ok && rc != SQLITE_DONE) {
        error = QString::fromUtf8(sqlite3_errmsg(db_)); ok = false;
    }
    sqlite3_finalize(stmt);
    if (!ok) { file.cancelWriting(); return false; }
    if (!file.commit()) { error = file.errorString(); return false; }
    count = rows;
    return true;
}

bool AttendanceDatabase::listDailyAttendance(QStringList& rows)
{
    // This lightweight UI formatter intentionally omits similarity. The export
    // path above retains the raw value for analysis and auditing.
    rows.clear();
    if (!db_) return false;
    // LEFT JOIN 保留历史记录：即使人员关联异常，也不静默隐藏考勤。
    // 姓名、工号取人员表当前值；本轮只浏览，不修改记录。
    const char* sql =
        "SELECT d.id,d.person_id,p.name,p.employee_no,d.attendance_date,"
        "d.check_time,d.is_test FROM daily_attendance d "
        "LEFT JOIN persons p ON p.id=d.person_id "
        "ORDER BY d.attendance_date DESC,d.id DESC LIMIT 100";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "List daily attendance failed:" << sqlite3_errmsg(db_);
        return false;
    }
    auto text = [stmt](int column) {
        const auto* data = sqlite3_column_text(stmt, column);
        return data ? QString::fromUtf8(reinterpret_cast<const char*>(data),
                                       sqlite3_column_bytes(stmt, column)) : QString();
    };
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        QString name = text(2);
        if (name.isEmpty()) name = QStringLiteral("人员 #%1（资料缺失）")
            .arg(sqlite3_column_int64(stmt, 1));
        const QString employee = text(3).isEmpty() ? QStringLiteral("—") : text(3);
        // 保留完整时间字符串及 +08:00，不依赖显示设备的当前时区转换。
        rows.append(QStringLiteral("%1｜工号 %2｜%3\n考勤日期：%4\n首次打卡：%5")
            .arg(name).arg(employee)
            .arg(sqlite3_column_int(stmt, 6) ? QStringLiteral("测试记录")
                                           : QStringLiteral("非测试记录"))
            .arg(text(4)).arg(text(5)));
    }
    const bool ok = rc == SQLITE_DONE;
    if (!ok) {
        rows.clear();
        qWarning() << "Read daily attendance failed:" << sqlite3_errmsg(db_);
    }
    sqlite3_finalize(stmt);
    return ok;
}

CheckInResult AttendanceDatabase::recordDailyAttendance(
    qint64 personId, double similarity, const QDateTime& time, QString& error)
{
    error.clear();
    // A board may run in another system zone, so derive both the unique calendar
    // key and the visible timestamp explicitly from the product's chosen zone.
    const QTimeZone zone("Asia/Shanghai");
    if (!db_ || personId <= 0 || !time.isValid() || !zone.isValid() ||
        !std::isfinite(similarity) || similarity < -1 || similarity > 1) {
        error = QStringLiteral("考勤参数无效");
        return CheckInResult::Failed;
    }
    const QDateTime local = time.toTimeZone(zone);
    const QByteArray day = local.date().toString(Qt::ISODate).toUtf8();
    const QByteArray stamp = local.toString(Qt::ISODateWithMs).toUtf8();
    // 先验证正式人员仍然存在，避免旧图库或错误 ID 生成考勤。
    sqlite3_stmt* person = nullptr;
    const char* personSql = "SELECT 1 FROM persons WHERE id=? "
                            "AND length(trim(employee_no))>0 AND length(trim(name))>0";
    if (sqlite3_prepare_v2(db_, personSql, -1, &person, nullptr) != SQLITE_OK) {
        error = QString::fromUtf8(sqlite3_errmsg(db_));
        return CheckInResult::Failed;
    }
    const bool exists = sqlite3_bind_int64(person, 1, personId) == SQLITE_OK &&
                        sqlite3_step(person) == SQLITE_ROW;
    sqlite3_finalize(person);
    if (!exists) {
        error = QStringLiteral("正式人员不存在或没有工号");
        return CheckInResult::Failed;
    }
    // Only the expected same-person/same-day conflict is ignored. Other SQL or
    // storage failures propagate so the GUI can retry rather than misreporting.
    const char* sql =
        "INSERT INTO daily_attendance(person_id,attendance_date,check_time,similarity,is_test) "
        "VALUES(?,?,?,?,1) ON CONFLICT(person_id,attendance_date) DO NOTHING";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = QString::fromUtf8(sqlite3_errmsg(db_));
        return CheckInResult::Failed;
    }
    const bool bound = sqlite3_bind_int64(stmt, 1, personId) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 2, day.constData(), day.size(), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 3, stamp.constData(), stamp.size(), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 4, similarity) == SQLITE_OK;
    const int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    const int changed = sqlite3_changes(db_);
    if (rc != SQLITE_DONE) error = QString::fromUtf8(sqlite3_errmsg(db_));
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return CheckInResult::Failed;
    return changed == 1 ? CheckInResult::Inserted : CheckInResult::AlreadyRecorded;
}

bool AttendanceDatabase::loadBoundEnrollmentSamples(
    std::vector<EnrollmentSample>& samples)
{
    samples.clear();
    if (!db_) return false;
    // INNER JOIN excludes unbound/legacy samples. Identity always comes from the
    // person record; a sample's historical label is never used to infer a person.
    const char* sql =
        "SELECT s.id,s.name,s.face_png,s.landmarks_json,"
        "s.source_width,s.source_height,s.crop_x,s.crop_y,s.crop_width,s.crop_height,"
        "p.id,p.name,p.employee_no FROM enrollment_samples s "
        "INNER JOIN persons p ON p.id=s.person_id "
        "WHERE p.id>0 AND length(trim(p.name))>0 "
        "AND length(trim(p.employee_no))>0 ORDER BY p.id,s.id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "Read bound samples failed:" << sqlite3_errmsg(db_);
        return false;
    }
    auto text = [stmt](int column) {
        const auto* data = sqlite3_column_text(stmt, column);
        return data ? QString::fromUtf8(reinterpret_cast<const char*>(data),
                                       sqlite3_column_bytes(stmt, column)) : QString();
    };
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        EnrollmentSample sample;
        sample.id = sqlite3_column_int64(stmt, 0);
        sample.name = text(1);
        // SQLite owns blob/text pointers until the next step/finalize. Deep-copy
        // now so this sample can safely outlive the statement and cross threads.
        sample.facePng = QByteArray(
            static_cast<const char*>(sqlite3_column_blob(stmt, 2)),
            sqlite3_column_bytes(stmt, 2));
        sample.landmarksJson = text(3);
        sample.sourceSize = QSize(sqlite3_column_int(stmt, 4), sqlite3_column_int(stmt, 5));
        sample.crop = QRect(sqlite3_column_int(stmt, 6), sqlite3_column_int(stmt, 7),
                            sqlite3_column_int(stmt, 8), sqlite3_column_int(stmt, 9));
        sample.personId = sqlite3_column_int64(stmt, 10);
        sample.personName = text(11);
        sample.employeeNo = text(12);
        samples.push_back(std::move(sample));
    }
    const bool ok = rc == SQLITE_DONE;
    if (!ok) {
        qWarning() << "Bound sample query failed:" << sqlite3_errmsg(db_);
        samples.clear();
    }
    sqlite3_finalize(stmt);
    return ok;
}

AttendanceDatabase::~AttendanceDatabase()
{
    // The main function joins worker threads before QApplication teardown, so no
    // worker can still be using gallery data when this GUI-owned handle closes.
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool AttendanceDatabase::open(const QString& path)
{
    // foreign_keys is a per-connection SQLite setting, not a schema property.
    const bool opened = sqlite3_open(
        path.toUtf8().constData(),
        &db_) == SQLITE_OK;
    // 外键校验是连接级配置，不能只依赖手动建表时开启过。
    return opened && sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr,
                                  nullptr) == SQLITE_OK;
}

bool AttendanceDatabase::initialize()
{
    // First create the original baseline tables. Additive migrations below keep
    // pre-existing student/test databases usable without dropping any records.
    if (!db_) return false;
    const char* sql = R"SQL(
        PRAGMA journal_mode=WAL;

        CREATE TABLE IF NOT EXISTS daily_attendance (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            person_id INTEGER NOT NULL REFERENCES persons(id),
            attendance_date TEXT NOT NULL,
            check_time TEXT NOT NULL,
            similarity REAL NOT NULL CHECK(similarity BETWEEN -1 AND 1),
            is_test INTEGER NOT NULL DEFAULT 1 CHECK(is_test IN (0, 1)),
            UNIQUE(person_id, attendance_date)
        );

        CREATE TABLE IF NOT EXISTS persons (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            feature TEXT NOT NULL,
            created_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS attendance (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            person_id INTEGER NOT NULL,
            check_time TEXT NOT NULL,
            UNIQUE(person_id, check_time)
        );

       CREATE TABLE IF NOT EXISTS unknown_detections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    face_count INTEGER NOT NULL,
    detected_at TEXT NOT NULL
);

              -- 暂存人工录入的姓名和人脸样本，不代表已完成身份识别。
        -- 图片以 PNG 二进制保存，后续可与元数据一起原子写入。
        CREATE TABLE IF NOT EXISTS enrollment_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL CHECK(length(trim(name)) > 0),
            face_png BLOB NOT NULL CHECK(length(face_png) > 0),
            landmarks_json TEXT NOT NULL,
            source_width INTEGER NOT NULL CHECK(source_width > 0),
            source_height INTEGER NOT NULL CHECK(source_height > 0),
            crop_x INTEGER NOT NULL,
            crop_y INTEGER NOT NULL,
            crop_width INTEGER NOT NULL CHECK(crop_width > 0),
            crop_height INTEGER NOT NULL CHECK(crop_height > 0),
            created_at TEXT NOT NULL
        );

    )SQL";

    char* error = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);

    if (rc != SQLITE_OK) {
        qWarning() << "SQLite initialize failed:"
                   << (error ? error : "unknown error");
        sqlite3_free(error);
        return false;
    }

    // Add employee_no and person_id only when absent. BEGIN IMMEDIATE prevents a
    // concurrent writer from observing a half-migrated schema; a failure rolls
    // the entire migration back.
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;
    auto column = [&](const char* query, const QString& name, const char* alter) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, query, -1, &stmt, nullptr) != SQLITE_OK) return false;
        bool found = false;
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            found |= QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) == name;
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE && (found || sqlite3_exec(db_, alter, nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    const bool migrated = column("PRAGMA table_info(persons)", "employee_no",
        "ALTER TABLE persons ADD COLUMN employee_no TEXT NOT NULL DEFAULT ''") &&
        column("PRAGMA table_info(enrollment_samples)", "person_id",
        "ALTER TABLE enrollment_samples ADD COLUMN person_id INTEGER REFERENCES persons(id)") &&
        sqlite3_exec(db_, "CREATE UNIQUE INDEX IF NOT EXISTS idx_persons_employee_no ON persons(employee_no) WHERE employee_no <> '';"
            "CREATE INDEX IF NOT EXISTS idx_enrollment_samples_person_id ON enrollment_samples(person_id);",
            nullptr, nullptr, nullptr) == SQLITE_OK;
    if (migrated && sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) return true;
    qWarning() << "Schema migration failed:" << sqlite3_errmsg(db_);
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}

bool AttendanceDatabase::findEmployee(const QString& employee, qint64& id, QString& name)
{
    // id=-1 distinguishes a valid "not found" query from a query error. This is
    // important for the enrollment UI: it may create a person only after a valid
    // negative lookup, never after an ambiguous database failure.
    id = -1; name.clear();
    sqlite3_stmt* stmt = nullptr;
    if (!db_ || sqlite3_prepare_v2(db_, "SELECT id,name FROM persons WHERE employee_no=?",
        -1, &stmt, nullptr) != SQLITE_OK) return false;
    const QByteArray key = employee.toUtf8();
    const int rc = sqlite3_bind_text(stmt, 1, key.constData(), key.size(), SQLITE_TRANSIENT) == SQLITE_OK
        ? sqlite3_step(stmt) : SQLITE_ERROR;
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
        name = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

qint64 AttendanceDatabase::savePersonEnrollment(const QString& name, const QString& employee,
    qint64 expectedPerson, const QByteArray& png, const QString& landmarks,
    const QSize& size, const QRect& crop, QString& error)
{
    error.clear();
    // Restrict employee identifiers to ASCII digits, but preserve the Qt string
    // rather than converting to an integer so leading zeros remain meaningful.
    bool digits = !employee.isEmpty() && employee.size() <= 20;
    for (QChar ch : employee) digits &= ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
    if (!db_ || !digits || name.trimmed().isEmpty() || name.size() > 40) {
        error = QStringLiteral("姓名或工号无效（工号为1至20位数字）"); return -1;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        error = QString::fromUtf8(sqlite3_errmsg(db_)); return -1;
    }
    // Centralize rollback: every return after BEGIN IMMEDIATE must either commit
    // a complete person/sample binding or remove all changes made by this call.
    auto fail = [&]() -> qint64 {
        if (error.isEmpty()) error = QString::fromUtf8(sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    };
    qint64 person = -1;
    QString existingName;
    if (!findEmployee(employee, person, existingName)) return fail();
    // Revalidate the UI's explicit confirmation under the writer lock. Without
    // this check, a changed employee record could receive the wrong face sample.
    if (person != expectedPerson || (person > 0 && existingName != name)) {
        error = QStringLiteral("人员资料已变化，请重新录入并确认"); return fail();
    }
    if (person < 0) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO persons(name,employee_no,feature,created_at) VALUES(?,?,'',?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return fail();
        const QByteArray n = name.trimmed().toUtf8(), e = employee.toUtf8();
        const QByteArray time = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8();
        const bool ok = sqlite3_bind_text(stmt,1,n.constData(),n.size(),SQLITE_TRANSIENT)==SQLITE_OK &&
            sqlite3_bind_text(stmt,2,e.constData(),e.size(),SQLITE_TRANSIENT)==SQLITE_OK &&
            sqlite3_bind_text(stmt,3,time.constData(),time.size(),SQLITE_TRANSIENT)==SQLITE_OK &&
            sqlite3_step(stmt)==SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok) return fail();
        person = sqlite3_last_insert_rowid(db_);
    }
    // The raw insert and the following binding are one transaction. A failed
    // binding cannot leave a detached PNG record or a newly created orphan person.
    const qint64 sample = saveEnrollmentSample(name, png, landmarks, size, crop);
    if (sample < 0) { error = QStringLiteral("样本保存失败，事务已撤销"); return fail(); }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE enrollment_samples SET person_id=? WHERE id=?",
        -1, &stmt, nullptr) != SQLITE_OK) return fail();
    const bool bound = sqlite3_bind_int64(stmt,1,person)==SQLITE_OK &&
        sqlite3_bind_int64(stmt,2,sample)==SQLITE_OK && sqlite3_step(stmt)==SQLITE_DONE && sqlite3_changes(db_)==1;
    sqlite3_finalize(stmt);
    if (!bound || sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) return fail();
    return sample;
}

bool AttendanceDatabase::addPerson(
    const QString& name,
    const QString& feature)
{
    // Legacy exercise API: it has no employee number and is not used by the new
    // enrollment UI. Keep it for backwards-compatible tooling only.
    const char* sql =
        "INSERT INTO persons(name, feature, created_at) "
        "VALUES (?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const QString now =
        QDateTime::currentDateTime().toString(Qt::ISODate);

    sqlite3_bind_text(stmt, 1, name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, feature.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, now.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool AttendanceDatabase::recordAttendance(int personId)
{
    // Legacy table/API retained for old tools. New UI uses daily_attendance,
    // which stores explicit similarity and timezone-aware timestamps.
    const QString today =
        QDate::currentDate().toString(Qt::ISODate);

    const char* sql =
        "INSERT OR IGNORE INTO attendance(person_id, check_time) "
        "VALUES (?, ?)";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, personId);
    sqlite3_bind_text(
        stmt, 2, today.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool AttendanceDatabase::recordUnknownDetection(int faceCount)
{
    // Diagnostic presence log only. It does not prove a face is an unknown
    // identity, and it must not be used as a security/audit conclusion.
    const char* sql =
        "INSERT INTO unknown_detections(face_count, detected_at) "
        "VALUES (?, ?)";

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(
            db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    const QString now =
        QDateTime::currentDateTime().toString(Qt::ISODate);

    sqlite3_bind_int(statement, 1, faceCount);
    sqlite3_bind_text(
        statement,
        2,
        now.toUtf8().constData(),
        -1,
        SQLITE_TRANSIENT
    );

    const bool ok =
        sqlite3_step(statement) == SQLITE_DONE;

    sqlite3_finalize(statement);
    return ok;
}

QString AttendanceDatabase::findPersonByFeature(
    const QString& feature)
{
    // Legacy exact-string lookup, retained for older experiments. The live path
    // uses numeric embeddings and cosine ranking instead.
    const char* sql =
        "SELECT name FROM persons WHERE feature = ? LIMIT 1";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }

    sqlite3_bind_text(
        stmt, 1, feature.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    QString result;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }

    sqlite3_finalize(stmt);
    return result;
}


qint64 AttendanceDatabase::saveEnrollmentSample(
    const QString& name,
    const QByteArray& facePng,
    const QString& landmarksJson,
    const QSize& sourceSize,
    const QRect& crop)
{
    // Validate structural evidence before binding SQL parameters. This method is
    // called inside savePersonEnrollment's transaction in the current UI path.
    const QString cleanName = name.trimmed();
    const QRect sourceRect(QPoint(0, 0), sourceSize);
    if (!db_ || cleanName.isEmpty() || facePng.isEmpty() ||
        landmarksJson.isEmpty() || sourceSize.isEmpty() ||
        crop.isEmpty() || !sourceRect.contains(crop)) {
        qWarning() << "Invalid enrollment sample";
        return -1;
    }

    const char* sql =
        "INSERT INTO enrollment_samples("
        "name,face_png,landmarks_json,source_width,source_height,"
        "crop_x,crop_y,crop_width,crop_height,created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "Prepare enrollment failed:" << sqlite3_errmsg(db_);
        return -1;
    }

    // SQLITE_TRANSIENT copies all temporary Qt buffers before this function
    // returns, so SQLite never observes a dangling QString/QByteArray pointer.
    const QByteArray nameUtf8 = cleanName.toUtf8();
    const QByteArray pointsUtf8 = landmarksJson.toUtf8();
    const QByteArray now = QDateTime::currentDateTime()
                               .toString(Qt::ISODateWithMs).toUtf8();

    bool bound =
        sqlite3_bind_text(stmt, 1, nameUtf8.constData(), nameUtf8.size(), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_blob(stmt, 2, facePng.constData(), facePng.size(), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 3, pointsUtf8.constData(), pointsUtf8.size(), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, sourceSize.width()) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 5, sourceSize.height()) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 6, crop.x()) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 7, crop.y()) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 8, crop.width()) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 9, crop.height()) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 10, now.constData(), now.size(), SQLITE_TRANSIENT) == SQLITE_OK;

    // A single INSERT keeps PNG, geometry and timestamp together. The enclosing
    // enrollment transaction handles the person_id binding and rollback.
    const bool saved = bound && sqlite3_step(stmt) == SQLITE_DONE;
    if (!saved)
        qWarning() << "Save enrollment failed:" << sqlite3_errmsg(db_);

    const qint64 id = saved ? sqlite3_last_insert_rowid(db_) : -1;
    sqlite3_finalize(stmt);
    return id;
}

bool AttendanceDatabase::listEnrollmentSamples(QStringList& rows)
{
    // Management needs a compact list, not the potentially large face BLOBs.
    rows.clear();
    if (!db_) {
        qWarning() << "Database is not open";
        return false;
    }

    // 列表仅查询元数据，不加载 PNG，避免浏览时读取大量图片。
    const char* sql =
        "SELECT id,name,created_at FROM enrollment_samples "
        "ORDER BY id DESC";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "List samples failed:" << sqlite3_errmsg(db_);
        return false;
    }

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const qint64 id = sqlite3_column_int64(stmt, 0);
        const QString name = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        const QString time = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));

        rows.append(QStringLiteral("#%1  %2  |  %3")
                        .arg(id).arg(name).arg(time));
    }

    const bool ok = rc == SQLITE_DONE;
    if (!ok) {
        qWarning() << "Read samples failed:" << sqlite3_errmsg(db_);
        rows.clear();
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool AttendanceDatabase::deleteEnrollmentSample(qint64 id)
{
    // Success requires exactly one affected row. Missing ids are reported as a
    // false result so the UI cannot claim it refreshed a non-existent sample.
    if (!db_ || id <= 0) {
        return false;
    }

    const char* sql =
        "DELETE FROM enrollment_samples WHERE id = ?;";

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(
            db_,
            sql,
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(statement, 1, id);

    const bool success =
        sqlite3_step(statement) == SQLITE_DONE &&
        sqlite3_changes(db_) == 1;

    sqlite3_finalize(statement);
    return success;
}

bool AttendanceDatabase::nameExists(const QString& name)
{
    // Legacy helper: same text name is not a modern uniqueness guarantee.
    if (!db_ || name.trimmed().isEmpty()) {
        return false;
    }

    const char* sql =
        "SELECT 1 FROM enrollment_samples "
        "WHERE lower(trim(name)) = lower(trim(?)) "
        "LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(
            db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "Check name failed:" << sqlite3_errmsg(db_);
        return false;
    }

    const QByteArray utf8Name = name.trimmed().toUtf8();
    sqlite3_bind_text(
        stmt, 1, utf8Name.constData(), -1, SQLITE_TRANSIENT);

    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

bool AttendanceDatabase::loadEnrollmentSample(
    qint64 id, EnrollmentSample& sample)
{
    sample = EnrollmentSample{};
    if (!db_ || id <= 0) return false;

    const char* sql =
        "SELECT id,name,face_png,landmarks_json,source_width,source_height,"
        "crop_x,crop_y,crop_width,crop_height "
        "FROM enrollment_samples WHERE id=?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "Prepare sample read failed:" << sqlite3_errmsg(db_);
        return false;
    }

    const bool bound = sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK;
    const int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    if (rc != SQLITE_ROW) {
        qWarning() << "Sample read failed or ID not found:" << id
                   << sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    // Copy every statement-owned value before finalizing. The caller may decode
    // this PNG after the database query is long gone.
    auto text = [stmt](int column) {
        return QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, column)),
            sqlite3_column_bytes(stmt, column));
    };
    sample.id = sqlite3_column_int64(stmt, 0);
    sample.name = text(1);
    sample.facePng = QByteArray(
        static_cast<const char*>(sqlite3_column_blob(stmt, 2)),
        sqlite3_column_bytes(stmt, 2));
    sample.landmarksJson = text(3);
    sample.sourceSize = QSize(
        sqlite3_column_int(stmt, 4), sqlite3_column_int(stmt, 5));
    sample.crop = QRect(
        sqlite3_column_int(stmt, 6), sqlite3_column_int(stmt, 7),
        sqlite3_column_int(stmt, 8), sqlite3_column_int(stmt, 9));
    sqlite3_finalize(stmt);
    return true;
}
