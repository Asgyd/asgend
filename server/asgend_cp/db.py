"""Storage abstraction: one portable SQL layer, two backends.

* ``sqlite:///path`` -- development/tests (WAL, synchronous=FULL, foreign keys).
* ``postgresql://user@host:5432/db`` -- production; psycopg 3 is imported
  lazily so the SQLite path stays standard-library only.

SQL is written with ``?`` placeholders; the PostgreSQL backend translates
them to ``%s``. Write transactions are serialized per workspace through the
``workspaces.event_seq`` row (SQLite: ``BEGIN IMMEDIATE``; PostgreSQL: row
lock taken by ``UPDATE .. RETURNING``), which makes event_id allocation and
commit order identical -- long-poll cursors never skip an event.
"""

from __future__ import annotations

import contextlib
import os
import queue
import sqlite3
import threading
import time

from .migrations import LATEST_VERSION, MIGRATIONS, SCHEMA_TABLE


class SchemaError(Exception):
    pass


class StorageError(Exception):
    pass


def translate_pg(sql: str) -> str:
    """Translate qmark parameters without changing quoted SQL text.

    Psycopg uses ``%s`` parameters and requires literal percent signs to be
    doubled when a parameter sequence is supplied. A global ``replace('?')``
    corrupts string literals such as ``'what?'`` and comments, so scan SQL
    lexical contexts and replace ``?`` only in normal SQL code.
    """
    out: list[str] = []
    i = 0
    state = "normal"  # normal, single, double, line_comment, block_comment, dollar
    dollar_delim = ""
    while i < len(sql):
        ch = sql[i]
        nxt = sql[i + 1] if i + 1 < len(sql) else ""

        if state == "normal":
            if ch == "'":
                state = "single"
                out.append(ch)
                i += 1
                continue
            if ch == '"':
                state = "double"
                out.append(ch)
                i += 1
                continue
            if ch == "-" and nxt == "-":
                state = "line_comment"
                out.append("--")
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block_comment"
                out.append("/*")
                i += 2
                continue
            if ch == "$":  # PostgreSQL dollar-quoted body: $tag$...$tag$
                end = sql.find("$", i + 1)
                if end != -1:
                    tag = sql[i + 1:end]
                    if not tag or (tag[0].isalpha() or tag[0] == "_") and all(
                            c.isalnum() or c == "_" for c in tag):
                        dollar_delim = sql[i:end + 1]
                        state = "dollar"
                        out.append(dollar_delim)
                        i = end + 1
                        continue
            if ch == "?":
                out.append("%s")
            elif ch == "%":
                out.append("%%")
            else:
                out.append(ch)
            i += 1
            continue

        if ch == "%":
            out.append("%%")
            i += 1
            continue
        if state == "single":
            out.append(ch)
            i += 1
            if ch == "\\" and i < len(sql):  # E'...' escape strings
                out.append(sql[i])
                i += 1
            elif ch == "'":
                if i < len(sql) and sql[i] == "'":  # SQL doubled quote
                    out.append(sql[i])
                    i += 1
                else:
                    state = "normal"
            continue
        if state == "double":
            out.append(ch)
            i += 1
            if ch == '"':
                if i < len(sql) and sql[i] == '"':
                    out.append(sql[i])
                    i += 1
                else:
                    state = "normal"
            continue
        if state == "line_comment":
            out.append(ch)
            i += 1
            if ch == "\n":
                state = "normal"
            continue
        if state == "block_comment":
            if ch == "*" and nxt == "/":
                out.append("*/")
                i += 2
                state = "normal"
            else:
                out.append(ch)
                i += 1
            continue
        if state == "dollar":
            if sql.startswith(dollar_delim, i):
                out.append(dollar_delim)
                i += len(dollar_delim)
                state = "normal"
            else:
                out.append(ch)
                i += 1
            continue

    return "".join(out)


