// SPDX-License-Identifier: MIT
#pragma once

#include "core/connectionmanager.h"
#include "core/profile.h"

#include <QMainWindow>
#include <QList>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QTableWidget;
class QTimer;

namespace asgend {
class CredentialStore;
class ProtocolAdapter;
class Task;
class Artifact;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi();
    void loadProfiles();
    void saveSettings();
    void updateConnectionUi(ConnectionManager::State state);
    void connectSelectedProfile();
    void editSelectedProfile();
    void addRemoteProfileAndPair();
    void refreshTasks();
    void refreshSelectedTask();
    void renderTasks(const QList<Task> &tasks);
    void renderSelectedTask(const Task &task);
    void submitTask();
    void cancelSelectedTask();
    void retrySelectedTask();
    void answerSelectedTask();
    void showArtifacts();
    void showNodes();
    void showAgents();
    void showModels();
    void showDeadLetters();
    void setStatus(const QString &message, bool error = false);
    QString selectedTaskId() const;
    const Task *selectedTask() const;
    ProtocolAdapter *adapter() const;

    std::unique_ptr<CredentialStore> m_store;
    ConnectionManager *m_connection = nullptr;
    AppSettings m_settings;
    QComboBox *m_profiles = nullptr;
    QComboBox *m_stateFilter = nullptr;
    QLineEdit *m_textFilter = nullptr;
    QTableWidget *m_tasks = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QPlainTextEdit *m_events = nullptr;
    QLineEdit *m_taskInput = nullptr;
    QLineEdit *m_agentInput = nullptr;
    QLineEdit *m_answerInput = nullptr;
    QLabel *m_connectionLabel = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_logoutButton = nullptr;
    QPushButton *m_submitButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_answerButton = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QList<Task> m_taskCache;
    QString m_selectedTaskId;
    qint64 m_eventCursor = 0;
    bool m_refreshInFlight = false;
};

} // namespace asgend
