// SPDX-License-Identifier: MIT
#include "core/connectionmanager.h"

#include "core/controlplaneadapter.h"
#include "core/coreadapter.h"
#include "core/credentialstore.h"
#include "core/logging.h"

#include <QRandomGenerator>

namespace asgend {

ConnectionManager::ConnectionManager(CredentialStore *store, QObject *parent)
    : QObject(parent), m_store(store), m_factory(&ConnectionManager::createAdapter)
{
    qRegisterMetaType<asgend::ConnectionManager::State>("asgend::ConnectionManager::State");
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &ConnectionManager::probe);
    m_heartbeat.setInterval(15000);
    connect(&m_heartbeat, &QTimer::timeout, this, [this]() {
        if (m_state == State::Connected) probe();
    });
}

ConnectionManager::~ConnectionManager()
{
    teardown();
}

ProtocolAdapter *ConnectionManager::createAdapter(const ConnectionProfile &profile, const QString &token, QObject *parent)
{
    if (profile.type == ConnectionProfile::Type::Remote) return new ControlPlaneAdapter(profile, token, parent);
    return new CoreAdapter(profile, token, parent);
}

void ConnectionManager::setReconnectPolicy(int initialMs, int maxMs, double jitter)
{
    m_initialMs = qMax(10, initialMs);
    m_maxMs = qMax(m_initialMs, maxMs);
    m_jitter = qBound(0.0, jitter, 0.5);
}

void ConnectionManager::setHeartbeatInterval(int ms)
{
    m_heartbeat.setInterval(qMax(100, ms));
}

int ConnectionManager::backoffDelay(int attempt, int initialMs, int maxMs)
{
    if (attempt < 0) attempt = 0;
    qint64 delay = initialMs;
    for (int i = 0; i < attempt && delay < maxMs; ++i) delay *= 2;
    return static_cast<int>(qMin<qint64>(delay, maxMs));
}

QString ConnectionManager::stateName(State state)
{
    switch (state) {
    case State::Disconnected: return tr("Disconnected");
    case State::Connecting: return tr("Connecting");
    case State::Connected: return tr("Connected");
    case State::Reconnecting: return tr("Reconnecting");
    case State::Offline: return tr("Offline");
    }
    return QString();
}

int ConnectionManager::nextRetryInMs() const
{
    if (!m_retryTimer.isActive()) return -1;
    return m_retryTimer.remainingTime();
}

void ConnectionManager::setState(State state)
{
    if (m_state == state) return;
    m_state = state;
    qCInfo(lcConn) << "connection state" << stateName(state) << "for profile" << m_profile.name;
    Q_EMIT stateChanged(state);
}

void ConnectionManager::teardown()
{
    m_retryTimer.stop();
    m_heartbeat.stop();
    ++m_generation;
    m_probing = false;
    if (m_adapter) {
        ProtocolAdapter *old = m_adapter;
        m_adapter = nullptr;
        old->abortAll();
        old->deleteLater();
    }
}

void ConnectionManager::connectTo(const ConnectionProfile &profile)
{
    teardown();
    m_profile = profile;
    m_info = ServerInfo();
    m_lastError = ApiError();
    m_attempt = 0;
    m_everConnected = false;

    const QString problem = profile.validate();
    if (!problem.isEmpty()) {
        m_lastError = ApiError(ApiError::Insecure, problem);
        Q_EMIT adapterChanged(nullptr);
        setState(State::Offline);
        Q_EMIT errorOccurred(m_lastError);
        return;
    }
    QString token;
    QString error;
    if (m_store && !m_store->readToken(profile.id, &token, &error))
        qCWarning(lcStore) << "could not read credential for profile" << profile.name << ":" << error;
    m_adapter = m_factory(profile, token, this);
    connect(m_adapter, &ProtocolAdapter::capabilitiesChanged, this, [this]() { Q_EMIT adapterChanged(m_adapter); });
    Q_EMIT adapterChanged(m_adapter);
    setState(State::Connecting);
    probe();
}

void ConnectionManager::disconnectFromServer()
{
    teardown();
    Q_EMIT adapterChanged(nullptr);
    setState(State::Disconnected);
}

void ConnectionManager::retryNow()
{
    if (!m_adapter) {
        if (!m_profile.id.isEmpty()) connectTo(m_profile);
        return;
    }
    if (m_state == State::Connected) return;
    m_retryTimer.stop();
    if (m_state == State::Offline) setState(State::Reconnecting);
    probe();
}

