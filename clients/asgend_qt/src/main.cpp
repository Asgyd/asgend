// SPDX-License-Identifier: MIT
#include "core/logging.h"
#include "ui/mainwindow.h"

#include <QApplication>
#include <QDir>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ASGEND"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("asgend.org"));
    QCoreApplication::setApplicationName(QStringLiteral("ASGEND Qt Client"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(ASGEND_QT_VERSION));
    asgend::Logging::install();
    asgend::MainWindow window;
    window.show();
    const int result = app.exec();
    asgend::Logging::uninstall();
    return result;
}
