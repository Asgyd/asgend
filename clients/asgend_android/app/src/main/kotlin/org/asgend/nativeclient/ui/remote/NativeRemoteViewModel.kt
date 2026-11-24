package org.asgend.nativeclient.ui.remote

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.asgend.nativeclient.data.ControlPlaneClient
import org.asgend.nativeclient.data.ControlPlaneArtifact
import org.asgend.nativeclient.data.ControlPlaneEvent
import org.asgend.nativeclient.data.ControlPlaneException
import org.asgend.nativeclient.data.ControlPlaneIdentity
import org.asgend.nativeclient.data.ControlPlaneNode
import org.asgend.nativeclient.data.ControlPlaneSettings
import org.asgend.nativeclient.data.ControlPlaneTask
import org.asgend.nativeclient.data.ControlPlaneWorkspace
import org.asgend.nativeclient.data.SavedRemoteUiState
import org.asgend.nativeclient.data.PendingRemoteOperation
import org.asgend.nativeclient.data.RemoteArtifactDownloader
import org.asgend.nativeclient.data.VerifiedRemoteArtifactFile
import org.asgend.nativeclient.data.SettingsStore
import java.io.File
import java.security.MessageDigest

class NativeRemoteViewModel(application: Application) : AndroidViewModel(application) {
    private val store = SettingsStore(application.applicationContext)
    private val remoteStatePersistenceMutex = Mutex()
    private var client: ControlPlaneClient? = null
    private var restoredRemoteTaskId: String? = null
    private var remoteStateRevision: Long = 0L
    var credentialCleanupNeedsRetry by mutableStateOf(false); private set
    var expiredOperationReview by mutableStateOf<PendingRemoteOperation?>(null); private set

    var settings by mutableStateOf(ControlPlaneSettings()); private set
    var settingsLoaded by mutableStateOf(false); private set
    var busy by mutableStateOf(false); private set
    var statusText by mutableStateOf("Not paired"); private set
    var error by mutableStateOf<String?>(null); private set
    var identity by mutableStateOf<ControlPlaneIdentity?>(null); private set
    var nodes by mutableStateOf<List<ControlPlaneNode>>(emptyList()); private set
    var refreshing by mutableStateOf(false); private set

    var remoteTasks by mutableStateOf<List<ControlPlaneTask>>(emptyList()); private set
    var remoteTaskCursor by mutableStateOf<String?>(null); private set
    var remoteTaskFilter by mutableStateOf("all"); private set
    var selectedRemoteTask by mutableStateOf<ControlPlaneTask?>(null); private set
    var remoteEvents by mutableStateOf<List<ControlPlaneEvent>>(emptyList()); private set
    var remoteEventCursor by mutableStateOf(0L); private set
    var remoteTaskBusy by mutableStateOf(false); private set
    var remoteTaskLoading by mutableStateOf(false); private set
    var remoteTaskError by mutableStateOf<String?>(null); private set
    var remoteTaskStatus by mutableStateOf("Ready"); private set
    var lastCreatedRemoteTaskId by mutableStateOf(""); private set

    var remoteArtifacts by mutableStateOf<List<ControlPlaneArtifact>>(emptyList()); private set
    var remoteArtifactsLoading by mutableStateOf(false); private set
    var remoteArtifactError by mutableStateOf<String?>(null); private set
    var downloadingRemoteArtifactId by mutableStateOf<String?>(null); private set
    var remoteArtifactDownloadProgress by mutableStateOf(0f); private set
    var verifiedRemoteArtifact by mutableStateOf<VerifiedRemoteArtifactFile?>(null); private set
    private var remoteArtifactDownloadJob: Job? = null
    private var remoteArtifactDownloadGeneration: Long = 0L
    private var remoteArtifactListJob: Job? = null
    private var remoteArtifactListGeneration: Long = 0L
    private var remoteArtifactsRefreshPending = false

    val currentWorkspace: ControlPlaneWorkspace?
        get() = identity?.workspaces?.firstOrNull { it.workspaceId == settings.workspaceId }
    val canOperate: Boolean get() = currentWorkspace?.role == "owner" || currentWorkspace?.role == "operator"
    val hasMoreRemoteTasks: Boolean get() = !remoteTaskCursor.isNullOrBlank()

