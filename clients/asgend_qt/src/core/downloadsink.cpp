// SPDX-License-Identifier: MIT
#include "core/downloadsink.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace asgend {

namespace {
QString trSink(const char *text)
{
    return QCoreApplication::translate("DownloadSink", text);
}
} // namespace

DownloadSink::DownloadSink(Options options) : m_options(std::move(options)) {}

DownloadSink::~DownloadSink()
{
    abort();
}

bool DownloadSink::open(ApiError *error)
{
    if (m_options.destinationPath.isEmpty()) {
        if (error) *error = ApiError(ApiError::Io, trSink("No destination file selected"));
        return false;
    }
    if (m_options.maxBytes > 0 && m_options.expectedSize > m_options.maxBytes) {
        if (error)
            *error = ApiError(ApiError::SizeLimit, trSink("%1 bytes announced, limit is %2 bytes")
                                                       .arg(m_options.expectedSize).arg(m_options.maxBytes));
        return false;
    }
    const QFileInfo info(m_options.destinationPath);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = ApiError(ApiError::Io, trSink("Cannot create directory %1").arg(info.absolutePath()));
        return false;
    }
    m_file = std::make_unique<QSaveFile>(m_options.destinationPath);
    m_file->setDirectWriteFallback(false);
    if (!m_file->open(QIODevice::WriteOnly)) {
        if (error) *error = ApiError(ApiError::Io, m_file->errorString());
        m_file.reset();
        return false;
    }
    m_sha.reset();
    m_written = 0;
    m_done = false;
    return true;
}

bool DownloadSink::checkAnnouncedSize(qint64 total, ApiError *error) const
{
    if (total < 0) return true;
    if (m_options.maxBytes > 0 && total > m_options.maxBytes) {
        if (error)
            *error = ApiError(ApiError::SizeLimit,
                              trSink("%1 bytes announced, limit is %2 bytes").arg(total).arg(m_options.maxBytes));
        return false;
    }
    if (m_options.expectedSize >= 0 && total != m_options.expectedSize) {
        if (error)
            *error = ApiError(ApiError::Protocol, trSink("Server announced %1 bytes but the artifact has %2 bytes")
                                                      .arg(total).arg(m_options.expectedSize));
        return false;
    }
    return true;
}

bool DownloadSink::write(const char *data, qint64 length, ApiError *error)
{
    if (!m_file || m_done) {
        if (error) *error = ApiError(ApiError::Io, trSink("Download file is not open"));
        return false;
    }
    if (length <= 0) return true;
    if (m_options.maxBytes > 0 && m_written + length > m_options.maxBytes) {
        if (error)
            *error = ApiError(ApiError::SizeLimit,
                              trSink("Download exceeds the limit of %1 bytes").arg(m_options.maxBytes));
        abort();
        return false;
    }
    if (m_file->write(data, length) != length) {
        if (error) *error = ApiError(ApiError::Io, m_file->errorString());
        abort();
        return false;
    }
    m_sha.addData(data, length);
    m_written += length;
    return true;
}

Result<DownloadResult> DownloadSink::finish(const QString &headerSha256)
{
    if (!m_file || m_done)
        return Result<DownloadResult>::failure(ApiError(ApiError::Io, trSink("Download file is not open")));
    DownloadResult result;
    result.path = m_options.destinationPath;
    result.bytes = m_written;
    result.sha256 = m_sha.hexDigest();
    result.expectedSha256 = m_options.expectedSha256.toLower();

    if (m_options.expectedSize >= 0 && m_written != m_options.expectedSize) {
        abort();
        return Result<DownloadResult>::failure(
            ApiError(ApiError::Protocol, trSink("Received %1 bytes, expected %2 bytes").arg(m_written).arg(m_options.expectedSize)));
    }

    QStringList expected;
    if (!m_options.expectedSha256.isEmpty()) expected << m_options.expectedSha256;
    if (!headerSha256.isEmpty()) expected << headerSha256;
    if (expected.isEmpty() && m_options.requireChecksum) {
        abort();
        return Result<DownloadResult>::failure(
            ApiError(ApiError::ChecksumMismatch, trSink("The server did not provide a SHA-256 checksum")));
    }
    for (const QString &candidate : expected) {
        if (!Sha256Verifier::equalHex(candidate, result.sha256)) {
            abort();
            ApiError error(ApiError::ChecksumMismatch,
                           trSink("expected %1, got %2").arg(candidate.toLower(), result.sha256));
            return Result<DownloadResult>::failure(error);
        }
    }
    if (result.expectedSha256.isEmpty() && !headerSha256.isEmpty()) result.expectedSha256 = headerSha256.toLower();
    result.checksumVerified = !expected.isEmpty();
    if (!m_file->commit()) {
        const QString message = m_file->errorString();
        m_file.reset();
        m_done = true;
        return Result<DownloadResult>::failure(ApiError(ApiError::Io, message));
    }
    m_file.reset();
    m_done = true;
    return Result<DownloadResult>::success(result);
}

void DownloadSink::abort()
{
    if (m_file) {
        m_file->cancelWriting();
        m_file.reset(); // QSaveFile removes the temporary file without commit.
    }
    m_done = true;
}

} // namespace asgend
