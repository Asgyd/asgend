// SPDX-License-Identifier: MIT
#include "ui/mainwindow.h"

#include "core/credentialstore.h"
#include "core/idempotency.h"
#include "core/protocoladapter.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QTextCursor>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

namespace asgend {

namespace {
QString taskSummary(const Task &task)
{
    QString summary = QStringLiteral("Task: %1\nTitle: %2\nState: %3\nAgent: %4\nNode: %5\nPriority: %6\nCreated: %7\nUpdated: %8")
        .arg(task.id, task.title, taskStateDisplayName(task.state), task.agent,
             task.nodeId, QString::number(task.priority),
             task.createdAt.isValid() ? task.createdAt.toLocalTime().toString(Qt::ISODate) : QStringLiteral("—"),
             task.updatedAt.isValid() ? task.updatedAt.toLocalTime().toString(Qt::ISODate) : QStringLiteral("—"));
    if (!task.sessionId.isEmpty()) summary += QStringLiteral("\nSession: %1").arg(task.sessionId);
    if (!task.error.isEmpty()) summary += QStringLiteral("\n\nError:\n%1").arg(task.error);
    if (!task.resultSummary.isEmpty()) summary += QStringLiteral("\n\nResult:\n%1").arg(task.resultSummary);
    if (task.question.isValid()) summary += QStringLiteral("\n\nWaiting for input:\n%1").arg(task.question.summary());
    return summary;
}

QString profileLabel(const ConnectionProfile &profile)
{
    return QStringLiteral("%1 — %2").arg(profile.name, ConnectionProfile::typeToString(profile.type));
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_store(CredentialStore::createDefault()),
      m_settings(AppSettings::load(AppSettings::defaultPath()))
{
    setWindowTitle(tr("ASGEND — Task workspace"));
    resize(1180, 760);
    m_connection = new ConnectionManager(m_store.get(), this);
    m_connection->setReconnectPolicy(m_settings.reconnectInitialMs, m_settings.reconnectMaxMs);
    m_connection->setHeartbeatInterval(m_settings.heartbeatMs);
    buildUi();
    loadProfiles();

    connect(m_connection, &ConnectionManager::stateChanged, this, [this](ConnectionManager::State state) {
        updateConnectionUi(state);
    });
    connect(m_connection, &ConnectionManager::connected, this, [this](const ServerInfo &info) {
        QString identity = info.service;
        if (!info.workspaceId.isEmpty()) identity += QStringLiteral(" · workspace ") + info.workspaceId;
        if (!info.role.isEmpty()) identity += QStringLiteral(" · role ") + info.role;
        setStatus(tr("Connected: %1").arg(identity));
        refreshTasks();
    });
    connect(m_connection, &ConnectionManager::errorOccurred, this, [this](const ApiError &error) {
        setStatus(error.userMessage(), true);
    });
    connect(m_connection, &ConnectionManager::profileUpdated, this, [this](const ConnectionProfile &profile) {
        for (ConnectionProfile &entry : m_settings.profiles) {
            if (entry.id == profile.id) entry = profile;
        }
        saveSettings();
    });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(qBound(1000, m_settings.taskRefreshMs, 60000));
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        if (m_connection->state() == ConnectionManager::State::Connected) {
            refreshTasks();
            refreshSelectedTask();
        }
    });
    m_refreshTimer->start();
    if (!m_store->isSecure()) setStatus(m_store->warning(), true);
}

MainWindow::~MainWindow()
{
    if (m_connection) m_connection->disconnectFromServer();
}

void MainWindow::buildUi()
{
    auto *toolbar = addToolBar(tr("Connection"));
    toolbar->setMovable(false);
    m_profiles = new QComboBox(this);
    m_profiles->setMinimumWidth(250);
    toolbar->addWidget(m_profiles);
    m_connectButton = new QPushButton(tr("Connect"), this);
    toolbar->addWidget(m_connectButton);
    auto *editButton = new QPushButton(tr("Profile…"), this);
    toolbar->addWidget(editButton);
    auto *pairButton = new QPushButton(tr("Pair remote…"), this);
    toolbar->addWidget(pairButton);
    m_logoutButton = new QPushButton(tr("Log out / revoke"), this);
    toolbar->addWidget(m_logoutButton);
    auto *nodesButton = new QPushButton(tr("Nodes"), this);
    toolbar->addWidget(nodesButton);
    auto *agentsButton = new QPushButton(tr("Agents"), this);
    toolbar->addWidget(agentsButton);
    auto *modelsButton = new QPushButton(tr("Models"), this);
    toolbar->addWidget(modelsButton);
    auto *deadButton = new QPushButton(tr("Dead letters"), this);
    toolbar->addWidget(deadButton);

    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    auto *filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(tr("State:"), root));
    m_stateFilter = new QComboBox(root);
    m_stateFilter->addItem(tr("All states"), QString());
    for (TaskState state : allTaskStates()) m_stateFilter->addItem(taskStateDisplayName(state), taskStateToString(state));
    filterRow->addWidget(m_stateFilter);
    filterRow->addWidget(new QLabel(tr("Search:"), root));
    m_textFilter = new QLineEdit(root);
    m_textFilter->setPlaceholderText(tr("Task id, title or prompt contains…"));
    filterRow->addWidget(m_textFilter, 1);
    auto *refreshButton = new QPushButton(tr("Refresh"), root);
    filterRow->addWidget(refreshButton);
    layout->addLayout(filterRow);

