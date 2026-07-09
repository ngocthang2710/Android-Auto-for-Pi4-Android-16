package com.android.car.aasdk

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

private const val TAG = "AaSdk_BtWireless"

// Same RFCOMM service UUID real Android Auto Wireless head units register
// (confirmed against multiple independent reverse-engineering write-ups:
// aa-proxy-rs, headunit-revived).
private val AA_WIRELESS_UUID: UUID = UUID.fromString("4de17a00-52cb-11e6-bdf4-0800200c9a66")

// f1x/aa-proxy-rs "ProxyMessageId" values -- kept in full (even the ones this
// class doesn't send) so logged msgId numbers are self-documenting.
private const val MSG_WIFI_START_REQUEST = 1
private const val MSG_WIFI_INFO_REQUEST = 2 // expected inbound at stage 2
private const val MSG_WIFI_INFO_RESPONSE = 3
private const val MSG_WIFI_VERSION_REQUEST = 4 // unused: not part of the 5-stage flow
private const val MSG_WIFI_VERSION_RESPONSE = 5
private const val MSG_WIFI_CONNECT_STATUS = 6 // expected inbound at stage 5
private const val MSG_WIFI_START_RESPONSE = 7 // expected inbound at stage 4

// WifiInfoResponse.SecurityMode / AccessPointType enum values (see
// WifiInfoResponse.proto).
private const val SECURITY_WPA2_PERSONAL = 8
private const val AP_TYPE_STATIC = 0

/**
 * Bluetooth RFCOMM handshake that lets a phone discover this head unit's
 * wireless-projection TCP listener (see AaSdkUsbService's WIFI_PROJECTION_PORT)
 * without a USB cable. Head unit is the RFCOMM server. Exact message order is
 * still being reverse-engineered against a real phone -- this build logs
 * every inbound frame it gets back so the real sequence can be inferred from
 * a live capture instead of guessed.
 *
 * Credentials sent to the phone (SSID/passphrase/BSSID) are for the AP this
 * head unit itself hosts and broadcasts (see [AaSdkSoftApHotspot]), not an
 * external/shared network -- the phone joins the head unit directly.
 */
class AaSdkBtWirelessHandshake(private val hotspot: AaSdkSoftApHotspot) {

    private var serverSocket: BluetoothServerSocket? = null
    private var acceptThread: Thread? = null

    // The currently-connected phone's RFCOMM link, if any -- tracked so
    // forceDisconnect() can close exactly this socket (closing serverSocket
    // alone only stops *future* accept() calls, it doesn't touch a socket
    // already handed to handleConnection()).
    @Volatile private var activeSocket: BluetoothSocket? = null

    fun start(tcpPort: Int) {
        val adapter = BluetoothAdapter.getDefaultAdapter()
        if (adapter == null) {
            Log.e(TAG, "No BluetoothAdapter, wireless handshake disabled")
            return
        }
        // This whole feature is optional/experimental on top of the working USB
        // flow -- e.g. a missing BLUETOOTH_ADVERTISE grant throws
        // SecurityException, not IOException. Catching Throwable here keeps any
        // failure contained to this thread instead of crashing the process
        // (and with it, the USB session running in the same service).
        acceptThread = Thread {
            try {
                val server = adapter.listenUsingRfcommWithServiceRecord(
                    "AndroidAutoWirelessHU", AA_WIRELESS_UUID)
                serverSocket = server
                Log.i(TAG, "RFCOMM listening on $AA_WIRELESS_UUID")
                while (!Thread.currentThread().isInterrupted) {
                    val socket = server.accept()
                    Log.i(TAG, "BT client connected: ${socket.remoteDevice?.address}")
                    handleConnection(socket, tcpPort)
                }
            } catch (e: Throwable) {
                Log.e(TAG, "RFCOMM listener stopped: ${e.message}", e)
            }
        }.apply { isDaemon = true; start() }
    }

    fun stop() {
        acceptThread?.interrupt()
        try {
            serverSocket?.close()
        } catch (e: IOException) {
            // already closed
        }
        serverSocket = null
        acceptThread = null
        // Safety net for a service teardown that races a live handleConnection
        // (interrupting acceptThread doesn't interrupt its blocking socket
        // reads) -- the AP shouldn't outlive the service either way.
        hotspot.stop()
    }

