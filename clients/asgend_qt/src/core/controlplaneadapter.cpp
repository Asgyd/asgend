// SPDX-License-Identifier: MIT
#include "core/controlplaneadapter.h"

#include "core/downloadsink.h"
#include "core/logging.h"
#include "core/redact.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslError>
#include <QSslSocket>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace asgend {

namespace {

constexpr int kMaxErrorBody = 64 * 1024;
constexpr qint64 kMaxJsonBody = 16LL * 1024 * 1024;

QString requestId()
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    QString id = QStringLiteral("qt-");
    for (int i = 0; i < 20; ++i) id += QLatin1Char(alphabet[QRandomGenerator::global()->bounded(36)]);
    return id;
}

QJsonObject unwrap(const QJsonObject &obj, const char *key)
{
    const QJsonValue inner = obj.value(QLatin1String(key));
    return inner.isObject() ? inner.toObject() : obj;
}

template <typename T>
void deliver(const Callback<T> &cb, const ApiError &error, T value)
{
    if (!cb) return;
    if (error.isError()) cb(Result<T>::failure(error));
    else cb(Result<T>::success(std::move(value)));
}

QString pathForLog(const QString &path)
{
    return Redactor::redact(path);
}

} // namespace

ControlPlaneAdapter::ControlPlaneAdapter(ConnectionProfile profile, QString token, QObject *parent)
    : ProtocolAdapter(parent), m_profile(std::move(profile)), m_token(std::move(token)),
      m_workspaceId(m_profile.workspaceId), m_role(QStringLiteral("viewer"))
{
    if (!m_token.isEmpty()) Redactor::addSecret(m_token);
    m_nam = new QNetworkAccessManager(this);
    // Redirects are never followed: they could forward the bearer token to a
    // different origin or downgrade the scheme.
    m_nam->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);

    m_configError = m_profile.validate();
    if (!m_configError.isEmpty()) {
        m_configErrorCode = ApiError::Insecure;
        return;
    }
    const QUrl base(m_profile.baseUrl.trimmed());
    m_tls = base.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
    if (m_tls) {
        if (!QSslSocket::supportsSsl()) {
            m_configError = QStringLiteral("TLS is not available (no Qt TLS backend/OpenSSL found)");
            m_configErrorCode = ApiError::Tls;
            return;
        }
        m_ssl = QSslConfiguration::defaultConfiguration();
        m_ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        m_ssl.setProtocol(QSsl::TlsV1_2OrLater);
        if (!m_profile.caCertificatePath.isEmpty()) {
            const QList<QSslCertificate> extra = QSslCertificate::fromPath(m_profile.caCertificatePath, QSsl::Pem);
            if (extra.isEmpty()) {
                m_configError = QStringLiteral("custom CA file could not be loaded: %1").arg(m_profile.caCertificatePath);
                m_configErrorCode = ApiError::Tls;
                return;
            }
            m_ssl.addCaCertificates(extra);
        }
    } else {
        qCWarning(lcNet) << "developer mode: plain HTTP to loopback Control Plane" << base.host();
    }
}

ControlPlaneAdapter::~ControlPlaneAdapter()
{
    const auto replies = m_replies;
    for (QNetworkReply *reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
    m_replies.clear();
}

Capabilities ControlPlaneAdapter::capabilities() const
{
    Capabilities c;
    const bool mutate = m_role == QLatin1String("owner") || m_role == QLatin1String("operator");
    c.submit = c.answer = c.cancel = c.retry = mutate;
    c.deadLetters = true;
    c.agents = false;
    c.models = false;
    c.manageModels = false;
    c.nodes = true;
    c.manageNodes = m_role == QLatin1String("owner");
    c.artifacts = true;
    c.longPoll = true;
    c.nodeRequiredForSubmit = true;
    c.role = m_role;
    return c;
}

QString ControlPlaneAdapter::endpointDescription() const
{
    return m_profile.baseUrl;
}

void ControlPlaneAdapter::setRole(const QString &role)
{
    const QString normalized = (role == QLatin1String("owner") || role == QLatin1String("operator")) ? role
                                                                                                       : QStringLiteral("viewer");
    if (normalized == m_role) return;
    m_role = normalized;
    Q_EMIT capabilitiesChanged();
}

QString ControlPlaneAdapter::seg(const QString &id)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(id));
}