    m_tasks = new QTableWidget(root);
    m_tasks->setColumnCount(5);
    m_tasks->setHorizontalHeaderLabels({tr("Task"), tr("Title / prompt"), tr("State"), tr("Agent"), tr("Updated")});
    m_tasks->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tasks->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tasks->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tasks->verticalHeader()->setVisible(false);
    m_tasks->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tasks->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tasks->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tasks->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tasks->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(m_tasks, 3);

    auto *split = new QSplitter(Qt::Horizontal, root);
    m_details = new QPlainTextEdit(split);
    m_details->setReadOnly(true);
    m_details->setPlaceholderText(tr("Select a task to inspect its status and result."));
    m_events = new QPlainTextEdit(split);
    m_events->setReadOnly(true);
    m_events->setPlaceholderText(tr("Task events appear here."));
    split->addWidget(m_details);
    split->addWidget(m_events);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 2);

    auto *composeRow = new QHBoxLayout();
    m_agentInput = new QLineEdit(root);
    m_agentInput->setPlaceholderText(tr("Agent (optional)"));
    m_agentInput->setMaximumWidth(190);
    m_taskInput = new QLineEdit(root);
    m_taskInput->setPlaceholderText(tr("Describe a task…"));
    m_submitButton = new QPushButton(tr("Create task"), root);
    composeRow->addWidget(m_agentInput);
    composeRow->addWidget(m_taskInput, 1);
    composeRow->addWidget(m_submitButton);
    layout->addLayout(composeRow);

    auto *actions = new QHBoxLayout();
    m_cancelButton = new QPushButton(tr("Cancel selected"), root);
    m_retryButton = new QPushButton(tr("Retry selected"), root);
    m_answerInput = new QLineEdit(root);
    m_answerInput->setPlaceholderText(tr("Answer if this task is waiting for input…"));
    m_answerButton = new QPushButton(tr("Send answer"), root);
    auto *artifactsButton = new QPushButton(tr("Artifacts…"), root);
    actions->addWidget(m_cancelButton);
    actions->addWidget(m_retryButton);
    actions->addWidget(m_answerInput, 1);
    actions->addWidget(m_answerButton);
    actions->addWidget(artifactsButton);
    layout->addLayout(actions);

    m_connectionLabel = new QLabel(tr("Disconnected"), root);
    layout->addWidget(m_connectionLabel);
    setCentralWidget(root);
    statusBar()->showMessage(tr("Ready"));

    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::connectSelectedProfile);
    connect(editButton, &QPushButton::clicked, this, &MainWindow::editSelectedProfile);
    connect(pairButton, &QPushButton::clicked, this, &MainWindow::addRemoteProfileAndPair);
    connect(m_logoutButton, &QPushButton::clicked, this, [this]() {
        if (m_connection->profile().type != ConnectionProfile::Type::Remote) return;
        if (QMessageBox::question(this, tr("Log out"), tr("Revoke this device token on the Control Plane and remove it locally?")) != QMessageBox::Yes) return;
        m_connection->logout(guarded<CommandResult>(this, [this](const Result<CommandResult> &result) {
            if (!result.ok()) { setStatus(tr("Server logout failed; local credential was removed: %1").arg(result.error.userMessage()), true); return; }
            setStatus(tr("Logged out and local credential removed"));
        }));
    });
    connect(nodesButton, &QPushButton::clicked, this, &MainWindow::showNodes);
    connect(agentsButton, &QPushButton::clicked, this, &MainWindow::showAgents);
    connect(modelsButton, &QPushButton::clicked, this, &MainWindow::showModels);
    connect(deadButton, &QPushButton::clicked, this, &MainWindow::showDeadLetters);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshTasks);
    connect(m_stateFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshTasks(); });
    connect(m_textFilter, &QLineEdit::returnPressed, this, &MainWindow::refreshTasks);
    connect(m_tasks, &QTableWidget::itemSelectionChanged, this, [this]() {
        const QString id = selectedTaskId();
        if (id != m_selectedTaskId) {
            m_selectedTaskId = id;
            m_eventCursor = 0;
            m_events->clear();
        }
        refreshSelectedTask();
    });
    connect(m_submitButton, &QPushButton::clicked, this, &MainWindow::submitTask);
    connect(m_taskInput, &QLineEdit::returnPressed, this, &MainWindow::submitTask);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::cancelSelectedTask);
    connect(m_retryButton, &QPushButton::clicked, this, &MainWindow::retrySelectedTask);
    connect(m_answerButton, &QPushButton::clicked, this, &MainWindow::answerSelectedTask);
    connect(artifactsButton, &QPushButton::clicked, this, &MainWindow::showArtifacts);
}

