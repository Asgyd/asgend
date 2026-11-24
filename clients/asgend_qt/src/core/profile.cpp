// SPDX-License-Identifier: MIT
#include "core/profile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace asgend {

namespace {

QString trProfile(const char *text)
{
    return QCoreApplication::translate("ConnectionProfile", text);
}

bool looksSecret(const QString &key)
{
    const QString k = key.toLower();
    static const char *needles[] = {"token", "password", "passwd", "secret", "api_key", "apikey",
                                    "credential", "bearer", "authorization", "private_key"};
    for (const char *needle : needles) {
        if (k.contains(QLatin1String(needle))) return true;
    }
    return false;
}

int clampInt(const QJsonObject &o, const char *key, int fallback, int min, int max)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return fallback;
    return qBound(min, v.toInt(fallback), max);
}

} // namespace

QString ConnectionProfile::typeToString(Type type)
{
    switch (type) {
    case Type::LocalSocket: return QStringLiteral("local_socket");
    case Type::LocalTcp: return QStringLiteral("local_tcp");
    case Type::Remote: return QStringLiteral("remote");
    }
    return QStringLiteral("local_socket");
}

ConnectionProfile::Type ConnectionProfile::typeFromString(const QString &value)
{
    if (value == QLatin1String("local_tcp") || value == QLatin1String("tcp")) return Type::LocalTcp;
    if (value == QLatin1String("remote") || value == QLatin1String("control_plane")) return Type::Remote;
    return Type::LocalSocket;
}

QString ConnectionProfile::defaultSocketPath()
{
    // install.sh places the core config at $XDG_CONFIG_HOME/asgend/config.json;
    // the core resolves server.unix_socket.path ("data/agent-orchestrator.sock")
    // relative to that directory.
    QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (config.isEmpty()) config = QDir::homePath() + QStringLiteral("/.config");
    return config + QStringLiteral("/asgend/data/agent-orchestrator.sock");
}

QString ConnectionProfile::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool ConnectionProfile::isLoopbackHost(const QString &host)
{
    QString h = host.trimmed().toLower();
    if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']'))) h = h.mid(1, h.size() - 2);
    if (h == QLatin1String("localhost") || h.endsWith(QLatin1String(".localhost"))) return true;
    QHostAddress address;
    if (!address.setAddress(h)) return false;
    return address.isLoopback();
}

QString ConnectionProfile::validate() const
{
    switch (type) {
    case Type::LocalSocket:
        if (socketPath.trimmed().isEmpty()) return trProfile("Socket path is required");
        return QString();
    case Type::LocalTcp:
        if (host.trimmed().isEmpty()) return trProfile("Host is required");
        if (port < 1 || port > 65535) return trProfile("Port must be between 1 and 65535");
        if (!isLoopbackHost(host))
            return trProfile("The raw core TCP API is unencrypted and is only allowed on a loopback address. "
                             "Use an SSH tunnel to a local port or the Control Plane for remote access.");
        return QString();
    case Type::Remote: {
        const QUrl u(baseUrl.trimmed(), QUrl::StrictMode);
        if (!u.isValid() || u.host().isEmpty()) return trProfile("A valid Control Plane URL is required");
        if (!u.userInfo().isEmpty()) return trProfile("Credentials must not be embedded in the URL");
        if (u.hasQuery() || u.hasFragment()) return trProfile("The Control Plane URL must not contain a query or fragment");
        const QString scheme = u.scheme().toLower();
        if (scheme == QLatin1String("https")) return QString();
        if (scheme == QLatin1String("http")) {
            if (devInsecureLoopback && isLoopbackHost(u.host())) return QString();
            return trProfile("Plain HTTP is refused. Use https:// (plain HTTP is only possible for a loopback "
                             "host with the developer flag enabled).");
        }
        return trProfile("Unsupported URL scheme");
    }
    }
    return QString();
}

QUrl ConnectionProfile::url(const QString &path) const
{
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    return QUrl(base + path);
}

QJsonObject ConnectionProfile::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("type"), typeToString(type));
    if (type == Type::LocalSocket) o.insert(QStringLiteral("socket_path"), socketPath);
    if (type == Type::LocalTcp) {
        o.insert(QStringLiteral("host"), host);
        o.insert(QStringLiteral("port"), port);
    }
    if (type == Type::Remote) {
        o.insert(QStringLiteral("base_url"), baseUrl);
        o.insert(QStringLiteral("workspace_id"), workspaceId);
        o.insert(QStringLiteral("ca_certificate_path"), caCertificatePath);
        o.insert(QStringLiteral("device_name"), deviceName);
        o.insert(QStringLiteral("dev_insecure_loopback"), devInsecureLoopback);
    }
    o.insert(QStringLiteral("request_timeout_ms"), requestTimeoutMs);
    return o;
}