int ControlPlaneAdapter::parseRetryAfter(const QByteArray &value)
{
    const QByteArray v = value.trimmed();
    if (v.isEmpty()) return -1;
    bool ok = false;
    const qlonglong seconds = v.toLongLong(&ok);
    if (ok) return seconds < 0 ? -1 : static_cast<int>(qMin<qlonglong>(seconds, 86400) * 1000);
    // HTTP-date, e.g. "Wed, 21 Oct 2015 07:28:00 GMT".
    QDateTime when = QLocale::c().toDateTime(QString::fromLatin1(v), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (!when.isValid()) return -1;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    when.setTimeZone(QTimeZone::UTC);
#else
    when.setTimeSpec(Qt::UTC);
#endif
    const qint64 delta = QDateTime::currentDateTimeUtc().msecsTo(when);
    return static_cast<int>(qBound(qint64(0), delta, qint64(86400000)));
}

QNetworkRequest ControlPlaneAdapter::buildRequest(const QUrl &url, const QString &bearer, bool json, int timeoutMs) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "asgend-qt/" ASGEND_QT_VERSION);
    request.setRawHeader("X-Request-ID", requestId().toLatin1());
    if (json) request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    if (!bearer.isEmpty()) request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(timeoutMs);
    if (m_tls) request.setSslConfiguration(m_ssl);
    return request;
}

void ControlPlaneAdapter::track(QNetworkReply *reply)
{
    m_replies.insert(reply);
    connect(reply, &QNetworkReply::sslErrors, this, [reply](const QList<QSslError> &errors) {
        // Never ignored. Recorded so the final error explains the failure.
        QStringList messages;
        for (const QSslError &e : errors) messages << e.errorString();
        reply->setProperty("asgend_ssl_errors", messages.join(QStringLiteral("; ")));
    });
}

void ControlPlaneAdapter::untrack(QNetworkReply *reply)
{
    m_replies.remove(reply);
    reply->deleteLater();
}

ApiError ControlPlaneAdapter::replyError(QNetworkReply *reply, const QByteArray &body) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->property("asgend_cancelled").toBool()) return ApiError(ApiError::Cancelled, QString());
    if (status >= 300) {
        ApiError e;
        e.httpStatus = status;
        const QJsonObject err = QJsonDocument::fromJson(body).object().value(QLatin1String("error")).toObject();
        e.serverCode = err.value(QLatin1String("code")).toString();
        e.message = Redactor::redact(err.value(QLatin1String("message")).toString());
        e.requestId = err.value(QLatin1String("request_id")).toString(QString::fromLatin1(reply->rawHeader("X-Request-ID")));
        e.code = ApiError::fromHttp(status, e.serverCode);
        if (status >= 300 && status < 400) {
            e.code = ApiError::Protocol;
            e.message = QStringLiteral("redirect refused (HTTP %1)").arg(status);
        }
        if (e.message.isEmpty()) e.message = QStringLiteral("HTTP %1").arg(status);
        e.retryAfterMs = parseRetryAfter(reply->rawHeader("Retry-After"));
        return e;
    }
    switch (reply->error()) {
    case QNetworkReply::NoError:
        return ApiError();
    case QNetworkReply::OperationCanceledError:
    case QNetworkReply::TimeoutError:
        return ApiError(ApiError::Timeout, reply->errorString());
    case QNetworkReply::SslHandshakeFailedError: {
        const QString detail = reply->property("asgend_ssl_errors").toString();
        return ApiError(ApiError::Tls, detail.isEmpty() ? reply->errorString() : detail);
    }
    case QNetworkReply::InsecureRedirectError:
    case QNetworkReply::TooManyRedirectsError:
        return ApiError(ApiError::Protocol, reply->errorString());
    default:
        return ApiError(ApiError::Network, Redactor::redact(reply->errorString()));
    }
}

