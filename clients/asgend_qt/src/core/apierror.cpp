// SPDX-License-Identifier: MIT
#include "core/apierror.h"

namespace asgend {

QString ApiError::codeName(Code c)
{
    switch (c) {
    case None: return QStringLiteral("none");
    case Network: return QStringLiteral("network");
    case Timeout: return QStringLiteral("timeout");
    case Tls: return QStringLiteral("tls");
    case Insecure: return QStringLiteral("insecure");
    case Unauthorized: return QStringLiteral("unauthorized");
    case Forbidden: return QStringLiteral("forbidden");
    case NotFound: return QStringLiteral("not_found");
    case Conflict: return QStringLiteral("conflict");
    case Gone: return QStringLiteral("gone");
    case PayloadTooLarge: return QStringLiteral("payload_too_large");
    case ChecksumMismatch: return QStringLiteral("checksum_mismatch");
    case Unprocessable: return QStringLiteral("unprocessable");
    case RateLimited: return QStringLiteral("rate_limited");
    case QuotaExceeded: return QStringLiteral("quota_exceeded");
    case Internal: return QStringLiteral("internal");
    case Unavailable: return QStringLiteral("unavailable");
    case InvalidRequest: return QStringLiteral("invalid_request");
    case Protocol: return QStringLiteral("protocol");
    case SizeLimit: return QStringLiteral("size_limit");
    case Io: return QStringLiteral("io");
    case Unsupported: return QStringLiteral("unsupported");
    case Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("unknown");
}

ApiError::Code ApiError::fromHttp(int status, const QString &serverCode)
{
    // The machine-readable code wins over the status where both are present.
    static const struct { const char *name; Code code; } table[] = {
        {"invalid_request", InvalidRequest}, {"unauthorized", Unauthorized},
        {"forbidden", Forbidden}, {"not_found", NotFound}, {"conflict", Conflict},
        {"gone", Gone}, {"payload_too_large", PayloadTooLarge},
        {"checksum_mismatch", ChecksumMismatch}, {"unprocessable", Unprocessable},
        {"rate_limited", RateLimited}, {"quota_exceeded", QuotaExceeded},
        {"internal", Internal}, {"unavailable", Unavailable},
    };
    for (const auto &entry : table) {
        if (serverCode == QLatin1String(entry.name)) return entry.code;
    }
    switch (status) {
    case 400: return InvalidRequest;
    case 401: return Unauthorized;
    case 403: return Forbidden;
    case 404: return NotFound;
    case 409: return Conflict;
    case 410: return Gone;
    case 413: return PayloadTooLarge;
    case 422: return Unprocessable;
    case 429: return RateLimited;
    case 502:
    case 503:
    case 504: return Unavailable;
    default: break;
    }
    if (status >= 500) return Internal;
    if (status >= 400) return InvalidRequest;
    if (status >= 300) return Protocol;
    return None;
}

ApiError::Code ApiError::fromCoreMessage(const QString &message)
{
    const QString m = message.toLower();
    if (m == QLatin1String("unauthorized")) return Unauthorized;
    if (m.startsWith(QLatin1String("unsupported op"))) return Unsupported;
    if (m.contains(QLatin1String("queue is full"))) return Unavailable;
    if (m.contains(QLatin1String("already terminal")) || m.contains(QLatin1String("not a failed")) ||
        m.contains(QLatin1String("not waiting")) || m.contains(QLatin1String("no pending")))
        return Conflict;
    if (m.contains(QLatin1String("not found")) || m.contains(QLatin1String("does not exist")) ||
        m.contains(QLatin1String("unknown task")))
        return NotFound;
    return InvalidRequest;
}

QString ApiError::userMessage() const
{
    QString base;
    switch (code) {
    case None: return QString();
    case Network: base = tr("Cannot reach the server"); break;
    case Timeout: base = tr("The request timed out"); break;
    case Tls: base = tr("Secure connection failed (certificate not trusted)"); break;
    case Insecure: base = tr("Connection rejected by the security policy"); break;
    case Unauthorized: base = tr("Not authorized: the token is missing, expired or revoked"); break;
    case Forbidden: base = tr("Permission denied for this operation"); break;
    case NotFound: base = tr("Not found (or not visible in this workspace)"); break;
    case Conflict: base = tr("The task is not in a state that allows this action"); break;
    case Gone: base = tr("The link or command has expired"); break;
    case PayloadTooLarge: base = tr("The request is too large"); break;
    case ChecksumMismatch: base = tr("Checksum mismatch: the file was rejected"); break;
    case Unprocessable: base = tr("The server could not process the request"); break;
    case RateLimited:
        base = retryAfterMs >= 0 ? tr("Too many requests; retry in %n second(s)", nullptr, (retryAfterMs + 999) / 1000)
                                 : tr("Too many requests; retry later");
        break;
    case QuotaExceeded: base = tr("Quota exceeded"); break;
    case Internal: base = tr("Internal server error"); break;
    case Unavailable: base = tr("The service is temporarily unavailable"); break;
    case InvalidRequest: base = tr("The request was rejected"); break;
    case Protocol: base = tr("Unexpected reply from the server"); break;
    case SizeLimit: base = tr("The file exceeds the configured download size limit"); break;
    case Io: base = tr("Local file error"); break;
    case Unsupported: base = tr("Not supported by this server"); break;
    case Cancelled: base = tr("Cancelled"); break;
    }
    if (!message.isEmpty() && message != base) return base + QStringLiteral(": ") + message;
    return base;
}

} // namespace asgend