class Tx:
    def __init__(self, raw, dialect: str):
        self.raw = raw
        self.dialect = dialect
        self.after_commit: list = []

    def _sql(self, sql: str) -> str:
        return translate_pg(sql) if self.dialect == "postgresql" else sql

    def q(self, sql: str, params=()) -> list:
        cur = self.raw.execute(self._sql(sql), tuple(params))
        if cur.description is None:
            return []
        rows = cur.fetchall()
        if self.dialect == "sqlite":
            return [dict(r) for r in rows]
        return [dict(r) for r in rows]

    def q1(self, sql: str, params=()):
        rows = self.q(sql, params)
        return rows[0] if rows else None

    def x(self, sql: str, params=()) -> int:
        cur = self.raw.execute(self._sql(sql), tuple(params))
        return cur.rowcount

    @property
    def for_update(self) -> str:
        return " FOR UPDATE" if self.dialect == "postgresql" else ""

    @property
    def skip_locked(self) -> str:
        return " FOR UPDATE SKIP LOCKED" if self.dialect == "postgresql" else ""


class Storage:
    dialect = "abstract"

    def __init__(self, pool_size: int = 16):
        self._pool: queue.LifoQueue = queue.LifoQueue()
        self._pool_size = max(1, pool_size)
        self._closed = False
        self._lock = threading.Lock()

    # -- backend hooks -------------------------------------------------------
    def _connect(self):  # pragma: no cover - abstract
        raise NotImplementedError

    def _begin(self, raw) -> None:  # pragma: no cover - abstract
        raise NotImplementedError

    def _commit(self, raw) -> None:
        raw.execute("COMMIT")

    def _rollback(self, raw) -> None:
        try:
            raw.execute("ROLLBACK")
        except Exception:
            pass

    def has_table(self, t: Tx, name: str) -> bool:  # pragma: no cover
        raise NotImplementedError

    def _lock_migrations(self, t: Tx) -> None:
        pass

    # -- pooling -------------------------------------------------------------
    @contextlib.contextmanager
    def _conn(self):
        if self._closed:
            raise StorageError("storage closed")
        try:
            raw = self._pool.get_nowait()
        except queue.Empty:
            raw = self._connect()
        ok = False
        try:
            yield raw
            ok = True
        finally:
            if ok and not self._closed and self._pool.qsize() < self._pool_size:
                self._pool.put(raw)
            else:
                try:
                    raw.close()
                except Exception:
                    pass

    @contextlib.contextmanager
    def tx(self):
        """Read-write transaction; after_commit callbacks run after COMMIT."""
        with self._conn() as raw:
            t = Tx(raw, self.dialect)
            self._begin(raw)
            try:
                yield t
            except BaseException:
                self._rollback(raw)
                raise
            self._commit(raw)
        for cb in t.after_commit:
            try:
                cb()
            except Exception:
                pass

    @contextlib.contextmanager
    def read(self):
        """Autocommit reads (each statement sees committed data)."""
        with self._conn() as raw:
            yield Tx(raw, self.dialect)

    def close(self) -> None:
        self._closed = True
        while True:
            try:
                self._pool.get_nowait().close()
            except queue.Empty:
                break
            except Exception:
                pass

    def ping(self) -> bool:
        try:
            with self.read() as t:
                t.q("SELECT 1 AS ok")
            return True
        except Exception:
            return False

    # -- schema --------------------------------------------------------------
    def schema_version(self) -> int:
        with self.read() as t:
            if not self.has_table(t, "schema_migrations"):
                return 0
            row = t.q1("SELECT MAX(version) AS v FROM schema_migrations")
            return int(row["v"] or 0) if row else 0

    def applied_migrations(self) -> list:
        with self.read() as t:
            if not self.has_table(t, "schema_migrations"):
                return []
            return t.q("SELECT version, name, applied_at FROM schema_migrations ORDER BY version")

    def migrate(self, target: int | None = None) -> list:
        """Apply pending migrations (each in its own transaction). Returns versions applied."""
        target = LATEST_VERSION if target is None else int(target)
        if target > LATEST_VERSION:
            raise SchemaError(f"unknown target schema version {target}")
        current = self.schema_version()
        if current > LATEST_VERSION:
            raise SchemaError(
                f"database schema v{current} is newer than this server supports (v{LATEST_VERSION})")
        applied = []
        for m in MIGRATIONS:
            if m.version <= current or m.version > target:
                continue
            with self.tx() as t:
                self._lock_migrations(t)
                t.x(SCHEMA_TABLE)
                if t.q1("SELECT version FROM schema_migrations WHERE version = ?", (m.version,)):
                    continue  # applied concurrently by another process
                for stmt in m.statements:
                    t.x(stmt)
                t.x("INSERT INTO schema_migrations (version, name, applied_at) VALUES (?, ?, ?)",
                    (m.version, m.name, int(time.time() * 1000)))
            applied.append(m.version)
        return applied

    def check_schema(self) -> int:
        v = self.schema_version()
        if v > LATEST_VERSION:
            raise SchemaError(
                f"database schema v{v} is newer than this server supports (v{LATEST_VERSION}); "
                "refusing to start")
        if v < LATEST_VERSION:
            raise SchemaError(
                f"database schema v{v} is older than v{LATEST_VERSION}; run 'migrate' first")
        return v