void ControlPlaneAdapter::send(Request r, JsonDone done)
{
    if (!m_configError.isEmpty()) {
        QTimer::singleShot(0, this, [done, code = m_configErrorCode, msg = m_configError]() {
            done(ApiError(code, msg), {}, 0);
        });
        return;
    }
    if (r.auth && m_token.isEmpty()) {
        QTimer::singleShot(0, this, [done]() {
            done(ApiError(ApiError::Unauthorized, QStringLiteral("no token configured; pair this device first")), {}, 401);
        });
        return;
    }
    QUrl url = m_profile.url(r.path);
    if (!r.query.isEmpty()) url.setQuery(r.query);
    const int timeout = r.timeoutMs > 0 ? r.timeoutMs : m_profile.requestTimeoutMs;
    QNetworkRequest request = buildRequest(url, r.auth ? m_token : QString(), r.hasBody, timeout);
    QNetworkReply *reply = nullptr;
    const QByteArray payload = r.hasBody ? QJsonDocument(r.body).toJson(QJsonDocument::Compact) : QByteArray();
    if (r.verb == "GET") reply = m_nam->get(request);
    else if (r.verb == "POST") reply = m_nam->post(request, payload);
    else if (r.verb == "PUT") reply = m_nam->put(request, payload);
    else reply = m_nam->sendCustomRequest(request, r.verb, payload);
    track(reply);
    qCDebug(lcNet) << "cp" << r.verb.constData() << pathForLog(r.path) << "attempt" << r.attempt;

    connect(reply, &QNetworkReply::finished, this, [this, reply, r, done]() {
        const QByteArray body = reply->bytesAvailable() > kMaxJsonBody ? QByteArray() : reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        ApiError error = replyError(reply, body);
        const QByteArray apiVersion = reply->rawHeader("X-ASGEND-API-Version");
        untrack(reply);

        if (error.code == ApiError::RateLimited && r.retrySafe && r.attempt < MaxRateLimitRetries &&
            error.retryAfterMs >= 0 && error.retryAfterMs <= MaxAutoRetryAfterMs) {
            qCInfo(lcNet) << "rate limited; retrying" << pathForLog(r.path) << "after" << error.retryAfterMs << "ms";
            Request again = r;
            again.attempt += 1;
            QTimer::singleShot(error.retryAfterMs, this, [this, again, done]() { send(again, done); });
            return;
        }
        QJsonObject obj;
        if (!error.isError()) {
            if (!apiVersion.isEmpty() && apiVersion.trimmed() != QByteArray::number(ApiVersion)) {
                error = ApiError(ApiError::Protocol, QStringLiteral("incompatible API version %1").arg(QString::fromLatin1(apiVersion)));
            } else if (!body.trimmed().isEmpty()) {
                QJsonParseError parseError;
                const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                    error = ApiError(ApiError::Protocol, QStringLiteral("invalid JSON reply"));
                else
                    obj = doc.object();
            }
        }
        if (error.isError() && error.code != ApiError::Cancelled)
            qCInfo(lcNet) << "cp" << r.verb.constData() << pathForLog(r.path) << "->" << ApiError::codeName(error.code)
                          << status << error.message;
        // Deliver from the event loop: the callback may destroy this adapter.
        QTimer::singleShot(0, this, [done, error, obj, status]() { done(error, obj, status); });
    });
}

bool ControlPlaneAdapter::requireWorkspace(ApiError *error) const
{
    if (!m_workspaceId.isEmpty()) return true;
    *error = ApiError(ApiError::InvalidRequest, QStringLiteral("workspace is not known yet; connect first"));
    return false;
}

void ControlPlaneAdapter::abortAll()
{
    const auto replies = m_replies;
    for (QNetworkReply *reply : replies) {
        reply->setProperty("asgend_cancelled", true);
        reply->abort();
    }
}

