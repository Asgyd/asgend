package org.asgend.nativeclient.ui.workspace

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ArrowBack
import androidx.compose.material.icons.outlined.CheckCircle
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material.icons.outlined.HelpOutline
import androidx.compose.material.icons.outlined.PendingActions
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Send
import androidx.compose.material.icons.outlined.StopCircle
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import org.asgend.nativeclient.data.CoreRepository
import org.asgend.nativeclient.ui.NativeAppViewModel
import org.json.JSONObject

private val terminalStatuses = setOf("completed", "completed_without_model", "failed", "cancelled")
private fun isInternalEventType(type: String): Boolean {
    val normalized = type.lowercase().replace('-', '_')
    return normalized.contains("reasoning") || normalized.contains("chain_of_thought") ||
        normalized.contains("private") || normalized.contains("internal") ||
        normalized == "thought" || normalized.contains("prompt_payload") || normalized == "llm_prompt"
}

@Composable
fun TaskWorkspaceScreen(model: NativeAppViewModel, modifier: Modifier = Modifier) {
    LaunchedEffect(model.connected, model.selectedTaskId) {
        var delayMs = 2_500L
        while (model.connected) {
            if (model.selectedTaskId.isNullOrBlank()) model.refreshTasks() else model.refreshTaskDetail()
            delay(delayMs)
            // Polling errors surface in the app-level error state; slowly back off on repeated failures.
            delayMs = if (model.error == null) 2_500L else (delayMs * 2).coerceAtMost(15_000L)
        }
    }
    val selectedTask = model.selectedTaskId
    if (!selectedTask.isNullOrBlank()) TaskDetailScreen(model, modifier) else TaskDashboardScreen(model, modifier)
}

@Composable
private fun TaskDashboardScreen(model: NativeAppViewModel, modifier: Modifier) {
    var taskText by remember { mutableStateOf("") }
    var priority by remember { mutableIntStateOf(5) }
    val statuses = listOf("all", "queued", "running", "waiting_input", "failed", "dead_letters", "completed", "completed_without_model", "cancelled")

    LazyColumn(
        modifier = modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(start = 16.dp, end = 16.dp, top = 14.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)) {
                Column(Modifier.fillMaxWidth().padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("Create a task", style = MaterialTheme.typography.headlineSmall, color = MaterialTheme.colorScheme.onPrimaryContainer)
                    Text("Tasks run in the same ASGEND core as CLI/TUI. The phone is a client, not a second orchestrator.", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onPrimaryContainer)
                    OutlinedTextField(
                        value = taskText,
                        onValueChange = { if (it.toByteArray(Charsets.UTF_8).size <= 100_000) taskText = it },
                        label = { Text("What should the agent do?") },
                        placeholder = { Text("Describe the desired result and constraints…") },
                        minLines = 3,
                        maxLines = 6,
                        enabled = model.connected && !model.taskBusy,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Priority", style = MaterialTheme.typography.labelLarge)
                        Slider(
                            value = priority.toFloat(), onValueChange = { priority = it.toInt() },
                            valueRange = 0f..9f, steps = 8, enabled = !model.taskBusy,
                            modifier = Modifier.weight(1f).padding(horizontal = 10.dp),
                        )
                        AssistChip(onClick = {}, enabled = false, label = { Text(priority.toString()) })
                    }
                    Button(
                        onClick = { model.createTask(taskText, priority); taskText = "" },
                        enabled = model.connected && !model.taskBusy && taskText.isNotBlank(),
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        if (model.taskBusy) CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                        else Icon(Icons.Outlined.Send, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text("Start task")
                    }
                }
            }
        }
        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Tasks", style = MaterialTheme.typography.headlineSmall)
                    Text(if (model.taskFilter == "dead_letters") "Terminally failed tasks · retry remains core-controlled" else "Shared queue · ${model.tasks.size} shown", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                IconButton(onClick = model::refreshTasks, enabled = model.connected) { Icon(Icons.Outlined.Refresh, contentDescription = "Refresh tasks") }
            }
        }
        item {
            androidx.compose.foundation.lazy.LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                items(statuses) { status ->
                    FilterChip(
                        selected = model.taskFilter == status,
                        onClick = { model.selectTaskFilter(status) },
                        label = { Text(status.replace('_', ' ')) },
                    )
                }
            }
        }
        if (!model.connected) item { ErrorPanel("Connect to the core to view shared tasks.", model.error) }
        if (model.connected && model.tasks.isEmpty()) item {
            EmptyPanel("No tasks in this view", "Create a task above or change the status filter.")
        }
        items(model.tasks, key = { it.optString("task_id") }) { task ->
            TaskListCard(task = task, onClick = { model.openTask(task.optString("task_id")) })
        }
        if (model.error != null) item { ErrorPanel("Core request failed", model.error) }
    }
}