    init {
        // A process can be killed after a successful download and before onCleared runs.
        // Remote artifacts have a dedicated private cache subdirectory so stale files can be removed safely.
        File(getApplication<Application>().cacheDir, "artifacts/remote").listFiles().orEmpty().forEach { stale ->
            runCatching { stale.delete() }
        }
        viewModelScope.launch {
            settings = store.loadControlPlaneSettings()
            settingsLoaded = true
            if (settings.paired) {
                store.loadRemoteUiState(settings.userId, settings.workspaceId)?.let { saved ->
                    restoredRemoteTaskId = saved.selectedTaskId
                    remoteTaskFilter = saved.taskFilter
                    remoteEventCursor = saved.eventCursor
                    remoteEvents = saved.events
                    remoteStateRevision = saved.revision
                }
                client = ControlPlaneClient.forEndpoint(settings.endpoint, token = settings.token)
                refresh()
            } else {
                statusText = "Not paired"
            }
        }
    }

    fun pair(endpointText: String, code: String, deviceName: String) {
        if (busy) return
        viewModelScope.launch {
            busy = true
            error = null
            statusText = "Checking endpoint…"
            try {
                val normalizedEndpoint = ControlPlaneClient.normalizeEndpoint(endpointText).toString()
                val anonymous = ControlPlaneClient.forEndpoint(normalizedEndpoint)
                if (!anonymous.health()) throw ControlPlaneException(null, "health", "The endpoint did not identify as a healthy ASGEND Control Plane.")
                statusText = "Redeeming one-time pairing code…"
                val result = anonymous.redeemPairing(code, deviceName)
                val next = ControlPlaneSettings(
                    endpoint = normalizedEndpoint,
                    workspaceId = result.workspaceId,
                    userId = result.userId,
                    role = result.role,
                    deviceName = deviceName.trim().ifBlank { "ASGEND Android" }.take(80),
                    token = result.token,
                )
                store.saveControlPlaneSettings(next)
                settings = next
                client = ControlPlaneClient.forEndpoint(normalizedEndpoint, token = result.token)
                statusText = "Paired · refreshing account…"
                refresh()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                error = failure.message
                statusText = if (settings.paired) "Paired · offline" else "Not paired"
            } catch (_: Exception) {
                error = "Could not complete pairing. Check the HTTPS endpoint and one-time code."
                statusText = if (settings.paired) "Paired · offline" else "Not paired"
            } finally {
                busy = false
            }
        }
    }

