// Tolerant JSON accessors. Unknown fields are ignored and wrong types fall
// back to defaults, as required by contracts/v1 §1 ("ignore unknown fields").

String asString(dynamic value, [String fallback = '']) => value is String ? value : fallback;

String? asStringOrNull(dynamic value) => value is String ? value : null;

int asInt(dynamic value, [int fallback = 0]) {
  if (value is int) return value;
  if (value is num) return value.toInt();
  if (value is String) return int.tryParse(value) ?? fallback;
  return fallback;
}

int? asIntOrNull(dynamic value) {
  if (value is int) return value;
  if (value is num) return value.toInt();
  if (value is String) return int.tryParse(value);
  return null;
}

bool asBool(dynamic value, [bool fallback = false]) => value is bool ? value : fallback;

Map<String, dynamic> asMap(dynamic value) {
  if (value is Map<String, dynamic>) return value;
  if (value is Map) return value.map((k, v) => MapEntry('$k', v));
  return <String, dynamic>{};
}

List<dynamic> asList(dynamic value) => value is List ? value : const <dynamic>[];

List<String> asStringList(dynamic value) => asList(value).whereType<String>().toList();

DateTime? asDate(dynamic value) => value is String ? DateTime.tryParse(value) : null;

String prettyKey(String value) => value.replaceAll('_', ' ');
