# Wireless Android Auto (WiFi projection)

Adds a **wireless** projection path on top of the existing wired (USB/AOAP)
head unit: once a phone has been Bluetooth-paired with the device, it can
launch Android Auto over WiFi with no cable, by connecting to a WiFi access
point **hosted by the Pi4 itself** (not a shared home/car WiFi network).

Confirmed working end-to-end on real hardware: BT handshake → SoftAP up →
TCP session → SSL auth → service discovery → video decoding and rendering on
the car screen.

## Why a self-hosted AP, not a shared network

Real "Wireless Android Auto" needs *some* WiFi link between phone and head
unit. Two designs exist in the wild:

1. Head unit joins the *same* WiFi network the phone is already on (home
   WiFi, a mobile hotspot, etc.) -- what this project's code did in an
   earlier iteration (read `WifiManager.connectionInfo` in STA mode).
2. Head unit hosts its **own** dedicated WiFi AP and tells the phone to join
   *that* -- what real in-car wireless AA head units, and DIY projects like
   `aa-proxy-rs`/AAWireless dongles, actually do. No dependency on a shared
   network existing or being reachable from both devices, works standalone
   in a car with no other WiFi around.

This project now does (2). The Pi4's `brcmfmac` WiFi chip is single-radio,
so hosting an AP means it cannot simultaneously stay connected as a WiFi
client (e.g. for `adb`-over-WiFi) -- an inherent hardware tradeoff, not a
bug.

## End-to-end flow

```
Phone (already BT-paired)                Pi4 head unit
──────────────────────────               ──────────────────────────
                                          AaSdkUsbService.onCreate():
                                            - TCP accept thread on :5288
                                            - RFCOMM server on a fixed UUID
BT RFCOMM connect ──────────────────────▶  AaSdkBtWirelessHandshake accepts
                                            AaSdkSoftApHotspot.start()
                                            (TetheringManager brings up the
                                             AP; polls for a real gateway IP)
◀── WifiStartRequest(HU ip, port=5288) ──  stage 1/5
── WifiInfoRequest ──────────────────────▶  stage 2/5
◀── WifiInfoResponse(ssid, psk, bssid) ──  stage 3/5
── WifiStartResponse ────────────────────▶  stage 4/5
(joins the AP as a WiFi client)
(BT link kept open, unparsed, as a
 keepalive the phone expects)
TCP connect to HU ip:5288 ──────────────▶  accept() → nativeOnTcpAccepted
                                            createAndroidAutoSessionTcp():
                                            same session/entity stack as USB
── VersionRequest ⇄ VersionResponse ────▶  (plaintext)
── SSL handshake ⇄ SSL handshake ───────▶  Cryptor (BoringSSL)
── AuthComplete ─────────────────────────▶
── ServiceDiscoveryRequest ─────────────▶
◀── ServiceDiscoveryResponse ────────────  video/audio/sensor/input channels
── ChannelOpenRequest (each channel) ───▶
◀── ChannelOpenResponse ─────────────────
── AVChannelSetupRequest (video) ───────▶
◀── AVChannelSetupResponse ──────────────
◀── VideoFocusIndication(unrequested) ──  proactive grant, phone starts
                                          encoding
── AVChannelStartIndication ────────────▶
── AVMediaIndication (H.264 NALs) ──────▶  AndroidVideoOutput (AMediaCodec)
                                          → VideoBlitter → on-screen Surface
── PingRequest (phone → HU) ────────────▶  ◀── PingResponse (HU → phone)
◀── PingRequest (HU → phone, Pinger) ────  ── PingResponse ──▶
```

## Components

### `AaSdkBtWirelessHandshake.kt` -- the discovery/handoff mechanism

A Bluetooth RFCOMM server registered on the same service UUID
(`4de17a00-52cb-11e6-bdf4-0800200c9a66`) real Android Auto Wireless head
units use (cross-checked against independent reverse-engineering write-ups:
`aa-proxy-rs`, `headunit-revived` -- there's no public official spec for this
handshake). The phone's own Android Auto app discovers this UUID on a
paired device and connects to it on its own; nothing in this codebase
initiates BT pairing itself.

