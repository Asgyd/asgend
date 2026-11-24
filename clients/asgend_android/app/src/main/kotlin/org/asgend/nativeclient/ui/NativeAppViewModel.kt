package org.asgend.nativeclient.ui

import android.app.Application
import java.io.File
import java.io.FileOutputStream
import java.security.MessageDigest
import java.util.UUID
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.asgend.nativeclient.data.CoreRepository
import org.asgend.nativeclient.data.CoreRpcClient
import org.asgend.nativeclient.data.CoreSettings
import org.asgend.nativeclient.data.RuntimeConfigDraft
import org.asgend.nativeclient.data.SettingsStore
import org.json.JSONObject
import java.util.UUID
import kotlin.math.min

data class DownloadedArtifact(val absolutePath: String, val fileName: String, val mimeType: String, val size: Long)

class NativeAppViewModel(application: Application) : AndroidViewModel(application) {
    private val settingsStore = SettingsStore(application.applicationContext)
    private var repository: CoreRepository? = null
    private var taskRefreshInFlight = false
    private var detailRefreshInFlight = false
    private var planRefreshInFlight = false
    private var fileRefreshInFlight = false
    private var artifactRefreshInFlight = false
    private var artifactDownloadInFlight = false
    private var lspDiagnosticsInFlight = false
    private var pendingSubmitFingerprint: String? = null
    private var pendingSubmitKey: String? = null
    private var pendingFollowUpFingerprint: String? = null
    private var pendingFollowUpKey: String? = null
    private var lastEventId = 0L

    var settings by mutableStateOf(CoreSettings()); private set
    var settingsLoaded by mutableStateOf(false); private set
    var connected by mutableStateOf(false); private set
    var connecting by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var version by mutableStateOf<String?>(null); private set

    var tasks by mutableStateOf<List<JSONObject>>(emptyList()); private set
    var taskFilter by mutableStateOf("all"); private set
    var selectedTaskId by mutableStateOf<String?>(null); private set
    var activeTask by mutableStateOf<JSONObject?>(null); private set
    var taskEvents by mutableStateOf<List<JSONObject>>(emptyList()); private set
    var taskQuestion by mutableStateOf<JSONObject?>(null); private set
    var taskBusy by mutableStateOf(false); private set
    var taskLoading by mutableStateOf(false); private set

    var planItems by mutableStateOf<List<JSONObject>>(emptyList()); private set
    var planScope by mutableStateOf("global"); private set
    var planKind by mutableStateOf("all"); private set
    var planLoading by mutableStateOf(false); private set
    var planRevision by mutableStateOf(0L); private set

    var workspaceTaskId by mutableStateOf(""); private set
    var workspacePath by mutableStateOf(""); private set
    var workspaceEntries by mutableStateOf<List<JSONObject>>(emptyList()); private set
    var workspaceLoading by mutableStateOf(false); private set
    var openedFilePath by mutableStateOf<String?>(null); private set
    var openedFileContent by mutableStateOf(""); private set
    var openedFileBytes by mutableStateOf(0L); private set
    var openedFilePartial by mutableStateOf(false); private set
    var fileBusy by mutableStateOf(false); private set
    var workspaceArtifacts by mutableStateOf<List<JSONObject>>(emptyList()); private set
    var artifactsLoading by mutableStateOf(false); private set
    var artifactsTruncated by mutableStateOf(false); private set
    var artifactsSummary by mutableStateOf(""); private set
    var artifactDownloadPath by mutableStateOf<String?>(null); private set
    var artifactDownloadProgress by mutableStateOf(0f); private set
    var artifactDownloadError by mutableStateOf<String?>(null); private set
    var downloadedArtifact by mutableStateOf<DownloadedArtifact?>(null); private set
    var lspDiagnosticsBusy by mutableStateOf(false); private set
    var lspDiagnosticsText by mutableStateOf<String?>(null); private set
    var lspDiagnosticsError by mutableStateOf<String?>(null); private set