void MainWindow::loadProfiles()
{
    if (m_settings.profiles.isEmpty()) m_settings = AppSettings::defaults();
    m_profiles->clear();
    int selected = 0;
    for (int i = 0; i < m_settings.profiles.size(); ++i) {
        const ConnectionProfile &profile = m_settings.profiles.at(i);
        m_profiles->addItem(profileLabel(profile), profile.id);
        if (profile.id == m_settings.activeProfileId) selected = i;
    }
    m_profiles->setCurrentIndex(selected);
}

void MainWindow::saveSettings()
{
    m_settings.activeProfileId = m_profiles->currentData().toString();
    QString error;
    if (!m_settings.save(AppSettings::defaultPath(), &error)) setStatus(tr("Could not save settings: %1").arg(error), true);
}

void MainWindow::updateConnectionUi(ConnectionManager::State state)
{
    const QString name = ConnectionManager::stateName(state);
    m_connectionLabel->setText(name + QStringLiteral(" · ") + m_connection->profile().name);
    m_connectButton->setText(state == ConnectionManager::State::Connected ? tr("Disconnect") : tr("Connect"));
    const bool connected = state == ConnectionManager::State::Connected;
    if (m_logoutButton) m_logoutButton->setEnabled(m_connection->profile().type == ConnectionProfile::Type::Remote && m_connection->adapter() != nullptr);
    m_submitButton->setEnabled(connected && (!m_connection->adapter() || m_connection->adapter()->capabilities().submit));
    if (!connected) {
        m_cancelButton->setEnabled(false);
        m_retryButton->setEnabled(false);
        m_answerButton->setEnabled(false);
    } else {
        refreshSelectedTask();
    }
}

void MainWindow::connectSelectedProfile()
{
    if (m_connection->state() == ConnectionManager::State::Connected ||
        m_connection->state() == ConnectionManager::State::Connecting ||
        m_connection->state() == ConnectionManager::State::Reconnecting) {
        m_connection->disconnectFromServer();
        setStatus(tr("Disconnected"));
        return;
    }
    const QString id = m_profiles->currentData().toString();
    for (const ConnectionProfile &profile : m_settings.profiles) {
        if (profile.id == id) {
            m_settings.activeProfileId = id;
            saveSettings();
            m_connection->connectTo(profile);
            return;
        }
    }
}

