// SPDX-License-Identifier: MIT
#include "core/credentialstore.h"

#include "core/logging.h"
#include "core/redact.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef ASGEND_QT_HAVE_QTKEYCHAIN
#include <QEventLoop>
#include <qt6keychain/keychain.h>
#endif

namespace asgend {

std::unique_ptr<CredentialStore> CredentialStore::createDefault()
{
#ifdef ASGEND_QT_HAVE_QTKEYCHAIN
    return std::make_unique<KeychainCredentialStore>();
#else
    return std::make_unique<FileCredentialStore>();
#endif
}

// ------------------------------------------------------------------- file

FileCredentialStore::FileCredentialStore(QString path) : m_path(std::move(path)) {}

QString FileCredentialStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/credentials.json");
}

QString FileCredentialStore::warning() const
{
    return QCoreApplication::translate("CredentialStore",
                                       "No system keychain is available: tokens are stored unencrypted in %1 "
                                       "(readable only by your user, mode 0600).")
        .arg(QDir::toNativeSeparators(m_path));
}

bool FileCredentialStore::load(QHash<QString, QString> *tokens, QString *error) const
{
    tokens->clear();
    QFile file(m_path);
    if (!file.exists()) return true;
#ifdef Q_OS_UNIX
    struct stat st;
    const QByteArray native = QFile::encodeName(m_path);
    if (::lstat(native.constData(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            if (error) *error = QStringLiteral("credential file must not be a symlink");
            return false;
        }
        if ((st.st_mode & 077) != 0) {
            qCWarning(lcStore) << "credential file had group/other permissions; tightening to 0600";
            ::chmod(native.constData(), S_IRUSR | S_IWUSR);
        }
    }
#endif
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject list = doc.object().value(QStringLiteral("tokens")).toObject();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it.value().isString()) tokens->insert(it.key(), it.value().toString());
    }
    return true;
}

bool FileCredentialStore::store(const QHash<QString, QString> &tokens, QString *error) const
{
    QJsonObject list;
    for (auto it = tokens.begin(); it != tokens.end(); ++it) list.insert(it.key(), it.value());
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("tokens"), list);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const QFileInfo info(m_path);
    QDir().mkpath(info.absolutePath());
#ifdef Q_OS_UNIX
    // Create the temporary file with 0600 from the start (no permission race),
    // then atomically replace the target.
    const QByteArray target = QFile::encodeName(m_path);
    const QByteArray temp = target + ".tmp";
    ::unlink(temp.constData());
    const int fd = ::open(temp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    qint64 offset = 0;
    while (offset < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + offset, static_cast<size_t>(data.size() - offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
            ::close(fd);
            ::unlink(temp.constData());
            return false;
        }
        offset += n;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(temp.constData(), target.constData()) != 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        ::unlink(temp.constData());
        return false;
    }
    return true;
#else
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(data);
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    QFile::setPermissions(m_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
#endif
}

bool FileCredentialStore::readToken(const QString &profileId, QString *token, QString *error)
{
    QHash<QString, QString> tokens;
    if (!load(&tokens, error)) return false;
    *token = tokens.value(profileId);
    if (!token->isEmpty()) Redactor::addSecret(*token);
    return true;
}

bool FileCredentialStore::writeToken(const QString &profileId, const QString &token, QString *error)
{
    QHash<QString, QString> tokens;
    if (!load(&tokens, error)) return false;
    if (token.isEmpty()) tokens.remove(profileId);
    else tokens.insert(profileId, token);
    if (!token.isEmpty()) Redactor::addSecret(token);
    return store(tokens, error);
}

bool FileCredentialStore::removeToken(const QString &profileId, QString *error)
{
    QHash<QString, QString> tokens;
    if (!load(&tokens, error)) return false;
    if (!tokens.contains(profileId)) return true;
    tokens.remove(profileId);
    return store(tokens, error);
}

// ----------------------------------------------------------------- memory

bool MemoryCredentialStore::readToken(const QString &profileId, QString *token, QString *)
{
    *token = m_tokens.value(profileId);
    if (!token->isEmpty()) Redactor::addSecret(*token);
    return true;
}

bool MemoryCredentialStore::writeToken(const QString &profileId, const QString &token, QString *)
{
    if (token.isEmpty()) m_tokens.remove(profileId);
    else {
        m_tokens.insert(profileId, token);
        Redactor::addSecret(token);
    }
    return true;
}

bool MemoryCredentialStore::removeToken(const QString &profileId, QString *)
{
    m_tokens.remove(profileId);
    return true;
}

// --------------------------------------------------------------- keychain
#ifdef ASGEND_QT_HAVE_QTKEYCHAIN
namespace {
const QString kService = QStringLiteral(ASGEND_QT_APP_ID);

bool runJob(QKeychain::Job *job, QString *error)
{
    QEventLoop loop;
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    if (job->error() != QKeychain::NoError && job->error() != QKeychain::EntryNotFound) {
        if (error) *error = job->errorString();
        return false;
    }
    return true;
}
} // namespace

bool KeychainCredentialStore::readToken(const QString &profileId, QString *token, QString *error)
{
    QKeychain::ReadPasswordJob job(kService);
    job.setAutoDelete(false);
    job.setKey(profileId);
    if (!runJob(&job, error)) return false;
    *token = job.error() == QKeychain::EntryNotFound ? QString() : job.textData();
    if (!token->isEmpty()) Redactor::addSecret(*token);
    return true;
}

bool KeychainCredentialStore::writeToken(const QString &profileId, const QString &token, QString *error)
{
    if (token.isEmpty()) return removeToken(profileId, error);
    Redactor::addSecret(token);
    QKeychain::WritePasswordJob job(kService);
    job.setAutoDelete(false);
    job.setKey(profileId);
    job.setTextData(token);
    return runJob(&job, error);
}

bool KeychainCredentialStore::removeToken(const QString &profileId, QString *error)
{
    QKeychain::DeletePasswordJob job(kService);
    job.setAutoDelete(false);
    job.setKey(profileId);
    return runJob(&job, error);
}
#endif

} // namespace asgend
