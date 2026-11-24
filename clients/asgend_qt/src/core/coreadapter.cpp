// SPDX-License-Identifier: MIT
#include "core/coreadapter.h"

#include "core/downloadsink.h"
#include "core/logging.h"
#include "core/redact.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTcpSocket>
#include <QTimer>

namespace asgend {

namespace {

// One request/response exchange over a fresh connection.
class CoreCall final : public QObject {
public:
    CoreCall(const ConnectionProfile &profile, QByteArray line, int timeoutMs, CoreAdapter::RawDone done,
             QObject *parent)
        : QObject(parent), m_profile(profile), m_line(std::move(line)), m_done(std::move(done))
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(timeoutMs);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            finish(ApiError(ApiError::Timeout, QStringLiteral("no reply from the core")), {});
        });
    }

    ~CoreCall() override
    {
        if (m_device) {
            m_device->disconnect(this);
            delete m_device;
            m_device = nullptr;
        }
    }

    void start()
    {
        m_timer.start();
        if (m_profile.type == ConnectionProfile::Type::LocalSocket) {
            auto *socket = new QLocalSocket(this);
            m_device = socket;
            connect(socket, &QLocalSocket::connected, this, [this]() { onConnected(); });
            connect(socket, &QIODevice::readyRead, this, [this]() { onReadyRead(); });
            connect(socket, &QLocalSocket::disconnected, this, [this]() { onClosed(); });
            connect(socket, &QLocalSocket::errorOccurred, this, [this, socket](QLocalSocket::LocalSocketError err) {
                if (err == QLocalSocket::PeerClosedError) {
                    onClosed();
                    return;
                }
                finish(ApiError(ApiError::Network, socket->errorString()), {});
            });
            socket->connectToServer(m_profile.socketPath);
        } else {
            auto *socket = new QTcpSocket(this);
            m_device = socket;
            connect(socket, &QTcpSocket::connected, this, [this]() { onConnected(); });
            connect(socket, &QIODevice::readyRead, this, [this]() { onReadyRead(); });
            connect(socket, &QTcpSocket::disconnected, this, [this]() { onClosed(); });
            connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError err) {
                if (err == QAbstractSocket::RemoteHostClosedError) {
                    onClosed();
                    return;
                }
                finish(ApiError(ApiError::Network, socket->errorString()), {});
            });
            socket->connectToHost(m_profile.host, static_cast<quint16>(m_profile.port));
        }
    }

    void abort() { finish(ApiError(ApiError::Cancelled, QString()), {}); }

private:
    void onConnected() { m_device->write(m_line); }

    void onReadyRead()
    {
        m_buffer += m_device->readAll();
        if (m_buffer.size() > CoreAdapter::MaxReplyBytes) {
            finish(ApiError(ApiError::Protocol, QStringLiteral("core reply exceeds %1 bytes").arg(CoreAdapter::MaxReplyBytes)), {});
            return;
        }
        const int newline = m_buffer.indexOf('\n');
        if (newline >= 0) parse(m_buffer.left(newline));
    }

    void onClosed()
    {
        if (m_finished) return;
        if (m_device) m_buffer += m_device->readAll();
        if (m_buffer.trimmed().isEmpty()) {
            finish(ApiError(ApiError::Network, QStringLiteral("connection closed without a reply")), {});
            return;
        }
        parse(m_buffer);
    }

    void parse(const QByteArray &line)
    {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            finish(ApiError(ApiError::Protocol, QStringLiteral("invalid JSON reply: %1").arg(error.errorString())), {});
            return;
        }
        const QJsonObject obj = doc.object();
        if (!obj.value(QLatin1String("ok")).toBool(false)) {
            const QString message = Redactor::redact(obj.value(QLatin1String("error")).toString(QStringLiteral("request failed")));
            ApiError e(ApiError::fromCoreMessage(message), message);
            if (e.code == ApiError::Unavailable || obj.value(QLatin1String("retryable")).toBool(false))
                e.code = ApiError::Unavailable;
            finish(e, obj);
            return;
        }
        finish(ApiError(), obj);
    }

    void finish(const ApiError &error, const QJsonObject &obj)
    {
        if (m_finished) return;
        m_finished = true;
        m_timer.stop();
        if (m_device) {
            m_device->disconnect(this);
            if (auto *tcp = qobject_cast<QTcpSocket *>(m_device)) tcp->abort();
            else if (auto *local = qobject_cast<QLocalSocket *>(m_device)) local->abort();
        }
        CoreAdapter::RawDone done = std::move(m_done);
        // Deliver asynchronously with the adapter as context so a callback may
        // safely destroy the adapter (and this call) without re-entrancy.
        QObject *context = parent();
        QTimer::singleShot(0, context, [done, error, obj]() {
            if (done) done(error, obj);
        });
        deleteLater();
    }

    ConnectionProfile m_profile;
    QByteArray m_line;
    QByteArray m_buffer;
    CoreAdapter::RawDone m_done;
    QIODevice *m_device = nullptr;
    QTimer m_timer;
    bool m_finished = false;

};