void MainWindow::editSelectedProfile()
{
    const QString id = m_profiles->currentData().toString();
    int profileIndex = -1;
    for (int i = 0; i < m_settings.profiles.size(); ++i) if (m_settings.profiles.at(i).id == id) profileIndex = i;
    if (profileIndex < 0) return;
    ConnectionProfile profile = m_settings.profiles.at(profileIndex);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Connection profile"));
    auto *form = new QFormLayout(&dialog);
    auto *name = new QLineEdit(profile.name, &dialog);
    auto *type = new QComboBox(&dialog);
    type->addItem(tr("Local Unix socket"), static_cast<int>(ConnectionProfile::Type::LocalSocket));
    type->addItem(tr("Local TCP (loopback only)"), static_cast<int>(ConnectionProfile::Type::LocalTcp));
    type->addItem(tr("Remote Control Plane (HTTPS)"), static_cast<int>(ConnectionProfile::Type::Remote));
    type->setCurrentIndex(type->findData(static_cast<int>(profile.type)));
    auto *socket = new QLineEdit(profile.socketPath, &dialog);
    auto *host = new QLineEdit(profile.host, &dialog);
    auto *port = new QSpinBox(&dialog); port->setRange(1, 65535); port->setValue(profile.port);
    auto *url = new QLineEdit(profile.baseUrl, &dialog); url->setPlaceholderText(QStringLiteral("https://control.example.org"));
    auto *ca = new QLineEdit(profile.caCertificatePath, &dialog);
    form->addRow(tr("Name"), name);
    form->addRow(tr("Transport"), type);
    form->addRow(tr("Unix socket path"), socket);
    form->addRow(tr("Loopback host"), host);
    form->addRow(tr("TCP port"), port);
    form->addRow(tr("Control Plane URL"), url);
    form->addRow(tr("Additional CA PEM (optional)"), ca);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    profile.name = name->text().trimmed();
    profile.type = static_cast<ConnectionProfile::Type>(type->currentData().toInt());
    profile.socketPath = socket->text().trimmed();
    profile.host = host->text().trimmed();
    profile.port = port->value();
    profile.baseUrl = url->text().trimmed();
    profile.caCertificatePath = ca->text().trimmed();
    const QString validation = profile.validate();
    if (profile.name.isEmpty() || !validation.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid profile"), profile.name.isEmpty() ? tr("Profile name is required") : validation);
        return;
    }
    m_settings.profiles[profileIndex] = profile;
    saveSettings();
    loadProfiles();
    m_profiles->setCurrentIndex(m_profiles->findData(profile.id));
    setStatus(tr("Profile saved; reconnect to apply changes."));
}

void MainWindow::addRemoteProfileAndPair()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Pair a remote Control Plane"));
    auto *form = new QFormLayout(&dialog);
    auto *name = new QLineEdit(tr("Remote workspace"), &dialog);
    auto *url = new QLineEdit(QStringLiteral("https://"), &dialog);
    auto *code = new QLineEdit(&dialog); code->setEchoMode(QLineEdit::Password);
    auto *device = new QLineEdit(QStringLiteral("ASGEND Qt on desktop"), &dialog);
    form->addRow(tr("Profile name"), name);
    form->addRow(tr("HTTPS endpoint"), url);
    form->addRow(tr("Pairing code"), code);
    form->addRow(tr("Device name"), device);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    ConnectionProfile profile;
    profile.id = ConnectionProfile::newId();
    profile.name = name->text().trimmed();
    profile.type = ConnectionProfile::Type::Remote;
    profile.baseUrl = url->text().trimmed();
    profile.deviceName = device->text().trimmed();
    const QString validation = profile.validate();
    if (profile.name.isEmpty() || code->text().trimmed().isEmpty() || !validation.isEmpty()) {
        QMessageBox::warning(this, tr("Pairing failed"), profile.name.isEmpty() ? tr("Profile name is required") :
                             (code->text().trimmed().isEmpty() ? tr("Pairing code is required") : validation));
        return;
    }
    setStatus(tr("Redeeming pairing code…"));
    m_connection->pair(profile, code->text().trimmed(), profile.deviceName,
                       guarded<PairingResult>(this, [this, profile](const Result<PairingResult> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); QMessageBox::warning(this, tr("Pairing failed"), result.error.userMessage()); return; }
        ConnectionProfile saved = profile;
        saved.workspaceId = result.value.workspaceId;
        m_settings.profiles.append(saved);
        m_settings.activeProfileId = saved.id;
        saveSettings();
        loadProfiles();
        m_profiles->setCurrentIndex(m_profiles->findData(saved.id));
        m_connection->connectTo(saved);
        setStatus(tr("Pairing complete; token stored in %1.").arg(m_store->backendName()));
        if (!m_store->isSecure()) QMessageBox::warning(this, tr("Credential storage warning"), m_store->warning());
    }));
}

