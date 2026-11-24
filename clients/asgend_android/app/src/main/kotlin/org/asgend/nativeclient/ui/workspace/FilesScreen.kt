package org.asgend.nativeclient.ui.workspace

import android.content.ClipData
import android.content.Intent
import android.widget.Toast
import androidx.core.content.FileProvider
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.asgend.nativeclient.ui.DownloadedArtifact
import org.asgend.nativeclient.ui.NativeAppViewModel
import org.json.JSONObject
import java.io.File

@Composable
fun FilesScreen(model: NativeAppViewModel, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    var chooserOpen by remember { mutableStateOf(false) }
    var fileMode by remember { mutableStateOf("files") }
    var editContent by remember(model.openedFilePath) { mutableStateOf(model.openedFileContent) }
    var confirmDiscard by remember { mutableStateOf(false) }
    var dirty by remember(model.openedFilePath) { mutableStateOf(false) }
    var diagnosticsPath by remember { mutableStateOf("") }
    var confirmDiagnostics by remember { mutableStateOf(false) }
    var showDiagnosticsResult by remember { mutableStateOf(false) }

    LaunchedEffect(model.connected, model.tasks.size) {
        if (model.connected) {
            model.refreshTasks()
            if (model.workspaceTaskId.isBlank() && model.tasks.isNotEmpty()) model.chooseWorkspaceTask(model.tasks.first().optString("task_id"))
        }
    }
    LaunchedEffect(model.lspDiagnosticsText, model.lspDiagnosticsError) {
        if (model.lspDiagnosticsText != null || model.lspDiagnosticsError != null) showDiagnosticsResult = true
    }

    Column(modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Workspace", style = MaterialTheme.typography.headlineMedium)
                Text("Task files, generated artifacts and project diagnostics", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(
                onClick = { if (fileMode == "artifacts") model.refreshArtifacts() else if (fileMode == "diagnostics") model.clearLspDiagnostics() else model.loadWorkspace() },
                enabled = model.connected && !model.artifactsLoading && !model.workspaceLoading && !model.lspDiagnosticsBusy,
            ) { Icon(Icons.Outlined.Refresh, contentDescription = "Refresh current view") }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(selected = fileMode == "files", onClick = { fileMode = "files" }, label = { Text("Files") })
            FilterChip(selected = fileMode == "artifacts", onClick = { fileMode = "artifacts"; model.refreshArtifacts() }, label = { Text("Artifacts") })
            FilterChip(selected = fileMode == "diagnostics", onClick = { fileMode = "diagnostics" }, label = { Text("LSP diagnostics") })
        }

        if (fileMode != "diagnostics") {
            Surface(color = MaterialTheme.colorScheme.surfaceContainerLow, shape = MaterialTheme.shapes.large) {
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("Task workspace", style = MaterialTheme.typography.labelLarge)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(model.tasks.firstOrNull { it.optString("task_id") == model.workspaceTaskId }?.optString("title", "Task") ?: "Select task", style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(model.workspaceTaskId.ifBlank { "No task selected" }, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                        FilledTonalButton(onClick = { chooserOpen = true }, enabled = model.tasks.isNotEmpty()) { Text("Change") }
                        DropdownMenu(expanded = chooserOpen, onDismissRequest = { chooserOpen = false }) {
                            model.tasks.forEach { task ->
                                DropdownMenuItem(
                                    text = { Text(task.optString("title", task.optString("task_id", "Task")), maxLines = 1, overflow = TextOverflow.Ellipsis) },
                                    onClick = { chooserOpen = false; model.chooseWorkspaceTask(task.optString("task_id")) },
                                )
                            }
                        }
                    }
                }
            }
        }

        when (fileMode) {
            "files" -> {
                if (model.workspaceTaskId.isNotBlank()) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        IconButton(onClick = model::parentWorkspace, enabled = model.workspacePath.isNotBlank()) { Icon(Icons.Outlined.FolderOpen, contentDescription = "Parent directory") }
                        Column(Modifier.weight(1f)) {
                            Text("/${model.workspacePath}", style = MaterialTheme.typography.titleSmall)
                            Text("Workspace root is enforced by the core; symlinks are not followed.", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        if (model.workspaceLoading) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    }
                    if (!model.workspaceLoading && model.workspaceEntries.isEmpty()) {
                        Card(modifier = Modifier.fillMaxWidth()) {
                            Column(Modifier.fillMaxWidth().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Icon(Icons.Outlined.FolderOpen, contentDescription = null, modifier = Modifier.size(34.dp), tint = MaterialTheme.colorScheme.primary)
                                Text("This directory is empty", style = MaterialTheme.typography.titleMedium)
                                Text("Files appear here as agents create them in this task workspace.", color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        items(model.workspaceEntries, key = { it.optString("name") }) { entry ->
                            WorkspaceEntryCard(entry) {
                                val nextPath = listOf(model.workspacePath, entry.optString("name")).filter { it.isNotBlank() }.joinToString("/")
                                if (entry.optString("kind") == "directory") model.navigateWorkspace(nextPath) else model.openWorkspaceFile(nextPath)
                            }
                        }
                    }
                } else {
                    Card { Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) { Text("Choose a task", style = MaterialTheme.typography.titleMedium); Text("Create a task or select one before browsing task-scoped files.", color = MaterialTheme.colorScheme.onSurfaceVariant) } }
                }
            }
            "artifacts" -> {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(model.artifactsSummary.ifBlank { "Generated outputs for the selected task" }, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (model.artifactsTruncated) Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer)) {
                        Text("The core reached an artifact scan limit. This list is incomplete; skipped files are not downloadable from this view.", Modifier.padding(12.dp), color = MaterialTheme.colorScheme.onTertiaryContainer, style = MaterialTheme.typography.bodySmall)
                    }
                    if (model.artifactDownloadError != null) Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
                        Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text(model.artifactDownloadError.orEmpty(), color = MaterialTheme.colorScheme.onErrorContainer, style = MaterialTheme.typography.bodySmall)
                            TextButton(onClick = model::clearArtifactDownloadError) { Text("Dismiss") }
                        }
                    }
                    if (model.artifactDownloadPath != null) {
                        Column(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text("Downloading and verifying ${model.artifactDownloadPath}", style = MaterialTheme.typography.labelMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                            androidx.compose.material3.LinearProgressIndicator(progress = { model.artifactDownloadProgress }, modifier = Modifier.fillMaxWidth())
                            Text("Size and SHA-256 are checked before export is enabled.", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    model.downloadedArtifact?.let { downloaded ->
                        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)) {
                            Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                Text("Verified copy ready", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onPrimaryContainer)
                                Text("${downloaded.fileName} · ${downloaded.size} bytes", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onPrimaryContainer)
                                Text("The original size and SHA-256 matched the core artifact index. Export opens the Android Sharesheet.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onPrimaryContainer)
                                Button(onClick = { shareVerifiedArtifact(context, downloaded) }) { Text("Share / save a copy") }
                            }
                        }
                    }
                    if (model.artifactsLoading) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center) { CircularProgressIndicator() }
                    if (!model.artifactsLoading && model.workspaceArtifacts.isEmpty()) Card { Text("No eligible generated artifacts found for this task.", Modifier.padding(16.dp), color = MaterialTheme.colorScheme.onSurfaceVariant) }
                    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        items(model.workspaceArtifacts, key = { it.optString("path") }) { artifact ->
                            ArtifactCard(
                                artifact = artifact,
                                downloading = model.artifactDownloadPath == artifact.optString("path"),
                                enabled = model.connected && model.artifactDownloadPath == null,
                                onDownload = { model.downloadArtifact(artifact) },
                            )
                        }
                    }
                }
            }
            else -> {
                LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    item {
                        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow)) {
                            Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                                Text("Code diagnostics", style = MaterialTheme.typography.titleLarge)
                                Text("Runs the core's configured language server against a regular file inside its configured project root (not the task workspace). The core enforces the path boundary and the 5 MiB file limit.", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                                OutlinedTextField(
                                    value = diagnosticsPath,
                                    onValueChange = { diagnosticsPath = it.take(1024) },
                                    modifier = Modifier.fillMaxWidth(),
                                    label = { Text("Relative project path") },
                                    placeholder = { Text("src/main.c") },
                                    singleLine = true,
                                    enabled = model.connected && !model.lspDiagnosticsBusy,
                                )
                                Button(onClick = { confirmDiagnostics = true }, enabled = model.connected && !model.lspDiagnosticsBusy && diagnosticsPath.isNotBlank()) {
                                    if (model.lspDiagnosticsBusy) CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                                    Text(if (model.lspDiagnosticsBusy) " Running diagnostics…" else "Run diagnostics")
                                }
                            }
                        }
                    }
                    item {
                        Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                            Text("Security boundary", style = MaterialTheme.typography.titleSmall)
                            Text("Absolute paths, traversal components and paths outside the configured project root are rejected. Diagnostics run on the core host, not on this phone.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        } }
                    }
                }
            }
        }
        model.error?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
    }

    val filePath = model.openedFilePath
    if (filePath != null) {
        AlertDialog(
            onDismissRequest = { if (dirty) confirmDiscard = true else model.closeWorkspaceFile() },
            title = { Text(filePath, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        if (model.openedFilePartial) "Read-only partial preview · ${model.openedFileBytes} bytes total" else "UTF-8 text · ${model.openedFileBytes} bytes · max 1 MiB",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    OutlinedTextField(
                        value = editContent,
                        onValueChange = { editContent = it; dirty = true },
                        modifier = Modifier.fillMaxWidth(),
                        minLines = 8,
                        maxLines = 18,
                        readOnly = model.openedFilePartial || model.fileBusy,
                        textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                    )
                    model.error?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
                }
            },
            confirmButton = {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (!model.openedFilePartial) Button(
                        onClick = { model.saveWorkspaceFile(filePath, editContent) { dirty = false } },
                        enabled = dirty && !model.fileBusy && editContent.toByteArray(Charsets.UTF_8).size <= 1024 * 1024,
                    ) { if (model.fileBusy) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp) else Icon(Icons.Outlined.Save, contentDescription = null); Text(" Save") }
                    TextButton(onClick = { if (dirty) confirmDiscard = true else model.closeWorkspaceFile() }) { Text("Close") }
                }
            },
        )
    }
    if (confirmDiscard) AlertDialog(
        onDismissRequest = { confirmDiscard = false },
        title = { Text("Discard unsaved edits?") },
        text = { Text("Your changes have not been written to the task workspace.") },
        confirmButton = { Button(onClick = { confirmDiscard = false; dirty = false; model.closeWorkspaceFile() }) { Text("Discard") } },
        dismissButton = { TextButton(onClick = { confirmDiscard = false }) { Text("Keep editing") } },
    )
    if (confirmDiagnostics) AlertDialog(
        onDismissRequest = { confirmDiagnostics = false },
        title = { Text("Run language-server diagnostics?") },
        text = { Text("The core will read this file and run its configured LSP process. The path must be relative to the core project root; only run this on a trusted core.") },
        confirmButton = { Button(onClick = { confirmDiagnostics = false; model.clearLspDiagnostics(); model.runLspDiagnostics(diagnosticsPath) }) { Text("Run") } },
        dismissButton = { TextButton(onClick = { confirmDiagnostics = false }) { Text("Cancel") } },
    )
    if (showDiagnosticsResult) AlertDialog(
        onDismissRequest = { showDiagnosticsResult = false; model.clearLspDiagnostics() },
        title = { Text(if (model.lspDiagnosticsError != null) "Diagnostics unavailable" else "LSP diagnostics") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text(model.lspDiagnosticsError ?: model.lspDiagnosticsText.orEmpty(), style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        },
        confirmButton = { TextButton(onClick = { showDiagnosticsResult = false; model.clearLspDiagnostics() }) { Text("Close") } },
    )
}

