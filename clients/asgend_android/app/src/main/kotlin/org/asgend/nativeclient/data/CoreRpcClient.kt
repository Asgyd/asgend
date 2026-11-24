package org.asgend.nativeclient.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

class CoreRpcException(message: String, cause: Throwable? = null) : IOException(message, cause)

/** Injectable transport seam so repository protocol contracts can be tested without sockets. */
fun interface CoreRpcTransport {
    suspend fun call(payload: JSONObject, timeoutMs: Int): JSONObject
    suspend fun call(payload: JSONObject): JSONObject = call(payload, 8_000)
}

/** Raw JSON-line client matching the existing C core protocol. */
class CoreRpcClient(private val settings: CoreSettings) : CoreRpcTransport {
    override suspend fun call(payload: JSONObject, timeoutMs: Int): JSONObject =
        withContext(Dispatchers.IO) { callBlocking(payload, timeoutMs) }

    private fun callBlocking(payload: JSONObject, timeoutMs: Int): JSONObject {
        val error = settings.validationError()
        if (error != null) throw CoreRpcException(error)
        val request = JSONObject(payload.toString())
        if (settings.token.isNotEmpty()) request.put("auth_token", settings.token)
        val wire = (request.toString() + "\n").toByteArray(Charsets.UTF_8)
        if (wire.size > MAX_REQUEST_BYTES) throw CoreRpcException("Request is larger than the 1 MiB client limit.")

        try {
            Socket().use { socket ->
                socket.tcpNoDelay = true
                socket.connect(InetSocketAddress(settings.host.trim(), settings.port), CONNECT_TIMEOUT_MS)
                socket.soTimeout = timeoutMs
                val output = socket.getOutputStream()
                output.write(wire)
                output.flush()
                val responseBytes = readLineBounded(socket, MAX_RESPONSE_BYTES, timeoutMs)
                if (responseBytes.isEmpty()) throw CoreRpcException("Core closed the connection without a response.")
                val response = runCatching { JSONObject(responseBytes.toString(Charsets.UTF_8)) }
                    .getOrElse { throw CoreRpcException("Core returned invalid JSON.", it) }
                if (!response.optBoolean("ok", false)) {
                    val serverError = response.optString("error", "Request failed.")
                    throw CoreRpcException(safeErrorText(serverError, settings.token))
                }
                return response
            }
        } catch (e: CoreRpcException) {
            throw e
        } catch (e: SocketTimeoutException) {
            throw CoreRpcException("Timed out waiting for the ASGEND core.", e)
        } catch (e: IOException) {
            throw CoreRpcException("Cannot reach ASGEND core at ${settings.host}:${settings.port}: ${safeErrorText(e.message.orEmpty(), settings.token)}", e)
        } catch (e: SecurityException) {
            throw CoreRpcException("Android denied the connection to the ASGEND core.", e)
        }
    }

    private fun readLineBounded(socket: Socket, maxBytes: Int, timeoutMs: Int): ByteArray {
        // Enforce one absolute response deadline rather than resetting a full idle timeout per byte.
        val input = BufferedInputStream(socket.getInputStream(), 16 * 1024)
        val deadlineNanos = System.nanoTime() + timeoutMs.toLong() * 1_000_000L
        val output = ByteArrayOutputStream(minOf(8192, maxBytes))
        while (output.size() <= maxBytes) {
            val remainingNanos = deadlineNanos - System.nanoTime()
            if (remainingNanos <= 0) throw SocketTimeoutException("overall response deadline exceeded")
            val remainingMillis = ((remainingNanos + 999_999L) / 1_000_000L).coerceIn(1L, Int.MAX_VALUE.toLong()).toInt()
            socket.soTimeout = remainingMillis
            val value = try {
                input.read()
            } catch (e: SocketTimeoutException) {
                throw e
            }
            if (value == -1 || value == '\n'.code) return output.toByteArray()
            if (value != '\r'.code) output.write(value)
            if (output.size() > maxBytes) throw CoreRpcException("Core response exceeded the 16 MiB limit.")
        }
        throw CoreRpcException("Core response exceeded the 16 MiB limit.")
    }

    companion object {
        const val CONNECT_TIMEOUT_MS = 5_000
        const val DEFAULT_TIMEOUT_MS = 8_000
        const val MAX_REQUEST_BYTES = 1 * 1024 * 1024
        const val MAX_RESPONSE_BYTES = 16 * 1024 * 1024
    }
}

internal fun jsonArrayObjects(array: JSONArray?): List<JSONObject> = buildList {
    if (array == null) return@buildList
    for (index in 0 until array.length()) {
        val value = array.optJSONObject(index) ?: continue
        add(value)
    }
}