ProtocolAdapter *MainWindow::adapter() const
{
    return m_connection ? m_connection->adapter() : nullptr;
}

void MainWindow::refreshTasks()
{
    ProtocolAdapter *current = adapter();
    if (!current || m_connection->state() != ConnectionManager::State::Connected || m_refreshInFlight) return;
    const Capabilities caps = current->capabilities();
    if (!caps.submit && !caps.cancel && !caps.retry && !caps.nodes) { setStatus(tr("Connected with read-only capabilities")); }
    TaskFilter filter;
    filter.state = m_stateFilter->currentData().toString();
    filter.text = m_textFilter->text().trimmed();
    filter.limit = 200;
    m_refreshInFlight = true;
    current->listTasks(filter, guarded<QList<Task>>(this, [this](const Result<QList<Task>> &result) {
        m_refreshInFlight = false;
        if (!result.ok()) { m_connection->reportFailure(result.error); setStatus(result.error.userMessage(), true); return; }
        renderTasks(result.value);
    }));
}

void MainWindow::renderTasks(const QList<Task> &tasks)
{
    const QString keepSelected = m_selectedTaskId;
    m_taskCache = tasks;
    m_tasks->setRowCount(tasks.size());
    int selectedRow = -1;
    for (int row = 0; row < tasks.size(); ++row) {
        const Task &task = tasks.at(row);
        auto *id = new QTableWidgetItem(task.id);
        id->setData(Qt::UserRole, task.id);
        m_tasks->setItem(row, 0, id);
        m_tasks->setItem(row, 1, new QTableWidgetItem(task.title.isEmpty() ? task.text.left(160) : task.title));
        m_tasks->setItem(row, 2, new QTableWidgetItem(taskStateDisplayName(task.state)));
        m_tasks->setItem(row, 3, new QTableWidgetItem(task.agent));
        m_tasks->setItem(row, 4, new QTableWidgetItem(task.updatedAt.isValid() ? task.updatedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString()));
        if (task.id == keepSelected) selectedRow = row;
    }
    if (selectedRow >= 0) m_tasks->selectRow(selectedRow);
    else if (!tasks.isEmpty()) m_tasks->selectRow(0);
    setStatus(tr("Loaded %n task(s)", nullptr, tasks.size()));
}

QString MainWindow::selectedTaskId() const
{
    const QModelIndexList rows = m_tasks->selectionModel()->selectedRows();
    if (rows.isEmpty()) return QString();
    const int row = rows.first().row();
    const QTableWidgetItem *item = m_tasks->item(row, 0);
    if (!item) return QString();
    const QString id = item->data(Qt::UserRole).toString();
    return id.isEmpty() ? item->text() : id;
}

const Task *MainWindow::selectedTask() const
{
    const QString id = selectedTaskId();
    for (const Task &task : m_taskCache) if (task.id == id) return &task;
    return nullptr;
}

