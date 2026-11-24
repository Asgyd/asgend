// SPDX-License-Identifier: MIT
#include "core/sha256verifier.h"

#include <QFile>

namespace asgend {

Sha256Verifier::Sha256Verifier() : m_hash(QCryptographicHash::Sha256) {}

void Sha256Verifier::reset()
{
    m_hash.reset();
    m_bytes = 0;
    m_finished = false;
    m_digest.clear();
}

void Sha256Verifier::addData(const char *data, qint64 length)
{
    if (m_finished) reset();
    if (length <= 0) return;
    m_hash.addData(QByteArrayView(data, static_cast<qsizetype>(length)));
    m_bytes += length;
}

QString Sha256Verifier::hexDigest()
{
    if (!m_finished) {
        m_digest = QString::fromLatin1(m_hash.result().toHex());
        m_finished = true;
    }
    return m_digest;
}

bool Sha256Verifier::isValidHex(const QString &hex)
{
    if (hex.size() != 64) return false;
    for (const QChar ch : hex) {
        const ushort u = ch.unicode();
        const bool digit = u >= '0' && u <= '9';
        const bool lower = u >= 'a' && u <= 'f';
        const bool upper = u >= 'A' && u <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

bool Sha256Verifier::equalHex(const QString &a, const QString &b)
{
    if (!isValidHex(a) || !isValidHex(b)) return false;
    const QByteArray x = a.toLatin1().toLower();
    const QByteArray y = b.toLatin1().toLower();
    unsigned char diff = 0;
    for (int i = 0; i < x.size(); ++i) diff |= static_cast<unsigned char>(x.at(i) ^ y.at(i));
    return diff == 0;
}

QString Sha256Verifier::hexOf(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString Sha256Verifier::hexOfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return QString();
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace asgend
