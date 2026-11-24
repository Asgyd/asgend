"""Content-addressed artifact blob store on a filesystem volume.

Layout under ``data_dir``::

    tmp/<artifact_id>.part        resumable upload in progress
    blobs/sha256/ab/cd/<sha256>   verified, immutable content

Blobs never live in the database; the DB keeps size/sha256/blob_key only.
Paths are derived exclusively from server-generated ids and verified hex
digests, never from client-supplied names.
"""

from __future__ import annotations

import hashlib
import os
import re
import threading

_ID_RE = re.compile(r"^art_[0-9A-Z]{26}$")
_HEX_RE = re.compile(r"^[0-9a-f]{64}$")


class BlobStore:
    def __init__(self, root: str):
        self.root = os.path.abspath(root)
        self.tmp_dir = os.path.join(self.root, "tmp")
        self.blob_dir = os.path.join(self.root, "blobs", "sha256")
        os.makedirs(self.tmp_dir, exist_ok=True)
        os.makedirs(self.blob_dir, exist_ok=True)
        self._locks: dict = {}
        self._locks_lock = threading.Lock()

    def lock_for(self, artifact_id: str) -> threading.Lock:
        with self._locks_lock:
            lk = self._locks.get(artifact_id)
            if lk is None:
                lk = self._locks[artifact_id] = threading.Lock()
            return lk

    def drop_lock(self, artifact_id: str) -> None:
        with self._locks_lock:
            self._locks.pop(artifact_id, None)

    def tmp_path(self, artifact_id: str) -> str:
        if not _ID_RE.match(artifact_id):
            raise ValueError("bad artifact id")
        return os.path.join(self.tmp_dir, artifact_id + ".part")

    def blob_path(self, sha256: str) -> str:
        if not _HEX_RE.match(sha256):
            raise ValueError("bad digest")
        return os.path.join(self.blob_dir, sha256[:2], sha256[2:4], sha256)

    def tmp_size(self, artifact_id: str) -> int:
        try:
            return os.path.getsize(self.tmp_path(artifact_id))
        except OSError:
            return 0

    def write_chunk(self, artifact_id: str, offset: int, stream, length: int,
                    bufsize: int = 1024 * 1024) -> int:
        """Write ``length`` bytes from ``stream`` at ``offset``; returns bytes written."""
        path = self.tmp_path(artifact_id)
        mode = "r+b" if os.path.exists(path) else "wb"
        written = 0
        with open(path, mode) as fh:
            fh.seek(offset)
            fh.truncate(offset)
            while written < length:
                buf = stream.read(min(bufsize, length - written))
                if not buf:
                    break
                fh.write(buf)
                written += len(buf)
            fh.flush()
            os.fsync(fh.fileno())
        return written

    def truncate(self, artifact_id: str, size: int) -> None:
        path = self.tmp_path(artifact_id)
        if os.path.exists(path):
            with open(path, "r+b") as fh:
                fh.truncate(size)

    def hash_tmp(self, artifact_id: str) -> tuple:
        h = hashlib.sha256()
        n = 0
        path = self.tmp_path(artifact_id)
        if not os.path.exists(path):
            return h.hexdigest(), 0
        with open(path, "rb") as fh:
            while True:
                buf = fh.read(1024 * 1024)
                if not buf:
                    break
                h.update(buf)
                n += len(buf)
        return h.hexdigest(), n

    def commit(self, artifact_id: str, sha256: str) -> str:
        """Move a verified upload to its content address (temp+rename)."""
        final = self.blob_path(sha256)
        os.makedirs(os.path.dirname(final), exist_ok=True)
        src = self.tmp_path(artifact_id)
        if not os.path.exists(src):
            open(src, "wb").close()  # zero-length artifact
        if os.path.exists(final):
            os.remove(src)            # identical content already stored
        else:
            os.replace(src, final)
        return sha256

    def discard_tmp(self, artifact_id: str) -> None:
        try:
            os.remove(self.tmp_path(artifact_id))
        except (OSError, ValueError):
            pass

    def delete_blob(self, sha256: str) -> None:
        try:
            os.remove(self.blob_path(sha256))
        except (OSError, ValueError):
            pass

    def open_blob(self, sha256: str):
        return open(self.blob_path(sha256), "rb")

    def blob_exists(self, sha256: str) -> bool:
        try:
            return os.path.exists(self.blob_path(sha256))
        except ValueError:
            return False

    def verify_blob(self, sha256: str) -> bool:
        h = hashlib.sha256()
        try:
            with self.open_blob(sha256) as fh:
                for buf in iter(lambda: fh.read(1024 * 1024), b""):
                    h.update(buf)
        except OSError:
            return False
        return h.hexdigest() == sha256

    def list_tmp(self) -> list:
        try:
            return [f[:-5] for f in os.listdir(self.tmp_dir) if f.endswith(".part")]
        except OSError:
            return []
