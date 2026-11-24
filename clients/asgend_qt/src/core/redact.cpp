// SPDX-License-Identifier: MIT
#include "core/redact.h"

#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace asgend {

namespace {

QMutex &secretsMutex()
{
    static QMutex mutex;
    return mutex;
}

QSet<QString> &secrets()
{
    static QSet<QString> set;
    return set;
}

const QRegularExpression &prefixedToken()
{
    static const QRegularExpression re(QStringLiteral("\\b(ucp|ncp|dlt)_[A-Za-z0-9._~+/=-]+"));
    return re;
}

const QRegularExpression &bearer()
{
    static const QRegularExpression re(QStringLiteral("(Bearer)\\s+[^\\s\"',;]+"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression &jsonMember()
{
    // "key": "value"  (value up to the closing quote; escaped quotes handled)
    static const QRegularExpression re(
        QStringLiteral("(\"(?:auth_token|token|node_token|access_token|refresh_token|api_key|apikey|"
                       "password|secret|ticket|enrollment_code|pairing_code|authorization)\"\\s*:\\s*\")"
                       "(?:[^\"\\\\]|\\\\.)*(\")"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression &queryParam()
{
    static const QRegularExpression re(
        QStringLiteral("([?&](?:token|access_token|auth_token|ticket|api_key)=)[^&\\s#]+"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression &headerLine()
{
    static const QRegularExpression re(
        QStringLiteral("((?:X-AO-Token|X-Api-Key|Cookie|Set-Cookie)\\s*:\\s*)[^\\r\\n]+"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

} // namespace

QString Redactor::redact(const QString &text)
{
    QString out = text;
    {
        QMutexLocker lock(&secretsMutex());
        // Longest first so that a secret containing another one is fully masked.
        QStringList list(secrets().begin(), secrets().end());
        std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
        for (const QString &secret : list) out.replace(secret, QLatin1String(Mask));
    }
    out.replace(jsonMember(), QStringLiteral("\\1***\\2"));
    out.replace(bearer(), QStringLiteral("\\1 ***"));
    out.replace(prefixedToken(), QStringLiteral("\\1_***"));
    out.replace(queryParam(), QStringLiteral("\\1***"));
    out.replace(headerLine(), QStringLiteral("\\1***"));
    return out;
}

void Redactor::addSecret(const QString &secret)
{
    if (secret.size() < 4) return;
    QMutexLocker lock(&secretsMutex());
    secrets().insert(secret);
}

void Redactor::removeSecret(const QString &secret)
{
    QMutexLocker lock(&secretsMutex());
    secrets().remove(secret);
}

void Redactor::clearSecrets()
{
    QMutexLocker lock(&secretsMutex());
    secrets().clear();
}

} // namespace asgend