ConnectionProfile ConnectionProfile::fromJson(const QJsonObject &obj)
{
    ConnectionProfile p;
    p.id = obj.value(QStringLiteral("id")).toString();
    if (p.id.isEmpty()) p.id = newId();
    p.name = obj.value(QStringLiteral("name")).toString();
    p.type = typeFromString(obj.value(QStringLiteral("type")).toString());
    p.socketPath = obj.value(QStringLiteral("socket_path")).toString();
    if (p.socketPath.isEmpty() && p.type == Type::LocalSocket) p.socketPath = defaultSocketPath();
    p.host = obj.value(QStringLiteral("host")).toString(QStringLiteral("127.0.0.1"));
    p.port = qBound(1, obj.value(QStringLiteral("port")).toInt(8765), 65535);
    p.baseUrl = obj.value(QStringLiteral("base_url")).toString();
    p.workspaceId = obj.value(QStringLiteral("workspace_id")).toString();
    p.caCertificatePath = obj.value(QStringLiteral("ca_certificate_path")).toString();
    p.deviceName = obj.value(QStringLiteral("device_name")).toString();
    p.devInsecureLoopback = obj.value(QStringLiteral("dev_insecure_loopback")).toBool(false);
    p.requestTimeoutMs = clampInt(obj, "request_timeout_ms", 15000, 1000, 120000);
    if (p.name.isEmpty()) p.name = typeToString(p.type);
    return p;
}

const ConnectionProfile *AppSettings::activeProfile() const
{
    for (const ConnectionProfile &p : profiles) {
        if (p.id == activeProfileId) return &p;
    }
    return profiles.isEmpty() ? nullptr : &profiles.first();
}

ConnectionProfile *AppSettings::profileById(const QString &id)
{
    for (ConnectionProfile &p : profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

QJsonObject AppSettings::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("settings_version"), CurrentVersion);
    QJsonArray list;
    for (const ConnectionProfile &p : profiles) list.append(p.toJson());
    o.insert(QStringLiteral("profiles"), list);
    o.insert(QStringLiteral("active_profile_id"), activeProfileId);
    QJsonObject reconnect;
    reconnect.insert(QStringLiteral("initial_ms"), reconnectInitialMs);
    reconnect.insert(QStringLiteral("max_ms"), reconnectMaxMs);
    reconnect.insert(QStringLiteral("heartbeat_ms"), heartbeatMs);
    reconnect.insert(QStringLiteral("core_poll_ms"), corePollMs);
    reconnect.insert(QStringLiteral("long_poll_wait_ms"), longPollWaitMs);
    reconnect.insert(QStringLiteral("task_refresh_ms"), taskRefreshMs);
    o.insert(QStringLiteral("reconnect"), reconnect);
    QJsonObject logging;
    logging.insert(QStringLiteral("level"), logLevel);
    logging.insert(QStringLiteral("to_file"), logToFile);
    o.insert(QStringLiteral("logging"), logging);
    QJsonObject downloads;
    downloads.insert(QStringLiteral("directory"), downloadDirectory);
    downloads.insert(QStringLiteral("max_bytes"), static_cast<double>(maxDownloadBytes));
    o.insert(QStringLiteral("downloads"), downloads);
    o.insert(QStringLiteral("notifications"), notifications);
    return o;
}

QStringList AppSettings::stripSecrets(QJsonObject &obj, const QString &prefix)
{
    QStringList removed;
    const QStringList keys = obj.keys();
    for (const QString &key : keys) {
        const QString path = prefix.isEmpty() ? key : prefix + QLatin1Char('.') + key;
        if (looksSecret(key)) {
            obj.remove(key);
            removed << path;
            continue;
        }
        QJsonValue value = obj.value(key);
        if (value.isObject()) {
            QJsonObject child = value.toObject();
            removed << stripSecrets(child, path);
            obj.insert(key, child);
        } else if (value.isArray()) {
            QJsonArray array = value.toArray();
            for (int i = 0; i < array.size(); ++i) {
                if (!array.at(i).isObject()) continue;
                QJsonObject child = array.at(i).toObject();
                removed << stripSecrets(child, QStringLiteral("%1[%2]").arg(path).arg(i));
                array.replace(i, child);
            }
            obj.insert(key, array);
        }
    }
    return removed;
}

