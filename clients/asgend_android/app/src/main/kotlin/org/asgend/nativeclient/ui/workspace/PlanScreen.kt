package org.asgend.nativeclient.ui.workspace

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.DeleteOutline
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.Flag
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.TaskAlt
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilledTonalButton
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import org.asgend.nativeclient.ui.NativeAppViewModel
import org.json.JSONObject

@Composable
fun PlanScreen(model: NativeAppViewModel, modifier: Modifier = Modifier) {
    var showCreate by remember { mutableStateOf(false) }
    var editItem by remember { mutableStateOf<JSONObject?>(null) }
    var deleteItem by remember { mutableStateOf<JSONObject?>(null) }
    var scopeText by remember(model.planScope) { mutableStateOf(model.planScope) }

    LaunchedEffect(model.connected) {
        if (model.connected) {
            model.loadPlan()
            while (model.connected) {
                try {
                    val notification = model.awaitPlanNotification()
                    if (notification.optBoolean("stopping", false)) break
                    if (notification.optBoolean("changed", false)) model.loadPlan()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    // A temporary connection loss should not spin or leak raw server details into the UI.
                    delay(2_000)
                }
            }
        }
    }
    Column(modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("To-do & Goals", style = MaterialTheme.typography.headlineMedium)
                Text("Shared core plan · changes are visible in CLI/TUI", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(onClick = { model.loadPlan() }, enabled = model.connected) { Icon(Icons.Outlined.Refresh, contentDescription = "Refresh plan") }
            Button(onClick = { showCreate = true }, enabled = model.connected) { Icon(Icons.Outlined.Add, contentDescription = null); Text(" Add") }
        }
        OutlinedTextField(
            value = scopeText,
            onValueChange = { scopeText = it },
            label = { Text("Scope ID (global or task ID)") },
            supportingText = { Text("Use global for shared items, or a task ID for task-scoped planning.") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("all" to "All", "todo" to "To-do", "goal" to "Goals").forEach { (kind, title) ->
                FilterChip(selected = model.planKind == kind, onClick = { model.loadPlan(scopeText, kind) }, label = { Text(title) })
            }
            FilledTonalButton(onClick = { model.loadPlan(scopeText, model.planKind) }, enabled = model.connected) { Text("Open scope") }
        }
        if (!model.connected) {
            Surface(color = MaterialTheme.colorScheme.errorContainer, shape = MaterialTheme.shapes.large) {
                Text(model.error ?: "Connect to the core to edit To-do and Goals.", Modifier.padding(14.dp), color = MaterialTheme.colorScheme.onErrorContainer)
            }
        }
        if (model.planLoading) LinearProgressIndicatorSafe()
        if (model.connected && model.planItems.isEmpty() && !model.planLoading) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.fillMaxWidth().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Icon(Icons.Outlined.TaskAlt, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                    Text("Nothing planned yet", style = MaterialTheme.typography.titleMedium)
                    Text("Add a To-do or Goal. This list uses the same plan store as the existing interface.", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        } else {
            LazyColumn(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(model.planItems, key = { it.optLong("item_id") }) { item ->
                    PlanItemCard(
                        item = item,
                        onToggle = { checked -> model.updatePlanItem(item, status = if (checked) "done" else "open") },
                        onEdit = { editItem = item },
                        onDelete = { deleteItem = item },
                    )
                }
            }
        }
        model.error?.let { err -> Text(err, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
    }

    if (showCreate) PlanEditorDialog(
        title = "Add plan item", initialKind = if (model.planKind == "goal") "goal" else "todo", initialTitle = "", initialDescription = "",
        onDismiss = { showCreate = false },
        onSave = { kind, title, description -> model.loadPlan(scopeText, model.planKind); model.addPlanItem(kind, title, description); model.loadPlan(scopeText, model.planKind); showCreate = false },
    )
    editItem?.let { item ->
        PlanEditorDialog(
            title = "Edit plan item", initialKind = item.optString("kind", "todo"), initialTitle = item.optString("title"), initialDescription = item.optString("description"),
            allowKindEdit = false,
            onDismiss = { editItem = null },
            onSave = { _, title, description -> model.updatePlanItem(item, title = title, description = description); editItem = null },
        )
    }
    deleteItem?.let { item ->
        AlertDialog(
            onDismissRequest = { deleteItem = null },
            title = { Text("Delete this item?") },
            text = { Text("“${item.optString("title")}” will be removed from ${model.planScope}. This cannot be undone.") },
            confirmButton = { Button(onClick = { model.deletePlanItem(item); deleteItem = null }) { Text("Delete") } },
            dismissButton = { TextButton(onClick = { deleteItem = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun PlanItemCard(item: JSONObject, onToggle: (Boolean) -> Unit, onEdit: () -> Unit, onDelete: () -> Unit) {
    val done = item.optString("status") == "done"
    Card(colors = CardDefaults.cardColors(containerColor = if (done) MaterialTheme.colorScheme.surfaceContainerLow else MaterialTheme.colorScheme.surfaceContainer)) {
        Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, top = 8.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Checkbox(checked = done, onCheckedChange = onToggle)
            Column(Modifier.weight(1f).padding(horizontal = 8.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(if (item.optString("kind") == "goal") Icons.Outlined.Flag else Icons.Outlined.TaskAlt, contentDescription = item.optString("kind"), tint = MaterialTheme.colorScheme.primary)
                    Text(" ${item.optString("kind", "todo").uppercase()}", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
                }
                Text(item.optString("title"), style = MaterialTheme.typography.titleMedium, maxLines = 3, overflow = TextOverflow.Ellipsis)
                item.optString("description").takeIf { it.isNotBlank() }?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            }
            IconButton(onClick = onEdit) { Icon(Icons.Outlined.Edit, contentDescription = "Edit item") }
            IconButton(onClick = onDelete) { Icon(Icons.Outlined.DeleteOutline, contentDescription = "Delete item") }
        }
    }
}

@Composable
private fun PlanEditorDialog(
    title: String,
    initialKind: String,
    initialTitle: String,
    initialDescription: String,
    allowKindEdit: Boolean = true,
    onDismiss: () -> Unit,
    onSave: (kind: String, title: String, description: String) -> Unit,
) {
    var kind by remember { mutableStateOf(initialKind) }
    var itemTitle by remember { mutableStateOf(initialTitle) }
    var description by remember { mutableStateOf(initialDescription) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (allowKindEdit) Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = kind == "todo", onClick = { kind = "todo" }, label = { Text("To-do") })
                    FilterChip(selected = kind == "goal", onClick = { kind = "goal" }, label = { Text("Goal") })
                }
                OutlinedTextField(itemTitle, { itemTitle = it.take(160) }, label = { Text("Title (max 160 chars)") }, singleLine = true)
                OutlinedTextField(description, { description = it.take(4096) }, label = { Text("Description (optional)") }, minLines = 2, maxLines = 5)
            }
        },
        confirmButton = { Button(onClick = { onSave(kind, itemTitle.trim(), description.trim()) }, enabled = itemTitle.isNotBlank()) { Text("Save") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun LinearProgressIndicatorSafe() {
    androidx.compose.material3.LinearProgressIndicator(Modifier.fillMaxWidth())
}