// --------------------------------------------------------------- identity

void ControlPlaneAdapter::redeemPairing(const QString &code, const QString &deviceName, Callback<PairingResult> cb)
{
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/pairing/redeem");
    r.auth = false;
    r.hasBody = true;
    r.body = {{QStringLiteral("code"), code.trimmed()}, {QStringLiteral("device_name"), deviceName}};
    send(r, [this, cb](const ApiError &e, const QJsonObject &o, int) {
        PairingResult p;
        p.token = o.value(QLatin1String("token")).toString();
        p.userId = o.value(QLatin1String("user_id")).toString();
        p.workspaceId = o.value(QLatin1String("workspace_id")).toString();
        p.role = o.value(QLatin1String("role")).toString();
        if (!e.isError() && !p.token.startsWith(QLatin1String("ucp_"))) {
            deliver(cb, ApiError(ApiError::Protocol, QStringLiteral("pairing reply did not contain a user token")), PairingResult());
            return;
        }
        if (!e.isError()) {
            Redactor::addSecret(p.token);
            m_token = p.token;
            if (!p.workspaceId.isEmpty()) m_workspaceId = p.workspaceId;
            setRole(p.role);
        }
        deliver(cb, e, p);
    });
}

void ControlPlaneAdapter::logout(Callback<CommandResult> cb)
{
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/auth/logout");
    r.hasBody = true;
    send(r, [this, cb](const ApiError &e, const QJsonObject &, int) {
        if (!e.isError() || e.code == ApiError::Unauthorized) m_token.clear();
        deliver(cb, e.code == ApiError::Unauthorized ? ApiError() : e, CommandResult{QString(), QString(), QStringLiteral("logged_out")});
    });
}

void ControlPlaneAdapter::ping(Callback<ServerInfo> cb)
{
    Request r;
    r.path = QStringLiteral("/v1/me");
    r.retrySafe = true;
    send(r, [this, cb](const ApiError &e, const QJsonObject &o, int) {
        ServerInfo info;
        info.service = QStringLiteral("asgend-control-plane");
        info.version = o.value(QLatin1String("server_version")).toString();
        const QJsonObject user = o.value(QLatin1String("user")).toObject();
        info.userId = user.value(QLatin1String("user_id")).toString(
            user.value(QLatin1String("id")).toString(o.value(QLatin1String("user_id")).toString()));
        if (e.isError()) {
            deliver(cb, e, info);
            return;
        }
        QString chosen = m_workspaceId;
        QString role;
        const QJsonArray workspaces = o.value(QLatin1String("workspaces")).toArray();
        for (const QJsonValue &v : workspaces) {
            const QJsonObject ws = v.toObject();
            const QString id = ws.value(QLatin1String("workspace_id")).toString(ws.value(QLatin1String("id")).toString());
            if (chosen.isEmpty()) chosen = id;
            if (id == chosen) {
                role = ws.value(QLatin1String("role")).toString();
                break;
            }
        }
        if (chosen.isEmpty() || (role.isEmpty() && !workspaces.isEmpty())) {
            deliver(cb, ApiError(ApiError::Forbidden, QStringLiteral("this account has no access to the configured workspace")), info);
            return;
        }
        if (role.isEmpty()) role = o.value(QLatin1String("role")).toString();
        m_workspaceId = chosen;
        setRole(role);
        info.workspaceId = m_workspaceId;
        info.role = m_role;
        deliver(cb, ApiError(), info);
    });
}

void ControlPlaneAdapter::health(Callback<QJsonObject> cb)
{
    Request r;
    r.path = QStringLiteral("/v1/health");
    r.auth = false;
    r.retrySafe = true;
    send(r, [cb](const ApiError &e, const QJsonObject &o, int) { deliver(cb, e, o); });
}

// ------------------------------------------------------------------ tasks

