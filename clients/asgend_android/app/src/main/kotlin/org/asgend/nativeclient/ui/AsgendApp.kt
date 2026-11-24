package org.asgend.nativeclient.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.Hub
import androidx.compose.material.icons.outlined.ListAlt
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.TaskAlt
import androidx.compose.material.icons.outlined.WifiOff
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import org.asgend.nativeclient.ui.remote.NativeRemoteViewModel
import org.asgend.nativeclient.ui.remote.RemoteControlSection

private enum class Destination(val title: String, val icon: ImageVector) {
    Workspace("Workspace", Icons.Outlined.TaskAlt),
    Plan("To-do & Goals", Icons.Outlined.ListAlt),
    Runtime("Runtime", Icons.Outlined.Hub),
    Files("Files", Icons.Outlined.FolderOpen),
    Settings("Settings", Icons.Outlined.Settings),
}

@Composable
fun AsgendApp(model: NativeAppViewModel = viewModel(), remoteModel: NativeRemoteViewModel = viewModel()) {
    var selectedName by rememberSaveable { mutableStateOf(Destination.Workspace.name) }
    var showConnectionSettings by rememberSaveable { mutableStateOf(false) }
    val selected = Destination.entries.firstOrNull { it.name == selectedName } ?: Destination.Workspace

    LaunchedEffect(model.settingsLoaded, model.settings.host) {
        if (model.settingsLoaded && model.settings.host.isBlank()) showConnectionSettings = true
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val expanded = maxWidth >= 640.dp
        if (expanded) {
            Row(Modifier.fillMaxSize()) {
                NavigationRail(
                    header = {
                        Surface(
                            color = MaterialTheme.colorScheme.primaryContainer,
                            shape = MaterialTheme.shapes.large,
                            modifier = Modifier.padding(vertical = 12.dp).size(52.dp),
                        ) {
                            Box(contentAlignment = Alignment.Center) {
                                Icon(Icons.Outlined.Hub, contentDescription = "ASGEND", tint = MaterialTheme.colorScheme.onPrimaryContainer)
                            }
                        }
                    },
                ) {
                    Destination.entries.forEach { destination ->
                        NavigationRailItem(
                            selected = destination == selected,
                            onClick = { selectedName = destination.name },
                            icon = { Icon(destination.icon, contentDescription = destination.title) },
                            label = { Text(destination.title) },
                        )
                    }
                }
                Box(Modifier.weight(1f).fillMaxSize()) {
                    MainScaffold(selected, model, remoteModel, onOpenSettings = { showConnectionSettings = true }, onSelect = { selectedName = it.name })
                }
            }
        } else {
            Scaffold(
                topBar = { AsgendTopBar(selected, model) },
                bottomBar = {
                    NavigationBar {
                        Destination.entries.forEach { destination ->
                            NavigationBarItem(
                                selected = destination == selected,
                                onClick = { selectedName = destination.name },
                                icon = { Icon(destination.icon, contentDescription = destination.title) },
                                label = { Text(destination.title) },
                            )
                        }
                    }
                },
            ) { padding ->
                DestinationBody(
                    destination = selected,
                    model = model,
                    remoteModel = remoteModel,
                    modifier = Modifier.padding(padding),
                    onOpenSettings = { showConnectionSettings = true },
                    onNavigate = { selectedName = it.name },
                )
            }
        }
    }

    if (expandedOrSettingsModal(showConnectionSettings)) {
        ConnectionSettingsDialog(
            settings = model.settings,
            busy = model.connecting,
            error = model.error,
            onDismiss = { showConnectionSettings = false },
            onSave = { host, port, token, ack -> model.saveSettings(host, port, token, ack); showConnectionSettings = false },
            onForgetToken = model::forgetToken,
        )
    }
}

