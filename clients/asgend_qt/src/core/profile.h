// SPDX-License-Identifier: MIT
// Connection profiles and application settings. Settings never contain
// credentials: tokens live only in the CredentialStore, keyed by profile id.
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace asgend {

struct ConnectionProfile {
    enum class Type { LocalSocket, LocalTcp, Remote };

    QString id;
    QString name;
    Type type = Type::LocalSocket;
    // LocalSocket
    QString socketPath;
    // LocalTcp (loopback only: the raw core API is unencrypted)
    QString host = QStringLiteral("127.0.0.1");
    int port = 8765;
    // Remote Control Plane
    QString baseUrl;           // https://cp.example.org
    QString workspaceId;       // ws_… (filled from pairing or /v1/me)
    QString caCertificatePath; // optional PEM bundle added to the system CAs
    QString deviceName;
    // Development only: permits plain http:// to a loopback host. TLS
    // certificate errors are never ignored, with or without this flag.
    bool devInsecureLoopback = false;
    int requestTimeoutMs = 15000;

    static QString typeToString(Type type);
    static Type typeFromString(const QString &value);
    static QString defaultSocketPath();
    static QString newId();
    static bool isLoopbackHost(const QString &host);

    // Empty string when the profile is acceptable for the security policy.
    QString validate() const;
    QUrl url(const QString &path) const;

    QJsonObject toJson() const;
    static ConnectionProfile fromJson(const QJsonObject &obj);
};

struct AppSettings {
    static constexpr int CurrentVersion = 1;

    QList<ConnectionProfile> profiles;
    QString activeProfileId;
    int reconnectInitialMs = 1000;
    int reconnectMaxMs = 30000;
    int heartbeatMs = 15000;
    int corePollMs = 1000;
    int longPollWaitMs = 25000;
    int taskRefreshMs = 5000;
    QString logLevel = QStringLiteral("info");
    bool logToFile = false;
    QString downloadDirectory;
    qint64 maxDownloadBytes = 256LL * 1024 * 1024;
    bool notifications = true;

    const ConnectionProfile *activeProfile() const;
    ConnectionProfile *profileById(const QString &id);

    QJsonObject toJson() const;
    // Unknown fields are ignored; any credential-looking fields are dropped
    // and reported in warnings. Older settings versions are migrated.
    static AppSettings fromJson(const QJsonObject &obj, QStringList *warnings = nullptr);

    // Export/import for sharing configuration between machines: never
    // contains credentials (tokens, passwords) by construction and by filter.
    QByteArray exportJson() const;
    static bool importJson(const QByteArray &json, AppSettings *out, QString *error, QStringList *warnings = nullptr);

    static QString defaultPath();
    bool save(const QString &path, QString *error = nullptr) const;
    static AppSettings load(const QString &path, QStringList *warnings = nullptr);
    static AppSettings defaults();

    // Removes keys whose names look like secrets, recursively. Returns the
    // removed key paths.
    static QStringList stripSecrets(QJsonObject &obj, const QString &prefix = QString());
};

} // namespace asgend
