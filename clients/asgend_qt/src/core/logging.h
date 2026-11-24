// SPDX-License-Identifier: MIT
// Logging: categories, level filter and a message handler that redacts every
// line (see Redactor) before writing to stderr and an optional log file.
#pragma once

#include <QLoggingCategory>
#include <QString>

Q_DECLARE_LOGGING_CATEGORY(lcNet)
Q_DECLARE_LOGGING_CATEGORY(lcConn)
Q_DECLARE_LOGGING_CATEGORY(lcUi)
Q_DECLARE_LOGGING_CATEGORY(lcStore)

namespace asgend {

class Logging {
public:
    // Levels: "error", "warning", "info", "debug". Unknown -> "info".
    static void setLevel(const QString &level);
    static QString level();
    // Installs the redacting handler. filePath may be empty (stderr only).
    static void install(const QString &filePath = QString());
    static void uninstall();
    // Last lines written (redacted), used by tests and the UI log view.
    static QStringList recentLines();
    static QString defaultLogFile();
};

} // namespace asgend
