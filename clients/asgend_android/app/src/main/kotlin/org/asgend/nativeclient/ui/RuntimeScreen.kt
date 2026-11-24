package org.asgend.nativeclient.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.OutlinedTextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.asgend.nativeclient.data.RuntimeConfigDraft

private enum class RuntimeSection(val label: String) {
    Health("Health"), Models("Models"), Agents("Agents"), Resources("Resources"), MCP("MCP"), Configuration("Config")
}

@Composable
fun RuntimeScreen(model: NativeAppViewModel, modifier: Modifier = Modifier) {
    var sectionName by rememberSaveable { mutableStateOf(RuntimeSection.Health.name) }
    val section = RuntimeSection.entries.firstOrNull { it.name == sectionName } ?: RuntimeSection.Health
    var pendingAction by remember { mutableStateOf<PendingModelAction?>(null) }

    LaunchedEffect(model.connected) {
        if (model.connected) model.refreshRuntime()
    }
    LaunchedEffect(section, model.connected) {
        if (section == RuntimeSection.Configuration && model.connected) model.refreshConfig()
    }

    Column(
        modifier = modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Runtime", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
                Text(
                    if (model.connected) "Live state from the ASGEND core" else "Connect to the core to inspect runtime state",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            OutlinedButton(onClick = model::refreshRuntime, enabled = model.connected && !model.runtimeLoading) {
                if (model.runtimeLoading) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                else androidx.compose.material3.Icon(Icons.Outlined.Refresh, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text("Refresh")
            }
        }

        if (!model.connected) {
            InfoCard("Core is offline", "Open Settings and connect to the core through a trusted VPN or SSH tunnel.")
            return@Column
        }
        if (model.runtimeLoading) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (model.runtimeError != null) {
            InfoCard("Some runtime data is unavailable", "One or more core requests failed. Refresh to try again.", error = true)
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())) {
            RuntimeSection.entries.forEach { option ->
                FilterChip(
                    selected = option == section,
                    onClick = { sectionName = option.name },
                    label = { Text(option.label) },
                )
            }
        }

        when (section) {
            RuntimeSection.Health -> HealthContent(model.runtimeHealth, model.runtimeModelStatus)
            RuntimeSection.Models -> ModelsContent(model) { id, kind, action, name -> pendingAction = PendingModelAction(id, kind, action, name) }
            RuntimeSection.Agents -> AgentsContent(model.runtimeAgents)
            RuntimeSection.Resources -> ResourcesContent(model.runtimeResources)
            RuntimeSection.MCP -> McpContent(model.runtimeMcpServers)
            RuntimeSection.Configuration -> ConfigContent(model)
        }
        Text(
            "Values are read from the core. Model credentials, raw config, local paths and internal error payloads are intentionally not shown here.",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    pendingAction?.let { action ->
        AlertDialog(
            onDismissRequest = { pendingAction = null },
            title = { Text("${action.action.replaceFirstChar { it.uppercase() }} model?") },
            text = { Text("This changes shared core state for ${action.name}. Other connected clients may be affected.") },
            confirmButton = {
                Button(onClick = {
                    model.performModelAction(action.modelId, action.kind, action.action)
                    pendingAction = null
                }) { Text(action.action.replaceFirstChar { it.uppercase() }) }
            },
            dismissButton = { TextButton(onClick = { pendingAction = null }) { Text("Cancel") } },
        )
    }
}

private data class PendingModelAction(val modelId: String, val kind: String, val action: String, val name: String)

@Composable
private fun HealthContent(health: JSONObject?, modelStatus: JSONObject?) {
    if (health == null) {
        InfoCard("Health not loaded", "Refresh runtime to request queue and worker status.")
        return
    }
    val taskCounts = health.optJSONObject("tasks_by_status") ?: JSONObject()
    val active = taskCounts.keys().asSequence().sumOf { taskCounts.optInt(it, 0) }
    val stats = listOf(
        "Queued / tracked tasks" to active.toString(),
        "Workers" to health.optInt("workers", 0).toString(),
        "Active connections" to health.optInt("active_connections", 0).toString(),
        "Priority aging" to "${health.optInt("priority_aging_seconds", 0)} s",
    )
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
            stats.take(2).forEach { (label, value) -> StatCard(label, value, Modifier.weight(1f)) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
            stats.drop(2).forEach { (label, value) -> StatCard(label, value, Modifier.weight(1f)) }
        }
        ElevatedCard {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Queue by status", style = MaterialTheme.typography.titleMedium)
                if (taskCounts.length() == 0) Text("No task counts reported", color = MaterialTheme.colorScheme.onSurfaceVariant)
                else taskCounts.keys().asSequence().sorted().forEach { key ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text(key.replace('_', ' '), style = MaterialTheme.typography.bodyMedium)
                        Text(taskCounts.optInt(key).toString(), style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold)
                    }
                }
                HorizontalDivider()
                StatusLine("Core draining", health.optBoolean("draining"))
                StatusLine("Unix socket", health.optBoolean("unix_socket_enabled"))
                StatusLine("Web UI enabled", health.optBoolean("ui_enabled"))
            }
        }
        modelStatus?.let { ModelSummary(it) }
    }
}

