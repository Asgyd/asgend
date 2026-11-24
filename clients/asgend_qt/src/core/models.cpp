// SPDX-License-Identifier: MIT
#include "core/models.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>

namespace asgend {

namespace {

struct StateName {
    TaskState state;
    const char *wire;
};

const StateName kStates[] = {
    {TaskState::PendingDelivery, "pending_delivery"},
    {TaskState::Delivered, "delivered"},
    {TaskState::Queued, "queued"},
    {TaskState::Running, "running"},
    {TaskState::WaitingInput, "waiting_input"},
    {TaskState::Completed, "completed"},
    {TaskState::CompletedWithoutModel, "completed_without_model"},
    {TaskState::Failed, "failed"},
    {TaskState::Cancelled, "cancelled"},
    {TaskState::DeliveryFailed, "delivery_failed"},
};

QString str(const QJsonObject &o, const char *key, const QString &fallback = QString())
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 17);
    return fallback;
}

QString firstString(const QJsonObject &o, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isString() && !v.toString().isEmpty()) return v.toString();
    }
    return QString();
}

int integer(const QJsonObject &o, const char *key, int fallback = 0)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toInt(fallback) : fallback;
}

qint64 integer64(const QJsonObject &o, const char *key, qint64 fallback = 0)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) return static_cast<qint64>(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const qint64 parsed = v.toString().toLongLong(&ok);
        if (ok) return parsed;
    }
    return fallback;
}

bool boolean(const QJsonObject &o, const char *key, bool fallback = false)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : fallback;
}

QString truncated(QString s, int max = 4000)
{
    if (s.size() > max) {
        s.truncate(max);
        s += QChar(0x2026);
    }
    return s;
}

QList<QuestionOption> parseOptions(const QJsonValue &value)
{
    QList<QuestionOption> out;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &entry : array) {
        QuestionOption option;
        if (entry.isString()) {
            option.id = entry.toString();
            option.label = entry.toString();
        } else if (entry.isObject()) {
            const QJsonObject o = entry.toObject();
            option.id = firstString(o, {"id", "value", "label"});
            option.label = firstString(o, {"label", "text", "title", "id"});
        } else {
            continue;
        }
        if (!option.label.isEmpty()) out.append(option);
    }
    return out;
}

QuestionItem itemFromObject(const QJsonObject &o)
{
    QuestionItem item;
    item.text = firstString(o, {"question", "text", "prompt", "message"});
    item.options = parseOptions(o.value(QLatin1String("options")));
    return item;
}

} // namespace

QString taskStateToString(TaskState state)
{
    for (const auto &entry : kStates) {
        if (entry.state == state) return QLatin1String(entry.wire);
    }
    return QStringLiteral("unknown");
}

TaskState taskStateFromString(const QString &state)
{
    for (const auto &entry : kStates) {
        if (state == QLatin1String(entry.wire)) return entry.state;
    }
    return TaskState::Unknown;
}

QString taskStateDisplayName(TaskState state)
{
    switch (state) {
    case TaskState::PendingDelivery: return QCoreApplication::translate("TaskState", "Pending delivery");
    case TaskState::Delivered: return QCoreApplication::translate("TaskState", "Delivered");
    case TaskState::Queued: return QCoreApplication::translate("TaskState", "Queued");
    case TaskState::Running: return QCoreApplication::translate("TaskState", "Running");
    case TaskState::WaitingInput: return QCoreApplication::translate("TaskState", "Waiting for input");
    case TaskState::Completed: return QCoreApplication::translate("TaskState", "Completed");
    case TaskState::CompletedWithoutModel: return QCoreApplication::translate("TaskState", "Completed without model");
    case TaskState::Failed: return QCoreApplication::translate("TaskState", "Failed");
    case TaskState::Cancelled: return QCoreApplication::translate("TaskState", "Cancelled");
    case TaskState::DeliveryFailed: return QCoreApplication::translate("TaskState", "Delivery failed");
    case TaskState::Unknown: break;
    }
    return QCoreApplication::translate("TaskState", "Unknown");
}

QList<TaskState> allTaskStates()
{
    QList<TaskState> out;
    for (const auto &entry : kStates) out.append(entry.state);
    return out;
}

