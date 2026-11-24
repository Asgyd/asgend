// SPDX-License-Identifier: MIT
// Credential storage. QtKeychain (Secret Service / KWallet) is used when it was
// found at configure time; otherwise a 0600 JSON file in AppConfigLocation is
// used and the UI shows a visible warning (isSecure() == false).
#pragma once

#include <QHash>
#include <QString>

#include <memory>

namespace asgend {

class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    virtual QString backendName() const = 0;
    virtual bool isSecure() const = 0;
    virtual QString warning() const { return QString(); }
    virtual bool readToken(const QString &profileId, QString *token, QString *error) = 0;
    virtual bool writeToken(const QString &profileId, const QString &token, QString *error) = 0;
    virtual bool removeToken(const QString &profileId, QString *error) = 0;

    // QtKeychain when available, the file store otherwise.
    static std::unique_ptr<CredentialStore> createDefault();
};

class FileCredentialStore final : public CredentialStore {
public:
    explicit FileCredentialStore(QString path = defaultPath());
    static QString defaultPath();

    QString backendName() const override { return QStringLiteral("file"); }
    bool isSecure() const override { return false; }
    QString warning() const override;
    bool readToken(const QString &profileId, QString *token, QString *error) override;
    bool writeToken(const QString &profileId, const QString &token, QString *error) override;
    bool removeToken(const QString &profileId, QString *error) override;
    QString path() const { return m_path; }

private:
    bool load(QHash<QString, QString> *tokens, QString *error) const;
    bool store(const QHash<QString, QString> &tokens, QString *error) const;
    QString m_path;
};

class MemoryCredentialStore final : public CredentialStore {
public:
    QString backendName() const override { return QStringLiteral("memory"); }
    bool isSecure() const override { return true; }
    bool readToken(const QString &profileId, QString *token, QString *) override;
    bool writeToken(const QString &profileId, const QString &token, QString *) override;
    bool removeToken(const QString &profileId, QString *) override;

private:
    QHash<QString, QString> m_tokens;
};

#ifdef ASGEND_QT_HAVE_QTKEYCHAIN
class KeychainCredentialStore final : public CredentialStore {
public:
    QString backendName() const override { return QStringLiteral("qtkeychain"); }
    bool isSecure() const override { return true; }
    bool readToken(const QString &profileId, QString *token, QString *error) override;
    bool writeToken(const QString &profileId, const QString &token, QString *error) override;
    bool removeToken(const QString &profileId, QString *error) override;
};
#endif

} // namespace asgend
