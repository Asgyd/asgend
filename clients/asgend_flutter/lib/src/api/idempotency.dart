import 'dart:math';

/// Generates `idempotency_key` values matching `^[A-Za-z0-9._:-]{8,128}$`
/// (contracts/v1 §3): `<kind>:<millis base36>-<96 random bits hex>`.
String newIdempotencyKey(String kind, {Random? random}) {
  final r = random ?? Random.secure();
  final buf = StringBuffer()
    ..write(kind.replaceAll(RegExp(r'[^A-Za-z0-9._-]'), ''))
    ..write(':')
    ..write(DateTime.now().millisecondsSinceEpoch.toRadixString(36))
    ..write('-');
  for (var i = 0; i < 12; i++) {
    buf.write(r.nextInt(256).toRadixString(16).padLeft(2, '0'));
  }
  return buf.toString();
}

final RegExp idempotencyKeyPattern = RegExp(r'^[A-Za-z0-9._:-]{8,128}$');

/// One user action ("submit this text to that node", "cancel task X").
///
/// The key is created once, when the user first triggers the action, and is
/// reused for every retry of the *same* action — including a retry the user
/// presses after an offline/timeout error. A different body (edited text,
/// other node) produces a new key, because the server answers 409 for the same
/// key with a different body.
class PendingAction {
  PendingAction._(this.kind, this.fingerprint, this.key);

  final String kind;
  final String fingerprint;
  final String key;

  static PendingAction create(String kind, String fingerprint, {Random? random}) =>
      PendingAction._(kind, fingerprint, newIdempotencyKey(kind, random: random));

  /// Returns [previous] if it describes the same action, otherwise a new one.
  static PendingAction reuseOrCreate(PendingAction? previous, String kind, String fingerprint, {Random? random}) {
    if (previous != null && previous.kind == kind && previous.fingerprint == fingerprint) return previous;
    return create(kind, fingerprint, random: random);
  }
}
