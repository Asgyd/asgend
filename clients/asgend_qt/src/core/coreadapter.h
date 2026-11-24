// SPDX-License-Identifier: MIT
// Adapter for the local core JSON-line API (docs/PROTOCOL.md): one JSON object
// + LF per connection, one reply + LF, then the core closes the connection.
// Transport: QLocalSocket (AF_UNIX on Linux) or QTcpSocket to a loopback port.
#pragma once

#include "core/profile.h"
#include "core/protocoladapter.h"

#include <functional>
#include <memory>

namespace asgend {

struct CoreDownload;

class CoreAdapter final : public ProtocolAdapter {
    Q_OBJECT
public:
    static constexpr qint64 MaxReplyBytes = 8LL * 1024 * 1024;
    static constexpr int ReadChunkBytes = 1048576; // core limit for workspace_read_bytes

    using RawDone = std::function<void(const ApiError &, const QJsonObject &)>;

    CoreAdapter(ConnectionProfile profile, QString authToken, QObject *parent = nullptr);
    ~CoreAdapter() override;

    Kind kind() const override { return Kind::Core; }
    Capabilities capabilities() const override;
    QString endpointDescription() const override;

    void ping(Callback<ServerInfo> cb) override;
    void health(Callback<QJsonObject> cb) override;
    void listTasks(const TaskFilter &filter, Callback<QList<Task>> cb) override;
    void getTask(const QString &taskId, Callback<Task> cb) override;
    void events(const QString &taskId, qint64 afterEventId, int waitMs, Callback<EventPage> cb) override;
    void submit(const SubmitRequest &request, Callback<Task> cb) override;
    void answer(const QString &taskId, const QString &answer, const QString &optionId,
                const QString &idempotencyKey, Callback<CommandResult> cb) override;
    void cancel(const QString &taskId, const QString &idempotencyKey, Callback<CommandResult> cb) override;
    void retry(const QString &taskId, const QString &idempotencyKey, Callback<CommandResult> cb) override;
    void deadLetters(int limit, Callback<QList<Task>> cb) override;
    void listAgents(Callback<QList<Agent>> cb) override;
    void listModels(Callback<QList<ModelInfo>> cb) override;
    void manageModel(const ModelInfo &model, ModelAction action, Callback<CommandResult> cb) override;
    void listNodes(Callback<QList<Node>> cb) override;
    void revokeNode(const QString &nodeId, Callback<CommandResult> cb) override;
    void listArtifacts(const QString &taskId, Callback<QList<Artifact>> cb) override;
    void downloadArtifact(const Artifact &artifact, const QString &destinationPath, qint64 maxBytes,
                          Callback<DownloadResult> cb) override;
    void abortAll() override;

    // Sends one request (auth_token is added here and never logged).
    void call(QJsonObject request, RawDone done, int timeoutMs = -1);

private:
    void downloadChunk(const std::shared_ptr<CoreDownload> &state);

    ConnectionProfile m_profile;
    QString m_token;
};

} // namespace asgend