template <typename T>
void deliver(const Callback<T> &cb, const ApiError &error, T value)
{
    if (!cb) return;
    if (error.isError()) cb(Result<T>::failure(error));
    else cb(Result<T>::success(std::move(value)));
}

} // namespace

struct CoreDownload {
    Artifact artifact;
    qint64 offset = 0;
    std::unique_ptr<DownloadSink> sink;
    Callback<DownloadResult> cb;
    bool sizeChecked = false;
};

CoreAdapter::CoreAdapter(ConnectionProfile profile, QString authToken, QObject *parent)
    : ProtocolAdapter(parent), m_profile(std::move(profile)), m_token(std::move(authToken))
{
    if (!m_token.isEmpty()) Redactor::addSecret(m_token);
}

CoreAdapter::~CoreAdapter()
{
    // Children (pending CoreCall objects) are deleted by QObject; their
    // queued callbacks use this adapter as context and are dropped with it.
}

Capabilities CoreAdapter::capabilities() const
{
    Capabilities c;
    c.submit = c.answer = c.cancel = c.retry = true;
    c.deadLetters = c.agents = c.models = c.manageModels = c.nodes = c.artifacts = true;
    c.manageNodes = false;
    c.longPoll = false;
    c.nodeRequiredForSubmit = false;
    c.role = QStringLiteral("local");
    return c;
}

QString CoreAdapter::endpointDescription() const
{
    if (m_profile.type == ConnectionProfile::Type::LocalSocket) return QStringLiteral("unix:%1").arg(m_profile.socketPath);
    return QStringLiteral("tcp://%1:%2").arg(m_profile.host).arg(m_profile.port);
}

void CoreAdapter::call(QJsonObject request, RawDone done, int timeoutMs)
{
    const QString op = request.value(QLatin1String("op")).toString();
    if (!m_token.isEmpty()) request.insert(QStringLiteral("auth_token"), m_token);
    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');
    qCDebug(lcNet) << "core request op" << op << "via" << endpointDescription();
    auto *c = new CoreCall(m_profile, line, timeoutMs > 0 ? timeoutMs : m_profile.requestTimeoutMs,
                           [op, done](const ApiError &error, const QJsonObject &obj) {
                               if (error.isError())
                                   qCInfo(lcNet) << "core op" << op << "failed:" << ApiError::codeName(error.code)
                                                 << error.message;
                               if (done) done(error, obj);
                           },
                           this);
    c->start();
}

void CoreAdapter::abortAll()
{
    const auto calls = findChildren<QObject *>(QString(), Qt::FindDirectChildrenOnly);
    for (QObject *child : calls) {
        if (auto *c = dynamic_cast<CoreCall *>(child)) c->abort();
    }
}

void CoreAdapter::ping(Callback<ServerInfo> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("ping")}}, [cb](const ApiError &e, const QJsonObject &o) {
        ServerInfo info;
        info.service = o.value(QLatin1String("service")).toString();
        info.version = o.value(QLatin1String("version")).toString();
        info.role = QStringLiteral("local");
        deliver(cb, e, info);
    });
}

void CoreAdapter::health(Callback<QJsonObject> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("health")}}, [cb](const ApiError &e, const QJsonObject &o) {
        QJsonObject copy = o;
        copy.remove(QStringLiteral("model_status")); // may describe endpoints; not needed for health
        deliver(cb, e, copy);
    });
}

