package org.asgend.nativeclient.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Encrypts local-core and remote Control Plane tokens with separate Android Keystore keys. */
internal class SecureTokenStore(context: Context) {
    private val prefs = context.getSharedPreferences("asgend_secure", Context.MODE_PRIVATE)
    private val coreAlias = "org.asgend.nativeclient.core-token.v1"
    private val remoteAlias = "org.asgend.nativeclient.control-plane-token.v1"

    /** Local-core token slot; preserved independently from the Control Plane token. */
    fun save(token: String) = saveToken(CORE_TOKEN_KEY, coreAlias, token, "core")
    fun load(): String = loadToken(CORE_TOKEN_KEY, coreAlias)
    fun clear() = clearToken(CORE_TOKEN_KEY, "core")

    /** User bearer token (`ucp_…`) uses a distinct preference key and Keystore alias. */
    fun saveRemote(token: String) = saveToken(REMOTE_TOKEN_KEY, remoteAlias, token, "Control Plane")
    fun loadRemote(): String = loadToken(REMOTE_TOKEN_KEY, remoteAlias)
    fun clearRemote() = clearToken(REMOTE_TOKEN_KEY, "Control Plane")

    private fun saveToken(key: String, alias: String, token: String, label: String) {
        if (token.isEmpty()) {
            clearToken(key, label)
            return
        }
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey(alias))
        val ciphertext = cipher.doFinal(token.toByteArray(Charsets.UTF_8))
        val combined = cipher.iv + ciphertext
        val ok = prefs.edit().putString(key, Base64.encodeToString(combined, Base64.NO_WRAP)).commit()
        check(ok) { "Could not persist encrypted $label token." }
    }

    private fun loadToken(key: String, alias: String): String {
        val encoded = prefs.getString(key, null) ?: return ""
        val bytes = Base64.decode(encoded, Base64.NO_WRAP)
        require(bytes.size > IV_SIZE) { "Stored token value is truncated." }
        val iv = bytes.copyOfRange(0, IV_SIZE)
        val ciphertext = bytes.copyOfRange(IV_SIZE, bytes.size)
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(alias), GCMParameterSpec(TAG_BITS, iv))
        return cipher.doFinal(ciphertext).toString(Charsets.UTF_8)
    }

    private fun clearToken(key: String, label: String) {
        check(prefs.edit().remove(key).commit()) { "Could not clear encrypted $label token." }
    }

    private fun getOrCreateKey(alias: String): SecretKey {
        val store = KeyStore.getInstance(ANDROID_KEY_STORE).apply { load(null) }
        val existing = store.getKey(alias, null) as? SecretKey
        if (existing != null) return existing

        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEY_STORE)
        generator.init(
            KeyGenParameterSpec.Builder(
                alias,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build(),
        )
        return generator.generateKey()
    }

    private companion object {
        const val ANDROID_KEY_STORE = "AndroidKeyStore"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val CORE_TOKEN_KEY = "encrypted_core_token"
        const val REMOTE_TOKEN_KEY = "encrypted_control_plane_token"
        const val IV_SIZE = 12
        const val TAG_BITS = 128
    }
}
