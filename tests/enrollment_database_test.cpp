/*
 * Transactional enrollment API regression test.
 *
 * The test uses a Qt temporary directory and synthetic values to prove leading
 * zero preservation, append behavior, rollback, and reusability after failure.
 * It never opens the production attendance database.
 */
#include "database.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDebug>
#include <cstdlib>

static void require(bool ok, const char* message) {
    if (!ok) { qCritical() << message; std::exit(1); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir folder;
    require(folder.isValid(), "temporary directory");
    AttendanceDatabase db;
    require(db.open(folder.path()+"/test.db") && db.initialize() && db.initialize(), "initialize twice");
    QString error, name;
    qint64 person;
    auto save = [&](QString n, QString e, qint64 expected, QByteArray png) {
        return db.savePersonEnrollment(n,e,expected,png,"[]",QSize(112,112),QRect(0,0,112,112),error);
    };
    require(save("person_a","001",-1,"test-png")>0,"create bound person");
    require(db.findEmployee("001",person,name) && person>0 && name=="person_a","leading zero lookup");
    require(save("person_a","001",person,"another-png")>0,"append existing person");
    require(save("different_name","001",person,"png")<0,"reject changed name");
    require(save("duplicate_name","001",-1,"png")<0,"reject stale new-person confirmation");
    require(save("person_b","002",-1,QByteArray())<0,"sample failure rolls back person");
    qint64 missing;
    require(db.findEmployee("002",missing,name) && missing<0,"no orphan person");
    require(save("person_b","002",-1,"png")>0,"transaction reusable after rollback");
    std::vector<EnrollmentSample> samples;
    require(db.loadBoundEnrollmentSamples(samples) && samples.size()==3,"bound gallery has three samples");
    require(samples[0].personId==samples[1].personId,"append keeps identity");
    require(db.deleteEnrollmentSample(samples[0].id),"delete sample");
    require(db.loadBoundEnrollmentSamples(samples) && samples.size()==2,"gallery reflects deletion");
    require(db.initialize(),"migration preserves data");
    require(db.loadBoundEnrollmentSamples(samples) && samples.size()==2,"data preserved");
    qInfo()<<"Enrollment database tests PASSED";
}
