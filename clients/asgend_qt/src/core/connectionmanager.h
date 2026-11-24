// SPDX-License-Identifier: MIT
// Owns the active ProtocolAdapter for the selected profile and its connection
// state: Connecting -> Connected; on loss Reconnecting with exponential
// backoff (+ optional jitter); Offline when user action is required (invalid
// or revoked credentials, TLS failure, policy violation) or after disconnect.
#pragma once

#include "core/apierror.h"
#include "core/models.h"
#include "core/profile.h"
#include "core/protocoladapter.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

namespace asgend {

class CredentialStore;

class ConnectionManager : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Connected, Reconnecting, Offline };
    Q_ENUM(State)

    using AdapterFactory =
        std::function<ProtocolAdapter *(const ConnectionProfile &, const QString &token, QObject *parent)>;

    explicit ConnectionManager(CredentialStore *store, QObject *parent = nullptr);
    ~ConnectionManager() override;

    void setReconnectPolicy(int initialMs, int maxMs, double jitter = 0.1);
    void setHeartbeatInterval(int ms);
    void setAdapterFactory(AdapterFactory factory) { m_factory = std::move(factory); }
    static ProtocolAdapter *createAdapter(const ConnectionProfile &profile, const QString &token, QObject *parent);

    void connectTo(const ConnectionProfile &profile);
    void disconnectFromServer();
    // Consumers report failures of their own requests; connectivity failures
    // trigger reconnect, authentication failures make the state Offline.
    void reportFailure(const ApiError &error);
    void retryNow();

    // Device pairing against a remote profile; stores the token on success.
    void pair(const ConnectionProfile &profile, const QString &code, const QString &deviceName,
              Callback<PairingResult> cb);
    // Revokes the remote token (if any) and removes it from the store.
    void logout(Callback<CommandResult> cb);

    State state() const { return m_state; }
    ProtocolAdapter *adapter() const { return m_adapter; }
    const ConnectionProfile &profile() const { return m_profile; }
    ServerInfo serverInfo() const { return m_info; }
    ApiError lastError() const { return m_lastError; }
    int attempt() const { return m_attempt; }
    int nextRetryInMs() const;
    CredentialStore *credentialStore() const { return m_store; }

    static int backoffDelay(int attempt, int initialMs, int maxMs);
    static QString stateName(State state);

Q_SIGNALS:
    void stateChanged(asgend::ConnectionManager::State state);
    void connected(const asgend::ServerInfo &info);
    void reconnected(const asgend::ServerInfo &info);
    void connectionLost(const asgend::ApiError &error);
    void errorOccurred(const asgend::ApiError &error);
    void retryScheduled(int delayMs, int attempt);
    void adapterChanged(asgend::ProtocolAdapter *adapter);
    void profileUpdated(const asgend::ConnectionProfile &profile);

private:
    void setState(State state);
    void probe();
    void onProbeResult(const Result<ServerInfo> &result, quint64 generation);
    void scheduleRetry();
    void lose(const ApiError &error);
    void teardown();
    bool isFatal(const ApiError &error) const;

    CredentialStore *m_store = nullptr;
    AdapterFactory m_factory;
    QPointer<ProtocolAdapter> m_adapter;
    ConnectionProfile m_profile;
    ServerInfo m_info;
    ApiError m_lastError;
    State m_state = State::Disconnected;
    QTimer m_retryTimer;
    QTimer m_heartbeat;
    QElapsedTimer m_retryClock;
    int m_initialMs = 1000;
    int m_maxMs = 30000;
    double m_jitter = 0.1;
    int m_attempt = 0;
    bool m_everConnected = false;
    bool m_probing = false;
    quint64 m_generation = 0;
};

} // namespace asgend
