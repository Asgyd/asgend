// SPDX-License-Identifier: MIT
#include "core/idempotency.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace asgend {

namespace {
const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
}

QString IdempotencyKey::ulid(qint64 msecsSinceEpoch)
{
    // 128 bits: 48-bit big-endian time, 80 random bits; encoded as 26 chars.
    quint8 bytes[16];
    quint64 time = static_cast<quint64>(msecsSinceEpoch) & 0xFFFFFFFFFFFFULL;
    for (int i = 5; i >= 0; --i) {
        bytes[i] = static_cast<quint8>(time & 0xFF);
        time >>= 8;
    }
    QRandomGenerator *rng = QRandomGenerator::system();
    for (int i = 6; i < 16; ++i) bytes[i] = static_cast<quint8>(rng->bounded(256));

    QString out;
    out.reserve(26);
    // 130 bits of output space; the first character carries the top 3 bits.
    int bitPos = -2; // virtual leading two zero bits
    for (int c = 0; c < 26; ++c) {
        int value = 0;
        for (int b = 0; b < 5; ++b, ++bitPos) {
            int bit = 0;
            if (bitPos >= 0) bit = (bytes[bitPos / 8] >> (7 - bitPos % 8)) & 1;
            value = (value << 1) | bit;
        }
        out += QLatin1Char(kCrockford[value]);
    }
    return out;
}

QString IdempotencyKey::generateAt(qint64 msecsSinceEpoch, const QString &kind)
{
    QString cleanKind;
    for (const QChar ch : kind) {
        if (ch.isLetterOrNumber() && ch.unicode() < 128) cleanKind += ch.toLower();
        else if (ch == QLatin1Char('_') || ch == QLatin1Char('-')) cleanKind += ch;
    }
    cleanKind.truncate(24);
    QString key = QStringLiteral("qtc:");
    if (!cleanKind.isEmpty()) key += cleanKind + QLatin1Char(':');
    key += ulid(msecsSinceEpoch);
    return key;
}

QString IdempotencyKey::generate(const QString &kind)
{
    return generateAt(QDateTime::currentMSecsSinceEpoch(), kind);
}

bool IdempotencyKey::isValid(const QString &key)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._:-]{8,128}$"));
    return re.match(key).hasMatch();
}

} // namespace asgend
