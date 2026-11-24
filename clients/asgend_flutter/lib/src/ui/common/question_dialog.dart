import 'package:flutter/material.dart';

import '../../models/remote_models.dart';

class QuestionAnswer {
  const QuestionAnswer({required this.answer, this.optionId = ''});
  final String answer;
  final String optionId;
}

/// Modal used by both modes when a task waits for user input: suggested
/// options plus a free-text answer.
class QuestionDialog extends StatefulWidget {
  const QuestionDialog({super.key, required this.prompt, this.allowDismiss = false});
  final QuestionPrompt prompt;

  /// Remote mode lets the user close the dialog and answer later.
  final bool allowDismiss;

  static Future<QuestionAnswer?> show(BuildContext context, QuestionPrompt prompt, {bool allowDismiss = false}) =>
      showDialog<QuestionAnswer>(
        context: context,
        barrierDismissible: allowDismiss,
        builder: (_) => QuestionDialog(prompt: prompt, allowDismiss: allowDismiss),
      );

  @override
  State<QuestionDialog> createState() => _QuestionDialogState();
}

class _QuestionDialogState extends State<QuestionDialog> {
  String? _selectedId;
  String? _selectedLabel;
  final _customCtl = TextEditingController();

  @override
  void dispose() {
    _customCtl.dispose();
    super.dispose();
  }

  void _select(String? id) => setState(() {
        _selectedId = id;
        _selectedLabel = widget.prompt.options
            .firstWhere((o) => o.id == id, orElse: () => QuestionOption(id: id ?? '', label: id ?? ''))
            .label;
        _customCtl.clear();
      });

  void _submit() {
    final custom = _customCtl.text.trim();
    final answer = custom.isNotEmpty ? custom : (_selectedLabel ?? '');
    if (answer.isEmpty) return;
    Navigator.pop(context, QuestionAnswer(answer: answer, optionId: custom.isNotEmpty ? '' : (_selectedId ?? '')));
  }

  @override
  Widget build(BuildContext context) {
    final options = widget.prompt.options;
    return AlertDialog(
      title: const Row(children: [
        Icon(Icons.help_outline),
        SizedBox(width: 8),
        Expanded(child: Text('The agent needs your input')),
      ]),
      content: SizedBox(
        width: 480,
        child: SingleChildScrollView(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
            Text(widget.prompt.text.isEmpty ? 'Please clarify how to proceed.' : widget.prompt.text),
            const SizedBox(height: 12),
            // Flutter 3.29 (the pinned SDK) has no RadioGroup; each tile carries the group state.
            Column(mainAxisSize: MainAxisSize.min, children: [
              for (final option in options)
                RadioListTile<String>(
                  value: option.id,
                  groupValue: _selectedId,
                  onChanged: _select,
                  title: Text(option.label),
                  dense: true,
                ),
            ]),
            TextField(
              key: const Key('question_custom_answer'),
              controller: _customCtl,
              minLines: 1,
              maxLines: 3,
              decoration: const InputDecoration(labelText: 'Or enter your own answer'),
            ),
          ]),
        ),
      ),
      actions: [
        if (widget.allowDismiss) TextButton(onPressed: () => Navigator.pop(context), child: const Text('Later')),
        FilledButton(onPressed: _submit, child: const Text('Answer and continue')),
      ],
    );
  }
}
