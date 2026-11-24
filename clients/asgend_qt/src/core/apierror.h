// SPDX-License-Identifier: MIT
// Unified error model shared by the core (JSON-line) and Control Plane (HTTPS)
// adapters. The UI only ever sees ApiError, never raw protocol failures.
#pragma once

#include <QCoreApplication>
#include <QString>

#include <functional>

namespace asgend {

struct ApiError {
    Q_DECLARE_TR_FUNCTIONS(ApiError)
public:
    enum Code {
        None = 0,
        Network,          // connection refused / reset / host not found
        Timeout,          // request deadline exceeded
        Tls,              // certificate or handshake failure (never ignored)
        Insecure,         // configuration rejected by the security policy
        Unauthorized,     // 401 / core "unauthorized"
        Forbidden,        // 403
        NotFound,         // 404
        Conflict,         // 409 (wrong task state, key reuse with other body)
        Gone,             // 410 (expired ticket/command)
        PayloadTooLarge,  // 413
        ChecksumMismatch, // 422 checksum_mismatch or local verification failure
        Unprocessable,    // 422
        RateLimited,      // 429 rate_limited (retryAfterMs set when known)
        QuotaExceeded,    // 429 quota_exceeded
        Internal,         // 500
        Unavailable,      // 503 / core queue full
        InvalidRequest,   // 400 / core validation error
        Protocol,         // malformed reply, unexpected redirect, oversized reply
        SizeLimit,        // local download size limit exceeded
        Io,               // local file system error
        Unsupported,      // operation not offered by this backend/version
        Cancelled         // aborted by the client
    };

    Code code = None;
    QString message;       // human readable, already redacted
    int httpStatus = 0;    // 0 for non-HTTP transports
    int retryAfterMs = -1; // from Retry-After, -1 when absent
    QString serverCode;    // raw "error.code" from the Control Plane
    QString requestId;     // X-Request-ID / error.request_id

    ApiError() = default;
    ApiError(Code c, QString msg) : code(c), message(std::move(msg)) {}

    bool isError() const { return code != None; }
    // Transient connectivity loss: the connection manager reconnects with backoff.
    bool isConnectivity() const { return code == Network || code == Timeout || code == Unavailable; }
    // The credential is not valid; reconnecting with the same token is pointless.
    bool isAuth() const { return code == Unauthorized; }

    static QString codeName(Code c);
    // Maps a Control Plane HTTP status + error.code (CONTRACT_SKETCH §2).
    static Code fromHttp(int status, const QString &serverCode);
    // Maps a core {"ok":false,"error":"..."} message (docs/PROTOCOL.md).
    static Code fromCoreMessage(const QString &message);
    // Localised text suitable for a status bar or message box.
    QString userMessage() const;
};

template <typename T>
struct Result {
    T value{};
    ApiError error;
    bool ok() const { return !error.isError(); }
    static Result success(T v) { Result r; r.value = std::move(v); return r; }
    static Result failure(ApiError e) { Result r; r.error = std::move(e); return r; }
};

template <typename T>
using Callback = std::function<void(const Result<T> &)>;

} // namespace asgend
