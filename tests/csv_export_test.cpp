/*
 * Board-side CSV regression test. It instantiates the real AttendanceDatabase
 * in a Qt temporary directory, inserts synthetic records, and validates export
 * semantics. No device database, image, or personal attendance row is opened.
 */
#include "database.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDebug>
#include <cstdlib>
static void require(bool ok, const char* what) {
    if (!ok) { qCritical() << what; std::exit(1); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    AttendanceDatabase db;
    require(db.open(dir.path()+"/test.db") && db.initialize(), "initialize");
    const QString path = dir.path()+"/out.csv";
    QString error;
    qint64 count = -1;
    require(db.exportDailyAttendanceCsv(path,count,error) && count==0, "empty export");
    QFile empty(path);
    require(empty.open(QIODevice::ReadOnly), "open empty export");
    const QByteArray header = empty.readAll(); empty.close();
    require(header.startsWith(QByteArray::fromHex("efbbbf")) && header.endsWith("\r\n"), "BOM and CRLF");
    const QString name = QString::fromUtf8("=张,\"三\"\n同学");
    require(db.savePersonEnrollment(name,"001",-1,"png","[]",QSize(112,112),
        QRect(0,0,112,112),error)>0, "create person");
    qint64 id;
    QString found;
    require(db.findEmployee("001",id,found) && id>0,"find person");
    const QDateTime start = QDateTime::fromString("2026-01-01T09:00:00+08:00",Qt::ISODate);
    for (int i=0;i<105;++i)
        require(db.recordDailyAttendance(id,.84,start.addDays(i),error)==CheckInResult::Inserted,"insert day");
    require(db.exportDailyAttendanceCsv(path,count,error) && count==105,"all rows beyond 100");
    QFile csv(path);
    require(csv.open(QIODevice::ReadOnly),"open CSV");
    const QByteArray bytes = csv.readAll(); csv.close();
    require(bytes.startsWith(header),"same header");
    require(bytes.contains(QString::fromUtf8("\"'=张,\"\"三\"\"\n同学\"").toUtf8()),"escaped text/formula prefix");
    require(bytes.contains("\"001\"") && bytes.contains("+08:00\""),"identifier and timezone");
    require(bytes.count("\"1\"\r\n")==105,"all test flags");
    require(!db.exportDailyAttendanceCsv(dir.path()+"/missing/out.csv",count,error) && count==0 && !error.isEmpty(),"write failure");
    QStringList rows;
    require(db.listDailyAttendance(rows) && rows.size()==100,"display limit unchanged");
    qInfo()<<"CSV export tests PASSED";
}
