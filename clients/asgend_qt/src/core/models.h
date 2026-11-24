// SPDX-License-Identifier: MIT
// Unified client models. Both adapters map their wire formats into these
// structures; unknown wire fields are ignored (CONTRACT_SKETCH §1).
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace asgend {

enum class TaskState {
    Unknown,
    PendingDelivery,
    Delivered,
    Queued,
    Running,
    WaitingInput,
    Completed,
    CompletedWithoutModel,
    Failed,
    Cancelled,
    DeliveryFailed
};

QString taskStateToString(TaskState state);
TaskState taskStateFromString(const QString &state);
QString taskStateDisplayName(TaskState state); // translated
QList<TaskState> allTaskStates();
bool isTerminal(TaskState state);
bool isRetryable(TaskState state);   // failed / delivery_failed
bool isCancellable(TaskState state); // any non-terminal state

struct QuestionOption {
    QString id;
    QString label;
};

struct QuestionItem {
    QString text;
    QList<QuestionOption> options;
};

struct Question {
    QList<QuestionItem> items;
    qint64 eventId = 0; // event that carried the question, 0 when from status
    bool isValid() const { return !items.isEmpty(); }
    QString summary() const;
    // Stable identity used to avoid asking the same question twice.
    QString signature() const;
    // Combines per-item answers the way the core CLI does for batch questions.
    static QString composeAnswer(const QList<QuestionItem> &items, const QStringList &answers);
};

struct Task {
    QString id;
    QString title;
    QString text;
    QString agent;
    QString nodeId;
    QString workspaceId;
    QString sessionId;
    QString coreTaskId;
    QString error;
    QString resultSummary;
    QString idempotencyKey;
    QString rawState;
    TaskState state = TaskState::Unknown;
    int priority = 5;
    int attempts = 0;
    int maxAttempts = 0;
    bool cancelRequested = false;
    bool stale = false;
    qint64 lastEventId = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    Question question;
};

struct TaskEvent {
    qint64 id = 0;
    QString taskId;
    QString type;
    QString actor;
    QString message;
    QString nodeId;
    qint64 seq = -1;
    QDateTime occurredAt;
    QJsonObject payload;
    bool isQuestion() const;
    QString displayText() const;
};

struct EventPage {
    QList<TaskEvent> events;
    qint64 lastEventId = 0;
};

struct Node {
    QString id;
    QString name;
    QString status; // online/offline/revoked (remote) or enabled/disabled (core)
    QString type;
    QString coreVersion;
    QStringList labels;
    QDateTime lastSeenAt;
};

struct Artifact {
    QString id;     // art_… (remote) or workspace-relative path (core)
    QString taskId;
    QString name;   // display name / basename
    QString mime;
    QString sha256; // lowercase hex, empty when unknown
    QString state;  // available/uploading/rejected/expired
    QString nodeId;
    qint64 size = -1;
    QDateTime createdAt;
    QDateTime expiresAt;
};

struct Agent {
    QString id;
    QString name;
    QString description;
    QString modelId;
};

struct ModelInfo {
    QString id;
    QString name;
    QString kind; // llm / decision
    QString nodeId;
    QString tier;
    QString lastError;
    bool enabled = false;
    bool running = false;
    bool available = false;
    bool managed = false;
};

struct ServerInfo {
    QString service;
    QString version;
    QString userId;
    QString workspaceId;
    QString role;
};

struct CommandResult {
    QString commandId;
    QString taskId;
    QString state;
};

struct PairingResult {
    QString token; // never logged, stored only in the credential store
    QString userId;
    QString workspaceId;
    QString role;
};

struct DownloadResult {
    QString path;
    qint64 bytes = 0;
    QString sha256;
    QString expectedSha256;
    bool checksumVerified = false; // true only when an expected checksum matched
};

namespace mapping {

QDateTime parseTimestamp(const QString &value);
Question questionFromCorePayload(const QJsonObject &payload);
Question questionFromRemote(const QJsonValue &value);
Task taskFromCore(const QJsonObject &obj);
Task taskFromRemote(const QJsonObject &obj);
TaskEvent eventFromCore(const QJsonObject &obj, const QString &taskId);
TaskEvent eventFromRemote(const QJsonObject &obj);
Node nodeFromCore(const QJsonObject &obj);
Node nodeFromRemote(const QJsonObject &obj);
Artifact artifactFromCore(const QJsonObject &obj, const QString &taskId);
Artifact artifactFromRemote(const QJsonObject &obj);
Agent agentFromCore(const QJsonObject &obj);
ModelInfo modelFromCore(const QJsonObject &obj);
CommandResult commandFromRemote(const QJsonObject &obj);
// Removes '/', '\\', NUL, control characters and leading dots; never empty.
QString sanitizeFileName(const QString &name);

} // namespace mapping

} // namespace asgend

Q_DECLARE_METATYPE(asgend::Task)
Q_DECLARE_METATYPE(asgend::TaskEvent)
Q_DECLARE_METATYPE(asgend::Question)