bool ConnectionManager::isFatal(const ApiError &error) const
{
    switch (error.code) {
    case ApiError::Unauthorized:
    case ApiError::Forbidden:
    case ApiError::Tls:
    case ApiError::Insecure:
        return true;
    default:
        return false;
    }
}

void ConnectionManager::probe()
{
    if (!m_adapter || m_probing) return;
    m_probing = true;
    const quint64 generation = m_generation;
    m_adapter->ping(guarded<ServerInfo>(this, [this, generation](const Result<ServerInfo> &r) {
        onProbeResult(r, generation);
    }));
}

void ConnectionManager::onProbeResult(const Result<ServerInfo> &result, quint64 generation)
{
    if (generation != m_generation) return; // stale probe from a previous adapter
    m_probing = false;
    if (result.ok()) {
        const bool wasLost = m_everConnected && m_state != State::Connected;
        const bool first = !m_everConnected;
        m_info = result.value;
        m_lastError = ApiError();
        m_attempt = 0;
        m_retryTimer.stop();
        if (m_profile.type == ConnectionProfile::Type::Remote && !m_info.workspaceId.isEmpty() &&
            m_profile.workspaceId != m_info.workspaceId) {
            m_profile.workspaceId = m_info.workspaceId;
            Q_EMIT profileUpdated(m_profile);
        }
        m_everConnected = true;
        if (!m_heartbeat.isActive()) m_heartbeat.start();
        setState(State::Connected);
        if (first) Q_EMIT connected(m_info);
        else if (wasLost) Q_EMIT reconnected(m_info);
        return;
    }
    const ApiError &error = result.error;
    if (error.code == ApiError::Cancelled) return;
    m_lastError = error;
    if (isFatal(error)) {
        m_heartbeat.stop();
        m_retryTimer.stop();
        setState(State::Offline);
        Q_EMIT errorOccurred(error);
        return;
    }
    if (m_state == State::Connected) {
        lose(error);
        return;
    }
    Q_EMIT errorOccurred(error);
    scheduleRetry();
}

void ConnectionManager::lose(const ApiError &error)
{
    m_heartbeat.stop();
    m_attempt = 0;
    setState(State::Reconnecting);
    Q_EMIT connectionLost(error);
    scheduleRetry();
}

void ConnectionManager::scheduleRetry()
{
    if (!m_adapter) return;
    int delay = backoffDelay(m_attempt, m_initialMs, m_maxMs);
    if (m_jitter > 0.0) {
        const double factor = 1.0 + (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * m_jitter;
        delay = qMax(10, static_cast<int>(delay * factor));
    }
    ++m_attempt;
    setState(State::Reconnecting);
    qCInfo(lcConn) << "reconnect attempt" << m_attempt << "in" << delay << "ms";
    Q_EMIT retryScheduled(delay, m_attempt);
    m_retryTimer.start(delay);
}

void ConnectionManager::reportFailure(const ApiError &error)
{
    if (!m_adapter || error.code == ApiError::Cancelled) return;
    if (isFatal(error) && error.code == ApiError::Unauthorized) {
        m_lastError = error;
        m_heartbeat.stop();
        m_retryTimer.stop();
        setState(State::Offline);
        Q_EMIT errorOccurred(error);
        return;
    }
    if (!error.isConnectivity()) return;
    if (m_state == State::Connected) {
        m_lastError = error;
        lose(error);
    }
}

void ConnectionManager::pair(const ConnectionProfile &profile, const QString &code, const QString &deviceName,
                             Callback<PairingResult> cb)
{
    auto *temp = new ControlPlaneAdapter(profile, QString(), this);
    temp->redeemPairing(code, deviceName, [this, temp, profile, cb](const Result<PairingResult> &r) {
        temp->deleteLater();
        if (r.ok() && m_store) {
            QString error;
            if (!m_store->writeToken(profile.id, r.value.token, &error)) {
                if (cb) cb(Result<PairingResult>::failure(ApiError(ApiError::Io, error)));
                return;
            }
        }
        if (cb) cb(r);
    });
}

void ConnectionManager::logout(Callback<CommandResult> cb)
{
    const QString profileId = m_profile.id;
    auto finish = [this, profileId, cb](const Result<CommandResult> &r) {
        QString error;
        if (m_store) m_store->removeToken(profileId, &error);
        disconnectFromServer();
        if (cb) cb(r);
    };
    auto *remote = qobject_cast<ControlPlaneAdapter *>(m_adapter.data());
    if (remote && remote->hasToken()) {
        remote->logout(guarded<CommandResult>(this, finish));
        return;
    }
    finish(Result<CommandResult>::success(CommandResult{QString(), QString(), QStringLiteral("logged_out")}));
}

} // namespace asgend