void CoreAdapter::listTasks(const TaskFilter &filter, Callback<QList<Task>> cb)
{
    QJsonObject request{{QStringLiteral("op"), QStringLiteral("list_tasks")},
                        {QStringLiteral("limit"), qBound(1, filter.limit, 200)}};
    // The core knows only its own states; remote-only states yield no rows.
    if (!filter.state.isEmpty()) request.insert(QStringLiteral("status"), filter.state);
    call(request, [cb](const ApiError &e, const QJsonObject &o) {
        QList<Task> tasks;
        for (const QJsonValue &v : o.value(QLatin1String("tasks")).toArray()) tasks << mapping::taskFromCore(v.toObject());
        deliver(cb, e, tasks);
    });
}

void CoreAdapter::getTask(const QString &taskId, Callback<Task> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("status")}, {QStringLiteral("task_id"), taskId}},
         [cb](const ApiError &e, const QJsonObject &o) { deliver(cb, e, mapping::taskFromCore(o)); });
}

void CoreAdapter::events(const QString &taskId, qint64 afterEventId, int waitMs, Callback<EventPage> cb)
{
    Q_UNUSED(waitMs) // the core has no long poll; TaskEventStream polls instead
    QJsonObject request{{QStringLiteral("op"), QStringLiteral("events")},
                        {QStringLiteral("task_id"), taskId},
                        {QStringLiteral("after_id"), static_cast<double>(afterEventId)},
                        {QStringLiteral("limit"), 200}};
    call(request, [cb, taskId, afterEventId](const ApiError &e, const QJsonObject &o) {
        EventPage page;
        page.lastEventId = afterEventId;
        for (const QJsonValue &v : o.value(QLatin1String("events")).toArray()) {
            TaskEvent event = mapping::eventFromCore(v.toObject(), taskId);
            if (event.id <= afterEventId) continue; // defensive: never go backwards
            page.lastEventId = qMax(page.lastEventId, event.id);
            page.events << event;
        }
        deliver(cb, e, page);
    });
}

void CoreAdapter::submit(const SubmitRequest &r, Callback<Task> cb)
{
    QJsonObject request{{QStringLiteral("op"), QStringLiteral("submit")},
                        {QStringLiteral("task"), r.text},
                        {QStringLiteral("priority"), qBound(0, r.priority, 9)}};
    if (!r.agent.isEmpty()) request.insert(QStringLiteral("agent"), r.agent);
    if (!r.sessionId.isEmpty()) request.insert(QStringLiteral("session_id"), r.sessionId);
    if (!r.idempotencyKey.isEmpty()) request.insert(QStringLiteral("idempotency_key"), r.idempotencyKey);
    call(request, [cb, r](const ApiError &e, const QJsonObject &o) {
        Task t;
        t.id = o.value(QLatin1String("task_id")).toString();
        t.coreTaskId = t.id;
        t.text = r.text;
        t.title = r.text.left(200);
        t.agent = r.agent;
        t.sessionId = r.sessionId;
        t.priority = o.value(QLatin1String("priority")).toInt(r.priority);
        t.idempotencyKey = r.idempotencyKey;
        t.rawState = QStringLiteral("queued");
        t.state = TaskState::Queued;
        if (!e.isError() && t.id.isEmpty()) {
            deliver(cb, ApiError(ApiError::Protocol, QStringLiteral("submit reply without task_id")), t);
            return;
        }
        deliver(cb, e, t);
    });
}

void CoreAdapter::answer(const QString &taskId, const QString &answer, const QString &optionId,
                         const QString &idempotencyKey, Callback<CommandResult> cb)
{
    Q_UNUSED(idempotencyKey) // the core answer op is naturally state-checked
    QJsonObject request{{QStringLiteral("op"), QStringLiteral("answer")},
                        {QStringLiteral("task_id"), taskId},
                        {QStringLiteral("answer"), answer}};
    if (!optionId.isEmpty()) request.insert(QStringLiteral("option_id"), optionId);
    call(request, [cb, taskId](const ApiError &e, const QJsonObject &o) {
        deliver(cb, e, CommandResult{QString(), taskId, o.value(QLatin1String("status")).toString()});
    });
}

void CoreAdapter::cancel(const QString &taskId, const QString &idempotencyKey, Callback<CommandResult> cb)
{
    Q_UNUSED(idempotencyKey)
    call({{QStringLiteral("op"), QStringLiteral("cancel")}, {QStringLiteral("task_id"), taskId}},
         [cb, taskId](const ApiError &e, const QJsonObject &o) {
             deliver(cb, e, CommandResult{QString(), taskId, o.value(QLatin1String("status")).toString()});
         });
}