void MainWindow::refreshSelectedTask()
{
    ProtocolAdapter *current = adapter();
    const QString id = selectedTaskId();
    if (!current || id.isEmpty() || m_connection->state() != ConnectionManager::State::Connected) return;
    const Task *cached = selectedTask();
    if (cached) {
        renderSelectedTask(*cached);
        const Capabilities caps = current->capabilities();
        m_cancelButton->setEnabled(caps.cancel && isCancellable(cached->state));
        m_retryButton->setEnabled(caps.retry && isRetryable(cached->state));
        m_answerButton->setEnabled(caps.answer && cached->state == TaskState::WaitingInput);
    }
    current->getTask(id, guarded<Task>(this, [this, id](const Result<Task> &result) {
        if (!result.ok()) { if (result.error.code != ApiError::NotFound) setStatus(result.error.userMessage(), true); return; }
        for (Task &task : m_taskCache) if (task.id == id) { task = result.value; break; }
        if (selectedTaskId() == id) renderSelectedTask(result.value);
    }));
    current->events(id, m_eventCursor, 0, guarded<EventPage>(this, [this, id](const Result<EventPage> &result) {
        if (!result.ok()) { if (result.error.code != ApiError::Unsupported) m_connection->reportFailure(result.error); return; }
        if (selectedTaskId() != id) return;
        for (const TaskEvent &event : result.value.events) {
            const QString line = QStringLiteral("[%1] %2%3%4\n")
                .arg(event.occurredAt.isValid() ? event.occurredAt.toLocalTime().toString(QStringLiteral("HH:mm:ss")) : QStringLiteral("--:--:--"),
                     event.type,
                     event.actor.isEmpty() ? QString() : QStringLiteral(" · ") + event.actor,
                     event.displayText().isEmpty() ? QString() : QStringLiteral(" — ") + event.displayText());
            m_events->moveCursor(QTextCursor::End);
            m_events->insertPlainText(line);
            m_eventCursor = qMax(m_eventCursor, event.id);
        }
        m_eventCursor = qMax(m_eventCursor, result.value.lastEventId);
    }));
}

void MainWindow::renderSelectedTask(const Task &task)
{
    m_details->setPlainText(taskSummary(task));
    const Capabilities caps = adapter() ? adapter()->capabilities() : Capabilities{};
    m_cancelButton->setEnabled(caps.cancel && isCancellable(task.state));
    m_retryButton->setEnabled(caps.retry && isRetryable(task.state));
    m_answerButton->setEnabled(caps.answer && task.state == TaskState::WaitingInput);
}

void MainWindow::submitTask()
{
    ProtocolAdapter *current = adapter();
    const QString text = m_taskInput->text().trimmed();
    if (!current || text.isEmpty()) return;
    if (!current->capabilities().submit) { setStatus(tr("This connection is read-only"), true); return; }
    SubmitRequest request;
    request.text = text;
    request.agent = m_agentInput->text().trimmed();
    request.idempotencyKey = IdempotencyKey::generate(QStringLiteral("submit"));
    m_submitButton->setEnabled(false);
    current->submit(request, guarded<Task>(this, [this](const Result<Task> &result) {
        m_submitButton->setEnabled(m_connection->state() == ConnectionManager::State::Connected);
        if (!result.ok()) { m_connection->reportFailure(result.error); setStatus(result.error.userMessage(), true); return; }
        m_taskInput->clear();
        setStatus(tr("Task submitted: %1").arg(result.value.id));
        refreshTasks();
    }));
}

void MainWindow::cancelSelectedTask()
{
    ProtocolAdapter *current = adapter(); const QString id = selectedTaskId();
    if (!current || id.isEmpty() || !current->capabilities().cancel) return;
    if (QMessageBox::question(this, tr("Cancel task"), tr("Request cancellation of task %1?").arg(id)) != QMessageBox::Yes) return;
    current->cancel(id, IdempotencyKey::generate(QStringLiteral("cancel")), guarded<CommandResult>(this, [this](const Result<CommandResult> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        setStatus(tr("Cancellation requested")); refreshTasks();
    }));
}

void MainWindow::retrySelectedTask()
{
    ProtocolAdapter *current = adapter(); const QString id = selectedTaskId();
    if (!current || id.isEmpty() || !current->capabilities().retry) return;
    current->retry(id, IdempotencyKey::generate(QStringLiteral("retry")), guarded<CommandResult>(this, [this](const Result<CommandResult> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        setStatus(tr("Retry requested")); refreshTasks();
    }));
}