AppSettings AppSettings::fromJson(const QJsonObject &input, QStringList *warnings)
{
    QJsonObject obj = input;
    const QStringList removed = stripSecrets(obj);
    if (warnings) {
        for (const QString &path : removed)
            *warnings << QCoreApplication::translate("AppSettings", "Ignored credential field \"%1\"").arg(path);
    }
    AppSettings s;
    const int version = obj.value(QStringLiteral("settings_version")).toInt(0);
    // Migration v0 -> v1: early drafts used a flat "endpoint" for remote URL
    // and top-level reconnect numbers.
    if (version == 0) {
        QJsonArray migrated;
        for (const QJsonValue &v : obj.value(QStringLiteral("profiles")).toArray()) {
            QJsonObject p = v.toObject();
            if (!p.contains(QStringLiteral("base_url")) && p.contains(QStringLiteral("endpoint")))
                p.insert(QStringLiteral("base_url"), p.value(QStringLiteral("endpoint")));
            migrated.append(p);
        }
        obj.insert(QStringLiteral("profiles"), migrated);
        if (!obj.contains(QStringLiteral("reconnect"))) {
            QJsonObject reconnect;
            if (obj.contains(QStringLiteral("reconnect_initial_ms")))
                reconnect.insert(QStringLiteral("initial_ms"), obj.value(QStringLiteral("reconnect_initial_ms")));
            if (obj.contains(QStringLiteral("reconnect_max_ms")))
                reconnect.insert(QStringLiteral("max_ms"), obj.value(QStringLiteral("reconnect_max_ms")));
            obj.insert(QStringLiteral("reconnect"), reconnect);
        }
    } else if (version > CurrentVersion && warnings) {
        *warnings << QCoreApplication::translate("AppSettings",
                                                 "Settings were written by a newer version; unknown fields are ignored");
    }
    for (const QJsonValue &v : obj.value(QStringLiteral("profiles")).toArray()) {
        if (v.isObject()) s.profiles.append(ConnectionProfile::fromJson(v.toObject()));
    }
    s.activeProfileId = obj.value(QStringLiteral("active_profile_id")).toString();
    const QJsonObject reconnect = obj.value(QStringLiteral("reconnect")).toObject();
    s.reconnectInitialMs = clampInt(reconnect, "initial_ms", 1000, 100, 60000);
    s.reconnectMaxMs = clampInt(reconnect, "max_ms", 30000, s.reconnectInitialMs, 600000);
    s.heartbeatMs = clampInt(reconnect, "heartbeat_ms", 15000, 1000, 600000);
    s.corePollMs = clampInt(reconnect, "core_poll_ms", 1000, 200, 60000);
    s.longPollWaitMs = clampInt(reconnect, "long_poll_wait_ms", 25000, 0, 30000);
    s.taskRefreshMs = clampInt(reconnect, "task_refresh_ms", 5000, 0, 600000);
    const QJsonObject logging = obj.value(QStringLiteral("logging")).toObject();
    s.logLevel = logging.value(QStringLiteral("level")).toString(QStringLiteral("info"));
    s.logToFile = logging.value(QStringLiteral("to_file")).toBool(false);
    const QJsonObject downloads = obj.value(QStringLiteral("downloads")).toObject();
    s.downloadDirectory = downloads.value(QStringLiteral("directory")).toString();
    const double maxBytes = downloads.value(QStringLiteral("max_bytes")).toDouble(256.0 * 1024 * 1024);
    s.maxDownloadBytes = static_cast<qint64>(qBound(1024.0, maxBytes, 64.0 * 1024 * 1024 * 1024));
    s.notifications = obj.value(QStringLiteral("notifications")).toBool(true);
    return s;
}

QByteArray AppSettings::exportJson() const
{
    QJsonObject o = toJson();
    stripSecrets(o); // defence in depth: nothing secret is ever serialised
    o.insert(QStringLiteral("exported_by"), QStringLiteral("asgend-qt " ASGEND_QT_VERSION));
    o.insert(QStringLiteral("contains_credentials"), false);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

bool AppSettings::importJson(const QByteArray &json, AppSettings *out, QString *error, QStringList *warnings)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QCoreApplication::translate("AppSettings", "Invalid settings JSON: %1").arg(parseError.errorString());
        return false;
    }
    AppSettings imported = fromJson(doc.object(), warnings);
    for (const ConnectionProfile &p : imported.profiles) {
        const QString problem = p.validate();
        if (!problem.isEmpty() && warnings)
            *warnings << QCoreApplication::translate("AppSettings", "Profile \"%1\": %2").arg(p.name, problem);
    }
    if (out) *out = imported;
    return true;
}

QString AppSettings::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.json");
}

bool AppSettings::save(const QString &path, QString *error) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

AppSettings AppSettings::load(const QString &path, QStringList *warnings)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return defaults();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        if (warnings) *warnings << QCoreApplication::translate("AppSettings", "Settings file is corrupt; defaults are used");
        return defaults();
    }
    AppSettings s = fromJson(doc.object(), warnings);
    if (s.profiles.isEmpty()) s.profiles = defaults().profiles;
    return s;
}

AppSettings AppSettings::defaults()
{
    AppSettings s;
    ConnectionProfile local;
    local.id = QStringLiteral("local-socket");
    local.name = QCoreApplication::translate("AppSettings", "Local core (Unix socket)");
    local.type = ConnectionProfile::Type::LocalSocket;
    local.socketPath = ConnectionProfile::defaultSocketPath();
    ConnectionProfile tcp;
    tcp.id = QStringLiteral("local-tcp");
    tcp.name = QCoreApplication::translate("AppSettings", "Local core (TCP 127.0.0.1:8765)");
    tcp.type = ConnectionProfile::Type::LocalTcp;
    s.profiles << local << tcp;
    s.activeProfileId = local.id;
    return s;
}

} // namespace asgend
