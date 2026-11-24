// SPDX-License-Identifier: MIT
// Adapter for the remote Control Plane user API (contracts/v1/CONTRACT_SKETCH.md §6).
// HTTPS with strict certificate verification (optional extra CA bundle),
// Bearer ucp_… user/device token, redirects refused, idempotency keys on
// every mutation, 429 Retry-After honoured for retry-safe requests, long-poll
// events with the after_event_id cursor, artifact download via short-lived
// download tickets with SHA-256 + size verification.
#pragma once

#include "core/profile.h"
#include "core/protocoladapter.h"

#include <QSet>
#include <QSslConfiguration>
#include <QUrlQuery>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace asgend {

class ControlPlaneAdapter final : public ProtocolAdapter {
    Q_OBJECT
public:
    static constexpr int MaxRateLimitRetries = 2;
    static constexpr int MaxAutoRetryAfterMs = 30000;
    static constexpr int ApiVersion = 1;

    ControlPlaneAdapter(ConnectionProfile profile, QString token, QObject *parent = nullptr);
    ~ControlPlaneAdapter() override;

    Kind kind() const override { return Kind::ControlPlane; }
    Capabilities capabilities() const override;
    QString endpointDescription() const override;

    QString workspaceId() const { return m_workspaceId; }
    QString role() const { return m_role; }
    bool hasToken() const { return !m_token.isEmpty(); }

    // POST /v1/pairing/redeem (no auth). On success the adapter uses the new
    // token; the caller persists it in the CredentialStore.
    void redeemPairing(const QString &code, const QString &deviceName, Callback<PairingResult> cb);
    // POST /v1/auth/logout: revokes the presenting token server-side.
    void logout(Callback<CommandResult> cb);

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

    // Retry-After: delta-seconds or HTTP-date -> milliseconds, -1 if absent/invalid.
    static int parseRetryAfter(const QByteArray &value);
    // Resolves a download URL from a ticket reply; empty when it is not on
    // the Control Plane origin (tickets are never sent to another host).
    QUrl resolveDownloadUrl(const QString &url, const QString &ticket) const;

private:
    struct Request {
        QByteArray verb = "GET";
        QString path;
        QUrlQuery query;
        QJsonObject body;
        bool hasBody = false;
        bool auth = true;
        bool retrySafe = false; // GET or carries an idempotency key
        int timeoutMs = 0;
        int attempt = 0;
    };
    using JsonDone = std::function<void(const ApiError &, const QJsonObject &, int status)>;

    void send(Request request, JsonDone done);
    QNetworkRequest buildRequest(const QUrl &url, const QString &bearer, bool json, int timeoutMs) const;
    ApiError replyError(QNetworkReply *reply, const QByteArray &body) const;
    void track(QNetworkReply *reply);
    void untrack(QNetworkReply *reply);
    bool requireWorkspace(ApiError *error) const;
    void command(const QString &taskId, const QString &verb, const QJsonObject &body, Callback<CommandResult> cb);
    void startDownload(const Artifact &artifact, const QUrl &url, const QString &ticket, const QString &destinationPath,
                       qint64 maxBytes, Callback<DownloadResult> cb);
    void setRole(const QString &role);
    static QString seg(const QString &id);

    ConnectionProfile m_profile;
    QString m_token;
    QString m_workspaceId;
    QString m_role;
    QString m_configError;
    ApiError::Code m_configErrorCode = ApiError::None;
    QSslConfiguration m_ssl;
    bool m_tls = true;
    QNetworkAccessManager *m_nam = nullptr;
    QSet<QNetworkReply *> m_replies;
};

} // namespace asgend