class SQLiteStorage(Storage):
    dialect = "sqlite"

    def __init__(self, path: str, pool_size: int = 16):
        super().__init__(pool_size)
        self.path = path
        d = os.path.dirname(os.path.abspath(path))
        os.makedirs(d, exist_ok=True)

    def _connect(self):
        raw = sqlite3.connect(self.path, timeout=30.0, isolation_level=None,
                              check_same_thread=False)
        raw.row_factory = sqlite3.Row
        raw.execute("PRAGMA journal_mode=WAL")
        raw.execute("PRAGMA synchronous=FULL")
        raw.execute("PRAGMA foreign_keys=ON")
        raw.execute("PRAGMA busy_timeout=30000")
        return raw

    def _begin(self, raw) -> None:
        raw.execute("BEGIN IMMEDIATE")

    def has_table(self, t: Tx, name: str) -> bool:
        return t.q1("SELECT name FROM sqlite_master WHERE type='table' AND name=?", (name,)) is not None

    def backup_to(self, dest_path: str) -> None:
        """Online consistent copy using the SQLite backup API."""
        with self._conn() as raw:
            dst = sqlite3.connect(dest_path)
            try:
                raw.backup(dst)
            finally:
                dst.close()


class PostgresStorage(Storage):
    dialect = "postgresql"

    def __init__(self, url: str, password: str = "", pool_size: int = 16):
        super().__init__(pool_size)
        self.url = url
        self.password = password
        try:
            import psycopg  # noqa: F401  (lazy import: only the PG backend needs it)
        except ImportError as exc:  # pragma: no cover - depends on environment
            raise StorageError("psycopg 3 is required for the PostgreSQL backend") from exc

    def _connect(self):
        import psycopg
        from psycopg.rows import dict_row
        kwargs = {"autocommit": True, "row_factory": dict_row}
        if self.password:
            kwargs["password"] = self.password
        return psycopg.connect(self.url, **kwargs)

    def _begin(self, raw) -> None:
        raw.execute("BEGIN")

    def has_table(self, t: Tx, name: str) -> bool:
        row = t.q1("SELECT to_regclass(?) IS NOT NULL AS present", (name,))
        return bool(row and row["present"])

    def _lock_migrations(self, t: Tx) -> None:
        t.q("SELECT pg_advisory_xact_lock(7336001) AS locked")


def open_storage(db_url: str, password: str = "", pool_size: int = 16) -> Storage:
    if db_url.startswith("sqlite:///"):
        return SQLiteStorage(db_url[len("sqlite:///"):], pool_size)
    if db_url.startswith("sqlite://"):
        return SQLiteStorage(db_url[len("sqlite://"):], pool_size)
    if db_url.startswith(("postgresql://", "postgres://")):
        return PostgresStorage(db_url, password, pool_size)
    raise StorageError("unsupported db_url scheme (use sqlite:/// or postgresql://)")