    // Ends the current phone's wireless AA session at the BT+WiFi level, not
    // just the AA (TCP) protocol level -- closing only the TCP socket
    // (AaSdkUsbService.resetWirelessSessionForReentry()'s first attempt) left
    // the phone's own AA app never reconnecting: from its side, the BT link
    // and WiFi AP both still looked alive, so it had no signal to redo
    // discovery. Closing activeSocket here makes handleConnection()'s
    // blocking read loop throw, which (via its finally block) tears down the
    // SoftAP and closes the BT link -- exactly what happens on a full app
    // process restart, which was the one thing observed to reliably make the
    // phone redo its 5-stage handshake and reconnect. No-op if no phone is
    // currently connected.
    fun forceDisconnect() {
        activeSocket?.let {
            Log.i(TAG, "forceDisconnect: closing active BT link to force phone reconnect")
            try {
                it.close()
            } catch (e: IOException) {
                // already closed
            }
        }
    }

    // Canonical 5-stage handshake, confirmed against aa-proxy-rs's own
    // stage-numbered log messages (our first attempt had this backwards --
    // WifiInfoResponse pushed before WifiStartRequest, never reading
    // WifiInfoRequest/WifiStartResponse in between -- which a real phone
    // answered with WifiConnectStatus(status=-7), i.e. rejected):
    //   1) HU    -> WifiStartRequest
    //   2) phone -> WifiInfoRequest
    //   3) HU    -> WifiInfoResponse
    //   4) phone -> WifiStartResponse
    //   5) phone -> WifiConnectStatus
    private fun handleConnection(socket: BluetoothSocket, tcpPort: Int) {
        // Started here, not when the service/BT listener comes up: a BT client
        // on this UUID means a phone is specifically asking for wireless AA,
        // whereas the service itself also starts for plain wired USB sessions
        // (see UsbAttachActivity) that never need WiFi at all. Pi4's WiFi
        // radio is single-radio (brcmfmac) -- switching it into AP mode drops
        // any STA connection (e.g. adb-over-wifi), so it must stay off unless
        // a phone is actually here to use it.
        hotspot.start()
        activeSocket = socket
        try {
            val out = socket.outputStream
            val input = socket.inputStream

            // Cold-start budget: bringing up hostapd + the AP interface from
            // scratch (not pre-warmed) can take a couple seconds.
            val info = hotspot.waitUntilReady(timeoutMs = 8000)
            if (info == null) {
                Log.e(TAG, "SoftAP not broadcasting yet, aborting handshake")
                return
            }

            sendFrame(out, MSG_WIFI_START_REQUEST, encodeWifiStartRequest(info.gatewayIp, tcpPort))
            Log.i(TAG, "Stage 1/5: sent WifiStartRequest(ip=${info.gatewayIp} port=$tcpPort)")

            logInboundFrame(readFrame(input, 3000), "Stage 2/5 (expect WifiInfoRequest)")

            sendFrame(out, MSG_WIFI_INFO_RESPONSE,
                encodeWifiInfoResponse(info.ssid, info.passphrase, info.bssid))
            Log.i(TAG, "Stage 3/5: sent WifiInfoResponse(ssid=${info.ssid} bssid=${info.bssid})")

            logInboundFrame(readFrame(input, 3000), "Stage 4/5 (expect WifiStartResponse)")
            logInboundFrame(readFrame(input, 3000), "Stage 5/5 (expect WifiConnectStatus)")

            // The active AA session now lives entirely on the TCP socket, but
            // the phone still expects this BT link to stay up (likely for
            // WifiPingRequest/Response keepalives, per the ProxyMessageId
            // enum). A fixed idle timeout here (previously 10s) made this
            // code hang up on its own every ~11.5s, which the phone read as a
            // dropped link and answered by redoing the entire handshake --
            // that was the "unstable connection" symptom. Block indefinitely
            // instead; only the phone closing its end (or the service tearing
            // down) should end this.
            try {
                while (input.read() >= 0) {
                    // Not parsing frames here: this is just "stay connected and
                    // don't hang up first." A future iteration could decode and
                    // answer WifiPingRequest explicitly if needed.
                }
            } catch (e: IOException) {
                // phone closed its end
            }
        } catch (e: Exception) {
            Log.e(TAG, "BT wireless handshake failed: ${e.message}")
        } finally {
            // The AA session itself runs over the TCP socket (WIFI_PROJECTION_PORT),
            // not this BT link, but this BT link is held open for the entire
            // session (see the indefinite read loop above) -- so it ending is
            // the right signal that the AP is no longer needed either.
            activeSocket = null
            hotspot.stop()
            try {
                socket.close()
            } catch (e: IOException) {
                // ignore
            }
        }
    }