Canonical 5-stage exchange once a phone connects (message IDs and field
layout match `aa-proxy-rs`'s own `ProxyMessageId` enum):

1. HU → `WifiStartRequest` (HU's IP on the AP subnet, TCP port 5288)
2. phone → `WifiInfoRequest`
3. HU → `WifiInfoResponse` (SSID, WPA2 passphrase, BSSID, security/AP type)
4. phone → `WifiStartResponse`
5. phone → `WifiConnectStatus` (not always observed within the read timeout
   in practice -- see [Known quirks](#known-quirks-not-bugs))

Messages are a minimal hand-rolled 4-byte-header + protobuf-lite wire
format (`ProtoWriter` in the same file) -- a full protobuf-lite dependency
wasn't justified for the two message types this class sends.

After stage 4, the BT socket is **not** closed: the phone keeps it open and
expects the head unit to too (for `WifiPingRequest`/keepalive traffic per the
`ProxyMessageId` enum, though this code doesn't parse those frames, just
avoids hanging up first). The real Android Auto session by this point lives
entirely on the separate TCP socket; the BT link ending is what this code
uses as the signal to tear the AP back down (see below).

### `AaSdkSoftApHotspot.kt` -- the self-hosted WiFi AP

Hosts a fixed-configuration AP (SSID `AndroidAuto-HU`, WPA2-PSK passphrase
`aasdkwireless`, 2.4GHz, MAC randomization disabled so the BSSID handed to
the phone over BT stays valid) via `TetheringManager.startTethering()`.

Three non-obvious things this class has to get right, each one a bug found
by testing on real hardware (see [Debugging log](#debugging-log-what-broke-and-why)):

- **Must go through `TetheringManager`, not `WifiManager.startTetheredHotspot()`
  directly.** The latter only brings up the SoftAP radio (hostapd); it does
  *not* provision the interface's IP/DHCP. Only `TetheringManager` drives the
  `IpServer`/`dnsmasq` side that actually gives the AP interface a usable
  gateway IP.
- **Must poll for the gateway IP, not trust it being present the instant the
  AP reports itself enabled.** `SoftApCallback.onInfoChanged()` can fire with
  a real BSSID while `wlan0`'s IPv4 address is still `0.0.0.0` -- IP
  provisioning is a separate async step slightly behind AP-enabled. So
  `waitUntilReady()` polls `NetworkInterface.getByName("wlan0")` itself
  instead of trusting the callback's timing.
- **Must use 2.4GHz, not 5GHz**, on this specific device. 5GHz channel
  selection requires a resolved regulatory domain (for DFS); this device's
  WiFi driver reports country code `"99"` (world mode) at the exact moment
  the AP interface is recreated, so `SoftApManager` rejects 5GHz outright
  (`"Failed to set country code, required for setting up soft ap in band: 2"`)
  and the AP state goes straight to `FAILED`, forever, on every attempt.

**Lifecycle is tied to the BT connection, not the service.** `start()`/`stop()`
are called from inside `AaSdkBtWirelessHandshake.handleConnection()`
(start when a phone BT-connects, stop when that BT link ends), not from
`AaSdkUsbService.onCreate()`/`onDestroy()`. This matters because the Pi4's
WiFi radio is single-radio: switching it into AP mode unconditionally on
service startup would mean a plain wired-USB-only session (which never
touches WiFi at all) would still silently commandeer the WiFi radio away
from station mode (e.g. dropping `adb`-over-WiFi) every time a phone is
plugged in. Gating AP startup on an actual incoming BT wireless request
keeps the two paths independent.

`start()` is idempotent (guarded by an `isActive` flag) so a phone's BT
retry storm racing an AP that's already up (or still coming up) doesn't
re-issue `startTetheredHotspot`/`TetheringManager.startTethering` calls on
top of an in-flight one.

### TCP transport plumbing (`aasdk`)

`f1x/aasdk` already ships a generic `Transport` interface with both a
`USBTransport` and a `TCPTransport` implementation upstream -- only the USB
one had ever been wired up in this port. This session wires up the TCP side:

- `AaSdkUsbService.kt`: a background thread listening on `ServerSocket(5288)`
  (`WIFI_PROJECTION_PORT`, the same port real Android Auto Wireless uses).
  Each accepted connection's fd is handed to native via
  `ParcelFileDescriptor.fromSocket(socket).detachFd()` + `nativeOnTcpAccepted`.
- `aasdk_jni.cpp`'s `nativeOnTcpAccepted`: shares the *same* single-session
  slot USB uses. A new TCP accept while a session is already running means
  the previous connection is no longer wanted (the phone wouldn't be redoing
  the whole BT+WiFi handshake otherwise) -- the stale session is torn down
  and replaced rather than the new connection being rejected (rejecting used
  to leak the new fd and left the phone's new TCP connection hanging, which
  it read as a dead head unit and retried forever).
- `AndroidAutoSession.cpp`: refactored so `createAndroidAutoSession` (USB)
  and the new `createAndroidAutoSessionTcp` both call a shared
  `finishSessionSetup()` that builds the SSL/crypto/messenger/services/entity
  stack identically -- they differ only in how the underlying `Transport` is
  constructed (`USBTransport` wrapping a libusb handle vs. `TCPTransport`
  wrapping a `TCPEndpoint` around an adopted `boost::asio::ip::tcp::socket`).
- Surface caching (`pendingWindow` in both `aasdk_jni.cpp`'s `NativeContext`
  and `AndroidAutoSession.cpp`'s `sessionSetSurface`) had to become
  reapply-on-every-session rather than consume-once: a phone-initiated
  reconnect (new BT+WiFi handshake → new TCP accept → new session replacing
  the old one) can happen while `AaSdkScreenActivity` is still resumed with
  its `Surface` untouched, so Kotlin never re-fires `surfaceChanged`/
  `nativeSetSurface` for the new session. Without caching the last-known-good
  window and reapplying it to each new session explicitly, the new session's
  `AndroidVideoOutput` would never receive a window at all.

### Bidirectional ping (`ControlServiceChannel` / `AndroidAutoEntity`)

Wired Android Auto only ever has the head unit ping the phone (`Pinger`,
already existed). **Wireless Android Auto pings in both directions** -- the
phone also sends its own `PingRequest` to the head unit, which this upstream
aasdk library had no handling for at all (`IControlServiceChannelEventHandler`
had no `onPingRequest`, `IControlServiceChannel` had no `sendPingResponse`).

Without this, `ControlServiceChannel::messageHandler`'s `default:` branch
logged `"message not handled: 11"` (`0x000b` = `PING_REQUEST`) and silently
dropped it. The phone, having sent a ping and gotten no pong back, tore down
the entire TCP session roughly 2.9 seconds later -- **this was the actual
cause of the "black screen"** during bring-up, not anything in the video
pipeline (which by then hadn't even started, since the session died first).

Added across `IControlServiceChannelEventHandler.hpp` (new `onPingRequest`),
`IControlServiceChannel.hpp`/`ControlServiceChannel.hpp` (new
`sendPingResponse`), `ControlServiceChannel.cpp` (dispatch + handler,
`sendPingResponse` uses `EncryptionType::ENCRYPTED` -- confirmed via logcat
that the phone's inbound `PingRequest` arrives through the `Cryptor` decrypt
path, matching every other post-auth response), and `AndroidAutoEntity.cpp`
(`onPingRequest` echoes the request's timestamp back in the response, the
expected ping/pong semantics).

### Video focus correctness (`VideoService.cpp`)

Two protocol-alignment fixes found while debugging why the phone would open
the video channel, accept setup, but then go silent and never send
`AVChannelStartIndication`:

- `VideoFocusIndication.unrequested`: this head unit proactively pushes
  video focus right after `AVChannelSetupResponse`, without the phone having
  asked for it first via `VideoFocusRequest`. The old code always sent
  `unrequested=false` -- mislabeling an unsolicited grant as "answering a
  request that was never made". Fixed: `unrequested=true` for the proactive
  post-setup push, `unrequested=false` only for the real reply inside
  `onVideoFocusRequest`.
- `AVChannelSetupResponse.configs`: echoed the phone's requested
  `config_index` back; openauto (the reference implementation this port is
  based on) hardcodes `0` here instead. Aligned with that.

Neither was independently confirmed as *the* fix in isolation (both landed
in the same build as the ping fix); see the debugging log below for what the
logs showed before/after.

### `VideoBlitter.kt` / `AndroidVideoOutput` two-stage rendering

Not wireless-specific, but part of the same working tree and needed for the
screen to survive `AaSdkScreenActivity` being recreated (exiting to the car
launcher and coming back) without going black. The decoder's output target
used to be whatever `Surface` the currently-visible Activity provided
directly; now:

- `AndroidVideoOutput`'s `ANativeWindow` is always `VideoBlitter`'s own
  permanent `SurfaceTexture`-backed input surface, attached exactly once at
  `AaSdkUsbService` startup -- never recreated or swapped for the life of
  the service.
- `VideoBlitter` owns a single dedicated GL context/thread that blits each
  decoded frame (a textured full-screen quad) onto whichever on-screen
  `EGLSurface` `AaSdkScreenActivity` currently provides via
  `attachDisplaySurface()` -- cheaply destroyed and recreated on every
  exit/reenter, decoupled entirely from the decoder.

This exists because three more direct approaches were tried and failed on
this device's `c2.v4l2.avc.decoder` first (see `VideoBlitter`'s kdoc for the
full account): `AMediaCodec_setOutputSurface()` returns `OK` but silently
renders nothing here; destroying/recreating the codec on Activity recreate
loses decode state the phone never knows to resend (the video channel is
never reopened just because the HU's `Surface` changed, so the next P-frame
has no preceding IDR to decode against -- permanently black); and reusing
one `SurfaceTexture` directly across separate `TextureView` instances
violates that API's own attach/detach contract.

## Debugging log: what broke and why

In the order they were found and fixed, each confirmed via `adb logcat` on
real hardware against a real phone:

1. **Crash-loop on every service start**: `AaSdkSoftApHotspot`'s constructor
   called `context.getSystemService(...)` eagerly, but it's constructed from
   `AaSdkUsbService`'s own field initializer -- which runs *before* Android
   calls `attachBaseContext()`, when the base `Context` is still null.
   Fixed by making the `WifiManager`/`TetheringManager` handles `by lazy`.
2. **AP always `WIFI_AP_STATE_FAILED` on 5GHz**: country-code/regulatory
   domain issue described above. Fixed by switching to `BAND_2GHZ`.
3. **AP reports `ENABLED` with a real BSSID, but the BT handshake still
   times out waiting for it**: `WifiManager.startTetheredHotspot()` doesn't
   provision IP/DHCP -- `wlan0` stayed at `0.0.0.0` forever. Fixed by
   switching to `TetheringManager.startTethering()`.
4. **Same symptom, closer**: even via `TetheringManager`, the gateway IP
   wasn't available at the exact instant `onInfoChanged()` first fired with
   a real BSSID (provisioning is a separate async step). Fixed by polling
   `NetworkInterface` in `waitUntilReady()` instead of computing the result
   once inside the callback.
5. **AP unconditionally on for wired-only sessions**: moved `start()`/`stop()`
   out of the service lifecycle and into the BT handshake lifecycle (see
   above) -- a deliberate design correction requested mid-session, not a
   crash/hang.
6. **BT handshake completes, TCP session starts, SSL auth completes,
   service discovery completes -- then the whole session dies ~2.9s later,
   every single time, screen stays black.** Root-caused to the unhandled
   inbound `PingRequest` (`"message not handled: 11"`). Fixed as described
   above.
7. **Same ~2.9s-after-setup death persisted even after the ping fix,
   identical timing to the millisecond** -- proved the ping fix, while a
   real and worthwhile correctness fix, was not *this* disconnect's cause
   (see the cross-run timing comparison that ruled it out). Root-caused
   instead to `VideoFocusIndication.unrequested` and the
   `AVChannelSetupResponse` config-index mismatch, both fixed together in
   the next build, which is the one confirmed working end-to-end.
8. **Wireless session goes silent when the HU screen is backgrounded and
   re-entered**: socket stays `ESTABLISHED` but the phone stops responding
   to anything, including pings, leaving a black screen with no local
   error signal. Three layered fixes were needed, each confirmed necessary
   against a real phone: (a) `AaSdkScreenActivity` now force-closes the
   native session (`nativeResetSession`) on a genuine re-entry, scoped to
   wireless only (`attachedDevice == null`) so USB sessions are untouched;
   (b) closing only the TCP session wasn't enough -- the phone's own AA app
   never redid discovery because its BT link and WiFi AP both still looked
   fine, so `AaSdkBtWirelessHandshake.forceDisconnect()` now tears down the
   BT link and SoftAP together, the one thing observed to reliably make the
   phone redo its full 5-stage handshake; (c) the resulting fast reconnect
   exposed `AMediaCodec_configure()` failing with `nativeWindowConnect ...
   Invalid argument (-22)` because the previous session's codec teardown
   (bound to the same persistent `VideoBlitter` input surface) hadn't
   finished asynchronously yet -- `AndroidVideoOutput::init()` now retries
   configure with a short backoff (up to 5 attempts, 150ms apart).
9. **Touch never registered on the phone, 93/93 misses** -- not a protocol
   bug: GearHead's own window manager (CAR.WM) locks its touch window to
   `video_configs[0]`, confirmed live via phone-side logcat
   (`selectedIndex=0, codecWidth=800, codecHeight=480`, an old low-res
   tier), while `VideoService`/`AaSdkScreenActivity` render at 1920x1080 --
   every touch landed outside GearHead's actual window bounds regardless of
   what coordinates were sent. `VideoService::onAVChannelSetupRequest`
   also never indexed `video_configs` by the phone's requested
   `config_index` and always echoed back index 0. Fixed by collapsing the
   advertised config list to the one resolution actually rendered, at
   index 0. Confirmed on hardware: touch-miss errors dropped from 93/93 to
   0. Not wireless-specific -- this is the shared `VideoService`/native
   session layer, so it fixes touch for wired sessions too.
10. **Head unit freezes on the last rendered frame forever if the
    transport dies without a clean signal** -- observed with a wireless
    session going transport-dead (repeated `SSL_READ`/`SSL_WRITE` channel
    errors, zero ping responses for 58+s) but no USB detach broadcast to
    react to (there being no USB involved). `AndroidAutoEntity` now runs a
    watchdog alongside `Pinger`: after ~4 missed 2s ping intervals (8s) the
    transport is considered fatally dead and the session stops itself.
    Polled from Java every 2s via `nativeCheckFatalError`; when set,
    `AaSdkUsbService` tears down the session exactly like a real accessory
    detach. Also shared with the wired path, which can hit the same silent
    SSL-error failure mode.

## Known quirks (not bugs)

- **Stage 5/5 (`WifiConnectStatus`) frequently logs `"no inbound frame
  (timed out or closed)"`** in `AaSdkBtWirelessHandshake`, including on runs
  where the session went on to work completely. Not currently investigated
  further -- the phone evidently doesn't always send it within the 3-second
  read window this code uses, and it isn't required for the session to
  proceed.
- **Enabling the AP drops any existing WiFi station connection** (e.g.
  `adb`-over-WiFi) -- inherent to the Pi4's single-radio `brcmfmac` chip, not
  fixable in software without dual-radio hardware.

## Permissions added

Beyond what wired-only `AaSdkUsbBridge` already had:

| Permission | Why | Where granted |
|---|---|---|
| `BLUETOOTH_ADVERTISE` | `listenUsingRfcommWithServiceRecord()` registers an SDP record, needs this on top of `BLUETOOTH_CONNECT` | dangerous/runtime — `device-patches/permissions/default-permissions-com.android.car.aasdk.xml` |
| `ACCESS_WIFI_STATE` / `CHANGE_WIFI_STATE` | `WifiManager` access | normal — manifest only |
| `NETWORK_SETTINGS` | `registerSoftApCallback()` | `signature\|privileged` — `app/com.android.car.aasdk.xml` privapp allowlist |
| `TETHER_PRIVILEGED` | `TetheringManager.startTethering/stopTethering` | `signature\|privileged` — same privapp allowlist |
| `INTERNET` | Plain `ServerSocket`/`Socket` use (the wireless-projection TCP listener) requires it even for a purely local/LAN socket | normal — manifest only |

## Apply

Same procedure as [README.md's Building section](../README.md#building) --
this feature's files are part of the same `aasdk/`, `app/`, and
`device-patches/` full-copy directories -- plus one extra patch this
feature needs on top:

```
cd <AOSP_ROOT>/frameworks/base
git apply <THIS_REPO>/platform-patches/MobileConnectionRepositoryImpl.kt.patch
```

See [systemui-bluetooth-crash-fix.md](systemui-bluetooth-crash-fix.md) for why.