    var runtimeHealth by mutableStateOf<JSONObject?>(null); private set
    var runtimeModelStatus by mutableStateOf<JSONObject?>(null); private set
    var runtimeAgents by mutableStateOf<List<JSONObject>>(emptyList()); private set
    var runtimeResources by mutableStateOf<JSONObject?>(null); private set
    var runtimeMcpServers by mutableStateOf<List<JSONObject>>(emptyList()); private set
    var runtimeLoading by mutableStateOf(false); private set
    var runtimeActionKey by mutableStateOf<String?>(null); private set
    var runtimeError by mutableStateOf<String?>(null); private set
    private var runtimeRefreshInFlight = false

    // The full config response stays private in memory so unchanged credentials and registry files
    // can be round-tripped to the existing core API. Never expose or persist this snapshot in UI state.
    private var configSnapshot: JSONObject? = null
    var configDraft by mutableStateOf(RuntimeConfigDraft()); private set
    var configBaseline by mutableStateOf<RuntimeConfigDraft?>(null); private set
    var configLoaded by mutableStateOf(false); private set
    var configLoading by mutableStateOf(false); private set
    var configSaving by mutableStateOf(false); private set
    var configError by mutableStateOf<String?>(null); private set
    var configRestartRequired by mutableStateOf(false); private set
    var configRevision by mutableStateOf(0); private set

    init {
        viewModelScope.launch {
            settings = settingsStore.load()
            settingsLoaded = true
            if (settings.configured && settings.vpnTunnelAcknowledged) connect()
        }
    }

    private fun repo(): CoreRepository = repository ?: CoreRepository(CoreRpcClient(settings)).also { repository = it }

    fun saveSettings(host: String, portText: String, token: String, tunnelAcknowledged: Boolean) {
        val port = portText.trim().toIntOrNull()
        val candidate = CoreSettings(host.trim(), port ?: -1, token, tunnelAcknowledged)
        val validation = candidate.validationError()
        if (validation != null) { error = validation; return }
        viewModelScope.launch {
            connecting = true; error = null
            runCatching { settingsStore.save(candidate) }
                .onSuccess { settings = candidate; repository = null; connect() }
                .onFailure { error = it.message ?: "Could not save connection settings." }
            connecting = false
        }
    }

    fun connect() {
        if (!settings.configured) { connected = false; error = "Configure the ASGEND core connection first."; return }
        if (!settings.vpnTunnelAcknowledged) { connected = false; error = "Confirm the trusted VPN/SSH tunnel before connecting."; return }
        viewModelScope.launch {
            connecting = true; error = null
            runCatching { CoreRpcClient(settings).call(JSONObject().put("op", "ping"), 5_000) }
                .onSuccess { reply ->
                    repository = CoreRepository(CoreRpcClient(settings))
                    connected = true
                    version = reply.optString("version").takeIf(String::isNotBlank)
                    refreshTasks()
                }
                .onFailure { connected = false; version = null; error = it.message?.take(500) ?: "Unable to connect to ASGEND core." }
            connecting = false
        }
    }

    fun forgetToken() {
        viewModelScope.launch { runCatching { settingsStore.clearToken() }.onSuccess { settings = settings.copy(token = ""); repository = null; error = null }.onFailure { error = it.message ?: "Could not clear saved token." } }
    }

    fun selectTaskFilter(filter: String) { taskFilter = filter; refreshTasks() }

    fun refreshTasks() {
        if (!connected || taskRefreshInFlight) return
        viewModelScope.launch {
            taskRefreshInFlight = true
            runCatching { if (taskFilter == "dead_letters") repo().deadLetters() else repo().listTasks(taskFilter) }
                .onSuccess { tasks = it; if (error != null) error = null }
                .onFailure { showOperationError(it) }
            taskRefreshInFlight = false
        }
    }