    private fun logInboundFrame(frame: Pair<Int, ByteArray>?, stage: String) {
        if (frame == null) {
            Log.i(TAG, "$stage: no inbound frame (timed out or closed)")
            return
        }
        val (msgId, payload) = frame
        val hex = payload.joinToString("") { "%02x".format(it) }
        Log.i(TAG, "$stage: received msgId=$msgId len=${payload.size} payload=$hex")
    }

    // Polling-based read-with-timeout: BluetoothSocket has no setSoTimeout,
    // so this checks available() on an interval instead of blocking forever.
    private fun readFrame(input: InputStream, timeoutMs: Long): Pair<Int, ByteArray>? {
        val deadline = System.currentTimeMillis() + timeoutMs
        val header = ByteArray(4)
        if (!readFully(input, header, deadline)) return null
        val len = ((header[0].toInt() and 0xFF) shl 8) or (header[1].toInt() and 0xFF)
        val msgId = ((header[2].toInt() and 0xFF) shl 8) or (header[3].toInt() and 0xFF)
        val payload = ByteArray(len)
        if (len > 0 && !readFully(input, payload, deadline)) return null
        return msgId to payload
    }

    private fun readFully(input: InputStream, buf: ByteArray, deadline: Long): Boolean {
        var offset = 0
        while (offset < buf.size) {
            if (System.currentTimeMillis() > deadline) return false
            if (input.available() > 0) {
                val n = input.read(buf, offset, buf.size - offset)
                if (n < 0) return false
                offset += n
            } else {
                Thread.sleep(20)
            }
        }
        return true
    }

    private fun sendFrame(out: OutputStream, messageId: Int, payload: ByteArray) {
        val header = byteArrayOf(
            (payload.size shr 8 and 0xFF).toByte(),
            (payload.size and 0xFF).toByte(),
            (messageId shr 8 and 0xFF).toByte(),
            (messageId and 0xFF).toByte(),
        )
        out.write(header)
        out.write(payload)
        out.flush()
    }

    private fun encodeWifiStartRequest(ipAddress: String, port: Int): ByteArray =
        ProtoWriter().apply {
            writeString(1, ipAddress)
            writeVarint(2, port.toLong())
        }.toByteArray()

    private fun encodeWifiInfoResponse(ssid: String, key: String, bssid: String): ByteArray =
        ProtoWriter().apply {
            writeString(1, ssid)
            writeString(2, key)
            writeString(3, bssid)
            writeVarint(4, SECURITY_WPA2_PERSONAL.toLong())
            writeVarint(5, AP_TYPE_STATIC.toLong())
        }.toByteArray()
}

// Minimal proto2 wire-format writer. WifiStartRequest/WifiInfoResponse only
// use string and varint (int32/enum) fields, so a full protobuf-lite
// dependency isn't needed for these two messages.
private class ProtoWriter {
    private val buf = ByteArrayOutputStream()

    fun writeString(fieldNumber: Int, value: String) {
        writeTag(fieldNumber, wireType = 2)
        val bytes = value.toByteArray(Charsets.UTF_8)
        writeRawVarint(bytes.size.toLong())
        buf.write(bytes)
    }

    fun writeVarint(fieldNumber: Int, value: Long) {
        writeTag(fieldNumber, wireType = 0)
        writeRawVarint(value)
    }

    fun toByteArray(): ByteArray = buf.toByteArray()

    private fun writeTag(fieldNumber: Int, wireType: Int) {
        writeRawVarint(((fieldNumber shl 3) or wireType).toLong())
    }

    private fun writeRawVarint(v: Long) {
        var value = v
        while (true) {
            if ((value and 0x7FL.inv()) == 0L) {
                buf.write(value.toInt())
                return
            }
            buf.write(((value and 0x7F) or 0x80).toInt())
            value = value ushr 7
        }
    }
}
