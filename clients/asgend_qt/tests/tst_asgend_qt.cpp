// SPDX-License-Identifier: MIT
#include "core/idempotency.h"
#include "core/models.h"
#include "core/profile.h"
#include "core/redact.h"
#include "core/sha256verifier.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace asgend;

class QtCoreTests final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void taskStateRoundTrips();
    void idempotencyKeysAreValidAndUnique();
    void profileRejectsRemoteHttpAndNonLoopbackTcp();
    void profileSettingsNeverExportSecrets();
    void redactorRemovesBearerAndTokenMaterial();
    void sha256VerifierChecksDigestAndLimitsInputShape();
};

void QtCoreTests::taskStateRoundTrips()
{
    for (TaskState state : allTaskStates()) {
        QCOMPARE(taskStateFromString(taskStateToString(state)), state);
        QVERIFY(!taskStateDisplayName(state).isEmpty());
    }
    QVERIFY(isTerminal(TaskState::Failed));
    QVERIFY(isRetryable(TaskState::DeliveryFailed));
    QVERIFY(!isCancellable(TaskState::Cancelled));
}

void QtCoreTests::idempotencyKeysAreValidAndUnique()
{
    const QString first = IdempotencyKey::generate(QStringLiteral("submit"));
    const QString second = IdempotencyKey::generate(QStringLiteral("submit"));
    QVERIFY(IdempotencyKey::isValid(first));
    QVERIFY(IdempotencyKey::isValid(second));
    QVERIFY(first != second);
    QVERIFY(!IdempotencyKey::isValid(QStringLiteral("bad key")));
}

void QtCoreTests::profileRejectsRemoteHttpAndNonLoopbackTcp()
{
    ConnectionProfile profile;
    profile.id = QStringLiteral("test");
    profile.name = QStringLiteral("test");
    profile.type = ConnectionProfile::Type::Remote;
    profile.baseUrl = QStringLiteral("http://example.com");
    QVERIFY(!profile.validate().isEmpty());
    profile.baseUrl = QStringLiteral("https://example.com");
    QVERIFY(profile.validate().isEmpty());
    profile.type = ConnectionProfile::Type::LocalTcp;
    profile.host = QStringLiteral("192.168.1.15");
    QVERIFY(!profile.validate().isEmpty());
    profile.host = QStringLiteral("127.0.0.1");
    QVERIFY(profile.validate().isEmpty());
}

void QtCoreTests::profileSettingsNeverExportSecrets()
{
    AppSettings settings = AppSettings::defaults();
    QJsonObject obj = settings.toJson();
    obj.insert(QStringLiteral("auth_token"), QStringLiteral("ucp_should_not_export"));
    const QStringList stripped = AppSettings::stripSecrets(obj);
    QVERIFY(!stripped.isEmpty());
    const QByteArray exported = settings.exportJson();
    QVERIFY(!exported.contains("token"));
    QVERIFY(exported.contains("contains_credentials"));
}

void QtCoreTests::redactorRemovesBearerAndTokenMaterial()
{
    const QString input = QStringLiteral("Authorization: Bearer ucp_1234567890 {\"auth_token\":\"topsecret\"}");
    const QString safe = Redactor::redact(input);
    QVERIFY(!safe.contains(QStringLiteral("ucp_1234567890")));
    QVERIFY(!safe.contains(QStringLiteral("topsecret")));
}

void QtCoreTests::sha256VerifierChecksDigestAndLimitsInputShape()
{
    QCOMPARE(Sha256Verifier::hexOf(QByteArrayLiteral("abc")),
             QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    QVERIFY(Sha256Verifier::isValidHex(QString(64, QLatin1Char('a'))));
    QVERIFY(!Sha256Verifier::isValidHex(QStringLiteral("xyz")));
    QVERIFY(Sha256Verifier::equalHex(QString(64, QLatin1Char('A')), QString(64, QLatin1Char('a'))));
}

QTEST_MAIN(QtCoreTests)
#include "tst_asgend_qt.moc"