@Composable
private fun ModelSummary(modelStatus: JSONObject) {
    val llmCount = modelStatus.optJSONArray("models")?.length() ?: 0
    val decisionCount = modelStatus.optJSONArray("decision_models")?.length() ?: 0
    ElevatedCard {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text("Model registry", style = MaterialTheme.typography.titleMedium)
            Text("$llmCount language model(s) · $decisionCount decision model(s)", style = MaterialTheme.typography.bodyMedium)
            Text("Default: ${modelStatus.optString("default_model_id").ifBlank { "not set" }}", style = MaterialTheme.typography.bodySmall)
            Text("Decision: ${modelStatus.optString("decision_model_id").ifBlank { "not set" }}", style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun ModelsContent(model: NativeAppViewModel, onActionRequested: (String, String, String, String) -> Unit) {
    val status = model.runtimeModelStatus
    if (status == null) {
        InfoCard("Models not loaded", "Refresh runtime to inspect model state.")
        return
    }
    val allModels = buildList {
        jsonObjects(status.optJSONArray("models")).forEach { add(it to "llm") }
        jsonObjects(status.optJSONArray("decision_models")).forEach { add(it to "decision") }
    }
    if (allModels.isEmpty()) InfoCard("No model profiles", "The core has not reported any LLM or decision model profiles.")
    allModels.forEach { (entry, kind) ->
        ModelCard(entry, kind, model, onActionRequested)
    }
    val nodes = jsonObjects(status.optJSONArray("nodes"))
    if (nodes.isNotEmpty()) {
        Text("Nodes", style = MaterialTheme.typography.titleLarge)
        nodes.forEach { node ->
            ElevatedCard {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(node.optString("name").ifBlank { node.optString("id", "Unnamed node") }, style = MaterialTheme.typography.titleMedium)
                    Text("${node.optString("type", "node")} · ${node.optString("status", if (node.optBoolean("enabled")) "enabled" else "disabled")}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    // Endpoints, credentials and raw registry JSON are deliberately not rendered.
                }
            }
        }
    }
}

@Composable
private fun ModelCard(entry: JSONObject, kind: String, model: NativeAppViewModel, onActionRequested: (String, String, String, String) -> Unit) {
    val id = entry.optString("id")
    val keyPrefix = "$kind:$id:"
    val busy = model.runtimeActionKey?.startsWith(keyPrefix) == true
    val enabled = entry.optBoolean("enabled")
    val running = entry.optBoolean("running")
    val managed = entry.optBoolean("managed")
    ElevatedCard {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Text(entry.optString("name").ifBlank { id.ifBlank { "Unnamed model" } }, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                    Text(id, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text("${if (kind == "decision") "Decision model" else "Language model"} · ${entry.optString("tier", "standard")}", style = MaterialTheme.typography.bodySmall)
                }
                if (busy) CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                StateChip(if (enabled) "Enabled" else "Disabled", enabled)
                StateChip(if (running) "Running" else if (managed) "Stopped" else "External", running)
                if (entry.optBoolean("available")) StateChip("Available", true)
            }
            Text(
                "RAM ${entry.optInt("ram_estimate_mb")} MiB · VRAM ${entry.optInt("vram_estimate_mb")} MiB · active requests ${entry.optInt("active_requests")}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text("Node: ${entry.optString("node_id").ifBlank { "local / default" }} · ${entry.optString("location_type").ifBlank { "unspecified" }}", style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                if (enabled) OutlinedButton(onClick = { onActionRequested(id, kind, "disable", entry.optString("name").ifBlank { id }) }, enabled = !busy && id.isNotBlank()) { Text("Disable") }
                else Button(onClick = { onActionRequested(id, kind, "enable", entry.optString("name").ifBlank { id }) }, enabled = !busy && id.isNotBlank()) { Text("Enable") }
                if (managed) {
                    if (running) OutlinedButton(onClick = { onActionRequested(id, kind, "stop", entry.optString("name").ifBlank { id }) }, enabled = !busy && id.isNotBlank()) { Text("Stop") }
                    else Button(onClick = { onActionRequested(id, kind, "start", entry.optString("name").ifBlank { id }) }, enabled = !busy && enabled && id.isNotBlank()) { Text("Start") }
                }
            }
        }
    }
}

@Composable
private fun AgentsContent(agents: List<JSONObject>) {
    if (agents.isEmpty()) {
        InfoCard("No agents reported", "Refresh runtime to load the core's agent registry.")
        return
    }
    agents.forEach { agent ->
        ElevatedCard {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(agent.optString("name").ifBlank { agent.optString("id", "Unnamed agent") }, style = MaterialTheme.typography.titleMedium)
                Text(agent.optString("id"), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                val description = agent.optString("description")
                if (description.isNotBlank()) Text(description, style = MaterialTheme.typography.bodyMedium)
                val modelId = agent.optString("model_id")
                if (modelId.isNotBlank()) Text("Linked model · $modelId", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.primary)
            }
        }
    }
}

@Composable
private fun ResourcesContent(resources: JSONObject?) {
    if (resources == null) {
        InfoCard("Resources not loaded", "Refresh runtime to inspect skills, tools, prompts, knowledge and instructions.")
        return
    }
    val files = resources.optJSONObject("files") ?: JSONObject()
    val categories = listOf("skills", "system_prompts", "tools", "knowledge", "instructions")
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Resource catalog", style = MaterialTheme.typography.titleLarge)
        categories.forEach { category ->
            val names = strings(files.optJSONArray(category))
            ElevatedCard {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                        Text(category.replace('_', ' ').replaceFirstChar { it.uppercase() }, style = MaterialTheme.typography.titleMedium)
                        StateChip("${names.size} file(s)", names.isNotEmpty())
                    }
                    names.take(8).forEach { name -> Text("• $name", style = MaterialTheme.typography.bodySmall) }
                    if (names.size > 8) Text("and ${names.size - 8} more", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        Text("Selection mode: ${resources.optString("selection_mode", "not reported")}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        // Absolute directories and index paths from the daemon are intentionally omitted.
    }
}

@Composable
private fun McpContent(servers: List<JSONObject>) {
    if (servers.isEmpty()) {
        InfoCard("No MCP servers configured", "The core reports no MCP server entries, or its status could not be loaded.")
        return
    }
    servers.forEach { server ->
        val connected = server.optBoolean("connected")
        ElevatedCard {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                        Text(server.optString("name").ifBlank { "Unnamed MCP server" }, style = MaterialTheme.typography.titleMedium)
                        Text(server.optString("transport", "unknown transport"), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    StateChip(server.optString("status", if (connected) "running" else "stopped"), connected)
                }
            }
        }
    }
}

private data class ConfigForm(
    val maxConnections: String,
    val requestTimeoutSeconds: String,
    val workers: String,
    val queueMaxPending: String,
    val maxRetries: String,
    val priorityAgingSeconds: String,
    val sandboxEnabled: Boolean,
    val requireLandlock: Boolean,
    val allowSandboxNetwork: Boolean,
    val sandboxCpuSeconds: String,
    val sandboxMemoryMb: String,
    val sandboxFileSizeMb: String,
    val sandboxOpenFiles: String,
) {
    fun toDraft(): RuntimeConfigDraft? = runCatching {
        RuntimeConfigDraft(
            maxConnections = maxConnections.toInt(),
            requestTimeoutSeconds = requestTimeoutSeconds.toInt(),
            workers = workers.toInt(),
            queueMaxPending = queueMaxPending.toInt(),
            maxRetries = maxRetries.toInt(),
            priorityAgingSeconds = priorityAgingSeconds.toInt(),
            sandboxEnabled = sandboxEnabled,
            requireLandlock = requireLandlock,
            allowSandboxNetwork = allowSandboxNetwork,
            sandboxCpuSeconds = sandboxCpuSeconds.toInt(),
            sandboxMemoryMb = sandboxMemoryMb.toInt(),
            sandboxFileSizeMb = sandboxFileSizeMb.toInt(),
            sandboxOpenFiles = sandboxOpenFiles.toInt(),
        )
    }.getOrNull()

    companion object {
        fun from(draft: RuntimeConfigDraft) = ConfigForm(
            draft.maxConnections.toString(), draft.requestTimeoutSeconds.toString(), draft.workers.toString(),
            draft.queueMaxPending.toString(), draft.maxRetries.toString(), draft.priorityAgingSeconds.toString(),
            draft.sandboxEnabled, draft.requireLandlock, draft.allowSandboxNetwork,
            draft.sandboxCpuSeconds.toString(), draft.sandboxMemoryMb.toString(),
            draft.sandboxFileSizeMb.toString(), draft.sandboxOpenFiles.toString(),
        )
    }
}

@Composable
private fun ConfigContent(model: NativeAppViewModel) {
    var form by remember(model.configRevision) { mutableStateOf(ConfigForm.from(model.configDraft)) }
    var formError by remember(model.configRevision) { mutableStateOf<String?>(null) }
    var pendingDraft by remember { mutableStateOf<RuntimeConfigDraft?>(null) }
    var showReloadConfirm by remember { mutableStateOf(false) }
    var securityChangeAcknowledged by remember { mutableStateOf(false) }
    val baseline = model.configBaseline

    if (!model.configLoaded && model.configLoading) {
        LinearProgressIndicator(Modifier.fillMaxWidth())
        InfoCard("Loading configuration", "Reading the supported safe settings from the core.")
        return
    }
    if (!model.configLoaded) {
        InfoCard("Configuration unavailable", model.configError ?: "Load configuration after connecting to the core.", error = model.configError != null)
        OutlinedButton(onClick = model::refreshConfig, enabled = model.connected && !model.configLoading) { Text("Load configuration") }
        return
    }

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        if (model.configRestartRequired) {
            InfoCard("Restart required", "The core reports that saved configuration differs from the active runtime, or that the latest save requires a restart.")
        }
        InfoCard(
            "Guided configuration",
            "Only bounded operational limits and sandbox switches are editable here. Credentials, URLs, paths, full model registry and agent JSON are not displayed.",
        )
        ConfigGroup("Server limits") {
            ConfigNumberField("Maximum connections", form.maxConnections, "1–4096", { form = form.copy(maxConnections = it); formError = null })
            ConfigNumberField("Request timeout (seconds)", form.requestTimeoutSeconds, "1–600", { form = form.copy(requestTimeoutSeconds = it); formError = null })
        }
        ConfigGroup("Runtime") {
            ConfigNumberField("Workers", form.workers, "1–64", { form = form.copy(workers = it); formError = null })
            ConfigNumberField("Pending queue limit", form.queueMaxPending, "1–1,000,000", { form = form.copy(queueMaxPending = it); formError = null })
            ConfigNumberField("Max retries", form.maxRetries, "0–10", { form = form.copy(maxRetries = it); formError = null })
            ConfigNumberField("Priority aging (seconds)", form.priorityAgingSeconds, "1–86400", { form = form.copy(priorityAgingSeconds = it); formError = null })
        }
        ConfigGroup("Sandbox policy") {
            ConfigSwitch("Enable sandbox", "Run task execution inside the configured sandbox.", form.sandboxEnabled) { form = form.copy(sandboxEnabled = it) }
            ConfigSwitch("Require Landlock", "Fail closed when the required filesystem sandbox cannot be applied.", form.requireLandlock) { form = form.copy(requireLandlock = it) }
            ConfigSwitch("Allow network inside sandbox", "Security-sensitive: enabling this lets sandboxed task processes access the network.", form.allowSandboxNetwork) { form = form.copy(allowSandboxNetwork = it) }
            ConfigNumberField("CPU limit (seconds)", form.sandboxCpuSeconds, "1–86400", { form = form.copy(sandboxCpuSeconds = it); formError = null })
            ConfigNumberField("Memory limit (MiB)", form.sandboxMemoryMb, "64–65536", { form = form.copy(sandboxMemoryMb = it); formError = null })
            ConfigNumberField("Per-file limit (MiB)", form.sandboxFileSizeMb, "1–4096", { form = form.copy(sandboxFileSizeMb = it); formError = null })
            ConfigNumberField("Open files limit", form.sandboxOpenFiles, "16–4096", { form = form.copy(sandboxOpenFiles = it); formError = null })
        }
        if (formError != null) Text(formError!!, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        if (model.configError != null) InfoCard("Configuration update failed", model.configError!!, error = true)
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
            Button(
                onClick = {
                    val candidate = form.toDraft()
                    if (candidate == null) formError = "Enter a whole number in each numeric field."
                    else {
                        val rangeError = candidate.validationError()
                        if (rangeError != null) formError = rangeError else {
                            formError = null
                            securityChangeAcknowledged = false
                            pendingDraft = candidate
                        }
                    }
                },
                enabled = !model.configSaving && !model.configLoading,
                modifier = Modifier.weight(1f),
            ) {
                if (model.configSaving) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp) else Text("Review & save")
            }
            OutlinedButton(
                onClick = {
                    val dirty = baseline != null && form != ConfigForm.from(baseline)
                    if (dirty) showReloadConfirm = true else model.refreshConfig()
                },
                enabled = !model.configSaving && !model.configLoading,
            ) { Text("Reload") }
        }
    }

    pendingDraft?.let { candidate ->
        val prior = baseline
        val securityWeakened = prior != null && (
            (prior.sandboxEnabled && !candidate.sandboxEnabled) ||
            (prior.requireLandlock && !candidate.requireLandlock) ||
            (!prior.allowSandboxNetwork && candidate.allowSandboxNetwork)
        )
        AlertDialog(
            onDismissRequest = { pendingDraft = null },
            title = { Text("Save core configuration?") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Only the explicitly listed server/runtime/sandbox settings are sent to the core. The core validates and atomically updates the main config file; model registry, agent profiles and hidden credentials are not part of this request.")
                    Text("The configuration file is updated on disk, but running services may continue using the current values until the core restarts.")
                    if (securityWeakened) {
                        Text("Security warning: these changes weaken sandbox policy (sandbox off, Landlock no longer required, or network enabled). Proceed only if this is intentional.", color = MaterialTheme.colorScheme.error)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Checkbox(checked = securityChangeAcknowledged, onCheckedChange = { securityChangeAcknowledged = it })
                            Text("I intentionally accept the sandbox security change.", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            },
            confirmButton = {
                Button(
                    onClick = { model.saveConfigDraft(candidate); pendingDraft = null },
                    enabled = !model.configSaving && (!securityWeakened || securityChangeAcknowledged),
                ) { Text("Save configuration") }
            },
            dismissButton = { TextButton(onClick = { pendingDraft = null }) { Text("Cancel") } },
        )
    }
    if (showReloadConfirm) {
        AlertDialog(
            onDismissRequest = { showReloadConfirm = false },
            title = { Text("Discard local edits?") },
            text = { Text("Reloading replaces the unsaved form values with the current configuration returned by the core.") },
            confirmButton = {
                Button(onClick = { showReloadConfirm = false; model.refreshConfig() }) { Text("Discard and reload") }
            },
            dismissButton = { TextButton(onClick = { showReloadConfirm = false }) { Text("Keep editing") } },
        )
    }
}

@Composable
private fun ConfigGroup(title: String, content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit) {
    ElevatedCard {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp), content = {
            Text(title, style = MaterialTheme.typography.titleLarge)
            HorizontalDivider()
            content()
        })
    }
}

@Composable
private fun ConfigNumberField(label: String, value: String, range: String, onValue: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = { newValue -> if (newValue.length <= 7 && newValue.all(Char::isDigit)) onValue(newValue) },
        label = { Text(label) },
        supportingText = { Text("Allowed range: $range") },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun ConfigSwitch(title: String, description: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall)
            Text(description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

@Composable
private fun StatCard(label: String, value: String, modifier: Modifier = Modifier) {
    Card(modifier = modifier, colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(value, style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.onSecondaryContainer)
            Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSecondaryContainer)
        }
    }
}

@Composable
private fun StateChip(label: String, active: Boolean) {
    Surface(
        color = if (active) MaterialTheme.colorScheme.tertiaryContainer else MaterialTheme.colorScheme.surfaceContainerHigh,
        shape = MaterialTheme.shapes.small,
    ) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
            style = MaterialTheme.typography.labelMedium,
            color = if (active) MaterialTheme.colorScheme.onTertiaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun StatusLine(label: String, value: Boolean) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        Text(if (value) "Yes" else "No", style = MaterialTheme.typography.labelLarge, color = if (value) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun InfoCard(title: String, message: String, error: Boolean = false) {
    Card(
        colors = CardDefaults.cardColors(containerColor = if (error) MaterialTheme.colorScheme.errorContainer else MaterialTheme.colorScheme.surfaceContainerHigh),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium, color = if (error) MaterialTheme.colorScheme.onErrorContainer else MaterialTheme.colorScheme.onSurface)
            Text(message, style = MaterialTheme.typography.bodyMedium, color = if (error) MaterialTheme.colorScheme.onErrorContainer else MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

private fun jsonObjects(array: JSONArray?): List<JSONObject> = buildList {
    if (array == null) return@buildList
    for (index in 0 until array.length()) array.optJSONObject(index)?.let(::add)
}

private fun strings(array: JSONArray?): List<String> = buildList {
    if (array == null) return@buildList
    for (index in 0 until array.length()) array.optString(index).takeIf { it.isNotBlank() }?.let(::add)
}
