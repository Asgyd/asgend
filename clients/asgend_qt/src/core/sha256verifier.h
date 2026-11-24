// SPDX-License-Identifier: MIT
// Streaming SHA-256 verification for downloaded artifacts.
#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace asgend {

class Sha256Verifier {
public:
    Sha256Verifier();
    void reset();
    void addData(const char *data, qint64 length);
    void addData(const QByteArray &data) { addData(data.constData(), data.size()); }
    qint64 bytesProcessed() const { return m_bytes; }
    // Lowercase hex of the digest; finalises the hash (further addData resets).
    QString hexDigest();

    static bool isValidHex(const QString &hex);
    // Case-insensitive, constant time for equal-length inputs.
    static bool equalHex(const QString &a, const QString &b);
    static QString hexOf(const QByteArray &data);
    // Hashes a file; returns empty string on I/O error.
    static QString hexOfFile(const QString &path);

private:
    QCryptographicHash m_hash;
    qint64 m_bytes = 0;
    bool m_finished = false;
    QString m_digest;
};

} // namespace asgend
