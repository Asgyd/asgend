# Stage 18 — Qt Control Plane HTTP method parity

**Snapshot:** 2026-10-27, based on Stage 17.

## Finding

The shared client/OpenAPI contract test discovered Qt Control Plane endpoint strings but assigned no HTTP method to Qt references. Consequently, it could reject undocumented paths while accepting a client call that used the wrong HTTP verb for a documented path.

## Changes

- Replaced Qt path-only extraction with request-block analysis of `ControlPlaneAdapter`.
- Infer each `Request` method from its explicit `verb` assignment or the `Request::verb` default in the header. This makes a change to the default verb visible to the contract test.
- Expand the shared task-command path using actual `answer`, `cancel`, and `retry` call sites, so the final path segment is checked as a documented static segment rather than treated as an arbitrary wildcard.
- Model the artifact download separately: its fallback `/v1/downloads/%1` path is extracted from `resolveDownloadUrl`, and its direct network method is checked against `startDownload`.
- Fail closed when a Qt `Request` has no unique literal versioned path, is not sent through the adapter, the ticket download is not a direct GET, or a `/v1/` path literal appears outside the inventory the scanner accounts for.
- Add a deterministic test of the full current Qt method/path set (16 request branches) and raise the minimum Qt discovery floor from 10 to 16.

## Verification

- `python3 tests/contracts/test_contracts.py`: **36 tests run, 35 passed, 1 optional skip**.
- Includes exact Qt method/path inventory coverage, dynamic command expansion, client-to-OpenAPI method/path parity, server/OpenAPI operation parity, and OpenAPI security parity.

## Scope and limits

This improves static contract verification; it does not execute the Qt UI or network requests against a live Control Plane, and it does not replace the Qt native build/Qt Test workflow or hosted GitHub Actions execution. Those checks remain external acceptance items.