void MainWindow::answerSelectedTask()
{
    ProtocolAdapter *current = adapter();
    const QString id = selectedTaskId();
    QString answer = m_answerInput->text().trimmed();
    QString optionId;
    const Task *task = selectedTask();
    if (!current || id.isEmpty() || !current->capabilities().answer) return;
    if (answer.isEmpty() && task && task->question.items.size() == 1 && !task->question.items.first().options.isEmpty()) {
        const QList<QuestionOption> options = task->question.items.first().options;
        QStringList labels;
        for (const QuestionOption &option : options) labels << option.label;
        bool ok = false;
        const QString selected = QInputDialog::getItem(this, tr("Answer task question"),
                                                        task->question.items.first().text, labels, 0, false, &ok);
        if (!ok) return;
        const int index = labels.indexOf(selected);
        if (index < 0) return;
        answer = options.at(index).label;
        optionId = options.at(index).id;
    } else if (answer.isEmpty()) {
        bool ok = false;
        answer = QInputDialog::getText(this, tr("Answer task question"),
                                       task && task->question.isValid() ? task->question.summary() : tr("Enter your answer"),
                                       QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || answer.isEmpty()) return;
    }
    current->answer(id, answer, optionId, IdempotencyKey::generate(QStringLiteral("answer")), guarded<CommandResult>(this, [this](const Result<CommandResult> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        m_answerInput->clear(); setStatus(tr("Answer sent")); refreshSelectedTask();
    }));
}

void MainWindow::showArtifacts()
{
    ProtocolAdapter *current = adapter(); const QString id = selectedTaskId();
    if (!current || id.isEmpty() || !current->capabilities().artifacts) return;
    current->listArtifacts(id, guarded<QList<Artifact>>(this, [this](const Result<QList<Artifact>> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        if (result.value.isEmpty()) { QMessageBox::information(this, tr("Artifacts"), tr("No artifacts for this task.")); return; }
        QStringList choices;
        for (const Artifact &artifact : result.value) choices << QStringLiteral("%1 (%2 bytes, SHA-256 %3)").arg(artifact.name).arg(artifact.size).arg(artifact.sha256.isEmpty() ? QStringLiteral("unknown") : artifact.sha256.left(12));
        bool ok = false;
        const QString picked = QInputDialog::getItem(this, tr("Download artifact"), tr("Choose an artifact"), choices, 0, false, &ok);
        if (!ok) return;
        const int index = choices.indexOf(picked); if (index < 0) return;
        const Artifact artifact = result.value.at(index);
        const QString destination = QFileDialog::getSaveFileName(this, tr("Save artifact"), artifact.name);
        if (destination.isEmpty()) return;
        const qint64 maxSize = m_settings.maxDownloadBytes;
        ProtocolAdapter *currentAdapter = adapter();
        if (!currentAdapter) return;
        currentAdapter->downloadArtifact(artifact, destination, maxSize, guarded<DownloadResult>(this, [this](const Result<DownloadResult> &download) {
            if (!download.ok()) { setStatus(download.error.userMessage(), true); QMessageBox::warning(this, tr("Download failed"), download.error.userMessage()); return; }
            setStatus(tr("Downloaded %1 bytes; SHA-256 %2").arg(download.value.bytes).arg(download.value.sha256));
            QMessageBox::information(this, tr("Download complete"), tr("File saved to %1\nSHA-256 verified: %2").arg(download.value.path, download.value.checksumVerified ? tr("yes") : tr("not provided by server")));
        }));
    }));
}

void MainWindow::showNodes()
{
    ProtocolAdapter *current = adapter(); if (!current) return;
    current->listNodes(guarded<QList<Node>>(this, [this](const Result<QList<Node>> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        if (result.value.isEmpty()) { QMessageBox::information(this, tr("Execution nodes"), tr("No nodes are registered.")); return; }
        QStringList choices;
        for (const Node &node : result.value) {
            choices << QStringLiteral("%1 — %2 — %3%4").arg(node.name.isEmpty() ? node.id : node.name, node.status, node.type,
                node.lastSeenAt.isValid() ? QStringLiteral(" — seen ") + node.lastSeenAt.toLocalTime().toString(Qt::ISODate) : QString());
        }
        ProtocolAdapter *currentAdapter = adapter();
        const bool canManage = currentAdapter && currentAdapter->capabilities().manageNodes;
        bool ok = false;
        const QString selected = QInputDialog::getItem(this, tr("Execution nodes"),
            canManage ? tr("Select a node (revoke is available for owners/operators)") : tr("Registered nodes (read-only)"), choices, 0, false, &ok);
        if (!ok) return;
        const int index = choices.indexOf(selected); if (index < 0) return;
        const Node node = result.value.at(index);
        if (!canManage) { QMessageBox::information(this, tr("Node details"), selected); return; }
        if (QMessageBox::question(this, tr("Revoke node"), tr("Revoke access for node %1? This cannot be undone from this dialog.").arg(node.id)) != QMessageBox::Yes) return;
        currentAdapter->revokeNode(node.id, guarded<CommandResult>(this, [this](const Result<CommandResult> &command) {
            if (!command.ok()) { setStatus(command.error.userMessage(), true); return; }
            setStatus(tr("Node revoke requested"));
        }));
    }));
}