void ControlPlaneAdapter::listTasks(const TaskFilter &filter, Callback<QList<Task>> cb)
{
    ApiError err;
    if (!requireWorkspace(&err)) {
        postError(cb, err);
        return;
    }
    Request r;
    r.path = QStringLiteral("/v1/workspaces/%1/tasks").arg(seg(m_workspaceId));
    r.retrySafe = true;
    if (!filter.state.isEmpty()) r.query.addQueryItem(QStringLiteral("state"), filter.state);
    if (!filter.nodeId.isEmpty()) r.query.addQueryItem(QStringLiteral("node_id"), filter.nodeId);
    r.query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, filter.limit, 200)));
    send(r, [cb](const ApiError &e, const QJsonObject &o, int) {
        QList<Task> tasks;
        for (const QJsonValue &v : o.value(QLatin1String("tasks")).toArray()) tasks << mapping::taskFromRemote(v.toObject());
        deliver(cb, e, tasks);
    });
}

void ControlPlaneAdapter::getTask(const QString &taskId, Callback<Task> cb)
{
    Request r;
    r.path = QStringLiteral("/v1/tasks/%1").arg(seg(taskId));
    r.retrySafe = true;
    send(r, [cb](const ApiError &e, const QJsonObject &o, int) {
        deliver(cb, e, mapping::taskFromRemote(unwrap(o, "task")));
    });
}

void ControlPlaneAdapter::events(const QString &taskId, qint64 afterEventId, int waitMs, Callback<EventPage> cb)
{
    Request r;
    r.path = QStringLiteral("/v1/tasks/%1/events").arg(seg(taskId));
    r.retrySafe = true;
    const int wait = qBound(0, waitMs, 30000);
    r.query.addQueryItem(QStringLiteral("after_event_id"), QString::number(qMax<qint64>(0, afterEventId)));
    r.query.addQueryItem(QStringLiteral("limit"), QStringLiteral("500"));
    r.query.addQueryItem(QStringLiteral("wait_ms"), QString::number(wait));
    r.timeoutMs = m_profile.requestTimeoutMs + wait;
    send(r, [cb, afterEventId](const ApiError &e, const QJsonObject &o, int) {
        EventPage page;
        page.lastEventId = afterEventId;
        for (const QJsonValue &v : o.value(QLatin1String("events")).toArray()) {
            TaskEvent event = mapping::eventFromRemote(v.toObject());
            if (event.id <= afterEventId) continue; // cursor is strictly increasing
            page.lastEventId = qMax(page.lastEventId, event.id);
            page.events << event;
        }
        const qint64 serverLast = static_cast<qint64>(o.value(QLatin1String("last_event_id")).toDouble(0));
        if (page.events.isEmpty() && serverLast > page.lastEventId) page.lastEventId = serverLast;
        deliver(cb, e, page);
    });
}

void ControlPlaneAdapter::submit(const SubmitRequest &s, Callback<Task> cb)
{
    ApiError err;
    if (!requireWorkspace(&err)) {
        postError(cb, err);
        return;
    }
    if (s.nodeId.isEmpty()) {
        postError(cb, ApiError(ApiError::InvalidRequest, QStringLiteral("an execution node must be selected")));
        return;
    }
    if (s.idempotencyKey.isEmpty()) {
        postError(cb, ApiError(ApiError::InvalidRequest, QStringLiteral("idempotency key is required")));
        return;
    }
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/workspaces/%1/tasks").arg(seg(m_workspaceId));
    r.hasBody = true;
    r.retrySafe = true;
    r.body = {{QStringLiteral("node_id"), s.nodeId},
              {QStringLiteral("task"), s.text},
              {QStringLiteral("priority"), qBound(0, s.priority, 9)},
              {QStringLiteral("idempotency_key"), s.idempotencyKey}};
    if (!s.agent.isEmpty()) r.body.insert(QStringLiteral("agent"), s.agent);
    if (!s.sessionId.isEmpty()) r.body.insert(QStringLiteral("session_id"), s.sessionId);
    send(r, [cb, s](const ApiError &e, const QJsonObject &o, int) {
        Task t = mapping::taskFromRemote(unwrap(o, "task"));
        if (t.text.isEmpty()) t.text = s.text;
        if (!e.isError() && t.id.isEmpty()) {
            deliver(cb, ApiError(ApiError::Protocol, QStringLiteral("submit reply without task_id")), t);
            return;
        }
        deliver(cb, e, t);
    });
}