bool isTerminal(TaskState state)
{
    switch (state) {
    case TaskState::Completed:
    case TaskState::CompletedWithoutModel:
    case TaskState::Failed:
    case TaskState::Cancelled:
    case TaskState::DeliveryFailed:
        return true;
    default:
        return false;
    }
}

bool isRetryable(TaskState state)
{
    return state == TaskState::Failed || state == TaskState::DeliveryFailed;
}

bool isCancellable(TaskState state)
{
    return state != TaskState::Unknown && !isTerminal(state);
}

QString Question::summary() const
{
    QStringList parts;
    for (const QuestionItem &item : items) parts << item.text;
    return parts.join(QLatin1Char('\n'));
}

QString Question::signature() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QuestionItem &item : items) {
        hash.addData(item.text.toUtf8());
        hash.addData(QByteArrayView("\x1f", 1));
        for (const QuestionOption &option : item.options) {
            hash.addData(option.label.toUtf8());
            hash.addData(QByteArrayView("\x1e", 1));
        }
    }
    return QString::fromLatin1(hash.result().toHex().left(24));
}

QString Question::composeAnswer(const QList<QuestionItem> &items, const QStringList &answers)
{
    if (answers.size() == 1) return answers.first();
    QString combined;
    for (int i = 0; i < answers.size(); ++i) {
        const QString question = i < items.size() ? items.at(i).text : QString();
        if (!combined.isEmpty()) combined += QLatin1Char('\n');
        combined += QStringLiteral("%1. %2 — %3").arg(i + 1).arg(question, answers.at(i));
    }
    return combined;
}

bool TaskEvent::isQuestion() const
{
    return type == QLatin1String("question") || type == QLatin1String("agent_question") ||
           type == QLatin1String("orchestrator_waiting_input");
}

QString TaskEvent::displayText() const
{
    if (!message.isEmpty()) return message;
    if (type == QLatin1String("state_changed")) {
        const QString to = firstString(payload, {"state", "to", "new_state"});
        if (!to.isEmpty()) return QCoreApplication::translate("TaskEvent", "State: %1")
                                      .arg(taskStateDisplayName(taskStateFromString(to)));
    }
    QString text = firstString(payload, {"message", "text", "summary", "question", "answer", "error", "content"});
    if (text.isEmpty() && payload.value(QLatin1String("event")).isObject())
        text = firstString(payload.value(QLatin1String("event")).toObject(), {"message", "text", "summary"});
    if (!text.isEmpty()) return truncated(text);
    if (!payload.isEmpty())
        return truncated(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)), 600);
    return type;
}

namespace mapping {

QDateTime parseTimestamp(const QString &value)
{
    if (value.isEmpty()) return QDateTime();
    QDateTime dt = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!dt.isValid()) dt = QDateTime::fromString(value, Qt::ISODate);
    if (!dt.isValid()) {
        // SQLite "YYYY-MM-DD HH:MM:SS" (UTC by convention).
        dt = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (dt.isValid()) dt.setTimeZone(QTimeZone::UTC);
#else
        if (dt.isValid()) dt.setTimeSpec(Qt::UTC);
#endif
    }
    return dt;
}

Question questionFromCorePayload(const QJsonObject &payload)
{
    Question q;
    const QJsonValue questions = payload.value(QLatin1String("questions"));
    if (questions.isArray()) {
        for (const QJsonValue &entry : questions.toArray()) {
            if (!entry.isObject()) continue;
            QuestionItem item = itemFromObject(entry.toObject());
            if (!item.text.isEmpty()) q.items.append(item);
        }
        if (q.isValid()) return q;
    }
    const QJsonValue question = payload.value(QLatin1String("question"));
    if (question.isObject()) {
        const QJsonObject inner = question.toObject();
        if (inner.value(QLatin1String("questions")).isArray()) return questionFromCorePayload(inner);
        QuestionItem item = itemFromObject(inner);
        if (!item.text.isEmpty()) q.items.append(item);
        return q;
    }
    QuestionItem item = itemFromObject(payload);
    if (!item.text.isEmpty()) q.items.append(item);
    return q;
}

