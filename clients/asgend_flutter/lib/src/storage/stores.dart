import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Non-secret preferences (endpoint, workspace id, UI options, caches).
abstract class KeyValueStore {
  Future<String?> getString(String key);
  Future<void> setString(String key, String value);
  Future<void> remove(String key);
  Future<Set<String>> keys();
}

/// Secrets: Control Plane user token, local core token.
/// Android implementation: flutter_secure_storage (RSA-OAEP key wrapped in the
/// Android Keystore + AES-GCM). The backing file is excluded from backups by
/// android:allowBackup="false" and the backup/data-extraction rules.
abstract class SecretStore {
  Future<String?> read(String key);
  Future<void> write(String key, String value);
  Future<void> delete(String key);
}

class SharedPrefsStore implements KeyValueStore {
  SharedPrefsStore([SharedPreferencesAsync? prefs]) : _prefs = prefs ?? SharedPreferencesAsync();
  final SharedPreferencesAsync _prefs;

  @override
  Future<String?> getString(String key) => _prefs.getString(key);
  @override
  Future<void> setString(String key, String value) => _prefs.setString(key, value);
  @override
  Future<void> remove(String key) => _prefs.remove(key);
  @override
  Future<Set<String>> keys() => _prefs.getKeys();
}

class PlatformSecretStore implements SecretStore {
  PlatformSecretStore([FlutterSecureStorage? storage]) : _storage = storage ?? const FlutterSecureStorage();
  final FlutterSecureStorage _storage;

  @override
  Future<String?> read(String key) => _storage.read(key: key);
  @override
  Future<void> write(String key, String value) => _storage.write(key: key, value: value);
  @override
  Future<void> delete(String key) => _storage.delete(key: key);
}

/// In-memory implementations for tests and for platforms without a keyring.
class MemoryKeyValueStore implements KeyValueStore {
  final Map<String, String> data = {};
  @override
  Future<String?> getString(String key) async => data[key];
  @override
  Future<void> setString(String key, String value) async => data[key] = value;
  @override
  Future<void> remove(String key) async => data.remove(key);
  @override
  Future<Set<String>> keys() async => data.keys.toSet();
}

class MemorySecretStore implements SecretStore {
  final Map<String, String> data = {};
  @override
  Future<String?> read(String key) async => data[key];
  @override
  Future<void> write(String key, String value) async => data[key] = value;
  @override
  Future<void> delete(String key) async => data.remove(key);
}