void ControlPlaneAdapter::command(const QString &taskId, const QString &verb, const QJsonObject &body,
                                  Callback<CommandResult> cb)
{
    if (body.value(QLatin1String("idempotency_key")).toString().isEmpty()) {
        postError(cb, ApiError(ApiError::InvalidRequest, QStringLiteral("idempotency key is required")));
        return;
    }
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/tasks/%1/%2").arg(seg(taskId), verb);
    r.hasBody = true;
    r.retrySafe = true;
    r.body = body;
    send(r, [cb, taskId](const ApiError &e, const QJsonObject &o, int) {
        CommandResult c = mapping::commandFromRemote(o);
        if (c.taskId.isEmpty()) c.taskId = taskId;
        deliver(cb, e, c);
    });
}

void ControlPlaneAdapter::answer(const QString &taskId, const QString &answer, const QString &optionId,
                                 const QString &idempotencyKey, Callback<CommandResult> cb)
{
    Q_UNUSED(optionId) // the v1 contract answer payload carries the text only
    command(taskId, QStringLiteral("answer"),
            {{QStringLiteral("answer"), answer}, {QStringLiteral("idempotency_key"), idempotencyKey}}, cb);
}

void ControlPlaneAdapter::cancel(const QString &taskId, const QString &idempotencyKey, Callback<CommandResult> cb)
{
    command(taskId, QStringLiteral("cancel"), {{QStringLiteral("idempotency_key"), idempotencyKey}}, cb);
}

void ControlPlaneAdapter::retry(const QString &taskId, const QString &idempotencyKey, Callback<CommandResult> cb)
{
    command(taskId, QStringLiteral("retry"), {{QStringLiteral("idempotency_key"), idempotencyKey}}, cb);
}

void ControlPlaneAdapter::deadLetters(int limit, Callback<QList<Task>> cb)
{
    // Remote dead letters = failed + delivery_failed tasks.
    auto collected = std::make_shared<QList<Task>>();
    auto remaining = std::make_shared<int>(2);
    auto firstError = std::make_shared<ApiError>();
    auto part = [cb, collected, remaining, firstError, limit](const Result<QList<Task>> &r) {
        if (!r.ok() && !firstError->isError()) *firstError = r.error;
        if (r.ok()) *collected << r.value;
        if (--*remaining > 0) return;
        std::sort(collected->begin(), collected->end(),
                  [](const Task &a, const Task &b) { return a.updatedAt > b.updatedAt; });
        while (collected->size() > limit) collected->removeLast();
        deliver(cb, firstError->isError() ? *firstError : ApiError(), *collected);
    };
    for (const QString &state : {QStringLiteral("failed"), QStringLiteral("delivery_failed")}) {
        TaskFilter f;
        f.state = state;
        f.limit = limit;
        listTasks(f, part);
    }
}

void ControlPlaneAdapter::listAgents(Callback<QList<Agent>> cb)
{
    postError(cb, ApiError(ApiError::Unsupported, QStringLiteral("the v1 Control Plane API has no agent listing")));
}

void ControlPlaneAdapter::listModels(Callback<QList<ModelInfo>> cb)
{
    postError(cb, ApiError(ApiError::Unsupported, QStringLiteral("the v1 Control Plane API has no model listing")));
}

void ControlPlaneAdapter::manageModel(const ModelInfo &, ModelAction, Callback<CommandResult> cb)
{
    postError(cb, ApiError(ApiError::Unsupported, QStringLiteral("model management is local-only")));
}