@Composable
private fun TaskListCard(task: JSONObject, onClick: () -> Unit) {
    val status = task.optString("status", "unknown")
    val icon = when (status) {
        "waiting_input" -> Icons.Outlined.HelpOutline
        "failed" -> Icons.Outlined.ErrorOutline
        "completed", "completed_without_model" -> Icons.Outlined.CheckCircle
        else -> Icons.Outlined.PendingActions
    }
    Card(onClick = onClick, modifier = Modifier.fillMaxWidth(), shape = MaterialTheme.shapes.extraLarge) {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Surface(color = statusColor(status), shape = MaterialTheme.shapes.large, modifier = Modifier.size(46.dp)) {
                androidx.compose.foundation.layout.Box(contentAlignment = Alignment.Center) {
                    Icon(icon, contentDescription = status, tint = statusContentColor(status))
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(task.optString("title", task.optString("task", "Task")), style = MaterialTheme.typography.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text("${status.replace('_', ' ')} · priority ${task.optInt("effective_priority", task.optInt("priority", 5))}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(task.optString("task_id"), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.outline, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Icon(Icons.Outlined.ChevronRight, contentDescription = "Open task", modifier = Modifier.size(18.dp))
        }
    }
}

@Composable
private fun TaskDetailScreen(model: NativeAppViewModel, modifier: Modifier) {
    val task = model.activeTask
    var followUp by remember(model.selectedTaskId) { mutableStateOf("") }
    var answerText by remember(model.selectedTaskId) { mutableStateOf("") }
    var showCancelConfirm by remember { mutableStateOf(false) }
    val status = task?.optString("status", "loading") ?: "loading"
    val terminal = status in terminalStatuses
    val taskId = model.selectedTaskId.orEmpty()
    val taskTitle = task?.optString("title")?.takeIf { it.isNotBlank() }
        ?: task?.optString("task")?.takeIf { it.isNotBlank() }
        ?: "Loading task…"

    Column(modifier.fillMaxSize()) {
        Surface(color = MaterialTheme.colorScheme.surfaceContainerLow) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = model::closeTask) { Icon(Icons.Outlined.ChevronLeft, contentDescription = "Back to tasks") }
                Column(Modifier.weight(1f)) {
                    Text(taskTitle, style = MaterialTheme.typography.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(status.replace('_', ' '), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (!terminal && task != null) IconButton(onClick = { showCancelConfirm = true }, enabled = !model.taskBusy) { Icon(Icons.Outlined.StopCircle, contentDescription = "Cancel task") }
                if (status == "failed") IconButton(onClick = model::retryActiveTask, enabled = !model.taskBusy) { Icon(Icons.Outlined.Refresh, contentDescription = "Retry failed task") }
            }
        }
        if (status == "running" || status == "queued") LinearProgressIndicator(Modifier.fillMaxWidth())
        if (model.error != null) ErrorPanel("Could not refresh task", model.error, Modifier.padding(horizontal = 12.dp, vertical = 6.dp))
        LazyColumn(
            modifier = Modifier.weight(1f).fillMaxWidth(),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 14.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            item {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow)) {
                    Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                        Text("Task details", style = MaterialTheme.typography.titleSmall)
                        Text("ID  $taskId", style = MaterialTheme.typography.bodySmall)
                        Text("Priority ${task?.optInt("priority", 5) ?: 5} · attempt ${task?.optInt("attempts", 0) ?: 0}/${task?.optInt("max_attempts", 0) ?: 0}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        task?.optString("error")?.takeIf { it.isNotBlank() }?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                    }
                }
            }
            val question = model.taskQuestion
            if (status == "waiting_input" && question != null) {
                item {
                    QuestionCard(
                        question = question,
                        busy = model.taskBusy,
                        onChoose = { answer, option -> model.answerQuestion(answer, option) },
                        freeText = answerText,
                        onFreeTextChange = { answerText = it },
                        onSendFreeText = { model.answerQuestion(answerText); answerText = "" },
                    )
                }
            }
            if (model.taskEvents.isEmpty() && model.taskLoading) item { androidx.compose.foundation.layout.Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator() } }
            if (model.taskEvents.isEmpty() && !model.taskLoading) item { EmptyPanel("No events yet", "Status updates and agent-visible actions will appear here.") }
            items(model.taskEvents.filterNot { isInternalEventType(it.optString("type")) }, key = { it.optLong("event_id") }) { event -> EventCard(event) }
            if (status in terminalStatuses && task != null) {
                val result = task.optJSONObject("result")
                result?.let { res ->
                    val summary = res.optString("summary").ifBlank { res.optString("answer") }
                    if (summary.isNotBlank()) item { Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) { Text("Result", style = MaterialTheme.typography.titleMedium); Text(summary) } } }
                }
            }
        }
        if (status != "waiting_input") {
            Surface(color = MaterialTheme.colorScheme.surfaceContainerLow) {
                Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = followUp, onValueChange = { followUp = it },
                        modifier = Modifier.weight(1f), minLines = 1, maxLines = 4,
                        label = { Text("Follow up in this task session") },
                        enabled = !model.taskBusy,
                    )
                    Button(onClick = { model.followUp(followUp); followUp = "" }, enabled = followUp.isNotBlank() && !model.taskBusy) {
                        Icon(Icons.Outlined.Send, contentDescription = "Send follow-up")
                    }
                }
            }
        }
    }

    if (showCancelConfirm) AlertDialog(
        onDismissRequest = { showCancelConfirm = false },
        title = { Text("Cancel this task?") },
        text = { Text("The core will request cancellation. A currently running tool may need to reach a cooperative checkpoint.") },
        confirmButton = { Button(onClick = { showCancelConfirm = false; model.cancelActiveTask() }) { Text("Request cancellation") } },
        dismissButton = { TextButton(onClick = { showCancelConfirm = false }) { Text("Keep running") } },
    )
}