@Composable
private fun WorkspaceEntryCard(entry: JSONObject, onOpen: () -> Unit) {
    val directory = entry.optString("kind") == "directory"
    Card(onClick = onOpen, modifier = Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Icon(if (directory) Icons.Outlined.Folder else Icons.Outlined.Description, contentDescription = entry.optString("kind"), tint = MaterialTheme.colorScheme.primary)
            Column(Modifier.weight(1f)) {
                Text(entry.optString("name"), style = MaterialTheme.typography.titleSmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text(if (directory) "Folder" else "${entry.optLong("size")} bytes", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Icon(if (directory) Icons.Outlined.FolderOpen else Icons.Outlined.Description, contentDescription = if (directory) "Open folder" else "Preview file", modifier = Modifier.size(18.dp))
        }
    }
}

@Composable
private fun ArtifactCard(artifact: JSONObject, downloading: Boolean, enabled: Boolean, onDownload: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow)) {
        Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Text(artifact.optString("path"), style = MaterialTheme.typography.titleSmall, maxLines = 3, overflow = TextOverflow.Ellipsis)
            Text("${artifact.optLong("size")} bytes · ${artifact.optString("mime", "unknown type")}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            val digest = artifact.optString("sha256")
            if (digest.isNotBlank()) Text("SHA-256 ${digest.take(16)}…", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.outline, fontFamily = FontFamily.Monospace)
            Button(onClick = onDownload, enabled = enabled && !downloading) {
                if (downloading) CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                Text(if (downloading) " Verifying…" else "Download and verify")
            }
        }
    }
}

private fun shareVerifiedArtifact(context: android.content.Context, artifact: DownloadedArtifact) {
    val file = File(artifact.absolutePath)
    if (!file.isFile || !file.canRead()) {
        Toast.makeText(context, "Verified artifact is no longer available; download it again.", Toast.LENGTH_LONG).show()
        return
    }
    try {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = artifact.mimeType
            putExtra(Intent.EXTRA_STREAM, uri)
            clipData = ClipData.newUri(context.contentResolver, artifact.fileName, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(Intent.createChooser(send, "Export verified artifact"))
    } catch (_: Exception) {
        Toast.makeText(context, "Android could not export this verified artifact.", Toast.LENGTH_LONG).show()
    }
}