void ControlPlaneAdapter::listNodes(Callback<QList<Node>> cb)
{
    ApiError err;
    if (!requireWorkspace(&err)) {
        postError(cb, err);
        return;
    }
    Request r;
    r.path = QStringLiteral("/v1/workspaces/%1/nodes").arg(seg(m_workspaceId));
    r.retrySafe = true;
    send(r, [cb](const ApiError &e, const QJsonObject &o, int) {
        QList<Node> nodes;
        for (const QJsonValue &v : o.value(QLatin1String("nodes")).toArray()) nodes << mapping::nodeFromRemote(v.toObject());
        deliver(cb, e, nodes);
    });
}

void ControlPlaneAdapter::revokeNode(const QString &nodeId, Callback<CommandResult> cb)
{
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/nodes/%1/revoke").arg(seg(nodeId));
    r.hasBody = true;
    send(r, [cb, nodeId](const ApiError &e, const QJsonObject &, int) {
        deliver(cb, e, CommandResult{QString(), nodeId, QStringLiteral("revoked")});
    });
}

// -------------------------------------------------------------- artifacts

void ControlPlaneAdapter::listArtifacts(const QString &taskId, Callback<QList<Artifact>> cb)
{
    Request r;
    r.path = QStringLiteral("/v1/tasks/%1/artifacts").arg(seg(taskId));
    r.retrySafe = true;
    send(r, [cb](const ApiError &e, const QJsonObject &o, int) {
        QList<Artifact> list;
        for (const QJsonValue &v : o.value(QLatin1String("artifacts")).toArray()) list << mapping::artifactFromRemote(v.toObject());
        deliver(cb, e, list);
    });
}

QUrl ControlPlaneAdapter::resolveDownloadUrl(const QString &url, const QString &ticket) const
{
    const QUrl base(m_profile.baseUrl.trimmed());
    if (url.isEmpty()) return m_profile.url(QStringLiteral("/v1/downloads/%1").arg(seg(ticket)));
    const QUrl candidate(url);
    if (candidate.isRelative()) {
        if (!url.startsWith(QLatin1Char('/'))) return QUrl();
        return m_profile.url(url);
    }
    const bool sameOrigin = candidate.scheme().compare(base.scheme(), Qt::CaseInsensitive) == 0 &&
                            candidate.host().compare(base.host(), Qt::CaseInsensitive) == 0 &&
                            candidate.port(base.scheme() == QLatin1String("https") ? 443 : 80) ==
                                base.port(base.scheme() == QLatin1String("https") ? 443 : 80);
    return sameOrigin ? candidate : QUrl();
}

void ControlPlaneAdapter::downloadArtifact(const Artifact &artifact, const QString &destinationPath, qint64 maxBytes,
                                           Callback<DownloadResult> cb)
{
    if (!artifact.state.isEmpty() && artifact.state != QLatin1String("available")) {
        postError(cb, ApiError(ApiError::Conflict, QStringLiteral("artifact is %1, not available").arg(artifact.state)));
        return;
    }
    if (maxBytes > 0 && artifact.size > maxBytes) {
        postError(cb, ApiError(ApiError::SizeLimit, QStringLiteral("%1 bytes, limit is %2 bytes").arg(artifact.size).arg(maxBytes)));
        return;
    }
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/artifacts/%1/download-ticket").arg(seg(artifact.id));
    r.hasBody = true;
    r.retrySafe = true; // a ticket request has no side effect beyond a new short-lived ticket
    send(r, [this, artifact, destinationPath, maxBytes, cb](const ApiError &e, const QJsonObject &o, int) {
        if (e.isError()) {
            deliver(cb, e, DownloadResult());
            return;
        }
        const QString ticket = o.value(QLatin1String("ticket")).toString();
        if (!ticket.startsWith(QLatin1String("dlt_"))) {
            deliver(cb, ApiError(ApiError::Protocol, QStringLiteral("download ticket reply without a dlt_ ticket")), DownloadResult());
            return;
        }
        Redactor::addSecret(ticket);
        const QUrl url = resolveDownloadUrl(o.value(QLatin1String("url")).toString(), ticket);
        if (!url.isValid() || url.isEmpty()) {
            deliver(cb, ApiError(ApiError::Protocol, QStringLiteral("download URL is not on the Control Plane origin")), DownloadResult());
            return;
        }
        startDownload(artifact, url, ticket, destinationPath, maxBytes, cb);
    });
}