private fun expandedOrSettingsModal(show: Boolean) = show

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AsgendTopBar(destination: Destination, model: NativeAppViewModel) {
    TopAppBar(
        title = {
            Column(verticalArrangement = Arrangement.spacedBy(1.dp)) {
                Text("ASGEND", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                Text(destination.title, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        actions = {
            Surface(
                color = if (model.connected) MaterialTheme.colorScheme.tertiaryContainer else MaterialTheme.colorScheme.surfaceContainerHigh,
                shape = MaterialTheme.shapes.extraLarge,
                modifier = Modifier.padding(end = 12.dp),
            ) {
                Row(Modifier.padding(horizontal = 10.dp, vertical = 7.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (!model.connected && model.connecting) CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
                    else Icon(
                        if (model.connected) Icons.Outlined.Hub else Icons.Outlined.WifiOff,
                        contentDescription = null,
                        modifier = Modifier.size(16.dp),
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(if (model.connected) "Connected" else if (model.connecting) "Connecting" else "Offline", style = MaterialTheme.typography.labelMedium)
                }
            }
        },
    )
}

@Composable
private fun MainScaffold(
    destination: Destination,
    model: NativeAppViewModel,
    remoteModel: NativeRemoteViewModel,
    onOpenSettings: () -> Unit,
    onSelect: (Destination) -> Unit,
) {
    Scaffold(topBar = { AsgendTopBar(destination, model) }) { padding ->
        DestinationBody(destination, model, remoteModel, Modifier.padding(padding), onOpenSettings, onSelect)
    }
}

@Composable
private fun DestinationBody(
    destination: Destination,
    model: NativeAppViewModel,
    remoteModel: NativeRemoteViewModel,
    modifier: Modifier = Modifier,
    onOpenSettings: () -> Unit,
    onNavigate: (Destination) -> Unit,
) {
    when (destination) {
        Destination.Workspace -> TaskWorkspaceScreen(model, modifier)
        Destination.Plan -> PlanScreen(model, modifier)
        Destination.Runtime -> RuntimeScreen(model, modifier)
        Destination.Files -> FilesScreen(model, modifier)
        Destination.Settings -> ConnectionSettingsScreen(model, remoteModel, modifier, onOpenSettings)
    }
}

@Composable
private fun WorkspacePlaceholder(
    model: NativeAppViewModel,
    modifier: Modifier,
    onOpenSettings: () -> Unit,
    onNavigate: (Destination) -> Unit,
) {
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Surface(
            color = MaterialTheme.colorScheme.primaryContainer,
            shape = MaterialTheme.shapes.extraLarge,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(Modifier.padding(22.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Work with your agents", style = MaterialTheme.typography.headlineMedium, color = MaterialTheme.colorScheme.onPrimaryContainer)
                Text(
                    "A native Android interface for the ASGEND core. Tasks, live events, planning and files stay backed by the same core database used by CLI/TUI.",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                )
                FilledTonalButton(onClick = if (model.connected) { { model.connect() } } else onOpenSettings) {
                    Icon(if (model.connected) Icons.Outlined.Hub else Icons.Outlined.WifiOff, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(if (model.connected) "Refresh core connection" else "Connect to core")
                }
            }
        }
        if (!model.connected) {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Core connection required", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onErrorContainer)
                    Text(
                        model.error ?: "Configure the core host and confirm that raw TCP is protected by a trusted VPN or SSH tunnel.",
                        color = MaterialTheme.colorScheme.onErrorContainer,
                    )
                    FilledTonalButton(onClick = onOpenSettings, colors = ButtonDefaults.filledTonalButtonColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Text("Connection settings")
                    }
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
            ShortcutCard("To-do & Goals", "Shared planning", Icons.Outlined.ListAlt, Modifier.weight(1f)) { onNavigate(Destination.Plan) }
            ShortcutCard("Runtime", "Health and models", Icons.Outlined.Hub, Modifier.weight(1f)) { onNavigate(Destination.Runtime) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
            ShortcutCard("Files", "Task workspace", Icons.Outlined.FolderOpen, Modifier.weight(1f)) { onNavigate(Destination.Files) }
            ShortcutCard("Settings", "Connection and secrets", Icons.Outlined.Settings, Modifier.weight(1f)) { onNavigate(Destination.Settings) }
        }
        Text("Native build foundation · API 37", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun ShortcutCard(title: String, subtitle: String, icon: ImageVector, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Card(modifier = modifier, onClick = onClick, shape = MaterialTheme.shapes.large) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(28.dp))
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun FeaturePlaceholder(title: String, subtitle: String, description: String, icon: ImageVector, modifier: Modifier, onOpenSettings: () -> Unit) {
    Column(modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp), verticalArrangement = Arrangement.spacedBy(18.dp)) {
        Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(40.dp))
        Text(title, style = MaterialTheme.typography.headlineMedium)
        Text(subtitle, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(description, style = MaterialTheme.typography.bodyLarge)
        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Connection", style = MaterialTheme.typography.titleMedium)
                Text("This screen will use the existing core API; it will not keep a second copy of the task state on the device.", style = MaterialTheme.typography.bodyMedium)
                FilledTonalButton(onClick = onOpenSettings) { Text("Connection settings") }
            }
        }
    }
}

@Composable
private fun ConnectionSettingsScreen(model: NativeAppViewModel, remoteModel: NativeRemoteViewModel, modifier: Modifier, onOpenSettings: () -> Unit) {
    Column(modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Text("Connection", style = MaterialTheme.typography.headlineMedium)
        Text("Configure the remote core host. On a physical phone, localhost points to the phone itself; use the core's LAN address or a trusted VPN.", style = MaterialTheme.typography.bodyLarge)
        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(if (model.connected) "Connected to ASGEND core" else "Not connected", style = MaterialTheme.typography.titleMedium)
                Text("${model.settings.host.ifBlank { "No host set" }}:${model.settings.port}", color = MaterialTheme.colorScheme.onSurfaceVariant)
                model.version?.let { Text("Core version $it", style = MaterialTheme.typography.bodySmall) }
                model.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                FilledTonalButton(onClick = onOpenSettings) { Text("Edit connection") }
                if (model.settings.token.isNotEmpty()) FilledTonalButton(onClick = model::forgetToken) { Text("Forget saved token") }
            }
        }
        RemoteControlSection(remoteModel, Modifier.padding(top = 4.dp))
    }
}

@Composable
private fun ConnectionSettingsDialog(
    settings: org.asgend.nativeclient.data.CoreSettings,
    busy: Boolean,
    error: String?,
    onDismiss: () -> Unit,
    onSave: (String, String, String, Boolean) -> Unit,
    onForgetToken: () -> Unit,
) {
    var host by remember(settings.host) { mutableStateOf(settings.host) }
    var port by remember(settings.port) { mutableStateOf(settings.port.toString()) }
    var token by remember(settings.token) { mutableStateOf(settings.token) }
    var ack by remember(settings.vpnTunnelAcknowledged) { mutableStateOf(settings.vpnTunnelAcknowledged) }
    androidx.compose.material3.AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Connect to ASGEND") },
        text = {
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                androidx.compose.material3.OutlinedTextField(host, { host = it }, label = { Text("Core host / VPN address") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                androidx.compose.material3.OutlinedTextField(port, { port = it.filter(Char::isDigit).take(5) }, label = { Text("TCP port") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                androidx.compose.material3.OutlinedTextField(token, { token = it }, label = { Text("Core token · encrypted at rest") }, singleLine = true, visualTransformation = androidx.compose.ui.text.input.PasswordVisualTransformation(), modifier = Modifier.fillMaxWidth())
                Surface(color = MaterialTheme.colorScheme.tertiaryContainer, shape = MaterialTheme.shapes.medium) {
                    Text(
                        "Core TCP has no TLS. Never expose it to the public Internet; use a trusted VPN or SSH tunnel. Prefer the HTTPS Control Plane for remote networks.",
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onTertiaryContainer,
                    )
                }
                CheckboxRow(
                    checked = ack,
                    onCheckedChange = { ack = it },
                    title = "I use a trusted VPN or SSH tunnel",
                )
                if (error != null) Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
        },
        confirmButton = {
            Button(onClick = { onSave(host, port, token, ack) }, enabled = !busy) {
                if (busy) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp) else Text("Save and connect")
            }
        },
        dismissButton = {
            androidx.compose.material3.TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

@Composable
private fun CheckboxRow(checked: Boolean, onCheckedChange: (Boolean) -> Unit, title: String) {
    androidx.compose.material3.Row(verticalAlignment = Alignment.CenterVertically) {
        androidx.compose.material3.Checkbox(checked = checked, onCheckedChange = onCheckedChange)
        Text(title, style = MaterialTheme.typography.bodyMedium)
    }
}
