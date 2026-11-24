# ADR-0003: Credential lifecycle, RBAC and fail-closed authorization

- **Status:** Accepted
- **Date:** 2026-10-21
- **Scope:** User, node and artifact-download authentication

## Decision

- User/device, Execution Node and artifact-download credentials use separate token namespaces and are validated only by their intended endpoint class. Store SHA-256 hashes rather than raw credentials. Secret values are returned only at issuance/rotation, transmitted only over TLS externally, and redacted from logs and audit records.
- User access is limited by a workspace role (`owner`, `operator`, `viewer`) checked on every request. Node access is limited by registered `node_id` and task ownership. Artifact download tickets are random, short-lived capabilities bound to the issuing user, workspace and one artifact; every download re-checks that binding and the user's current membership.
- Pairing and enrollment codes are short-lived, single use and rate limited. Node token rotation allows only a bounded transition window; explicit revoke disables credentials and expires pending commands. Logout revokes the presented user/device token.
- Missing, malformed, expired or revoked credentials are denied. Cross-workspace references are reported as `not_found`. Authorization/storage failures do not fall back to unauthenticated access or weaker alternate identity.
- Unknown fields are rejected for permission-granting inputs whose interpretation can widen access (pairing code role, enrollment creation and member replacement). Compatible informational objects tolerate unknown fields for forward compatibility.
- API errors return safe codes/messages plus `request_id`; `details` cannot contain tokens, prompt contents, arbitrary paths or exception dumps. Request bodies and authorization headers are not logged.

## Rationale

The token classes have distinct trust boundaries: users initiate operations, nodes report execution and hold workspace-level execution authority, while download tickets grant only temporary access to one artifact. Hash-at-rest storage limits damage from a database-only leak. Rechecking membership during a ticket download ensures a previously issued capability does not outlive a removed user's authorization.

## Consequences and operational requirements

- Credential issuance, rotation, logout and revoke require integration tests, including old-token grace-period behavior and fail-closed cases.
- Key material and server secrets must be injected at runtime and rotated through the deployment environment; they must not be committed in config examples or emitted by diagnostics.
- Rate limits, TLS validation, `Host`/origin checks, audit retention and recovery procedures remain deployment responsibilities. If any such control cannot be enforced, the affected remote endpoint must not be exposed publicly.