Question questionFromRemote(const QJsonValue &value)
{
    Question q;
    if (!value.isObject()) return q;
    const QJsonObject o = value.toObject();
    if (o.value(QLatin1String("question")).isObject()) return questionFromRemote(o.value(QLatin1String("question")));
    QuestionItem item = itemFromObject(o);
    if (!item.text.isEmpty()) q.items.append(item);
    return q;
}

Task taskFromCore(const QJsonObject &obj)
{
    Task t;
    t.id = str(obj, "task_id");
    t.coreTaskId = t.id;
    t.title = str(obj, "title");
    t.text = str(obj, "task");
    if (t.title.isEmpty()) t.title = t.text.left(200);
    t.rawState = str(obj, "status");
    t.state = taskStateFromString(t.rawState);
    t.sessionId = str(obj, "session_id");
    t.agent = str(obj, "agent");
    t.priority = integer(obj, "priority", 5);
    t.attempts = integer(obj, "attempts");
    t.maxAttempts = integer(obj, "max_attempts");
    t.cancelRequested = boolean(obj, "cancel_requested");
    t.createdAt = parseTimestamp(str(obj, "created_at"));
    t.updatedAt = parseTimestamp(str(obj, "updated_at"));
    t.error = str(obj, "error");
    const QJsonValue result = obj.value(QLatin1String("result"));
    if (result.isObject()) {
        const QJsonObject r = result.toObject();
        t.resultSummary = truncated(firstString(r, {"summary", "answer", "content", "output", "message"}));
        if (t.state == TaskState::WaitingInput) t.question = questionFromCorePayload(r);
    } else if (result.isString()) {
        t.resultSummary = truncated(result.toString());
    }
    return t;
}

Task taskFromRemote(const QJsonObject &obj)
{
    Task t;
    t.id = str(obj, "task_id");
    t.workspaceId = str(obj, "workspace_id");
    t.nodeId = str(obj, "node_id");
    t.coreTaskId = str(obj, "core_task_id");
    t.title = str(obj, "title");
    t.text = str(obj, "task");
    t.agent = str(obj, "agent");
    t.sessionId = str(obj, "session_id");
    t.priority = integer(obj, "priority", 5);
    t.rawState = str(obj, "state");
    t.state = taskStateFromString(t.rawState);
    t.attempts = integer(obj, "attempts");
    t.createdAt = parseTimestamp(str(obj, "created_at"));
    t.updatedAt = parseTimestamp(str(obj, "updated_at"));
    t.lastEventId = integer64(obj, "last_event_id");
    t.stale = boolean(obj, "stale");
    t.idempotencyKey = str(obj, "idempotency_key");
    t.resultSummary = truncated(str(obj, "result_summary"));
    t.error = str(obj, "error");
    t.question = questionFromRemote(obj.value(QLatin1String("question")));
    return t;
}

TaskEvent eventFromCore(const QJsonObject &obj, const QString &taskId)
{
    TaskEvent e;
    e.id = integer64(obj, "event_id");
    e.taskId = str(obj, "task_id", taskId);
    e.type = str(obj, "type", QStringLiteral("event"));
    e.actor = str(obj, "actor");
    e.message = truncated(str(obj, "message"));
    e.occurredAt = parseTimestamp(str(obj, "created_at"));
    e.payload = obj.value(QLatin1String("payload")).toObject();
    if (e.message.isEmpty() && e.type == QLatin1String("agent_message"))
        e.message = truncated(firstString(e.payload, {"content", "summary"}));
    return e;
}

TaskEvent eventFromRemote(const QJsonObject &obj)
{
    TaskEvent e;
    e.id = integer64(obj, "event_id");
    e.taskId = str(obj, "task_id");
    e.type = str(obj, "type", QStringLiteral("event"));
    e.nodeId = str(obj, "node_id");
    e.seq = obj.value(QLatin1String("seq")).isDouble() ? integer64(obj, "seq") : -1;
    e.occurredAt = parseTimestamp(str(obj, "occurred_at"));
    e.payload = obj.value(QLatin1String("payload")).toObject();
    e.actor = e.nodeId.isEmpty() ? QStringLiteral("control-plane") : e.nodeId;
    if (e.type == QLatin1String("core_event")) {
        // A forwarded core event keeps its own type/message inside the payload.
        const QString inner = firstString(e.payload, {"message"});
        if (!inner.isEmpty()) e.message = truncated(inner);
        const QString innerActor = firstString(e.payload, {"actor"});
        if (!innerActor.isEmpty()) e.actor = innerActor;
    }
    return e;
}

