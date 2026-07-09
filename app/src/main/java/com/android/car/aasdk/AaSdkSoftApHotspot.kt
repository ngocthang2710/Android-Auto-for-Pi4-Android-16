package com.android.car.aasdk

import android.content.Context
import android.net.TetheringManager
import android.net.TetheringManager.TetheringRequest
import android.net.wifi.SoftApConfiguration
import android.net.wifi.SoftApInfo
import android.net.wifi.SoftApState
import android.net.wifi.WifiClient
import android.net.wifi.WifiManager
import android.util.Log
import java.net.NetworkInterface
import java.util.concurrent.Executors

private const val TAG = "AaSdk_SoftAp"

private const val HOTSPOT_SSID = "AndroidAuto-HU"
// WPA2-PSK requires 8-63 chars. Fixed rather than random: this class (not a
// saved WifiConfiguration) is the sole source of truth for the credentials,
// so there's no masked-PSK problem (see git history of readWifiKey()) and no
// need to persist/rotate anything across boots.
private const val HOTSPOT_PASSPHRASE = "aasdkwireless"

data class HotspotInfo(
    val ssid: String,
    val passphrase: String,
    val bssid: String,
    val gatewayIp: String,
)

/**
 * Hosts a fixed-config Wi-Fi AP so a phone can join this head unit directly
 * (no shared external network needed) as the transport for wireless Android
 * Auto. [AaSdkBtWirelessHandshake] reports [currentInfo] to the phone over
 * Bluetooth once it's populated.
 */
class AaSdkSoftApHotspot(private val context: Context) {

    // Lazy, not eager: this class is constructed from AaSdkUsbService's own
    // field initializer, which runs during the Service's constructor -- i.e.
    // before Android calls attachBaseContext(), when getSystemService() would
    // NPE on a still-unattached base Context. Deferring until start() (called
    // later, once the service is fully up) avoids that.
    private val wifiManager by lazy { context.getSystemService(Context.WIFI_SERVICE) as WifiManager }
    private val tetheringManager by lazy {
        context.getSystemService(Context.TETHERING_SERVICE) as TetheringManager
    }
    private val executor = Executors.newSingleThreadExecutor()
    private var callback: WifiManager.SoftApCallback? = null

    @Volatile private var iface: String? = null
    @Volatile private var bssid: String? = null

    /**
     * Set once resolveIpv4() first succeeds for the current AP session (see
     * waitUntilReady()) -- not recomputed afterwards, so a transient DHCP/
     * netd hiccup after the phone has already joined can't null this back out
     * from under an active session.
     */
    @Volatile private var resolvedIp: String? = null

    /** True once start() has been called and stop() hasn't happened since --
     *  makes start() idempotent against a phone's BT retry storm racing an
     *  AP that's already up or still coming up (see handleConnection()). */
    @Volatile private var isActive = false

    val currentInfo: HotspotInfo?
        get() {
            val ip = resolvedIp ?: return null
            val b = bssid ?: return null
            return HotspotInfo(HOTSPOT_SSID, HOTSPOT_PASSPHRASE, b, ip)
        }

