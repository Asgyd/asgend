package org.asgend.nativeclient.ui.remote

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDone
import androidx.compose.material.icons.outlined.CloudOff
import androidx.compose.material.icons.outlined.Devices
import androidx.compose.material.icons.outlined.FileDownload
import androidx.compose.material.icons.outlined.Logout
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import org.asgend.nativeclient.data.ControlPlaneArtifact
import org.asgend.nativeclient.data.ControlPlaneTask
import androidx.core.content.FileProvider
import android.content.ClipData
import android.content.Intent
import java.io.File

@Composable
fun RemoteControlSection(model: NativeRemoteViewModel, modifier: Modifier = Modifier) {
    var endpoint by rememberSaveable(model.settings.endpoint) { mutableStateOf(model.settings.endpoint) }
    var deviceName by rememberSaveable(model.settings.deviceName) { mutableStateOf(model.settings.deviceName) }
    // A one-time pairing code is deliberately not saveable and never written to a bundle/preferences.
    var pairingCode by remember { mutableStateOf("") }
    var confirmLogout by remember { mutableStateOf(false) }
    var confirmCancelTask by remember { mutableStateOf<ControlPlaneTask?>(null) }
    var confirmRetryTask by remember { mutableStateOf<ControlPlaneTask?>(null) }
    var confirmExpiredOperationReview by remember { mutableStateOf(false) }

    LaunchedEffect(model.settingsLoaded, model.settings.endpoint, model.settings.deviceName) {
        if (model.settings.endpoint.isNotBlank()) endpoint = model.settings.endpoint
        if (model.settings.deviceName.isNotBlank()) deviceName = model.settings.deviceName
    }
    LaunchedEffect(model.selectedRemoteTask?.taskId, model.settings.paired) {
        val taskId = model.selectedRemoteTask?.taskId ?: return@LaunchedEffect
        model.refreshRemoteArtifacts(taskId)
        while (model.settings.paired && model.selectedRemoteTask?.taskId == taskId) {
            val success = model.pollRemoteTaskEvents(taskId)
            delay(if (success) 350L else 2_000L)
        }
    }

    Column(modifier = modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(14.dp)) {
        Text("Remote Control Plane", style = MaterialTheme.typography.headlineSmall)
        Text(
            "HTTPS workspace access is independent from the local core TCP connection. Remote mutations require the current workspace role to be owner or operator; the server enforces permissions again.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (model.credentialCleanupNeedsRetry) {
            Surface(color = MaterialTheme.colorScheme.errorContainer, shape = MaterialTheme.shapes.medium) {
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        "The session is disabled in memory, but encrypted credential cleanup did not finish. No authenticated Control Plane requests will be sent until you pair again.",
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Button(onClick = model::retryCredentialCleanup, enabled = !model.busy) { Text("Retry local credential cleanup") }
                }
            }
        }
        Card {
            Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Icon(
                        if (model.settings.paired && model.statusText.startsWith("Connected")) Icons.Outlined.CloudDone else Icons.Outlined.CloudOff,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                    )
                    Column(Modifier.weight(1f)) {
                        Text(model.statusText, style = MaterialTheme.typography.titleMedium)
                        Text(
                            if (model.settings.paired) "${model.settings.endpoint} · ${model.currentWorkspace?.role ?: model.settings.role}"
                            else "Not paired · no Control Plane token is stored",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    if (model.settings.paired) {
                        androidx.compose.material3.IconButton(onClick = model::refresh, enabled = !model.refreshing && !model.busy) {
                            Icon(Icons.Outlined.Refresh, contentDescription = "Refresh remote session")
                        }
                    }
                }
                if (!model.settings.paired) {
                    OutlinedTextField(
                        value = endpoint, onValueChange = { endpoint = it }, label = { Text("HTTPS endpoint") },
                        placeholder = { Text("https://control.example.com") },
                        supportingText = { Text("HTTPS only. Redirects, URL credentials, query strings and fragments are refused.") },
                        singleLine = true, enabled = !model.busy, modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = deviceName, onValueChange = { deviceName = it.take(80) }, label = { Text("Device name") },
                        singleLine = true, enabled = !model.busy, modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = pairingCode,
                        onValueChange = { pairingCode = it.uppercase().filter { ch -> ch.isLetterOrDigit() || ch == '-' || ch == ' ' }.take(64) },
                        label = { Text("One-time pairing code") }, placeholder = { Text("ABCDE-12345") },
                        visualTransformation = PasswordVisualTransformation(), singleLine = true, enabled = !model.busy,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Surface(color = MaterialTheme.colorScheme.tertiaryContainer, shape = MaterialTheme.shapes.medium) {
                        Text(
                            "The pairing code is sent once over HTTPS and is not saved. The returned user token is encrypted with a separate Android Keystore key.",
                            Modifier.padding(12.dp), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onTertiaryContainer,
                        )
                    }
                    Button(
                        onClick = { val code = pairingCode; pairingCode = ""; model.pair(endpoint, code, deviceName) },
                        enabled = !model.busy && endpoint.isNotBlank() && pairingCode.isNotBlank(),
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        if (model.busy) CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                        else { Icon(Icons.Outlined.CloudDone, contentDescription = null); Spacer(Modifier.width(8.dp)); Text("Pair device") }
                    }
                } else {
                    Text("Signed in as ${model.identity?.displayName?.ifBlank { model.settings.userId } ?: model.settings.userId}", style = MaterialTheme.typography.bodyLarge)
                    Text("User ID: ${model.settings.userId}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (model.identity?.workspaces.orEmpty().size > 1) {
                        Text("Workspace", style = MaterialTheme.typography.titleMedium)
                        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            model.identity?.workspaces.orEmpty().forEach { ws ->
                                FilterChip(
                                    selected = ws.workspaceId == model.settings.workspaceId,
                                    onClick = { model.selectWorkspace(ws.workspaceId) },
                                    enabled = !model.busy && !model.remoteTaskBusy,
                                    label = { Text("${ws.name} · ${ws.role}") },
                                )
                            }
                        }
                    } else {
                        model.currentWorkspace?.let { Text("Workspace: ${it.name} · ${it.role}", style = MaterialTheme.typography.titleMedium) }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Icon(Icons.Outlined.Devices, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                        Text("${model.nodes.size} node(s)", style = MaterialTheme.typography.titleSmall)
                    }
                    model.nodes.forEach { node ->
                        Surface(color = MaterialTheme.colorScheme.surfaceContainerLow, shape = MaterialTheme.shapes.medium) {
                            Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text(node.name, style = MaterialTheme.typography.titleSmall)
                                    Text("${node.nodeId} · ${node.coreVersion.ifBlank { "core version unknown" }}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                                Text(node.status, style = MaterialTheme.typography.labelMedium, color = if (node.status == "online") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                    RemoteTasksSection(
                        model = model,
                        onConfirmCancel = { confirmCancelTask = it },
                        onConfirmRetry = { confirmRetryTask = it },
                    )
                    RemoteArtifactsSection(model = model)
                    if (model.expiredOperationReview != null) {
                        Surface(color = MaterialTheme.colorScheme.errorContainer, shape = MaterialTheme.shapes.medium) {
                            Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text(
                                    "An idempotent operation is older than seven days and its server outcome is unknown. Check the remote task list and event history before clearing this retry record.",
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onErrorContainer,
                                )
                                OutlinedButton(onClick = { confirmExpiredOperationReview = true }, enabled = !model.remoteTaskBusy && !model.busy) {
                                    Text("I checked remote state…")
                                }
                            }
                        }
                    }
                    FilledTonalButton(onClick = { confirmLogout = true }, enabled = !model.busy, modifier = Modifier.fillMaxWidth()) {
                        Icon(Icons.Outlined.Logout, contentDescription = null); Spacer(Modifier.width(8.dp)); Text("Sign out and revoke token")
                    }
                }
                model.error?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
            }
        }
    }

    if (confirmExpiredOperationReview) AlertDialog(
        onDismissRequest = { confirmExpiredOperationReview = false },
        title = { Text("Clear expired retry metadata?") },
        text = { Text("Confirm only if you checked the remote workspace and resolved whether the original operation was accepted. Clearing permits a later retry to use a new key and can create a duplicate if your review was incomplete.") },
        confirmButton = { Button(onClick = { confirmExpiredOperationReview = false; model.confirmExpiredOperationReviewed() }, enabled = !model.busy) { Text("Clear after review") } },
        dismissButton = { TextButton(onClick = { confirmExpiredOperationReview = false }) { Text("Keep record") } },
    )

    if (confirmLogout) AlertDialog(
        onDismissRequest = { confirmLogout = false }, title = { Text("Sign out from Control Plane?") },
        text = { Text("ASGEND will request server-side token revocation. The token is removed locally even if the server is offline. Local core connection credentials are not affected.") },
        confirmButton = { Button(onClick = { confirmLogout = false; model.logout() }, enabled = !model.busy) { Text("Sign out") } },
        dismissButton = { TextButton(onClick = { confirmLogout = false }) { Text("Cancel") } },
    )
    confirmCancelTask?.let { task -> AlertDialog(
        onDismissRequest = { confirmCancelTask = null }, title = { Text("Request remote task cancellation?") },
        text = { Text("This sends a durable cancellation command to the Control Plane. The execution node may apply it asynchronously.") },
        confirmButton = { Button(onClick = { confirmCancelTask = null; model.cancelRemoteTask(task.taskId) }, enabled = model.canOperate && !model.remoteTaskBusy) { Text("Request cancellation") } },
        dismissButton = { TextButton(onClick = { confirmCancelTask = null }) { Text("Keep running") } },
    ) }
    confirmRetryTask?.let { task -> AlertDialog(
        onDismissRequest = { confirmRetryTask = null }, title = { Text("Retry remote task?") },
        text = { Text("The Control Plane will validate whether this task can be retried. Acceptance is asynchronous and does not prove the node has started execution.") },
        confirmButton = { Button(onClick = { confirmRetryTask = null; model.retryRemoteTask(task.taskId) }, enabled = model.canOperate && !model.remoteTaskBusy) { Text("Retry task") } },
        dismissButton = { TextButton(onClick = { confirmRetryTask = null }) { Text("Cancel") } },
    ) }
}

@Composable
private fun RemoteArtifactsSection(model: NativeRemoteViewModel) {
    val context = LocalContext.current
    val task = model.selectedRemoteTask ?: return
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Column(Modifier.weight(1f)) {
                Text("Remote artifacts", style = MaterialTheme.typography.titleLarge)
                Text("Files are downloaded through a short-lived single-artifact ticket without sending the user bearer token to the byte endpoint. A file is shareable only after size and SHA-256 verification.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            TextButton(onClick = { model.refreshRemoteArtifacts(task.taskId) }, enabled = !model.remoteArtifactsLoading) { Text("Refresh") }
        }
        if (model.remoteArtifactsLoading) CircularProgressIndicator(Modifier.size(22.dp))
        model.remoteArtifactError?.let { message ->
            Surface(color = MaterialTheme.colorScheme.errorContainer, shape = MaterialTheme.shapes.medium) {
                Column(Modifier.fillMaxWidth().padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(message, color = MaterialTheme.colorScheme.onErrorContainer, style = MaterialTheme.typography.bodySmall)
                    TextButton(onClick = model::clearRemoteArtifactError) { Text("Dismiss") }
                }
            }
        }
        if (!model.remoteArtifactsLoading && model.remoteArtifacts.isEmpty() && model.remoteArtifactError == null) {
            Text("No remote artifacts are registered for this task yet.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (model.remoteArtifacts.isNotEmpty()) {
            LazyColumn(
                modifier = Modifier.fillMaxWidth().heightIn(max = 480.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                items(model.remoteArtifacts, key = { it.artifactId }) { artifact ->
                    RemoteArtifactCard(artifact = artifact, model = model)
                }
            }
        }
        model.verifiedRemoteArtifact?.let { verified ->
            Card {
                Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Verified download", style = MaterialTheme.typography.titleMedium)
                    Text(verified.artifact.name, style = MaterialTheme.typography.bodyLarge)
                    Text("${formatArtifactSize(verified.artifact.size)} · ${verified.artifact.mime}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text("SHA-256: ${verified.sha256}", style = MaterialTheme.typography.bodySmall)
                    Button(onClick = { shareVerifiedRemoteArtifact(context, verified.file, verified.artifact.name, verified.artifact.mime) }, modifier = Modifier.fillMaxWidth()) {
                        Icon(Icons.Outlined.Share, contentDescription = null); Spacer(Modifier.width(8.dp)); Text("Share verified file")
                    }
                }
            }
        }
    }
}

@Composable
private fun RemoteArtifactCard(artifact: ControlPlaneArtifact, model: NativeRemoteViewModel) {
    val downloading = model.downloadingRemoteArtifactId == artifact.artifactId
    Card {
        Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Column(Modifier.weight(1f)) {
                    Text(artifact.name, style = MaterialTheme.typography.titleSmall)
                    Text("${formatArtifactSize(artifact.size)} · ${artifact.mime}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Text(artifact.state, style = MaterialTheme.typography.labelMedium, color = if (artifact.available) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Text("SHA-256: ${artifact.sha256}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (downloading) {
                LinearProgressIndicator(progress = { model.remoteArtifactDownloadProgress }, modifier = Modifier.fillMaxWidth())
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Downloading ${formatArtifactSize((artifact.size * model.remoteArtifactDownloadProgress).toLong())} / ${formatArtifactSize(artifact.size)}", modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodySmall)
                    TextButton(onClick = model::cancelRemoteArtifactDownload) { Text("Cancel") }
                }
            } else {
                Button(
                    onClick = { model.downloadRemoteArtifact(artifact) },
                    enabled = artifact.available && model.downloadingRemoteArtifactId == null && model.verifiedRemoteArtifact?.artifact?.artifactId != artifact.artifactId,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Icon(Icons.Outlined.FileDownload, contentDescription = null); Spacer(Modifier.width(8.dp)); Text(if (artifact.available) "Download and verify" else "Not downloadable")
                }
            }
        }
    }
}

private fun shareVerifiedRemoteArtifact(context: android.content.Context, file: File, name: String, mime: String) {
    val root = File(context.cacheDir, "artifacts").canonicalFile
    val actual = file.canonicalFile
    require(actual.path.startsWith(root.path + File.separator) && actual.isFile) { "Only a verified app-private artifact can be shared." }
    val safeMime = mime.substringBefore(';').takeIf { it.matches(Regex("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+")) } ?: "application/octet-stream"
    val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", actual)
    val intent = Intent(Intent.ACTION_SEND).apply {
        type = safeMime
        putExtra(Intent.EXTRA_STREAM, uri)
        putExtra(Intent.EXTRA_TITLE, name.take(255))
        clipData = ClipData.newUri(context.contentResolver, name.take(255), uri)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    context.startActivity(Intent.createChooser(intent, "Share verified artifact"))
}

private fun formatArtifactSize(size: Long): String = when {
    size < 1024 -> "$size B"
    size < 1024 * 1024 -> "%.1f KiB".format(size / 1024.0)
    else -> "%.1f MiB".format(size / (1024.0 * 1024.0))
}

@Composable
private fun RemoteTasksSection(
    model: NativeRemoteViewModel,
    onConfirmCancel: (ControlPlaneTask) -> Unit,
    onConfirmRetry: (ControlPlaneTask) -> Unit,
) {
    var newTask by rememberSaveable(model.settings.workspaceId) { mutableStateOf("") }
    var selectedNode by rememberSaveable(model.settings.workspaceId) { mutableStateOf("") }
    var nodeMenuExpanded by remember { mutableStateOf(false) }
    var answerText by rememberSaveable(model.selectedRemoteTask?.taskId) { mutableStateOf("") }
    var priority by rememberSaveable(model.settings.workspaceId) { mutableStateOf(5f) }

    LaunchedEffect(model.nodes, model.settings.workspaceId) {
        if (model.nodes.none { it.nodeId == selectedNode }) selectedNode = model.nodes.firstOrNull()?.nodeId.orEmpty()
    }
    LaunchedEffect(model.selectedRemoteTask?.taskId, model.selectedRemoteTask?.question?.text) { answerText = "" }
    LaunchedEffect(model.lastCreatedRemoteTaskId) { if (model.lastCreatedRemoteTaskId.isNotBlank()) newTask = "" }

    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text("Remote tasks", style = MaterialTheme.typography.titleLarge)
        Text("Tasks are stored by the Control Plane. Commands are durable requests; execution state changes when the remote node reports events.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (model.canOperate) {
            OutlinedTextField(
                value = newTask, onValueChange = { newTask = it.take(262_144) },
                label = { Text("New task") }, placeholder = { Text("Describe work for the selected node") },
                minLines = 2, maxLines = 5, modifier = Modifier.fillMaxWidth(), enabled = !model.remoteTaskBusy,
            )
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Priority ${priority.toInt()}", style = MaterialTheme.typography.labelLarge)
                Slider(value = priority, onValueChange = { priority = it }, valueRange = 0f..9f, steps = 8, modifier = Modifier.weight(1f), enabled = !model.remoteTaskBusy)
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Column(Modifier.weight(1f)) {
                    Text("Execution node", style = MaterialTheme.typography.labelLarge)
                    TextButton(onClick = { nodeMenuExpanded = true }, enabled = model.nodes.isNotEmpty() && !model.remoteTaskBusy) {
                        Text(model.nodes.firstOrNull { it.nodeId == selectedNode }?.name ?: "Select node")
                    }
                    DropdownMenu(expanded = nodeMenuExpanded, onDismissRequest = { nodeMenuExpanded = false }) {
                        model.nodes.forEach { node -> DropdownMenuItem(
                            text = { Text("${node.name} · ${node.status}") },
                            onClick = { selectedNode = node.nodeId; nodeMenuExpanded = false },
                        ) }
                    }
                }
                Button(
                    onClick = { model.createRemoteTask(selectedNode, newTask, priority.toInt()) },
                    enabled = !model.remoteTaskBusy && selectedNode.isNotBlank() && newTask.isNotBlank(),
                ) { Text(if (model.remoteTaskBusy) "Working…" else "Create") }
            }
        } else {
            Text("This membership is read-only. Task submission and commands are disabled.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            NativeRemoteViewModel.REMOTE_TASK_FILTERS.forEach { filter ->
                FilterChip(
                    selected = filter == model.remoteTaskFilter,
                    onClick = { model.setRemoteTaskFilter(filter) },
                    enabled = !model.remoteTaskLoading && !model.remoteTaskBusy,
                    label = { Text(filter.replace('_', ' ')) },
                )
            }
            TextButton(onClick = { model.loadRemoteTasks() }, enabled = !model.remoteTaskLoading) { Text("Refresh") }
        }
        if (model.remoteTaskLoading) CircularProgressIndicator(Modifier.size(22.dp))
        Text(model.remoteTaskStatus, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        model.remoteTaskError?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error) }
        model.remoteTasks.forEach { task ->
            Card(onClick = { model.openRemoteTask(task.taskId) }, modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(task.title, style = MaterialTheme.typography.titleMedium)
                    Text("${task.state.replace('_', ' ')} · ${task.nodeId}", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.primary)
                    Text(task.taskId, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (task.stale) Text("Node status is stale/offline; state may be out of date.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                }
            }
        }
        if (model.hasMoreRemoteTasks) TextButton(onClick = { model.loadRemoteTasks(loadMore = true) }, enabled = !model.remoteTaskLoading && !model.remoteTaskBusy) { Text("Load more tasks") }

        model.selectedRemoteTask?.let { task ->
            Card {
                Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(task.title, style = MaterialTheme.typography.titleLarge)
                            Text("${task.state.replace('_', ' ')} · ${task.taskId}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        TextButton(onClick = { model.openRemoteTask(task.taskId) }, enabled = !model.remoteTaskBusy) { Text("Refresh") }
                    }
                    if (task.stale) Text("Latest remote state may be stale because the node is offline or revoked.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                    if (task.resultSummary.isNotBlank() && task.state.startsWith("completed")) Text(task.resultSummary, style = MaterialTheme.typography.bodyMedium)
                    if (task.state == "failed" && task.error.isNotBlank()) Text("Task failed. Details withheld from the remote timeline to avoid leaking provider data.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                    task.question?.let { question ->
                        val questionText = question.text
                        if (questionText.isNotBlank()) {
                            Surface(color = MaterialTheme.colorScheme.tertiaryContainer, shape = MaterialTheme.shapes.medium) {
                                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                    Text("Waiting for your answer", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onTertiaryContainer)
                                    Text(questionText, color = MaterialTheme.colorScheme.onTertiaryContainer)
                                    question.options.forEach { option ->
                                        FilledTonalButton(onClick = { answerText = option }, modifier = Modifier.fillMaxWidth(), enabled = model.canOperate && !model.remoteTaskBusy) { Text(option) }
                                    }
                                    OutlinedTextField(value = answerText, onValueChange = { answerText = it.take(65_536) }, label = { Text("Your answer") }, minLines = 2, modifier = Modifier.fillMaxWidth(), enabled = model.canOperate && !model.remoteTaskBusy)
                                    Button(onClick = { model.answerRemoteTask(task.taskId, answerText) }, enabled = model.canOperate && !model.remoteTaskBusy && answerText.isNotBlank(), modifier = Modifier.fillMaxWidth()) { Text("Submit answer") }
                                }
                            }
                        }
                    }
                    if (model.canOperate && task.state in ACTIVE_STATES) {
                        FilledTonalButton(onClick = { onConfirmCancel(task) }, enabled = !model.remoteTaskBusy, modifier = Modifier.fillMaxWidth()) { Text("Request cancellation") }
                    }
                    if (model.canOperate && task.state in RETRYABLE_STATES) {
                        Button(onClick = { onConfirmRetry(task) }, enabled = !model.remoteTaskBusy, modifier = Modifier.fillMaxWidth()) { Text("Retry task") }
                    }
                    Text("Timeline", style = MaterialTheme.typography.titleMedium)
                    model.remoteEvents.forEach { event ->
                        Surface(color = MaterialTheme.colorScheme.surfaceContainerLow, shape = MaterialTheme.shapes.medium) {
                            Column(Modifier.fillMaxWidth().padding(10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                Text(event.safeSummary, style = MaterialTheme.typography.bodyMedium)
                                Text("#${event.eventId} · ${event.occurredAt}", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                    Text("Event cursor: ${model.remoteEventCursor} · ${model.remoteTaskStatus}", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

private val ACTIVE_STATES = setOf("pending_delivery", "delivered", "queued", "running", "waiting_input")
private val RETRYABLE_STATES = setOf("failed", "delivery_failed")

