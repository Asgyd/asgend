// SPDX-License-Identifier: MIT
// Writes a streamed download to a temporary file next to the destination,
// hashing on the fly and enforcing a size limit. The destination file appears
// only after a successful checksum verification (atomic rename via QSaveFile).
#pragma once

#include "core/apierror.h"
#include "core/models.h"
#include "core/sha256verifier.h"

#include <QSaveFile>
#include <QString>

#include <memory>

namespace asgend {

class DownloadSink {
public:
    struct Options {
        QString destinationPath;
        qint64 maxBytes = 256LL * 1024 * 1024;
        qint64 expectedSize = -1;   // -1 unknown
        QString expectedSha256;     // empty unknown
        bool requireChecksum = true;
    };

    explicit DownloadSink(Options options);
    ~DownloadSink();

    bool open(ApiError *error);
    // Announced total (e.g. Content-Length); fails early if over the limit or
    // inconsistent with the expected size.
    bool checkAnnouncedSize(qint64 total, ApiError *error) const;
    bool write(const char *data, qint64 length, ApiError *error);
    bool write(const QByteArray &data, ApiError *error) { return write(data.constData(), data.size(), error); }
    qint64 bytesWritten() const { return m_written; }
    // Verifies size and checksum(s); commits on success, discards otherwise.
    // headerSha256 is an additional expected checksum (X-Checksum-SHA256).
    Result<DownloadResult> finish(const QString &headerSha256 = QString());
    void abort();

private:
    Options m_options;
    std::unique_ptr<QSaveFile> m_file;
    Sha256Verifier m_sha;
    qint64 m_written = 0;
    bool m_done = false;
};

} // namespace asgend
