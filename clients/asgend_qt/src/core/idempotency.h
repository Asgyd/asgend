// SPDX-License-Identifier: MIT
// Idempotency keys for mutating commands (CONTRACT_SKETCH §3):
// ^[A-Za-z0-9._:-]{8,128}$, unique per (workspace, operation kind).
#pragma once

#include <QString>

namespace asgend {

class IdempotencyKey {
public:
    // "qtc:<kind>:<ULID>" where ULID = 48-bit ms timestamp + 80 random bits in
    // Crockford base32 (26 chars, time-sortable). kind is sanitised.
    static QString generate(const QString &kind = QString());
    // Generates with an explicit timestamp (tests).
    static QString generateAt(qint64 msecsSinceEpoch, const QString &kind = QString());
    static QString ulid(qint64 msecsSinceEpoch);
    static bool isValid(const QString &key);
};

} // namespace asgend
