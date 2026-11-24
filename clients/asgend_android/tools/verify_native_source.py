#!/usr/bin/env python3
"""Dependency-free source contract checks for ASGEND's native Android client."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "clients" / "asgend_android" / "app"

def read(rel: str) -> str:
    path = ROOT / rel
    if not path.is_file():
        raise AssertionError(f"required source file missing: {rel}")
    return path.read_text(encoding="utf-8")

def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    build = read("clients/asgend_android/app/build.gradle.kts")
    manifest = read("clients/asgend_android/app/src/main/AndroidManifest.xml")
    provider_paths = read("clients/asgend_android/app/src/main/res/xml/artifact_file_paths.xml")
    network = read("clients/asgend_android/app/src/main/res/xml/network_security_config.xml")
    app_shell = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/AsgendApp.kt")
    runtime = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/RuntimeScreen.kt")
    repo = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/data/CoreRepository.kt")
    view_model = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/NativeAppViewModel.kt")
    tests = read("clients/asgend_android/app/src/test/java/org/asgend/nativeclient/data/CoreRepositoryContractTest.kt")
    config_draft = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/data/RuntimeConfigDraft.kt")
    config_tests = read("clients/asgend_android/app/src/test/java/org/asgend/nativeclient/data/RuntimeConfigDraftTest.kt")
    contract_tests = read("clients/asgend_android/app/src/test/java/org/asgend/nativeclient/data/CoreRepositoryContractTest.kt")
    task_screen = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/workspace/TaskWorkspaceScreen.kt")
    file_screen = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/workspace/FilesScreen.kt")
    workflow = read(".github/workflows/native-android.yml")
    remote_client = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/data/ControlPlaneClient.kt")
    remote_store = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/data/SettingsStore.kt")
    remote_tokens = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/data/SecureTokenStore.kt")
    remote_view_model = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/remote/NativeRemoteViewModel.kt")
    remote_screen = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/remote/RemoteControlScreen.kt")
    remote_tests = read("clients/asgend_android/app/src/test/java/org/asgend/nativeclient/data/ControlPlaneClientContractTest.kt")
    remote_task_vm = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/remote/NativeRemoteViewModel.kt")
    remote_task_screen = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/remote/RemoteControlScreen.kt")
    artifact_transfer = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/data/RemoteArtifactTransfer.kt")
    artifact_tests = read("clients/asgend_android/app/src/test/java/org/asgend/nativeclient/data/RemoteArtifactTransferContractTest.kt")

    check(re.search(r"compileSdk\s*=\s*37\b", build) is not None, "compileSdk must target Android API 37")
    check(re.search(r"targetSdk\s*=\s*37\b", build) is not None, "targetSdk must target Android API 37")
    check('android:usesCleartextTraffic="false"' in manifest, "manifest must disallow cleartext network traffic")
    check('cleartextTrafficPermitted="false"' in network, "network security config must disallow cleartext")
    check('android:exported="false"' in manifest and 'android:grantUriPermissions="true"' in manifest, "artifact FileProvider must be non-exported and grant URI access explicitly")
    check('<cache-path name="verified_artifacts" path="artifacts/" />' in provider_paths, "artifact FileProvider must expose only the verified cache subdirectory")
    check("Destination.Runtime -> RuntimeScreen(model, modifier)" in app_shell, "Runtime destination must be connected to a real screen")
    check('Configuration("Config")' in runtime and "RuntimeSection.Configuration -> ConfigContent(model)" in runtime, "Configuration navigation section must be present in the Runtime enum and dispatch")
    check('"model_enable", "model_disable", "model_start", "model_stop"' in repo, "model actions must be allow-listed")
    check('kind == "llm" || kind == "decision"' in repo, "model kind must be validated")
    check("180_000" in repo, "model actions must use the bounded long operation timeout")
    check("One or more runtime requests failed" in view_model, "runtime errors must be generic")
    check("The core rejected the model operation" in view_model, "model-action errors must be generic")
    for sensitive_key in ("api_key", "auth_token", "base_url", "last_error", "log_path", "resource_index_path", "project_memory_root"):
        pat = re.compile(rf"(?:optString|optInt|optLong|optBoolean|optJSONObject|optJSONArray|getString)\s*\(\s*\"{re.escape(sensitive_key)}\"")
        check(pat.search(runtime) is None, f"runtime UI must not project sensitive field {sensitive_key}")
    check("modelControlUsesValidatedCoreOperationAndLongTimeout" in tests, "model operation contract test missing")
    check("modelControlRejectsUnknownOperationAndKindBeforeTransport" in tests, "model allow-list negative test missing")
    check("platforms;android-37" in workflow, "Android CI must install API 37")
    check('"config_update_settings"' in repo and "configUpdateSettings" in repo, "Android guided config must use the non-destructive settings endpoint")
    check("toSettingsJson()" in config_draft and 'put("auth_token"' not in config_draft and 'put("api_key"' not in config_draft,
          "guided config network payload must contain only approved non-secret settings")
    check("legacy_fixed_role_id(id)" in read("src/config_editor.c"), "legacy fixed-role agent profiles must survive full-bundle save")
    check("settings update was rejected" in read("src/server.c"), "core settings endpoint must return a generic safe error fallback")
    check("I intentionally accept the sandbox security change" in runtime, "weaker sandbox policy must require explicit acknowledgement")
    check("fun validationError()" in config_draft and "1..1_000_000" in config_draft, "config fields must be range-validated locally")
    check("parsesOnlyApprovedSettingsAndPreservesHiddenFieldsOnUpdate" in config_tests and "validatesCoreRangesBeforeAnySave" in config_tests and "serializesOnlyAllowListedFieldsForNonDestructiveCoreUpdate" in config_tests,
          "guided config allow-list/preservation/range tests missing")
    check('"dead_letters"' in repo and '"artifact_list"' in repo and '"workspace_read_bytes"' in repo and '"lsp_diagnostics"' in repo and '"plan_wait"' in repo, "local CLI/TUI operations must be mapped to the core API")
    plan_screen = read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/workspace/PlanScreen.kt")
    check('"dead_letters"' in task_screen, "dead-letter queue must be available from task filters")
    check("awaitPlanNotification" in plan_screen and 'optBoolean("changed"' in plan_screen, "To-do/Goals must refresh on core plan revisions")
    check("planRevision" in view_model and "planWait" in repo, "plan revision polling needs ViewModel state and a bounded repository long-poll")
    check("workspaceArtifacts" in file_screen and "artifactsTruncated" in file_screen, "artifact inventory must disclose truncated core scans")
    check("MessageDigest.getInstance(\"SHA-256\")" in view_model and "MAX_NATIVE_ARTIFACT_EXPORT_BYTES" in view_model and "workspaceReadBytes" in view_model, "binary artifact export must be chunked, size-bounded and SHA-256 verified")
    check("shareVerifiedArtifact" in file_screen and "FileProvider.getUriForFile" in file_screen, "verified artifacts must be exported through a private FileProvider")
    check("lspDiagnostics" in file_screen and "configured project root" in file_screen, "LSP diagnostics must explain project-root boundary")
    for expected_test in ("deadLettersUseBoundedCoreOperation", "planNotificationsUseRevisionLongPollAndExtraClientDeadline", "planWaitRejectsInvalidRevisionAndTimeoutBeforeTransport", "artifactsAreRequestedForSelectedTaskOnly", "binaryReadUsesBoundedChunksAndValidatesResponseEnvelope", "binaryReadRejectsUnsafePathBeforeTransport", "lspDiagnosticsRejectAbsoluteAndTraversalPathsBeforeNetwork", "lspDiagnosticsUseRelativePathAndBoundedResponse"):
        check(expected_test in contract_tests, f"native core operation contract test missing: {expected_test}")
    check('parsed.scheme.equals("https", true)' in remote_client and 'connection.instanceFollowRedirects = false' in remote_client, "Control Plane transport must require HTTPS and refuse redirects")
    check('if (authenticated) headers["Authorization"] = "Bearer $token"' in remote_client, "remote bearer must only be sent through the Authorization header")
    check('MAX_JSON_RESPONSE_BYTES' in remote_client and 'x-asgend-api-version' in remote_client and 'bodyVersionIsOne' in remote_client, "remote JSON responses must be bounded and header/body API-version checked")
    check('remoteAlias = "org.asgend.nativeclient.control-plane-token.v1"' in remote_tokens and 'saveRemote' in remote_tokens and 'loadRemote' in remote_tokens, "Control Plane token must use a separate Keystore alias")
    check('secureTokens.saveRemote(settings.token)' in remote_store and 'encrypted_control_plane_token' in remote_tokens, "Control Plane token must not overwrite the local-core token slot")
    check('store.clearControlPlaneSettings()' in remote_view_model and 'active?.logout()' in remote_view_model, "remote sign-out must attempt revoke and clear local credentials")
    check('RemoteControlSection(remoteModel' in read("clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/AsgendApp.kt"), "Control Plane screen must be reachable from native Settings")
    check('RemoteOperationLedger' in remote_store and 'pendingRemoteOperation' in remote_store, "remote mutation keys must be durably committed before network calls")
    check('credentialCleanupNeedsRetry' in remote_view_model and 'retryCredentialCleanup' in remote_view_model, "failed local credential cleanup must be visible and retryable")
    check('expiredOperationReview' in remote_view_model and 'discardExpiredRemoteOperation' in remote_store, "expired ambiguous mutations must require explicit review before clearing idempotency metadata")
    check('rejectsMissingOrMismatchedBodyApiVersion' in remote_tests, "body API-version contract test missing")
    check('RemoteOperationLedgerTest.kt' in '\n'.join(str(p.relative_to(ROOT)) for p in ROOT.glob('clients/asgend_android/app/src/test/java/org/asgend/nativeclient/data/*.kt')), "durable idempotency ledger tests missing")
    check('pairingCode' in remote_screen and 'rememberSaveable' not in remote_screen.split('var pairingCode', 1)[1].split('var confirmLogout', 1)[0], "one-time pairing code must not enter saved UI state")
    for expected_test in ("endpointNormalizationRequiresHttpsAndRemovesV1Suffix", "pairingUsesUnauthenticatedHttpsPostAndDoesNotPutCodeInUrl", "authenticatedIdentityUsesBearerHeaderAndParsesMembership", "rejectsApiVersionMismatchAndRedirects", "noTokenFailsBeforeNetworkCall", "nodeRequestEncodesWorkspaceAsPathSegment"):
        check(expected_test in remote_tests, f"Control Plane contract test missing: {expected_test}")
    check("suspend fun listTasks(" in remote_client and '"/v1/workspaces/${segment(workspaceId)}/tasks"' in remote_client, "remote task listing must use the OpenAPI workspace route")
    check('"idempotency_key"' in remote_client and "requireIdempotencyKey" in remote_client, "remote mutations must validate and send idempotency keys")
    check('"after_event_id"' in remote_client and '"wait_ms"' in remote_client and "waitMs + 20_000" in remote_client, "remote event poll must use a cursor and bounded extra deadline")
    check("safeEventSummary" in remote_client and "val safeSummary: String" in remote_client and "val payload: JSONObject" not in remote_client, "remote events must map to safe summaries instead of retaining raw payload")
    check("suspend fun requireOperatorClient()" in remote_task_vm and "val me = active.me()" in remote_task_vm and 'membership.role != "owner" && membership.role != "operator"' in remote_task_vm, "every remote mutation must refresh membership and require owner/operator")
    check("selectWorkspace(workspaceId: String)" in remote_task_vm and "saveActiveControlPlaneWorkspace" in remote_task_vm, "workspace selection must persist the selected membership only")
    check("pollRemoteTaskEvents" in remote_task_vm and "remoteEventCursor = maxOf(remoteEventCursor" in remote_task_vm and "DISPLAYABLE_EVENT_TYPES" in remote_task_vm, "remote event loop must keep a monotonic deduplicated cursor and filter display types")
    check("onConfirmCancel" in remote_task_screen and "onConfirmRetry" in remote_task_screen and "model.canOperate" in remote_task_screen, "cancel/retry need confirmation and owner/operator UI gating")
    check("REMOTE_TASK_FILTERS" in remote_task_vm and '"completed_without_model"' in remote_task_vm and '"delivery_failed"' in remote_task_vm, "remote task filters must cover v1 task states")
    for expected_test in ("remoteTaskListUsesContractFiltersAndOpaqueCursor", "taskSubmissionCarriesIdempotencyKeyAndValidatedFields", "eventLongPollUsesMonotonicCursorAndExtraTransportDeadline", "answerCancelAndRetryUseDistinctContractPathsAndIdempotencyKeys", "eventSummaryNeverProjectsUntrustedPayloadText", "unknownEventTypeAdvancesPageWithoutPayloadProjection", "taskFiltersAndCommandKeysRejectInvalidValuesBeforeTransport"):
        check(expected_test in remote_tests, f"remote task contract test missing: {expected_test}")
    check("suspend fun listTaskArtifacts(taskId: String)" in remote_client and "suspend fun createArtifactDownloadTicket(artifactId: String)" in remote_client and "suspend fun openArtifactDownload(ticket: ControlPlaneDownloadTicket)" in remote_client, "Control Plane artifact inventory/ticket/stream methods are required")
    check("interface ControlPlaneArtifactDownloadTransport" in artifact_transfer and 'setRequestProperty("Authorization"' not in artifact_transfer, "raw artifact ticket transport must not attach a bearer token")
    check("instanceFollowRedirects = false" in artifact_transfer and "sameOrigin(endpoint, uri)" in remote_client, "artifact download must refuse redirects and enforce same-origin ticket URLs")
    check("MAX_SIZE_BYTES = 268_435_456L" in artifact_transfer and 'header("content-length")' in artifact_transfer, "artifact stream must enforce the documented size cap and explicit content length")
    check('header("x-checksum-sha256")' in artifact_transfer and 'MessageDigest.getInstance("SHA-256")' in artifact_transfer and "partial?.let { runCatching { it.delete() } }" in artifact_transfer, "artifact download must validate checksum and remove unverified partials")
    check("if (!isValidArtifactName(artifact.name))" in artifact_transfer and "refusing to create a download ticket" in artifact_transfer, "direct downloader invocation must validate artifact filename before network access")
    check("artifactListValidatesRequiredMetadataAndTaskBinding" in artifact_tests and "checksumAndSizeMismatchDeletePartialFile" in artifact_tests and "invalidArtifactNamesAndUnsupportedStatesNeverDownload" in artifact_tests, "negative artifact transfer tests missing")
    check('fresh.any { it.type == "artifact_available" }' in remote_task_vm and "refreshRemoteArtifacts(taskId)" in remote_task_vm, "artifact_available events must refresh selected task's artifact inventory")
    check("RemoteArtifactsSection" in remote_task_screen and "shareVerifiedRemoteArtifact" in remote_task_screen and 'FileProvider.getUriForFile' in remote_task_screen, "remote artifacts must be exposed only after verified download through FileProvider")
    print("native Android source contract: PASS")
    print("checked: API 37, network/secret boundaries, local RPC parity, plan notifications, verified artifact export, HTTPS endpoint policy, bearer-header-only auth, separate Control Plane Keystore slot, pairing lifecycle, remote workspace/task cursor operations, idempotency, owner/operator authorization, event payload privacy, response limits/version, contract tests and Android CI")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"native Android source contract: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
