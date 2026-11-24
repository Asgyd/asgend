// SPDX-License-Identifier: MIT
#include "core/logging.h"

#include "core/redact.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStringList>

#include <cstdio>

Q_LOGGING_CATEGORY(lcNet, "asgend.net", QtInfoMsg)
Q_LOGGING_CATEGORY(lcConn, "asgend.connection", QtInfoMsg)
Q_LOGGING_CATEGORY(lcUi, "asgend.ui", QtInfoMsg)
Q_LOGGING_CATEGORY(lcStore, "asgend.store", QtInfoMsg)

namespace asgend {

namespace {

struct LogState {
    QMutex mutex;
    QString level = QStringLiteral("info");
    QFile file;
    QStringList recent;
    QtMessageHandler previous = nullptr;
    bool installed = false;
};

LogState &state()
{
    static LogState s;
    return s;
}

const char *typeName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info";
    case QtWarningMsg: return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg: return "fatal";
    }
    return "log";
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const QString line = QStringLiteral("%1 [%2] %3: %4")
                             .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                                  QLatin1String(typeName(type)),
                                  QLatin1String(context.category ? context.category : "default"),
                                  Redactor::redact(message));
    LogState &s = state();
    {
        QMutexLocker lock(&s.mutex);
        s.recent.append(line);
        while (s.recent.size() > 500) s.recent.removeFirst();
        if (s.file.isOpen()) {
            s.file.write(line.toUtf8());
            s.file.write("\n");
            s.file.flush();
        }
    }
    const QByteArray bytes = line.toLocal8Bit();
    std::fprintf(stderr, "%s\n", bytes.constData());
    std::fflush(stderr);
    if (type == QtFatalMsg) std::abort();
}

} // namespace

void Logging::setLevel(const QString &level)
{
    static const QStringList known = {QStringLiteral("error"), QStringLiteral("warning"),
                                      QStringLiteral("info"), QStringLiteral("debug")};
    const QString normalized = known.contains(level) ? level : QStringLiteral("info");
    {
        QMutexLocker lock(&state().mutex);
        state().level = normalized;
    }
    QString rules = QStringLiteral("asgend.*.debug=%1\nasgend.*.info=%2\nasgend.*.warning=%3\n")
                        .arg(normalized == QLatin1String("debug") ? QStringLiteral("true") : QStringLiteral("false"),
                             (normalized == QLatin1String("debug") || normalized == QLatin1String("info"))
                                 ? QStringLiteral("true") : QStringLiteral("false"),
                             normalized == QLatin1String("error") ? QStringLiteral("false") : QStringLiteral("true"));
    QLoggingCategory::setFilterRules(rules);
}

QString Logging::level()
{
    QMutexLocker lock(&state().mutex);
    return state().level;
}

void Logging::install(const QString &filePath)
{
    LogState &s = state();
    {
        QMutexLocker lock(&s.mutex);
        if (s.file.isOpen()) s.file.close();
        if (!filePath.isEmpty()) {
            QDir().mkpath(QFileInfo(filePath).absolutePath());
            s.file.setFileName(filePath);
            if (s.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
                s.file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
        if (s.installed) return;
        s.installed = true;
    }
    s.previous = qInstallMessageHandler(handler);
}

void Logging::uninstall()
{
    LogState &s = state();
    QMutexLocker lock(&s.mutex);
    if (!s.installed) return;
    qInstallMessageHandler(s.previous);
    s.installed = false;
    if (s.file.isOpen()) s.file.close();
}

QStringList Logging::recentLines()
{
    QMutexLocker lock(&state().mutex);
    return state().recent;
}

QString Logging::defaultLogFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
           QStringLiteral("/logs/asgend-qt.log");
}

} // namespace asgend
