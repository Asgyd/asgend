import 'package:flutter/material.dart';

enum BannerKind { info, warning, error }

/// Full-width state banner (offline, stale data, permission denied, …).
class StatusBanner extends StatelessWidget {
  const StatusBanner({super.key, required this.text, this.kind = BannerKind.warning, this.icon, this.action, this.onAction});

  final String text;
  final BannerKind kind;
  final IconData? icon;
  final String? action;
  final VoidCallback? onAction;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final (bg, fg) = switch (kind) {
      BannerKind.info => (scheme.secondaryContainer, scheme.onSecondaryContainer),
      BannerKind.warning => (scheme.tertiaryContainer, scheme.onTertiaryContainer),
      BannerKind.error => (scheme.errorContainer, scheme.onErrorContainer),
    };
    return Material(
      color: bg,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
        child: Row(children: [
          Icon(icon ?? (kind == BannerKind.error ? Icons.error_outline : Icons.info_outline), color: fg),
          const SizedBox(width: 8),
          Expanded(child: Text(text, style: TextStyle(color: fg))),
          if (action != null && onAction != null) TextButton(onPressed: onAction, child: Text(action!)),
        ]),
      ),
    );
  }
}

/// Confirmation before handing a potentially executable file (APK, script,
/// binary, HTML…) to another app. Returns true only on explicit consent.
Future<bool> confirmExecutable(BuildContext context, String name) async {
  final result = await showDialog<bool>(
    context: context,
    builder: (dialogContext) => AlertDialog(
      icon: const Icon(Icons.warning_amber_rounded),
      title: const Text('Potentially executable file'),
      content: Text('"$name" can run code or install software when opened. '
          'Its checksum matches what the node uploaded, but that does not make it safe. '
          'Continue only if you trust the task that produced it.'),
      actions: [
        TextButton(onPressed: () => Navigator.pop(dialogContext, false), child: const Text('Cancel')),
        FilledButton(onPressed: () => Navigator.pop(dialogContext, true), child: const Text('I understand, continue')),
      ],
    ),
  );
  return result == true;
}

String formatAge(Duration d) {
  if (d.inSeconds < 60) return '${d.inSeconds} s ago';
  if (d.inMinutes < 60) return '${d.inMinutes} min ago';
  if (d.inHours < 48) return '${d.inHours} h ago';
  return '${d.inDays} d ago';
}

String formatBytes(int bytes) {
  if (bytes < 1024) return '$bytes B';
  if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KiB';
  if (bytes < 1024 * 1024 * 1024) return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MiB';
  return '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(2)} GiB';
}