@Composable
private fun QuestionCard(
    question: JSONObject,
    busy: Boolean,
    onChoose: (String, String?) -> Unit,
    freeText: String,
    onFreeTextChange: (String) -> Unit,
    onSendFreeText: () -> Unit,
) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer)) {
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("Your answer is needed", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onTertiaryContainer)
            Text(question.optString("question", "Please clarify this requirement."), color = MaterialTheme.colorScheme.onTertiaryContainer)
            CoreRepository.questionOptions(question).forEachIndexed { index, option ->
                val id = option.optString("id").takeIf { it.isNotBlank() }
                val label = option.optString("label").ifBlank { option.optString("text", "Option ${index + 1}") }
                FilledTonalButton(onClick = { onChoose(label, id) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text("${index + 1}. $label") }
            }
            OutlinedTextField(freeText, onFreeTextChange, label = { Text("Or write your own answer") }, enabled = !busy, modifier = Modifier.fillMaxWidth(), minLines = 1, maxLines = 4)
            Button(onClick = onSendFreeText, enabled = !busy && freeText.isNotBlank(), modifier = Modifier.align(Alignment.End)) { Text("Send answer") }
        }
    }
}

@Composable
private fun EventCard(event: JSONObject) {
    val type = event.optString("type", "event")
    val message = event.optString("message").ifBlank { type.replace('_', ' ') }
    val actor = event.optString("actor", "system")
    val color = when (type.lowercase()) {
        "task_failed", "error", "tool_error" -> MaterialTheme.colorScheme.errorContainer
        "question", "agent_question" -> MaterialTheme.colorScheme.tertiaryContainer
        "task_completed", "completed" -> MaterialTheme.colorScheme.secondaryContainer
        else -> MaterialTheme.colorScheme.surfaceContainerLow
    }
    Card(colors = CardDefaults.cardColors(containerColor = color), shape = MaterialTheme.shapes.large) {
        Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(type.replace('_', ' '), style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                Text(actor, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Text(message, style = MaterialTheme.typography.bodyMedium)
            event.optString("created_at").takeIf { it.isNotBlank() }?.let { Text(it, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        }
    }
}

@Composable
private fun EmptyPanel(title: String, description: String) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.fillMaxWidth().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Icon(Icons.Outlined.PendingActions, contentDescription = null, modifier = Modifier.size(32.dp), tint = MaterialTheme.colorScheme.primary)
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(description, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun ErrorPanel(title: String, details: String?, modifier: Modifier = Modifier) {
    Card(modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.onErrorContainer)
            if (!details.isNullOrBlank()) Text(details, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onErrorContainer)
        }
    }
}

@Composable
private fun statusColor(status: String) = when (status) {
    "failed" -> MaterialTheme.colorScheme.errorContainer
    "waiting_input" -> MaterialTheme.colorScheme.tertiaryContainer
    "completed", "completed_without_model" -> MaterialTheme.colorScheme.secondaryContainer
    else -> MaterialTheme.colorScheme.surfaceContainerHigh
}

@Composable
private fun statusContentColor(status: String) = when (status) {
    "failed" -> MaterialTheme.colorScheme.onErrorContainer
    "waiting_input" -> MaterialTheme.colorScheme.onTertiaryContainer
    "completed", "completed_without_model" -> MaterialTheme.colorScheme.onSecondaryContainer
    else -> MaterialTheme.colorScheme.onSurface
}