void ControlPlaneAdapter::startDownload(const Artifact &artifact, const QUrl &url, const QString &ticket,
                                        const QString &destinationPath, qint64 maxBytes, Callback<DownloadResult> cb)
{
    DownloadSink::Options options;
    options.destinationPath = destinationPath;
    options.maxBytes = maxBytes;
    options.expectedSize = artifact.size;
    options.expectedSha256 = artifact.sha256;
    options.requireChecksum = true;
    auto sink = std::make_shared<DownloadSink>(options);
    ApiError openError;
    if (!sink->open(&openError)) {
        postError(cb, openError);
        return;
    }
    // The ticket (not the user token) authorises the download. It is in the
    // path and repeated as bearer so either server convention works; the
    // user token is never sent to the download endpoint.
    QNetworkRequest request = buildRequest(url, ticket, false, qMax(m_profile.requestTimeoutMs, 60000));
    request.setRawHeader("Accept", "*/*");
    QNetworkReply *reply = m_nam->get(request);
    reply->setReadBufferSize(1024 * 1024);
    track(reply);
    qCInfo(lcNet) << "downloading artifact" << artifact.id << "to" << destinationPath;

    auto failure = std::make_shared<ApiError>();
    auto errorBody = std::make_shared<QByteArray>();
    auto headerChecked = std::make_shared<bool>(false);
    const QString artifactId = artifact.id;

    auto pump = [this, reply, sink, failure, errorBody, headerChecked, artifactId]() {
        if (failure->isError()) {
            reply->readAll();
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 0) return; // headers not parsed yet
        if (status != 200 && status != 206) {
            const QByteArray chunk = reply->readAll();
            if (errorBody->size() < kMaxErrorBody) errorBody->append(chunk.left(kMaxErrorBody - errorBody->size()));
            return;
        }
        if (!*headerChecked) {
            *headerChecked = true;
            const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
            ApiError e;
            if (length.isValid() && !sink->checkAnnouncedSize(length.toLongLong(), &e)) {
                *failure = e;
                sink->abort();
                reply->setProperty("asgend_local_abort", true);
                reply->abort();
                return;
            }
        }
        const QByteArray chunk = reply->readAll();
        ApiError e;
        if (!sink->write(chunk, &e)) {
            *failure = e;
            reply->setProperty("asgend_local_abort", true);
            reply->abort();
            return;
        }
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        Q_EMIT downloadProgress(artifactId, sink->bytesWritten(), length.isValid() ? length.toLongLong() : -1);
    };
    connect(reply, &QIODevice::readyRead, this, pump);
    connect(reply, &QNetworkReply::finished, this, [this, reply, sink, failure, errorBody, pump, cb]() {
        if (!failure->isError() && reply->error() == QNetworkReply::NoError) pump();
        Result<DownloadResult> result;
        if (failure->isError()) {
            result = Result<DownloadResult>::failure(*failure);
        } else {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const ApiError e = replyError(reply, *errorBody);
            if (e.isError() || (status != 200 && status != 206)) {
                sink->abort();
                result = Result<DownloadResult>::failure(e.isError() ? e : ApiError(ApiError::Protocol, QStringLiteral("HTTP %1").arg(status)));
            } else {
                result = sink->finish(QString::fromLatin1(reply->rawHeader("X-Checksum-SHA256")).trimmed());
            }
        }
        if (!result.ok()) sink->abort();
        untrack(reply);
        QTimer::singleShot(0, this, [cb, result]() { if (cb) cb(result); });
    });
}

} // namespace asgend