void CoreAdapter::retry(const QString &taskId, const QString &idempotencyKey, Callback<CommandResult> cb)
{
    Q_UNUSED(idempotencyKey)
    call({{QStringLiteral("op"), QStringLiteral("retry_task")}, {QStringLiteral("task_id"), taskId}},
         [cb, taskId](const ApiError &e, const QJsonObject &o) {
             deliver(cb, e, CommandResult{QString(), taskId, o.value(QLatin1String("status")).toString()});
         });
}

void CoreAdapter::deadLetters(int limit, Callback<QList<Task>> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("dead_letters")}, {QStringLiteral("limit"), qBound(1, limit, 200)}},
         [cb](const ApiError &e, const QJsonObject &o) {
             QList<Task> tasks;
             for (const QJsonValue &v : o.value(QLatin1String("dead_letters")).toArray())
                 tasks << mapping::taskFromCore(v.toObject());
             deliver(cb, e, tasks);
         });
}

void CoreAdapter::listAgents(Callback<QList<Agent>> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("list_agents")}}, [cb](const ApiError &e, const QJsonObject &o) {
        QList<Agent> agents;
        for (const QJsonValue &v : o.value(QLatin1String("agents")).toArray()) agents << mapping::agentFromCore(v.toObject());
        deliver(cb, e, agents);
    });
}

void CoreAdapter::listModels(Callback<QList<ModelInfo>> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("models")}}, [cb](const ApiError &e, const QJsonObject &o) {
        QList<ModelInfo> models;
        const QJsonObject status = o.value(QLatin1String("model_status")).toObject();
        for (const char *key : {"models", "decision_models"}) {
            for (const QJsonValue &v : status.value(QLatin1String(key)).toArray()) models << mapping::modelFromCore(v.toObject());
        }
        deliver(cb, e, models);
    });
}

void CoreAdapter::manageModel(const ModelInfo &model, ModelAction action, Callback<CommandResult> cb)
{
    QString op;
    switch (action) {
    case ModelAction::Start: op = QStringLiteral("model_start"); break;
    case ModelAction::Stop: op = QStringLiteral("model_stop"); break;
    case ModelAction::Enable: op = QStringLiteral("model_enable"); break;
    case ModelAction::Disable: op = QStringLiteral("model_disable"); break;
    }
    QJsonObject request{{QStringLiteral("op"), op},
                        {QStringLiteral("model_id"), model.id},
                        {QStringLiteral("kind"), model.kind == QLatin1String("decision") ? QStringLiteral("decision")
                                                                                         : QStringLiteral("llm")}};
    // Starting a model may take a while (process launch + readiness).
    call(request, [cb, model](const ApiError &e, const QJsonObject &) {
        deliver(cb, e, CommandResult{QString(), model.id, QStringLiteral("ok")});
    }, qMax(m_profile.requestTimeoutMs, 120000));
}

void CoreAdapter::listNodes(Callback<QList<Node>> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("models")}}, [cb](const ApiError &e, const QJsonObject &o) {
        QList<Node> nodes;
        const QJsonObject status = o.value(QLatin1String("model_status")).toObject();
        for (const QJsonValue &v : status.value(QLatin1String("nodes")).toArray()) nodes << mapping::nodeFromCore(v.toObject());
        deliver(cb, e, nodes);
    });
}

void CoreAdapter::revokeNode(const QString &nodeId, Callback<CommandResult> cb)
{
    Q_UNUSED(nodeId)
    postError(cb, ApiError(ApiError::Unsupported, QStringLiteral("node revocation is a Control Plane operation")));
}