void MainWindow::showAgents()
{
    ProtocolAdapter *current = adapter(); if (!current) return;
    current->listAgents(guarded<QList<Agent>>(this, [this](const Result<QList<Agent>> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        QStringList lines;
        for (const Agent &agent : result.value) lines << QStringLiteral("%1 (%2)%3").arg(agent.name, agent.id,
            agent.description.isEmpty() ? QString() : QStringLiteral("\n  ") + agent.description);
        QMessageBox::information(this, tr("Agents"), lines.isEmpty() ? tr("No agents are available.") : lines.join(QLatin1Char('\n')));
    }));
}

void MainWindow::showModels()
{
    ProtocolAdapter *current = adapter(); if (!current) return;
    current->listModels(guarded<QList<ModelInfo>>(this, [this](const Result<QList<ModelInfo>> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        if (result.value.isEmpty()) { QMessageBox::information(this, tr("Models"), tr("No models are registered.")); return; }
        QStringList choices;
        for (const ModelInfo &model : result.value) {
            choices << QStringLiteral("%1 [%2] — %3, %4%5")
                .arg(model.name.isEmpty() ? model.id : model.name, model.kind,
                     model.enabled ? tr("enabled") : tr("disabled"),
                     model.running ? tr("running") : tr("stopped"),
                     model.nodeId.isEmpty() ? QString() : QStringLiteral(" · node ") + model.nodeId);
        }
        ProtocolAdapter *currentAdapter = adapter();
        const bool canManage = currentAdapter && currentAdapter->capabilities().manageModels;
        bool ok = false;
        const QString choice = QInputDialog::getItem(this, tr("Models"),
            canManage ? tr("Choose a model to inspect or manage") : tr("Registered models (read-only)"), choices, 0, false, &ok);
        if (!ok) return;
        const int index = choices.indexOf(choice); if (index < 0) return;
        const ModelInfo model = result.value.at(index);
        if (!canManage) { QMessageBox::information(this, tr("Model details"), choice + (model.lastError.isEmpty() ? QString() : QStringLiteral("\n") + model.lastError)); return; }
        const QStringList actions{tr("Enable"), tr("Disable"), tr("Start"), tr("Stop")};
        const QString actionText = QInputDialog::getItem(this, tr("Manage model"), tr("Action for %1").arg(model.name), actions, 0, false, &ok);
        if (!ok) return;
        ModelAction action = ModelAction::Enable;
        if (actionText == actions.at(1)) action = ModelAction::Disable;
        else if (actionText == actions.at(2)) action = ModelAction::Start;
        else if (actionText == actions.at(3)) action = ModelAction::Stop;
        currentAdapter->manageModel(model, action, guarded<CommandResult>(this, [this](const Result<CommandResult> &command) {
            if (!command.ok()) { setStatus(command.error.userMessage(), true); return; }
            setStatus(tr("Model operation accepted"));
        }));
    }));
}

void MainWindow::showDeadLetters()
{
    ProtocolAdapter *current = adapter(); if (!current) return;
    current->deadLetters(100, guarded<QList<Task>>(this, [this](const Result<QList<Task>> &result) {
        if (!result.ok()) { setStatus(result.error.userMessage(), true); return; }
        QStringList lines;
        for (const Task &task : result.value) lines << QStringLiteral("%1 — %2 — %3").arg(task.id, taskStateDisplayName(task.state), task.error);
        QMessageBox::information(this, tr("Dead letters"), lines.isEmpty() ? tr("No dead-letter tasks.") : lines.join(QLatin1Char('\n')));
    }));
}

void MainWindow::setStatus(const QString &message, bool error)
{
    statusBar()->showMessage(message, error ? 10000 : 5000);
}

} // namespace asgend
