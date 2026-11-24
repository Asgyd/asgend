// SPDX-License-Identifier: MIT
// Log redaction. Every log line passes through Redactor::redact() before it
// is written anywhere; tokens are never logged (roadmap Q1.2 item 6).
#pragma once

#include <QString>
#include <QStringList>

namespace asgend {

class Redactor {
public:
    static constexpr const char *Mask = "***";

    // Pattern-based redaction of well-known secret shapes:
    //  - ASGEND token prefixes ucp_/ncp_/dlt_ (any context, incl. URL paths),
    //  - "Authorization: Bearer …" / "Bearer …",
    //  - JSON members auth_token/token/node_token/api_key/password/secret/…,
    //  - query parameters token=/access_token=/ticket=,
    //  - plus every exact secret registered with addSecret().
    static QString redact(const QString &text);

    // Registers a runtime secret (e.g. the core auth_token, which has no
    // recognisable prefix). Secrets shorter than 4 characters are ignored.
    static void addSecret(const QString &secret);
    static void removeSecret(const QString &secret);
    static void clearSecrets();
};

} // namespace asgend