void CoreAdapter::listArtifacts(const QString &taskId, Callback<QList<Artifact>> cb)
{
    call({{QStringLiteral("op"), QStringLiteral("artifact_list")}, {QStringLiteral("task_id"), taskId}},
         [this, cb, taskId](const ApiError &e, const QJsonObject &o) {
             if (e.code == ApiError::Unsupported) {
                 // Older cores (< 0.9.9): list top-level workspace files without checksums.
                 call({{QStringLiteral("op"), QStringLiteral("workspace_list")},
                       {QStringLiteral("task_id"), taskId},
                       {QStringLiteral("path"), QString()}},
                      [cb, taskId](const ApiError &e2, const QJsonObject &o2) {
                          QList<Artifact> list;
                          for (const QJsonValue &v : o2.value(QLatin1String("entries")).toArray()) {
                              const QJsonObject entry = v.toObject();
                              if (entry.value(QLatin1String("kind")).toString() != QLatin1String("file")) continue;
                              list << mapping::artifactFromCore(entry, taskId);
                          }
                          deliver(cb, e2, list);
                      });
                 return;
             }
             QList<Artifact> list;
             for (const QJsonValue &v : o.value(QLatin1String("artifacts")).toArray())
                 list << mapping::artifactFromCore(v.toObject(), taskId);
             deliver(cb, e, list);
         });
}

void CoreAdapter::downloadArtifact(const Artifact &artifact, const QString &destinationPath, qint64 maxBytes,
                                   Callback<DownloadResult> cb)
{
    auto state = std::make_shared<CoreDownload>();
    state->artifact = artifact;
    state->cb = cb;
    DownloadSink::Options options;
    options.destinationPath = destinationPath;
    options.maxBytes = maxBytes;
    options.expectedSize = artifact.size;
    options.expectedSha256 = artifact.sha256;
    // artifact_list always reports sha256; only the legacy workspace_list
    // fallback lacks it, and the result is then flagged as unverified.
    options.requireChecksum = !artifact.sha256.isEmpty();
    state->sink = std::make_unique<DownloadSink>(options);
    ApiError error;
    if (!state->sink->open(&error)) {
        postError(cb, error);
        return;
    }
    downloadChunk(state);
}

void CoreAdapter::downloadChunk(const std::shared_ptr<CoreDownload> &state)
{
    QJsonObject request{{QStringLiteral("op"), QStringLiteral("workspace_read_bytes")},
                        {QStringLiteral("task_id"), state->artifact.taskId},
                        {QStringLiteral("path"), state->artifact.id},
                        {QStringLiteral("offset"), static_cast<double>(state->offset)},
                        {QStringLiteral("limit"), ReadChunkBytes}};
    call(request, [this, state](const ApiError &e, const QJsonObject &o) {
        auto fail = [state](ApiError err) {
            state->sink->abort();
            if (state->cb) state->cb(Result<DownloadResult>::failure(err));
        };
        if (e.isError()) {
            ApiError err = e;
            if (err.code == ApiError::Unsupported)
                err.message = QStringLiteral("the core does not support workspace_read_bytes (requires ASGEND 0.9.9+)");
            fail(err);
            return;
        }
        const qint64 total = static_cast<qint64>(o.value(QLatin1String("size")).toDouble(-1));
        if (!state->sizeChecked) {
            ApiError err;
            if (!state->sink->checkAnnouncedSize(total, &err)) {
                fail(err);
                return;
            }
            state->sizeChecked = true;
        }
        const qint64 replyOffset = static_cast<qint64>(o.value(QLatin1String("offset")).toDouble(-1));
        if (replyOffset != state->offset) {
            fail(ApiError(ApiError::Protocol, QStringLiteral("unexpected chunk offset")));
            return;
        }
        const auto decoded = QByteArray::fromBase64Encoding(o.value(QLatin1String("data_b64")).toString().toLatin1(),
                                                            QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            fail(ApiError(ApiError::Protocol, QStringLiteral("invalid base64 chunk")));
            return;
        }
        ApiError err;
        if (!state->sink->write(decoded.decoded, &err)) {
            fail(err);
            return;
        }
        state->offset += decoded.decoded.size();
        Q_EMIT downloadProgress(state->artifact.id, state->offset, total);
        const bool eof = o.value(QLatin1String("eof")).toBool(false);
        const qint64 next = static_cast<qint64>(o.value(QLatin1String("next_offset")).toDouble(-1));
        if (eof) {
            const Result<DownloadResult> result = state->sink->finish();
            if (state->cb) state->cb(result);
            return;
        }
        if (next != state->offset || decoded.decoded.isEmpty()) {
            fail(ApiError(ApiError::Protocol, QStringLiteral("download made no progress")));
            return;
        }
        downloadChunk(state);
    });
}

} // namespace asgend
