package com.android.car.aasdk

import android.content.Context
import android.net.TetheringManager
import android.net.TetheringManager.TetheringRequest
import android.net.wifi.SoftApConfiguration
import android.net.wifi.SoftApInfo
import android.net.wifi.SoftApState
import android.net.wifi.WifiClient
import android.net.wifi.WifiManager
import android.os.SystemClock
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

// How long after the most recent startTethering attempt an AP that still
// isn't broadcasting is treated as wedged rather than merely slow. Must be
// comfortably above a worst-case legitimate bring-up (a 5GHz failure plus
// the in-flight 2.4GHz fallback fits inside handleConnection's 12s
// waitUntilReady budget), and every fallback/restart resets the clock.
private const val AP_WEDGE_RESTART_MS = 15_000L

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

    /** Band of the most recent startTethering attempt -- read by the AP
     *  callback to decide whether a failure should retry on 2.4GHz. */
    @Volatile private var requestedBand = SoftApConfiguration.BAND_5GHZ

    /** When startTethering() last ran (elapsedRealtime), for wedge detection. */
    @Volatile private var lastTetherAttemptMs = 0L

    fun start() {
        if (isActive) {
            // Self-heal: isActive alone proved insufficient as the no-op
            // guard. Observed live 2026-07-10: a session teardown's
            // stopTethering raced the phone's instant-reconnect
            // startTethering; the new attempt failed, its 2.4GHz fallback
            // failed too (fallbackTo2GhzIfNeeded dead-ends once
            // requestedBand is already 2.4GHz), and isActive stayed true
            // with no AP and nothing scheduled to retry -- every
            // handshake then timed out at waitUntilReady forever (wlan0
            // sat in STA mode while this class believed an AP was up).
            // The phone retries the handshake every ~12s, so this check
            // runs regularly: if the AP still isn't broadcasting well past
            // a worst-case bring-up, restart tethering from scratch.
            if (currentInfo == null &&
                SystemClock.elapsedRealtime() - lastTetherAttemptMs > AP_WEDGE_RESTART_MS) {
                Log.w(TAG, "AP still not ready ${AP_WEDGE_RESTART_MS}ms after last attempt" +
                    " -- assuming wedged, restarting tethering")
                tetheringManager.stopTethering(TetheringManager.TETHERING_WIFI)
                startTethering(SoftApConfiguration.BAND_5GHZ)
            }
            return
        }
        isActive = true
        // 5GHz first: it's not just for throughput -- phones refuse to start
        // wireless-AA projection over a 2.4GHz link (observed live: the phone
        // joined the 2.4GHz AP, completed SSL/service discovery/channel opens
        // over TCP, then went silent and dropped the connection ~5s in,
        // every time). 5GHz needs a resolved regulatory domain, which is why
        // this used to fail with "Failed to set country code, required for
        // setting up soft ap in band: 2" -- fixed at the source by replacing
        // ro.boot.wificountrycode=00 (world mode) with a real ISO code in
        // device/brcm/rpi4/vendor.prop. The 2.4GHz fallback below keeps a
        // usable (if phone-dependent) AP even if 5GHz ever fails again.
        startTethering(SoftApConfiguration.BAND_5GHZ)
    }

    private fun buildConfig(band: Int) = SoftApConfiguration.Builder()
        .setSsid(HOTSPOT_SSID)
        .setPassphrase(HOTSPOT_PASSPHRASE, SoftApConfiguration.SECURITY_TYPE_WPA2_PSK)
        .apply {
            if (band == SoftApConfiguration.BAND_5GHZ) {
                // Pin channel 36 (5180MHz) instead of letting the framework
                // auto-select: auto-selection on this driver picked channel
                // 34 (5170MHz), a legacy channel brcmfmac can't beacon on --
                // hostapd died with "HT40 channel pair (34, 38) not allowed"
                // then "Failed to set beacon parameters", AP-DISABLED
                // (observed live 2026-07-10). 5180 is proven good on this
                // exact radio: `cmd wifi start-softap ... -f 5180` came up
                // AP-ENABLED at 80MHz/11ac, and the same chip STA-connects
                // to 5180MHz networks.
                setChannel(36, SoftApConfiguration.BAND_5GHZ)
            } else {
                setBand(band)
            }
        }
        // The BSSID is reported to the phone once, over BT, at handshake
        // time -- it must not change under us afterwards.
        .setMacRandomizationSetting(SoftApConfiguration.RANDOMIZATION_NONE)
        // This class fully owns the AP's lifetime (every session/handshake
        // teardown path ends in stop()). The framework's own idle shutdown
        // (10 min with no clients) would disable the AP behind our back
        // while isActive stays true -- after which every start() no-ops
        // against an AP that no longer exists and wireless AA is dead until
        // the service restarts.
        .setAutoShutdownEnabled(false)
        .build()

    private fun startTethering(band: Int) {
        requestedBand = band
        lastTetherAttemptMs = SystemClock.elapsedRealtime()

        val cb = object : WifiManager.SoftApCallback {
            override fun onStateChanged(state: SoftApState) {
                Log.i(TAG, "AP state=${state.state} iface=${state.iface}")
                iface = state.iface
                if (state.state != WifiManager.WIFI_AP_STATE_ENABLED) {
                    bssid = null
                    resolvedIp = null
                }
                if (state.state == WifiManager.WIFI_AP_STATE_FAILED) {
                    fallbackTo2GhzIfNeeded(band, "AP state FAILED")
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
                // frequency makes the 5GHz-vs-2.4GHz outcome visible in
                // logcat without digging through SoftApManager's own logs.
                Log.i(TAG, "AP info: iface=$iface bssid=$b freq=${softApInfo.frequency}MHz")
                bssid = b
            }
        }
        callback?.let { wifiManager.unregisterSoftApCallback(it) }
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
            .setSoftApConfiguration(buildConfig(band))
            // No carrier/entitlement setup exists on this device -- this is a
            // private, always-should-work AP for a directly-attached phone,
            // not carrier-metered tethering.
            .setExemptFromEntitlementCheck(true)
            .setShouldShowEntitlementUi(false)
            .build()
        tetheringManager.startTethering(request, executor, object : TetheringManager.StartTetheringCallback {
            override fun onTetheringStarted() {
                Log.i(TAG, "TetheringManager: onTetheringStarted (band=$band)")
            }

            override fun onTetheringFailed(error: Int) {
                Log.e(TAG, "TetheringManager: onTetheringFailed error=$error")
                fallbackTo2GhzIfNeeded(band, "tethering error=$error")
            }
        })
    }

    // Last-resort degradation: a 2.4GHz AP is known to make (at least some)
    // phones abort wireless-AA projection right after connecting, but it is
    // still strictly better than no AP at all -- and it was the only band
    // this device could serve before the country-code fix, so keep it as a
    // safety net against regulatory-domain regressions.
    private fun fallbackTo2GhzIfNeeded(failedBand: Int, reason: String) {
        if (!isActive) return
        // A single attempt can fail through two callbacks (SoftApCallback's
        // AP state FAILED and StartTetheringCallback's onTetheringFailed),
        // and the earlier band's failure can arrive after the fallback has
        // already moved requestedBand on -- acting on such a stale report
        // would wrongly condemn the in-flight attempt.
        if (failedBand != requestedBand) return
        if (requestedBand == SoftApConfiguration.BAND_2GHZ) {
            // Both bands have now failed -- typically because this attempt
            // raced a still-in-flight stopTethering (tethering error=5,
            // observed live 2026-07-10). Nothing further will happen on its
            // own, so mark the attempt clock as expired: the phone's next
            // RFCOMM reconnect calls start(), whose wedge check then
            // restarts tethering immediately instead of sitting out the
            // full AP_WEDGE_RESTART_MS window.
            Log.e(TAG, "2.4GHz fallback failed too ($reason), letting the next start() restart tethering")
            lastTetherAttemptMs = 0L
            return
        }
        Log.e(TAG, "5GHz AP failed ($reason), falling back to 2.4GHz")
        tetheringManager.stopTethering(TetheringManager.TETHERING_WIFI)
        startTethering(SoftApConfiguration.BAND_2GHZ)
    }

    fun stop() {
        // Idempotent, and a no-op when start() never ran -- callers use
        // stop() as a safety net (e.g. AaSdkBtWirelessHandshake.stop()), and
        // an AP that was never started must not trigger a system-wide
        // stopTethering (observed live 2026-07-10: the decline retry storm
        // called this ~15x/second, spamming TetheringManager the whole time).
        if (!isActive) return
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
            // Dead-end marker from fallbackTo2GhzIfNeeded: both bands have
            // failed and nothing more will happen on its own -- burning the
            // rest of this budget just delays the reconnect whose start()
            // performs the restart.
            if (lastTetherAttemptMs == 0L) return null
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