    fun start() {
        if (isActive) return
        isActive = true

        val config = SoftApConfiguration.Builder()
            .setSsid(HOTSPOT_SSID)
            .setPassphrase(HOTSPOT_PASSPHRASE, SoftApConfiguration.SECURITY_TYPE_WPA2_PSK)
            // BAND_5GHZ was tried first for throughput, but on this device
            // SoftApManager rejects it: "Failed to set country code, required
            // for setting up soft ap in band: 2" -- wificond reports the
            // driver's country code as "99" (world mode, not a real ISO code)
            // at the exact moment the AP interface is (re)created, and 5GHz
            // channel selection needs a resolved regulatory domain (DFS)
            // where 2.4GHz doesn't. Confirmed via logcat: every attempt with
            // BAND_5GHZ went straight to WIFI_AP_STATE_FAILED, so the BT
            // handshake never got real credentials and the phone never saw a
            // WiFi session -- that was the actual cause of the black screen,
            // not anything in the video/session pipeline.
            .setBand(SoftApConfiguration.BAND_2GHZ)
            // The BSSID is reported to the phone once, over BT, at handshake
            // time -- it must not change under us afterwards.
            .setMacRandomizationSetting(SoftApConfiguration.RANDOMIZATION_NONE)
            .build()

        val cb = object : WifiManager.SoftApCallback {
            override fun onStateChanged(state: SoftApState) {
                Log.i(TAG, "AP state=${state.state} iface=${state.iface}")
                iface = state.iface
                if (state.state != WifiManager.WIFI_AP_STATE_ENABLED) {
                    bssid = null
                    resolvedIp = null
                }
            }

            override fun onConnectedClientsChanged(info: SoftApInfo, clients: List<WifiClient>) {
                // Not needed: this class only reports hotspot credentials, not
                // per-client state.
            }

            override fun onInfoChanged(softApInfo: SoftApInfo) {
                // Only cache the BSSID here -- NOT the gateway IP. The
                // tethering framework assigns wlan0's IPv4 address
                // asynchronously, a short but unpredictable time *after* this
                // callback (confirmed via logcat: onInfoChanged already had a
                // real BSSID while resolveIpv4() still returned null every
                // time), so waitUntilReady() polls for the IP itself instead
                // of trusting a one-shot check made here.
                val b = softApInfo.bssid?.toString() ?: return
                Log.i(TAG, "AP info: iface=$iface bssid=$b")
                bssid = b
            }
        }
        callback = cb
        wifiManager.registerSoftApCallback(executor, cb)

        // WifiManager.startTetheredHotspot() alone only brings up the SoftAP
        // radio (hostapd) -- confirmed via logcat: state reached ENABLED with
        // a real BSSID, but wlan0's IPv4 address stayed 0.0.0.0 forever, no
        // dnsmasq/IpServer ever started for it. Actual IP provisioning for a
        // tethered interface only happens when the AP is brought up via
        // TetheringManager, which is what really drives IpServer. Going
        // through TetheringManager instead (which itself calls into
        // WifiManager) is what Settings' own hotspot toggle does.
        val request = TetheringRequest.Builder(TetheringManager.TETHERING_WIFI)
            .setSoftApConfiguration(config)
            // No carrier/entitlement setup exists on this device -- this is a
            // private, always-should-work AP for a directly-attached phone,
            // not carrier-metered tethering.
            .setExemptFromEntitlementCheck(true)
            .setShouldShowEntitlementUi(false)
            .build()
        tetheringManager.startTethering(request, executor, object : TetheringManager.StartTetheringCallback {
            override fun onTetheringStarted() {
                Log.i(TAG, "TetheringManager: onTetheringStarted")
            }

            override fun onTetheringFailed(error: Int) {
                Log.e(TAG, "TetheringManager: onTetheringFailed error=$error")
            }
        })
    }

    fun stop() {
        isActive = false
        resolvedIp = null
        bssid = null
        iface = null
        callback?.let { wifiManager.unregisterSoftApCallback(it) }
        callback = null
        tetheringManager.stopTethering(TetheringManager.TETHERING_WIFI)
    }

    /**
     * Blocks the calling thread (the BT RFCOMM accept thread, in practice)
     * until the AP is confirmed broadcasting with a resolvable gateway IP, or
     * [timeoutMs] elapses. Polls resolveIpv4() itself rather than relying on
     * a single computation in onInfoChanged() -- see that callback's comment.
     */
    fun waitUntilReady(timeoutMs: Long): HotspotInfo? {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            currentInfo?.let { return it }
            val ifaceName = iface
            val b = bssid
            if (ifaceName != null && b != null) {
                val ip = resolveIpv4(ifaceName)
                if (ip != null) {
                    resolvedIp = ip
                    Log.i(TAG, "AP up: iface=$ifaceName bssid=$b ip=$ip")
                    return currentInfo
                }
            }
            Thread.sleep(50)
        }
        return null
    }

    private fun resolveIpv4(ifaceName: String): String? = try {
        NetworkInterface.getByName(ifaceName)?.inetAddresses?.asSequence()
            ?.firstOrNull { it.hostAddress?.contains(':') == false }
            ?.hostAddress
    } catch (e: Exception) {
        Log.e(TAG, "resolveIpv4($ifaceName) failed: ${e.message}")
        null
    }
}