Node nodeFromCore(const QJsonObject &obj)
{
    Node n;
    n.id = str(obj, "id");
    n.name = str(obj, "name", n.id);
    n.type = str(obj, "type");
    n.status = boolean(obj, "enabled", true) ? QStringLiteral("enabled") : QStringLiteral("disabled");
    // Deliberately not copied: endpoint credentials such as "api_key".
    return n;
}

Node nodeFromRemote(const QJsonObject &obj)
{
    Node n;
    n.id = str(obj, "node_id");
    n.name = str(obj, "name", n.id);
    n.status = str(obj, "status");
    n.coreVersion = str(obj, "core_version");
    n.lastSeenAt = parseTimestamp(str(obj, "last_seen_at"));
    for (const QJsonValue &label : obj.value(QLatin1String("labels")).toArray()) {
        if (label.isString()) n.labels << label.toString();
    }
    return n;
}

Artifact artifactFromCore(const QJsonObject &obj, const QString &taskId)
{
    Artifact a;
    a.taskId = taskId;
    a.id = str(obj, "path");
    if (a.id.isEmpty()) a.id = str(obj, "name");
    const int slash = a.id.lastIndexOf(QLatin1Char('/'));
    a.name = slash >= 0 ? a.id.mid(slash + 1) : a.id;
    a.size = integer64(obj, "size", -1);
    a.sha256 = str(obj, "sha256").toLower();
    a.mime = str(obj, "mime", QStringLiteral("application/octet-stream"));
    a.state = QStringLiteral("available");
    return a;
}

Artifact artifactFromRemote(const QJsonObject &obj)
{
    Artifact a;
    a.id = str(obj, "artifact_id");
    a.taskId = str(obj, "task_id");
    a.nodeId = str(obj, "node_id");
    a.name = str(obj, "name");
    a.mime = str(obj, "mime");
    a.size = integer64(obj, "size", -1);
    a.sha256 = str(obj, "sha256").toLower();
    a.state = str(obj, "state");
    a.createdAt = parseTimestamp(str(obj, "created_at"));
    a.expiresAt = parseTimestamp(str(obj, "expires_at"));
    return a;
}

Agent agentFromCore(const QJsonObject &obj)
{
    Agent a;
    a.id = str(obj, "id");
    a.name = str(obj, "name", a.id);
    a.description = str(obj, "description");
    a.modelId = str(obj, "model_id");
    return a;
}

ModelInfo modelFromCore(const QJsonObject &obj)
{
    ModelInfo m;
    m.id = str(obj, "id");
    m.name = str(obj, "name", m.id);
    m.kind = str(obj, "kind", QStringLiteral("llm"));
    m.nodeId = str(obj, "node_id");
    m.tier = str(obj, "tier");
    m.lastError = str(obj, "last_error");
    m.enabled = boolean(obj, "enabled");
    m.running = boolean(obj, "running");
    m.available = boolean(obj, "available");
    m.managed = boolean(obj, "managed");
    return m;
}

CommandResult commandFromRemote(const QJsonObject &obj)
{
    const QJsonObject o = obj.value(QLatin1String("command")).isObject()
                              ? obj.value(QLatin1String("command")).toObject() : obj;
    CommandResult c;
    c.commandId = str(o, "command_id");
    c.taskId = str(o, "task_id");
    c.state = str(o, "state");
    return c;
}

QString sanitizeFileName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch.unicode() == 0 || ch.unicode() < 0x20 ||
            ch.unicode() == 0x7f)
            out += QLatin1Char('_');
        else
            out += ch;
    }
    while (out.startsWith(QLatin1Char('.'))) out.remove(0, 1);
    out = out.trimmed();
    if (out.size() > 255) out.truncate(255);
    if (out.isEmpty()) out = QStringLiteral("artifact.bin");
    return out;
}

} // namespace mapping
} // namespace asgend