    fun refresh() {
        if (refreshing || !settings.paired) return
        viewModelScope.launch {
            refreshing = true
            error = null
            try {
                val active = activeClient()
                val me = active.me()
                val membership = me.workspaces.firstOrNull { it.workspaceId == settings.workspaceId }
                val workspaceNodes = if (membership != null) active.listNodes(settings.workspaceId) else emptyList()
                val workspaceTasks = if (membership != null) active.listTasks(
                    settings.workspaceId, state = remoteTaskFilter.takeIf { it != "all" }, limit = 50,
                ) else null
                // Commit a coherent snapshot only after all network reads for this refresh succeeded.
                identity = me
                settings = settings.copy(role = membership?.role ?: "unknown", userId = me.userId)
                nodes = workspaceNodes
                statusText = if (membership == null) "Paired · workspace access unavailable" else "Connected · ${membership.role}"
                if (membership == null) {
                    error = "This token no longer has membership in the selected workspace. Select another workspace or pair again."
                    clearRemoteTaskState()
                } else {
                    remoteTasks = workspaceTasks!!.tasks
                    remoteTaskCursor = workspaceTasks.nextCursor
                    remoteTaskStatus = if (remoteTasks.isEmpty()) "No tasks match this filter" else "${remoteTasks.size} task(s) loaded"
                    restoredRemoteTaskId?.let { taskId ->
                        try {
                            selectedRemoteTask = active.getTask(taskId)
                            restoredRemoteTaskId = null
                        } catch (failure: ControlPlaneException) {
                            if (failure.statusCode == 401 || failure.statusCode == 403) throw failure
                            if (failure.statusCode == 404) {
                                restoredRemoteTaskId = null
                                remoteEvents = emptyList()
                                remoteEventCursor = 0L
                            }
                            selectedRemoteTask = null
                        }
                    }
                    persistRemoteUiState()
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                onControlPlaneFailure(failure)
            } catch (_: Exception) {
                statusText = if (settings.paired) "Paired · offline" else "Not paired"
                error = "Could not refresh the Control Plane session. Check the network and try again."
            } finally {
                refreshing = false
            }
        }
    }

    fun selectWorkspace(workspaceId: String) {
        if (busy || remoteTaskBusy || workspaceId == settings.workspaceId) return
        viewModelScope.launch {
            busy = true
            error = null
            try {
                val active = activeClient()
                val latest = active.me()
                val target = latest.workspaces.firstOrNull { it.workspaceId == workspaceId }
                    ?: throw ControlPlaneException(403, "forbidden", "This account no longer has access to that workspace.")
                // Stage all new-workspace data before persisting or publishing the selection.
                val targetNodes = active.listNodes(target.workspaceId)
                val targetPage = active.listTasks(
                    target.workspaceId,
                    state = remoteTaskFilter.takeIf { it != "all" },
                    limit = 50,
                )
                store.saveActiveControlPlaneWorkspace(target.workspaceId, target.role)
                identity = latest
                settings = settings.copy(workspaceId = target.workspaceId, role = target.role, userId = latest.userId)
                nodes = targetNodes
                remoteTasks = targetPage.tasks
                remoteTaskCursor = targetPage.nextCursor
                selectedRemoteTask = null
                clearRemoteArtifactState()
                remoteEvents = emptyList()
                remoteEventCursor = 0L
                remoteTaskStatus = if (remoteTasks.isEmpty()) "No tasks match this filter" else "${remoteTasks.size} task(s) loaded"
                restoredRemoteTaskId = null
                persistRemoteUiState()
                statusText = "Connected · ${target.role}"
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                onControlPlaneFailure(failure)
            } catch (_: Exception) {
                // Selection and visible workspace state are unchanged until all fetches + local commit succeed.
                error = "Could not load the selected workspace. The previous workspace remains selected."
            } finally {
                busy = false
            }
        }
    }

    fun setRemoteTaskFilter(filter: String) {
        if (filter !in REMOTE_TASK_FILTERS || filter == remoteTaskFilter) return
        remoteTaskFilter = filter
        remoteTaskCursor = null
        remoteTasks = emptyList()
        selectedRemoteTask = null
        clearRemoteArtifactState()
        remoteEvents = emptyList()
        remoteEventCursor = 0L
        persistRemoteUiState()
        loadRemoteTasks()
    }

    fun loadRemoteTasks(loadMore: Boolean = false) {
        if (!settings.paired || remoteTaskLoading || remoteTaskBusy) return
        if (loadMore && !hasMoreRemoteTasks) return
        viewModelScope.launch {
            remoteTaskLoading = true
            remoteTaskError = null
            try {
                loadRemoteTasksInternal(activeClient(), append = loadMore)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                onControlPlaneFailure(failure, remote = true)
            } catch (_: Exception) {
                remoteTaskError = "Could not load remote tasks. Check the connection and retry."
            } finally {
                remoteTaskLoading = false
            }
        }
    }

    private suspend fun loadRemoteTasksInternal(active: ControlPlaneClient, append: Boolean) {
        val cursor = if (append) remoteTaskCursor else null
        val state = remoteTaskFilter.takeIf { it != "all" }
        val page = active.listTasks(settings.workspaceId, state = state, limit = 50, cursor = cursor)
        remoteTasks = if (append) (remoteTasks + page.tasks).distinctBy { it.taskId } else page.tasks
        remoteTaskCursor = page.nextCursor
        remoteTaskStatus = if (remoteTasks.isEmpty()) "No tasks match this filter" else "${remoteTasks.size} task(s) loaded"
    }

    fun createRemoteTask(nodeId: String, taskText: String, priority: Int = 5) {
        if (taskText.isBlank() || taskText.length > 262_144 || priority !in 0..9 || nodes.none { it.nodeId == nodeId }) {
            remoteTaskError = "Enter a task within the supported length, choose a listed node, and use a priority from 0 to 9."
            return
        }
        if (remoteTaskBusy || !canOperate) {
            remoteTaskError = if (!canOperate) "This workspace role is read-only." else "A remote task operation is already in progress."
            return
        }
        val canonicalTask = taskText.trim()
        val digest = sha256("${settings.userId}|${settings.workspaceId}|$nodeId|$priority|$canonicalTask")
        val slot = "create:$digest"
        viewModelScope.launch {
            remoteTaskBusy = true
            remoteTaskError = null
            try {
                val active = requireOperatorClient()
                val pending = store.pendingRemoteOperation(slot, digest)
                if (pending.expired) {
                    expiredOperationReview = pending
                    remoteTaskError = "A matching submission has an unresolved outcome and its retry window expired. Inspect the remote task list before submitting it again."
                    return@launch
                }
                val created = active.createTask(settings.workspaceId, nodeId, canonicalTask, pending.key, priority = priority)
                // The server accepted the task. If local ledger cleanup fails, retaining the same
                // key is safe; do not misreport this confirmed operation as an unknown submission.
                remoteTaskFilter = "all"
                remoteTaskCursor = null
                remoteTasks = emptyList()
                clearRemoteArtifactState()
                selectedRemoteTask = created
                lastCreatedRemoteTaskId = created.taskId
                remoteEvents = emptyList()
                remoteEventCursor = 0L
                remoteTaskStatus = "Task accepted. The node may start it asynchronously."
                val recoveryStateSaved = persistRemoteUiStateNow(selectedTaskId = created.taskId)
                val localLedgerCleared = if (recoveryStateSaved) {
                    try {
                        store.confirmRemoteOperation(slot, pending.key)
                        true
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (_: Exception) { false }
                } else false
                if (!localLedgerCleared) {
                    val warning = if (!recoveryStateSaved) {
                        "Task accepted, but selected-task recovery state could not be saved. The original idempotency key is retained to prevent duplicates."
                    } else {
                        "Task accepted, but local retry metadata could not be cleared. The same request remains safely deduplicated."
                    }
                    remoteTaskError = listOfNotNull(remoteTaskError, warning).distinct().joinToString(" ")
                }
                try {
                    loadRemoteTasksInternal(active, append = false)
                    remoteTaskStatus = "Task accepted. The node may start it asynchronously."
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    remoteTaskError = listOfNotNull(
                        remoteTaskError,
                        "Task accepted, but the task list could not be refreshed. Open the task or refresh the list.",
                    ).distinct().joinToString(" ")
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                onControlPlaneFailure(failure, remote = true)
            } catch (_: Exception) {
                remoteTaskError = "Task submission could not be confirmed. Retry this exact request to reuse its idempotency key."
            } finally {
                remoteTaskBusy = false
            }
        }
    }

    fun openRemoteTask(taskId: String) {
        if (remoteTaskBusy) return
        viewModelScope.launch {
            remoteTaskBusy = true
            remoteTaskError = null
            try {
                val active = activeClient()
                val previouslySelectedTaskId = selectedRemoteTask?.taskId
                val opened = active.getTask(taskId)
                if (previouslySelectedTaskId != opened.taskId) clearRemoteArtifactState()
                selectedRemoteTask = opened
                remoteEvents = emptyList()
                remoteEventCursor = 0L
                remoteTaskStatus = "Loading event history…"
                persistRemoteUiState()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                onControlPlaneFailure(failure, remote = true)
            } catch (_: Exception) {
                remoteTaskError = "Could not open this remote task."
            } finally {
                remoteTaskBusy = false
            }
        }
    }

    /** One bounded long-poll. The caller's LaunchedEffect owns its lifecycle and cancels this loop on navigation. */
    suspend fun pollRemoteTaskEvents(taskId: String): Boolean {
        if (!settings.paired || selectedRemoteTask?.taskId != taskId) return false
        return try {
            val page = activeClient().taskEvents(taskId, afterEventId = remoteEventCursor, limit = 100, waitMs = 25_000)
            if (selectedRemoteTask?.taskId != taskId) return false
            if (page.lastEventId < remoteEventCursor) {
                // Server indicated the cursor was reset/stale. Clear the old window and resume from the server cursor.
                remoteEvents = emptyList()
                remoteEventCursor = page.lastEventId.coerceAtLeast(0L)
                remoteTaskStatus = "Event cursor reset by server; resuming from a fresh history window."
                persistRemoteUiState()
                true
            } else {
                val fresh = page.events.filter { it.eventId > remoteEventCursor && it.type in DISPLAYABLE_EVENT_TYPES }
                if (fresh.isNotEmpty()) {
                    remoteEvents = (remoteEvents + fresh).distinctBy { it.eventId }.sortedBy { it.eventId }.takeLast(500)
                    remoteTaskStatus = "Live · ${remoteEvents.size} visible event(s)"
                    if (fresh.any { it.type in TASK_REFRESH_EVENT_TYPES }) {
                        selectedRemoteTask = activeClient().getTask(taskId)
                        if (selectedRemoteTask?.state == "waiting_input") remoteTaskStatus = "Waiting for your answer"
                    }
                    // Artifact inventory changes independently of the task state; refresh it on this event.
                    if (fresh.any { it.type == "artifact_available" }) refreshRemoteArtifacts(taskId)
                }
                // Advance for unknown types too, otherwise an unknown event would create a permanent poll loop.
                remoteEventCursor = maxOf(remoteEventCursor, page.lastEventId, page.events.maxOfOrNull { it.eventId } ?: 0L)
                persistRemoteUiState()
                true
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: ControlPlaneException) {
            onControlPlaneFailure(failure, remote = true)
            false
        } catch (_: Exception) {
            remoteTaskError = "Connection interrupted while reading remote task events. Retrying with the same event cursor."
            false
        }
    }

    fun answerRemoteTask(taskId: String, answer: String) = runRemoteCommand("answer", taskId, sha256(answer.trim())) { active, key -> active.answerTask(taskId, answer, key) }
    fun cancelRemoteTask(taskId: String) = runRemoteCommand("cancel", taskId, sha256("cancel:$taskId")) { active, key -> active.cancelTask(taskId, key) }
    fun retryRemoteTask(taskId: String) = runRemoteCommand("retry", taskId, sha256("retry:$taskId")) { active, key -> active.retryTask(taskId, key) }

    private fun runRemoteCommand(kind: String, taskId: String, payloadDigest: String, operation: suspend (ControlPlaneClient, String) -> Any) {
        if (remoteTaskBusy || !canOperate) {
            remoteTaskError = if (!canOperate) "This workspace role is read-only." else "A remote task operation is already in progress."
            return
        }
        val workspaceId = settings.workspaceId
        val digest = sha256("${settings.userId}|$workspaceId|$kind|$taskId|$payloadDigest")
        val slot = "command:$digest"
        viewModelScope.launch {
            remoteTaskBusy = true
            remoteTaskError = null
            try {
                val active = requireOperatorClient()
                val pending = store.pendingRemoteOperation(slot, digest)
                if (pending.expired) {
                    expiredOperationReview = pending
                    remoteTaskError = "A matching command has an unresolved outcome and its retry window expired. Refresh the task before sending another command."
                    return@launch
                }
                operation(active, pending.key)
                val recoveryStateSaved = persistRemoteUiStateNow()
                val localLedgerCleared = if (recoveryStateSaved) {
                    try {
                        store.confirmRemoteOperation(slot, pending.key)
                        true
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (_: Exception) { false }
                } else false
                remoteTaskStatus = "${kind.replaceFirstChar { it.uppercase() }} command accepted. Execution changes asynchronously on the node."
                if (!localLedgerCleared) {
                    val warning = if (!recoveryStateSaved) {
                        "Command accepted, but recovery state could not be saved. The original idempotency key is retained to prevent duplicates."
                    } else {
                        "Command accepted, but local retry metadata could not be cleared. The same operation key is retained to prevent duplicate commands."
                    }
                    remoteTaskError = listOfNotNull(remoteTaskError, warning).distinct().joinToString(" ")
                }
                try {
                    selectedRemoteTask = active.getTask(taskId)
                    loadRemoteTasksInternal(active, append = false)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    remoteTaskError = listOfNotNull(
                        remoteTaskError,
                        "Command accepted, but the refreshed task state is not available yet. The live event stream will update it.",
                    ).distinct().joinToString(" ")
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                onControlPlaneFailure(failure, remote = true)
            } catch (_: Exception) {
                remoteTaskError = "The command could not be confirmed. Repeat only to retry the same idempotent command."
            } finally {
                remoteTaskBusy = false
            }
        }
    }

    fun refreshRemoteArtifacts(taskId: String? = selectedRemoteTask?.taskId) {
        val expectedTaskId = taskId?.takeIf { it.isNotBlank() } ?: return
        if (!settings.paired) return
        if (remoteArtifactsLoading) {
            if (selectedRemoteTask?.taskId == expectedTaskId) remoteArtifactsRefreshPending = true
            return
        }
        remoteArtifactsRefreshPending = false
        val generation = ++remoteArtifactListGeneration
        remoteArtifactsLoading = true
        remoteArtifactError = null
        remoteArtifactListJob?.cancel()
        remoteArtifactListJob = viewModelScope.launch {
            try {
                val loaded = activeClient().listTaskArtifacts(expectedTaskId)
                if (generation == remoteArtifactListGeneration && selectedRemoteTask?.taskId == expectedTaskId) {
                    remoteArtifacts = loaded
                    val activeVerified = verifiedRemoteArtifact
                    if (activeVerified != null && loaded.none {
                        it.artifactId == activeVerified.artifact.artifactId &&
                            it.sha256 == activeVerified.artifact.sha256 && it.size == activeVerified.artifact.size
                    }) {
                        deleteVerifiedRemoteArtifact()
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                if (generation == remoteArtifactListGeneration) {
                    onControlPlaneFailure(failure, remote = true)
                    remoteArtifactError = when (failure.statusCode) {
                        403 -> "Your current workspace role does not permit viewing these artifacts."
                        404 -> "The selected task or its artifacts no longer exist. Refresh the task list."
                        410 -> "The artifact retention period has expired."
                        429 -> "Artifact listing is rate limited. Retry shortly."
                        else -> "Remote artifacts could not be loaded. Check the connection and retry."
                    }
                }
            } catch (_: Exception) {
                if (generation == remoteArtifactListGeneration) remoteArtifactError = "Remote artifacts could not be loaded. Check the connection and retry."
            } finally {
                if (generation == remoteArtifactListGeneration) {
                    remoteArtifactsLoading = false
                    remoteArtifactListJob = null
                    val refreshAgain = remoteArtifactsRefreshPending && selectedRemoteTask?.taskId == expectedTaskId
                    remoteArtifactsRefreshPending = false
                    if (refreshAgain) refreshRemoteArtifacts(expectedTaskId)
                }
            }
        }
    }

    fun downloadRemoteArtifact(artifact: ControlPlaneArtifact) {
        val selectedTaskId = selectedRemoteTask?.taskId
        if (!settings.paired || selectedTaskId.isNullOrBlank() || artifact.taskId != selectedTaskId || downloadingRemoteArtifactId != null) return
        if (!artifact.available) {
            remoteArtifactError = "This artifact is not available for download (state: ${artifact.state})."
            return
        }
        if (artifact.size !in 0L..ControlPlaneArtifact.MAX_SIZE_BYTES || !artifact.sha256.matches(Regex("[0-9a-f]{64}"))) {
            remoteArtifactError = "The artifact metadata is invalid or exceeds the supported 256 MiB limit."
            return
        }
        remoteArtifactDownloadJob?.cancel()
        val generation = ++remoteArtifactDownloadGeneration
        viewModelScope.launch {
            downloadingRemoteArtifactId = artifact.artifactId
            remoteArtifactDownloadProgress = 0f
            remoteArtifactError = null
            try {
                val targetDir = File(getApplication<Application>().cacheDir, "artifacts/remote")
                val verified = RemoteArtifactDownloader(activeClient()).download(artifact, targetDir) { received, total ->
                    if (generation == remoteArtifactDownloadGeneration) {
                        val progress = if (total <= 0L) 1f else (received.toDouble() / total.toDouble()).toFloat().coerceIn(0f, 1f)
                        viewModelScope.launch {
                            if (generation == remoteArtifactDownloadGeneration) remoteArtifactDownloadProgress = progress
                        }
                    }
                }
                if (generation != remoteArtifactDownloadGeneration || selectedRemoteTask?.taskId != selectedTaskId) {
                    runCatching { verified.file.delete() }
                    return@launch
                }
                deleteVerifiedRemoteArtifact()
                verifiedRemoteArtifact = verified
                remoteArtifactDownloadProgress = 1f
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: ControlPlaneException) {
                if (failure.statusCode == 401 || failure.statusCode == 403) onControlPlaneFailure(failure, remote = true)
                remoteArtifactError = when (failure.statusCode) {
                    401 -> "The Control Plane session expired. Pair this device again before downloading."
                    403 -> "Your workspace role no longer permits this download."
                    404 -> "The artifact or ticket is no longer available. Refresh the artifact list."
                    410 -> "The download ticket expired twice. Retry to request a fresh ticket."
                    413 -> "This artifact exceeds the client download limit."
                    429 -> "Artifact download is rate limited. Retry shortly."
                    else -> when (failure.errorCode) {
                        "checksum_mismatch" -> "Artifact size or SHA-256 verification failed. The incomplete file was deleted."
                        "redirect" -> "The artifact server redirected the request; the download was refused."
                        "version" -> "The artifact server API version is incompatible."
                        else -> "The artifact download failed. Incomplete files are not exposed."
                    }
                }
            } catch (_: Exception) {
                remoteArtifactError = "The artifact download failed. Incomplete files are not exposed. Retry when the connection is stable."
            } finally {
                if (generation == remoteArtifactDownloadGeneration) {
                    downloadingRemoteArtifactId = null
                    remoteArtifactDownloadJob = null
                }
            }
        }.also { remoteArtifactDownloadJob = it }
    }

    fun cancelRemoteArtifactDownload() {
        remoteArtifactDownloadGeneration++
        remoteArtifactDownloadJob?.cancel()
        remoteArtifactDownloadJob = null
        downloadingRemoteArtifactId = null
        remoteArtifactDownloadProgress = 0f
    }

    fun clearRemoteArtifactError() { remoteArtifactError = null }
    fun reportRemoteArtifactShareFailure() {
        remoteArtifactError = "The verified file could not be shared. Confirm app storage permissions and try again."
    }

    private fun clearRemoteArtifactState() {
        remoteArtifactListGeneration++
        remoteArtifactsRefreshPending = false
        remoteArtifactListJob?.cancel()
        remoteArtifactListJob = null
        remoteArtifactsLoading = false
        cancelRemoteArtifactDownload()
        remoteArtifacts = emptyList()
        remoteArtifactError = null
        remoteArtifactDownloadProgress = 0f
        deleteVerifiedRemoteArtifact()
    }

    private fun deleteVerifiedRemoteArtifact() {
        verifiedRemoteArtifact?.let { artifact -> runCatching { artifact.file.delete() } }
        verifiedRemoteArtifact = null
    }

    override fun onCleared() {
        remoteArtifactListGeneration++
        remoteArtifactsRefreshPending = false
        remoteArtifactListJob?.cancel()
        remoteArtifactListJob = null
        cancelRemoteArtifactDownload()
        deleteVerifiedRemoteArtifact()
        super.onCleared()
    }

    private suspend fun requireOperatorClient(): ControlPlaneClient {
        val active = activeClient()
        val me = active.me() // Refresh membership immediately before every mutating command.
        identity = me
        val membership = me.workspaces.firstOrNull { it.workspaceId == settings.workspaceId }
            ?: throw ControlPlaneException(403, "forbidden", "This account no longer has access to the selected workspace.")
        settings = settings.copy(role = membership.role, userId = me.userId)
        if (membership.role != "owner" && membership.role != "operator") {
            throw ControlPlaneException(403, "forbidden", "This workspace role is read-only.")
        }
        return active
    }

    private fun activeClient(): ControlPlaneClient = client ?: run {
        if (!settings.paired) throw ControlPlaneException(401, "unauthorized", "Pair this device with the Control Plane first.")
        ControlPlaneClient.forEndpoint(settings.endpoint, token = settings.token).also { client = it }
    }

    private suspend fun onControlPlaneFailure(failure: ControlPlaneException, remote: Boolean = false) {
        if (failure.statusCode == 401) {
            // Fail closed in memory first, even if encrypted-token deletion or preference commit fails.
            settings = settings.copy(token = "", workspaceId = "", userId = "", role = "unknown")
            client = null
            identity = null
            nodes = emptyList()
            clearRemoteArtifactState()
            clearRemoteTaskState()
            try {
                store.clearControlPlaneSettings()
                credentialCleanupNeedsRetry = false
                statusText = "Session expired · pair again"
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                credentialCleanupNeedsRetry = true
                statusText = "Session expired · local credential cleanup incomplete"
            }
        }
        if (remote) remoteTaskError = failure.message else error = failure.message
        if (credentialCleanupNeedsRetry) {
            error = "The session was cleared in memory, but local credential deletion failed. Retry local cleanup before pairing again."
        }
        statusText = when {
            failure.statusCode == 401 && credentialCleanupNeedsRetry -> "Session expired · cleanup retry required"
            failure.statusCode == 401 -> "Session expired · pair again"
            failure.statusCode == 403 -> "Connected · permission denied"
            failure.statusCode == 429 -> "Connected · rate limited"
            failure.statusCode != null && failure.statusCode in 400..499 -> "Connected · request rejected"
            settings.paired -> "Paired · offline"
            else -> "Session expired · pair again"
        }
    }

    /** Clear an expired unresolved idempotency entry only after the user checks remote state. */
    fun confirmExpiredOperationReviewed() {
        val pending = expiredOperationReview ?: return
        if (remoteTaskBusy || busy) return
        viewModelScope.launch {
            busy = true
            try {
                store.discardExpiredRemoteOperation(pending.slot, pending.digest, pending.key)
                expiredOperationReview = null
                remoteTaskError = "Expired retry metadata was cleared after review. Check the remote task list before submitting the operation again."
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                remoteTaskError = "The expired retry record could not be cleared. No new command key was issued."
            } finally {
                busy = false
            }
        }
    }

    fun retryCredentialCleanup() {
        if (!credentialCleanupNeedsRetry || busy) return
        viewModelScope.launch {
            busy = true
            try {
                store.clearControlPlaneSettings()
                credentialCleanupNeedsRetry = false
                error = null
                statusText = "Local credential cleanup complete · pair again"
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                error = "Local credential cleanup still failed. Restart the app and retry; no authenticated request will be sent while signed out."
                statusText = "Credential cleanup retry required"
            } finally {
                busy = false
            }
        }
    }

    private fun clearRemoteTaskState() {
        remoteTasks = emptyList()
        remoteTaskCursor = null
        selectedRemoteTask = null
        remoteEvents = emptyList()
        remoteEventCursor = 0L
        remoteTaskError = null
        persistRemoteUiState()
    }

    fun logout() {
        if (busy || !settings.paired) return
        viewModelScope.launch {
            busy = true
            error = null
            val active = client
            // Disable authenticated operations in memory before any local deletion can fail.
            settings = settings.copy(token = "", workspaceId = "", userId = "", role = "unknown")
            client = null
            identity = null
            nodes = emptyList()
            clearRemoteArtifactState()
            clearRemoteTaskState()
            try {
                try {
                    active?.logout()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    // Revocation is best-effort while offline; local token deletion is still mandatory.
                }
                store.clearControlPlaneSettings()
                credentialCleanupNeedsRetry = false
                statusText = "Signed out"
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                credentialCleanupNeedsRetry = true
                error = "The session is disabled in memory, but local credential deletion failed. Retry local cleanup before pairing again."
                statusText = "Sign out incomplete · cleanup retry required"
            } finally {
                busy = false
            }
        }
    }

    private fun persistRemoteUiState() {
        val userId = settings.userId
        val workspaceId = settings.workspaceId
        if (userId.isBlank() || workspaceId.isBlank()) return
        val snapshot = SavedRemoteUiState(
            selectedTaskId = selectedRemoteTask?.taskId ?: restoredRemoteTaskId,
            taskFilter = remoteTaskFilter,
            eventCursor = remoteEventCursor,
            events = remoteEvents.takeLast(100),
            revision = ++remoteStateRevision,
        )
        viewModelScope.launch {
            try {
                remoteStatePersistenceMutex.withLock { store.saveRemoteUiState(userId, workspaceId, snapshot) }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                remoteTaskError = "Remote state could not be saved for process recovery. Refresh before relying on restored history."
            }
        }
    }

    /** Awaitable persistence path used before an idempotency record can be removed. */
    private suspend fun persistRemoteUiStateNow(selectedTaskId: String? = selectedRemoteTask?.taskId): Boolean {
        val userId = settings.userId
        val workspaceId = settings.workspaceId
        if (userId.isBlank() || workspaceId.isBlank()) return false
        val snapshot = SavedRemoteUiState(
            selectedTaskId = selectedTaskId ?: restoredRemoteTaskId,
            taskFilter = remoteTaskFilter,
            eventCursor = remoteEventCursor,
            events = remoteEvents.takeLast(100),
            revision = ++remoteStateRevision,
        )
        return try {
            remoteStatePersistenceMutex.withLock { store.saveRemoteUiState(userId, workspaceId, snapshot) }
            true
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            remoteTaskError = "Remote state could not be saved for process recovery. The pending idempotency key was retained."
            false
        }
    }

    private fun sha256(value: String): String = MessageDigest.getInstance("SHA-256")
        .digest(value.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }

    companion object {
        val REMOTE_TASK_FILTERS = listOf("all", "pending_delivery", "delivered", "queued", "running", "waiting_input", "completed", "completed_without_model", "failed", "cancelled", "delivery_failed")
        private val DISPLAYABLE_EVENT_TYPES = setOf("state_changed", "question", "answer_submitted", "core_event", "artifact_available", "command_status", "delivery_failed")
        private val TASK_REFRESH_EVENT_TYPES = setOf("state_changed", "question", "answer_submitted", "command_status", "delivery_failed", "artifact_available")
    }
}