    fun createTask(text: String, priority: Int) {
        if (taskBusy) return
        val cleaned = text.trim()
        if (cleaned.isEmpty()) { error = "Describe the task before starting it."; return }
        val fingerprint = "$priority\u0000$cleaned"
        if (pendingSubmitFingerprint != fingerprint || pendingSubmitKey == null) {
            pendingSubmitFingerprint = fingerprint
            pendingSubmitKey = "android-${UUID.randomUUID()}"
        }
        val key = pendingSubmitKey!!
        viewModelScope.launch {
            taskBusy = true; error = null
            runCatching { repo().submitTask(cleaned, priority, key) }
                .onSuccess { response ->
                    pendingSubmitFingerprint = null; pendingSubmitKey = null
                    refreshTasks()
                    openTask(response.optString("task_id"))
                }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    fun openTask(taskId: String) {
        if (taskId.isBlank()) return
        selectedTaskId = taskId
        activeTask = null
        taskEvents = emptyList()
        taskQuestion = null
        taskLoading = true
        lastEventId = 0L
        refreshTaskDetail()
    }

    fun closeTask() {
        selectedTaskId = null
        activeTask = null
        taskEvents = emptyList()
        taskQuestion = null
        lastEventId = 0L
        refreshTasks()
    }

    fun refreshTaskDetail() {
        val taskId = selectedTaskId ?: return
        if (!connected || detailRefreshInFlight) return
        viewModelScope.launch {
            detailRefreshInFlight = true
            runCatching {
                val status = repo().taskStatus(taskId)
                val newEvents = repo().taskEvents(taskId, lastEventId, 100)
                status to newEvents
            }.onSuccess { (status, newEvents) ->
                activeTask = status
                taskLoading = false
                val merged = taskEvents.toMutableList()
                val known = merged.mapTo(mutableSetOf()) { CoreRepository.eventId(it) }
                for (event in newEvents) {
                    val id = CoreRepository.eventId(event)
                    if (id > 0 && known.add(id)) merged += event
                    if (id > lastEventId) lastEventId = id
                }
                taskEvents = merged.sortedBy { CoreRepository.eventId(it) }
                taskQuestion = CoreRepository.parseQuestionFromStatus(status)
                    ?: taskEvents.asReversed().firstNotNullOfOrNull { event ->
                        if (event.optString("type") == "question" || event.optString("type") == "agent_question") CoreRepository.parseQuestionFromEvent(event) else null
                    }
                if (error != null) error = null
            }.onFailure { taskLoading = false; showOperationError(it) }
            detailRefreshInFlight = false
        }
    }

    fun answerQuestion(answer: String, optionId: String? = null) {
        val taskId = selectedTaskId ?: return
        if (taskBusy || answer.isBlank()) return
        viewModelScope.launch {
            taskBusy = true; error = null
            runCatching { repo().answerTask(taskId, answer, optionId) }
                .onSuccess { taskQuestion = null; refreshTaskDetail() }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    fun followUp(text: String) {
        val taskId = selectedTaskId ?: return
        val cleaned = text.trim()
        if (taskBusy || cleaned.isBlank()) return
        val fingerprint = "$taskId\u0000$cleaned"
        if (pendingFollowUpFingerprint != fingerprint || pendingFollowUpKey == null) {
            pendingFollowUpFingerprint = fingerprint
            pendingFollowUpKey = "android-followup-${UUID.randomUUID()}"
        }
        val key = pendingFollowUpKey!!
        viewModelScope.launch {
            taskBusy = true; error = null
            runCatching { repo().submitTask(cleaned, 5, key, "task-$taskId") }
                .onSuccess { response ->
                    pendingFollowUpFingerprint = null; pendingFollowUpKey = null
                    refreshTasks(); openTask(response.optString("task_id"))
                }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    fun cancelActiveTask() {
        val task = activeTask ?: return
        if (taskBusy) return
        viewModelScope.launch {
            taskBusy = true
            runCatching { repo().cancelTask(task.optString("task_id")) }
                .onSuccess { refreshTaskDetail(); refreshTasks() }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    fun retryActiveTask() {
        val task = activeTask ?: return
        if (task.optString("status") != "failed" || taskBusy) return
        viewModelScope.launch {
            taskBusy = true
            runCatching { repo().retryTask(task.optString("task_id")) }
                .onSuccess { refreshTaskDetail(); refreshTasks() }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    suspend fun awaitPlanNotification(): JSONObject {
        val response = repo().planWait(planRevision, 15_000)
        val nextRevision = response.optLong("revision", planRevision)
        if (nextRevision >= 0L) planRevision = nextRevision
        return response
    }

    fun loadPlan(scope: String = planScope, kind: String = planKind) {
        planScope = scope.ifBlank { "global" }; planKind = kind
        if (!connected || planRefreshInFlight) return
        viewModelScope.launch {
            planRefreshInFlight = true; planLoading = true
            runCatching { repo().planList(planScope, planKind) }
                .onSuccess { planItems = it; if (error != null) error = null }
                .onFailure { showOperationError(it) }
            planLoading = false; planRefreshInFlight = false
        }
    }

    fun addPlanItem(kind: String, title: String, description: String) {
        if (title.isBlank() || taskBusy) return
        viewModelScope.launch {
            taskBusy = true
            runCatching { repo().planAdd(planScope, kind, title, description) }
                .onSuccess { reloadPlanAfterMutation() }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    fun updatePlanItem(item: JSONObject, title: String? = null, description: String? = null, status: String? = null) {
        val id = item.optLong("item_id", 0L)
        if (id <= 0 || taskBusy) return
        viewModelScope.launch {
            taskBusy = true
            runCatching { repo().planUpdate(planScope, id, title, description, status) }
                .onSuccess { reloadPlanAfterMutation() }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    fun deletePlanItem(item: JSONObject) {
        val id = item.optLong("item_id", 0L)
        if (id <= 0 || taskBusy) return
        viewModelScope.launch {
            taskBusy = true
            runCatching { repo().planDelete(planScope, id) }
                .onSuccess { reloadPlanAfterMutation() }
                .onFailure { showOperationError(it) }
            taskBusy = false
        }
    }

    private fun reloadPlanAfterMutation() {
        viewModelScope.launch {
            // Let any in-flight query finish before refreshing the mutation result.
            while (planRefreshInFlight) delay(50)
            if (!connected) return@launch
            planRefreshInFlight = true
            planLoading = true
            runCatching { repo().planList(planScope, planKind) }
                .onSuccess { planItems = it }
                .onFailure { showOperationError(it) }
            planLoading = false
            planRefreshInFlight = false
        }
    }

    fun chooseWorkspaceTask(taskId: String) {
        if (artifactDownloadInFlight) return
        downloadedArtifact?.let { old -> runCatching { File(old.absolutePath).delete() } }
        workspaceTaskId = taskId
        workspacePath = ""
        workspaceEntries = emptyList()
        workspaceArtifacts = emptyList()
        artifactsSummary = ""
        artifactsTruncated = false
        downloadedArtifact = null
        artifactDownloadError = null
        loadWorkspace()
        refreshArtifacts()
    }

    fun navigateWorkspace(path: String) { workspacePath = path; loadWorkspace() }

    fun parentWorkspace() {
        workspacePath = workspacePath.substringBeforeLast('/', "")
        loadWorkspace()
    }

    fun loadWorkspace() {
        if (!connected || workspaceTaskId.isBlank() || fileRefreshInFlight) return
        viewModelScope.launch {
            fileRefreshInFlight = true; workspaceLoading = true
            runCatching { repo().workspaceList(workspaceTaskId, workspacePath) }
                .onSuccess { result -> workspaceEntries = org.asgend.nativeclient.data.jsonArrayObjects(result.optJSONArray("entries")); if (error != null) error = null }
                .onFailure { showOperationError(it) }
            workspaceLoading = false; fileRefreshInFlight = false
        }
    }

    fun refreshArtifacts() {
        val taskId = workspaceTaskId
        if (!connected || taskId.isBlank() || artifactRefreshInFlight) return
        viewModelScope.launch {
            artifactRefreshInFlight = true
            artifactsLoading = true
            runCatching { repo().artifactList(taskId) }
                .onSuccess { reply ->
                    if (workspaceTaskId == taskId) {
                        workspaceArtifacts = org.asgend.nativeclient.data.jsonArrayObjects(reply.optJSONArray("artifacts"))
                        artifactsTruncated = reply.optBoolean("truncated", false)
                        val skips = listOf(
                            "too large" to reply.optInt("skipped_too_large", 0),
                            "too deep" to reply.optInt("skipped_too_deep", 0),
                            "special files" to reply.optInt("skipped_special", 0),
                            "unsafe names" to reply.optInt("skipped_unsafe_name", 0),
                            "unreadable" to reply.optInt("skipped_unreadable", 0),
                            "internal" to reply.optInt("skipped_internal", 0),
                        ).filter { it.second > 0 }
                        artifactsSummary = buildString {
                            append("${reply.optInt("count", workspaceArtifacts.size)} artifacts")
                            if (artifactsTruncated) append(" · incomplete scan: ${reply.optString("truncated_reason", "core limit")}")
                            if (skips.isNotEmpty()) append(" · skipped " + skips.joinToString { "${it.second} ${it.first}" })
                        }
                    }
                    if (error != null) error = null
                }
                .onFailure { showOperationError(it) }
            artifactsLoading = false
            artifactRefreshInFlight = false
            // If the selected task changed while the previous artifact request was in flight,
            // immediately fetch the newly selected task instead of leaving stale results visible.
            if (connected && workspaceTaskId.isNotBlank() && workspaceTaskId != taskId) refreshArtifacts()
        }
    }

    fun openWorkspaceFile(path: String) {
        if (workspaceTaskId.isBlank()) return
        viewModelScope.launch {
            fileBusy = true; error = null
            runCatching { repo().workspaceRead(workspaceTaskId, path) }
                .onSuccess { reply -> openedFilePath = path; openedFileContent = reply.optString("content"); openedFileBytes = reply.optLong("size", openedFileContent.toByteArray().size.toLong()); openedFilePartial = false }
                .onFailure {
                    runCatching { repo().workspaceReadChunk(workspaceTaskId, path) }
                        .onSuccess { reply ->
                            openedFilePath = path
                            openedFileContent = reply.optString("content")
                            openedFileBytes = reply.optLong("size", openedFileContent.toByteArray().size.toLong())
                            openedFilePartial = !reply.optBoolean("eof", false)
                            if (openedFilePartial) error = "Showing a bounded preview. Large files open read-only."
                        }
                        .onFailure { second -> showOperationError(second) }
                }
            fileBusy = false
        }
    }

    fun closeWorkspaceFile() { openedFilePath = null; openedFileContent = ""; openedFileBytes = 0L; openedFilePartial = false }

    fun saveWorkspaceFile(path: String, content: String, onSaved: () -> Unit = {}) {
        if (fileBusy || openedFilePartial) return
        viewModelScope.launch {
            fileBusy = true; error = null
            runCatching { repo().workspaceWrite(workspaceTaskId, path, content) }
                .onSuccess { openedFileContent = content; openedFileBytes = content.toByteArray().size.toLong(); closeWorkspaceFile(); loadWorkspace(); refreshArtifacts(); onSaved() }
                .onFailure { showOperationError(it) }
            fileBusy = false
        }
    }


    fun downloadArtifact(artifact: JSONObject) {
        if (!connected || artifactDownloadInFlight || workspaceTaskId.isBlank()) return
        val taskId = workspaceTaskId
        val relativePath = artifact.optString("path")
        val expectedSize = artifact.optLong("size", -1L)
        val expectedSha = artifact.optString("sha256").lowercase()
        val mime = artifact.optString("mime", "application/octet-stream")
        if (relativePath.isBlank() || expectedSize < 0L || expectedSize > MAX_NATIVE_ARTIFACT_EXPORT_BYTES || !expectedSha.matches(Regex("[0-9a-f]{64}"))) {
            artifactDownloadError = "This artifact is missing valid integrity metadata or exceeds the 64 MiB native export limit."
            return
        }
        viewModelScope.launch {
            artifactDownloadInFlight = true
            artifactDownloadPath = relativePath
            artifactDownloadProgress = 0f
            artifactDownloadError = null
            val priorDownload = downloadedArtifact
            downloadedArtifact = null
            var partial: File? = null
            try {
                runCatching {
                val cache = File(getApplication<Application>().cacheDir, "artifacts")
                if (!cache.exists() && !cache.mkdirs()) throw IllegalStateException("Artifact cache is unavailable.")
                partial = File.createTempFile("asgend-artifact-", ".part", cache)
                val digest = MessageDigest.getInstance("SHA-256")
                var offset = 0L
                if (expectedSize == 0L) {
                    val empty = repo().workspaceReadBytes(taskId, relativePath, 0L, 1)
                    if (empty.size != 0L || empty.bytes.isNotEmpty() || empty.nextOffset != 0L || !empty.eof) {
                        throw IllegalStateException("Empty artifact changed during download.")
                    }
                }
                FileOutputStream(partial!!).use { output ->
                    while (offset < expectedSize) {
                        val chunk = repo().workspaceReadBytes(taskId, relativePath, offset, ARTIFACT_CHUNK_BYTES)
                        if (chunk.size != expectedSize || chunk.nextOffset <= offset || chunk.nextOffset - offset != chunk.bytes.size.toLong()) {
                            throw IllegalStateException("Artifact size changed during download.")
                        }
                        if (chunk.bytes.isEmpty()) throw IllegalStateException("Artifact transfer made no progress.")
                        output.write(chunk.bytes)
                        digest.update(chunk.bytes)
                        offset = chunk.nextOffset
                        artifactDownloadProgress = (offset.toDouble() / expectedSize.toDouble()).toFloat().coerceIn(0f, 1f)
                    }
                    output.fd.sync()
                }
                val actualSha = digest.digest().joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
                if (offset != expectedSize || actualSha != expectedSha) throw IllegalStateException("Artifact integrity check failed.")
                if (expectedSize == 0L) artifactDownloadProgress = 1f
                val rawName = relativePath.substringAfterLast('/').take(80)
                val safeName = rawName.replace(Regex("[^A-Za-z0-9._ -]"), "_").trim().takeIf { it.isNotBlank() && it != "." && it != ".." } ?: "artifact.bin"
                val completed = File(cache, "${UUID.randomUUID()}-$safeName")
                if (!partial!!.renameTo(completed)) throw IllegalStateException("Could not finalize verified artifact.")
                partial = null
                DownloadedArtifact(completed.absolutePath, safeName, mime.takeIf { it.matches(Regex("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+")) } ?: "application/octet-stream", expectedSize)
            }.onSuccess { verified ->
                downloadedArtifact = verified
                priorDownload?.let { old -> runCatching { File(old.absolutePath).delete() } }
                }.onFailure { failure ->
                    partial?.let { runCatching { it.delete() } }
                    if (failure is CancellationException) throw failure
                    downloadedArtifact = priorDownload
                    artifactDownloadError = "The artifact could not be downloaded and integrity-verified. The source file may have changed or the transfer was interrupted."
                }
            } finally {
                artifactDownloadInFlight = false
                artifactDownloadPath = null
            }
        }
    }

    fun clearArtifactDownloadError() { artifactDownloadError = null }

    fun runLspDiagnostics(relativePath: String) {
        if (!connected || lspDiagnosticsInFlight) return
        val path = relativePath.trim()
        viewModelScope.launch {
            lspDiagnosticsInFlight = true
            lspDiagnosticsBusy = true
            lspDiagnosticsError = null
            lspDiagnosticsText = null
            runCatching { repo().lspDiagnostics(path) }
                .onSuccess { lspDiagnosticsText = it.ifBlank { "No diagnostics returned." } }
                .onFailure {
                    lspDiagnosticsError = "Could not run diagnostics. Verify that the relative path is inside the configured project root and an LSP server is enabled for this file type."
                }
            lspDiagnosticsBusy = false
            lspDiagnosticsInFlight = false
        }
    }

    fun clearLspDiagnostics() { lspDiagnosticsText = null; lspDiagnosticsError = null }

    companion object {
        const val ARTIFACT_CHUNK_BYTES = 256 * 1024
        const val MAX_NATIVE_ARTIFACT_EXPORT_BYTES = 64L * 1024L * 1024L
    }

    fun refreshRuntime() {
        if (!connected || runtimeRefreshInFlight) return
        viewModelScope.launch {
            runtimeRefreshInFlight = true
            runtimeLoading = true
            val currentRepo = repo()
            val healthResult = runCatching { currentRepo.health() }
            val modelsResult = runCatching { currentRepo.models() }
            val agentsResult = runCatching { currentRepo.agents() }
            val resourcesResult = runCatching { currentRepo.resources() }
            val mcpResult = runCatching { currentRepo.mcpStatus() }
            healthResult.onSuccess { runtimeHealth = it }
            modelsResult.onSuccess { runtimeModelStatus = it.optJSONObject("model_status") ?: it }
            agentsResult.onSuccess { runtimeAgents = it }
            resourcesResult.onSuccess { runtimeResources = it }
            mcpResult.onSuccess { runtimeMcpServers = it }
            val failures = listOf(healthResult, modelsResult, agentsResult, resourcesResult, mcpResult).filter { it.isFailure }
            runtimeError = if (failures.isEmpty()) null else "One or more runtime requests failed. Check the core connection and refresh."
            runtimeLoading = false
            runtimeRefreshInFlight = false
        }
    }

    fun refreshConfig() {
        if (!connected || configLoading || configSaving) return
        viewModelScope.launch {
            configLoading = true
            configError = null
            try {
                val response = repo().configGet()
                val main = response.optJSONObject("main") ?: throw IllegalStateException("missing main config")
                val draft = RuntimeConfigDraft.fromMain(main)
                // Keep secrets only in a private, transient in-memory snapshot. Never serialize it to logs/UI/preferences.
                configSnapshot = JSONObject(response.toString())
                configDraft = draft
                configBaseline = draft
                configRestartRequired = response.optBoolean("restart_required", false)
                configLoaded = true
                configRevision++
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Throwable) {
                configError = "Unable to load configuration from the core. Check the connection and try again."
            } finally {
                configLoading = false
            }
        }
    }

    fun saveConfigDraft(candidate: RuntimeConfigDraft) {
        if (configSaving) return
        val validation = candidate.validationError()
        if (validation != null) {
            configError = validation
            return
        }
        val snapshot = configSnapshot
        if (snapshot == null || !configLoaded) {
            configError = "Load the current core configuration before saving."
            return
        }
        viewModelScope.launch {
            configSaving = true
            configError = null
            try {
                val nextSnapshot = JSONObject(snapshot.toString())
                val mainSnapshot = nextSnapshot.optJSONObject("main")
                    ?: throw IllegalStateException("missing configuration snapshot")
                candidate.applyTo(mainSnapshot)
                // The network payload contains only explicit allow-listed fields. Hidden values stay in memory
                // only to support the currently loaded snapshot; they are never sent back in this operation.
                val response = repo().configUpdateSettings(candidate.toSettingsJson())
                configSnapshot = nextSnapshot
                configDraft = candidate
                configBaseline = candidate
                configRestartRequired = response.optBoolean("restart_required", true)
                configRevision++
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Throwable) {
                // Do not reflect raw server errors here: the config API can process fields containing credentials.
                configError = "The core rejected the configuration update. No success was reported; reload config before retrying."
            } finally {
                configSaving = false
            }
        }
    }

    fun performModelAction(modelId: String, kind: String, action: String) {
        if (runtimeActionKey != null) return
        val op = when (action) {
            "enable" -> "model_enable"
            "disable" -> "model_disable"
            "start" -> "model_start"
            "stop" -> "model_stop"
            else -> return
        }
        runtimeActionKey = "$kind:$modelId:$action"
        runtimeError = null
        viewModelScope.launch {
            runCatching { repo().modelAction(op, modelId, kind) }
                .onFailure { runtimeError = "The core rejected the model operation. Refresh state and verify the model configuration." }
            runtimeActionKey = null
            // Refresh the snapshot after either success or error so the screen shows actual core state.
            refreshRuntime()
        }
    }

    private fun showOperationError(throwable: Throwable) {
        error = (throwable.message ?: "The core request failed.").take(500)
        if (error?.contains("Cannot reach core", ignoreCase = true) == true || error?.contains("Timed out", ignoreCase = true) == true) connected = false
    }
}
