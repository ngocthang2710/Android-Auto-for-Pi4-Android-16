# Wireless AA stabilization: from "connects once, then dies" to stable 5GHz projection

Wireless Android Auto (see [../README-wireless.md](../README-wireless.md))
could complete its whole bring-up — BT handshake, SoftAP, TCP, SSL, video on
screen — and then reliably fall apart: the phone aborted projection ~5-20s in,
reconnect attempts spiraled into storms or wedged permanently, and one native
crash could kill wireless until reboot. This document describes the full patch
series that took it to a stable session, in dependency order. Every fix below
was root-caused against live logs/btsnoop on the real Pi4 + phone, and the
final result is **confirmed working: a wireless AA session on a 5GHz AP that
survives video + audio + touch simultaneously.**

![Stable wireless Android Auto session on the Pi4: Spotify playing alongside
Google Maps in split-screen, no cable attached](screenshot-wireless-stable.png)

*Captured live from the Pi4 (`adb exec-out screencap`) during a wireless
session after this patch series: music + maps rendering simultaneously —
exactly the split-video-plus-audio load that used to kill every session at
the ~20s mark (section 2).*

All commits land on the `aa_wireless` branch.

---

## Complete file inventory

Every file this patch series changes, with its exact location in the AOSP-16
build tree (paths are relative to the tree root — on the reference machine
that root is `/home/thangnn/Android-16/source/`) and where it lives in this
repo. `app/` and `aasdk/` are full-source mirrors (copy the whole directory
over); `device-patches/` and `platform-patches/` are `git apply`-style diffs
against the pristine tree.

### Changed in this series

| # | AOSP tree path | Repo path | Fix |
|---|---|---|---|
| 1 | `hardware/broadcom/wlan/bcmdhd/wifi_hal/wifi_hal.cpp` | `platform-patches/wifi_hal.cpp.patch` | §1b `REQ_SET_REG` fallback for brcmfmac country code |
| 2 | `device/brcm/rpi4/vendor.prop` | `device-patches/vendor.prop.patch` | §1a `ro.boot.wificountrycode` `00` → `VN` |
| 3 | `device/brcm/rpi4/aosp_rpi4_car.mk` | `device-patches/aosp_rpi4_car.mk.patch` | §1c `bluetooth.profile.map.client.enabled=false` (patch also carries the earlier wired-era product changes) |
| 4 | `external/aasdk/src/Messenger/MessageInStream.cpp` | `aasdk/src/Messenger/MessageInStream.cpp` | §2 per-channel frame reassembly (the freeze) |
| 5 | `external/aasdk/include/f1x/aasdk/Messenger/MessageInStream.hpp` | `aasdk/include/f1x/aasdk/Messenger/MessageInStream.hpp` | §2 members for per-channel reassembly |
| 6 | `external/aasdk/src/Messenger/Cryptor.cpp` | `aasdk/src/Messenger/Cryptor.cpp` | §3 `SSL_write(nullptr)` guards; per-frame log removal |
| 7 | `packages/apps/AaSdkUsbBridge/src/main/java/com/android/car/aasdk/AaSdkBtWirelessHandshake.kt` | `app/src/main/java/com/android/car/aasdk/AaSdkBtWirelessHandshake.kt` | §5.1 parked declines; §5.2 AP teardown ownership |
| 8 | `packages/apps/AaSdkUsbBridge/src/main/java/com/android/car/aasdk/AaSdkSoftApHotspot.kt` | `app/src/main/java/com/android/car/aasdk/AaSdkSoftApHotspot.kt` | §1c pinned channel 36; §5.3 wedge self-heal; §5.4 band-tagged failure callbacks |
| 9 | `packages/apps/AaSdkUsbBridge/src/main/java/com/android/car/aasdk/AaSdkUsbService.kt` | `app/src/main/java/com/android/car/aasdk/AaSdkUsbService.kt` | §4 `START_STICKY` + self-promotion; §5.5 re-entry reset gating |
| 10 | `external/aasdk/session/src/AVInputService.cpp` | `aasdk/session/src/AVInputService.cpp` | §6 channel receive re-arm |
| 11 | `external/aasdk/session/src/AudioService.cpp` | `aasdk/session/src/AudioService.cpp` | §6 channel receive re-arm |
| 12 | `external/aasdk/session/src/BluetoothService.cpp` | `aasdk/session/src/BluetoothService.cpp` | §6 channel receive re-arm |
| 13 | `external/aasdk/session/src/InputService.cpp` | `aasdk/session/src/InputService.cpp` | §6 channel receive re-arm |
| 14 | `external/aasdk/session/src/SensorService.cpp` | `aasdk/session/src/SensorService.cpp` | §6 channel receive re-arm |
| 15 | `external/aasdk/aasdk_proto/TouchEventData.proto` | `aasdk/aasdk_proto/TouchEventData.proto` | §6 proto3 `optional` on `action_index` |
| 16 | `external/aasdk/aasdk_proto/TouchLocationData.proto` | `aasdk/aasdk_proto/TouchLocationData.proto` | §6 proto3 `optional` on `x`/`y`/`pointer_id` |

### The rest of the project's tree footprint (earlier commits, unchanged here)

Listed so the full set of files this project touches in the AOSP tree is in
one place — nothing outside these paths (plus the two mirror directories) is
modified:

| AOSP tree path | Repo path | Purpose |
|---|---|---|
| `external/aasdk/` (entire directory: `Android.bp`, `aasdk_proto/`, `include/`, `src/`, `session/`, `jni/`) | `aasdk/` | the ported aasdk library + native session layer (new directory, not in stock AOSP) |
| `packages/apps/AaSdkUsbBridge/` (entire directory: `Android.bp`, `AndroidManifest.xml`, `com.android.car.aasdk.xml`, all `*.kt`) | `app/` | the head-unit app: `AaSdkUsbService`, `AaSdkScreenActivity`, `AaSdkBtWirelessHandshake`, `AaSdkSoftApHotspot`, `AaSdkWirelessConfirmActivity`, `AaSdkBootReceiver`, `UsbAttachActivity`, `AaSdkAoapService`, `VideoBlitter` (new directory) |
| `device/brcm/rpi4/sepolicy/platform_app.te` | `device-patches/sepolicy_platform_app.te.patch` | allow `platform_app` `r_dir_file` on `usb_device` for libusb |
| `device/brcm/rpi4/permissions/Android.bp` | `device-patches/permissions/Android.bp` | registers the two default-permission XMLs below |
| `device/brcm/rpi4/permissions/default-permissions-com.android.car.aasdk.xml` | `device-patches/permissions/default-permissions-com.android.car.aasdk.xml` | runtime-permission pre-grants for the app |
| `device/brcm/rpi4/permissions/default-permissions-com.android.systemui.xml` | `device-patches/permissions/default-permissions-com.android.systemui.xml` | `BLUETOOTH_CONNECT` grant — CarSystemUI crash-loops without it |
| `frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/pipeline/mobile/data/repository/prod/MobileConnectionRepositoryImpl.kt` | `platform-patches/MobileConnectionRepositoryImpl.kt.patch` | SystemUI crash on BT-HFP telephony subscription (modem-less build) |
| `frameworks/base/services/core/java/com/android/server/net/NetworkPolicyManagerService.java` | `platform-patches/NetworkPolicyManagerService.java.patch` | `system_server` boot crash-loop on missing telephony |

Note on `device/brcm/rpi4` and `frameworks/base`: in the reference tree these
changes exist partly as local git commits inside those subtrees, so a plain
`git status` there does **not** show everything — the patch files above are
generated against the pristine upstream base
(`git diff <upstream-commit> -- <file>`), and contain the complete delta.

---

## 1. The 5GHz chain (three stacked blockers)

Phones (GearHead) complete the entire wireless handshake on a 2.4GHz AP but
abort projection seconds after the channels open. A 5GHz AP is effectively
mandatory — and on this device THREE independent layers each prevented 5GHz:

### 1a. Regulatory domain was world mode (`00`)

`device/brcm/rpi4/vendor.prop` shipped `ro.boot.wificountrycode=00`. In world
mode the WiFi framework refuses any 5GHz SoftAP ("Failed to set country
code, required for setting up soft ap in band: 2").

**Fix** (`device-patches/vendor.prop.patch`): set a real ISO code
(`ro.boot.wificountrycode=VN`). This must be flashed with the **vendor**
image — flashing system alone silently keeps world mode.

### 1b. The vendor WiFi HAL cannot set the country code on brcmfmac

Even with the prop fixed, `SoftApManager` still aborted with the same error:
`WifiNative.setApCountryCode()` calls the legacy Broadcom HAL
(`libwifi-hal-bcm`), whose `wifi_set_country_code()` sends a `GOOGLE_OUI`
nl80211 **vendor command that the upstream `brcmfmac` driver rejects**
(`EOPNOTSUPP` — the same reason this HAL logs "Failed to get driver version:
NOT_SUPPORTED" on every boot). The driver itself was demonstrably 5GHz-ready
(`iw reg get` showed VN with IR-enabled 5GHz channels).

**Fix** (`platform-patches/wifi_hal.cpp.patch`, for
`hardware/broadcom/wlan/bcmdhd/wifi_hal/wifi_hal.cpp`): when the vendor
command fails, fall back to the standard nl80211 regulatory hint
(`NL80211_CMD_REQ_SET_REG` + `NL80211_ATTR_REG_ALPHA2` — exactly what
`iw reg set` issues). Verified live: `Vendor set-country-code failed (-3),
retrying as REQ_SET_REG(VN)` → `result: 0`. Note for future debugging: this
code lives in the **shared library** `libwifi-hal.so` inside the
`com.android.hardware.wifi` vendor APEX, not in the
`android.hardware.wifi-service` binary.

### 1c. Automatic channel selection picks channel 34, which brcmfmac can't beacon on

Past the country check, hostapd was handed **channel 34 (5170MHz)** — the
first entry of the driver's frequency list, a legacy channel — and died with
`HT40 channel pair (34, 38) not allowed` → `Failed to set beacon parameters`
→ `AP-DISABLED`.

**Fix** (`app/.../AaSdkSoftApHotspot.kt`): pin the 5GHz attempt to **channel
36 (5180MHz)** via `SoftApConfiguration.Builder.setChannel(36, BAND_5GHZ)`
instead of `setBand()`. Channel 36 was proven good on this exact radio
(`cmd wifi start-softap ... -f 5180` → AP-ENABLED, 80MHz, 11ac). The 2.4GHz
fallback path still uses band auto-selection and remains as a safety net.

Also in the same file: `bluetooth.profile.map.client.enabled=false`
(`device-patches/aosp_rpi4_car.mk.patch`) — the device has no
`telephony.messaging`, so the MAP client profile crash-looped
`com.android.bluetooth` (44+ crashes per logcat buffer), and each crash killed
every RFCOMM socket including the wireless handshake listener.

---

## 2. The freeze: aasdk cannot parse interleaved channel frames (the deepest bug)

With 5GHz up, every session still **froze ~20s in**: video mid-stream (16KB
split frames), audio playing, pings healthy in both directions — then every
channel errored simultaneously with aasdk error 29
(`MESSENGER_INTERTWINED_CHANNELS`), and the watchdog tore the session down
10s later. The screen stayed on the last decoded frame: the "freeze".

**Root cause** (`aasdk/src/Messenger/MessageInStream.cpp`): upstream
f1x/aasdk keeps **one** message-reassembly buffer and hard-rejects any frame
whose channel differs from the message currently being reassembled. But a
message larger than 16KB is split into FIRST/MIDDLE/LAST frames, and GearHead
over wireless TCP **legally interleaves other channels' frames** (audio,
sensor) between them. The moment heavy video coincided with audio, the whole
messenger died. USB transfers rarely interleave — which is why the wired path
never tripped this, and why the bug looked like a "wireless problem".

**Fix** (`aasdk/src/Messenger/MessageInStream.{cpp,hpp}`): per-channel
reassembly — a `map<ChannelId, Message>` of in-progress messages plus a
pointer to the message the frame currently on the wire belongs to.
FIRST/BULK opens (and replaces) that channel's entry, MIDDLE/LAST appends to
it, and an orphan MIDDLE/LAST is salvaged as a fresh message instead of
killing the stream. TLS decryption order is unaffected: interleaving only
happens at frame boundaries and each frame is decrypted on arrival.

---

## 3. Native crash: `SSL_write(nullptr)` SIGSEGV on session teardown

A queued send racing session teardown crashed the whole app process:
`Cryptor::encrypt` → `SSL_write` on a NULL `SSL*`, because `Cryptor::deinit()`
had already freed it (and never reset `isActive_`).

**Fix** (`aasdk/src/Messenger/Cryptor.cpp`): `deinit()` resets `isActive_`;
`encrypt/decrypt/doHandshake/readHandshakeBuffer/writeHandshakeBuffer` throw a
normal `error::Error` when `ssl_ == nullptr` — both `MessageOutStream` send
paths already catch it, so the race now ends in a clean channel error instead
of SIGSEGV. Per-frame encrypt/decrypt logging was also removed (60-120
lines/s during video: wasted Pi4 CPU and rotated the logcat buffer within
minutes, repeatedly destroying debugging evidence).

## 4. Service lifecycle: one crash used to kill wireless until reboot

After the crash above, Android restored only the top Activity; its
`bindService(BIND_AUTO_CREATE)` recreated `AaSdkUsbService` **bound-only**, so
the moment that Activity finished, the system destroyed the service and both
listeners (RFCOMM + TCP) with it. Nothing was left listening — wireless AA
was dead until reboot.

**Fix** (`app/.../AaSdkUsbService.kt`): `onStartCommand` returns
`START_STICKY` (was `START_NOT_STICKY`, and bailed on a null intent),
re-asserts `startForeground()` on every start, and `onCreate` self-promotes
the service to started state via `startForegroundService(self)` — so it
survives its last unbind no matter how it was created, and the system
restarts it after a process death.

## 5. Reconnect-flow fixes (storms, wedges, livelocks)

Five distinct failure loops in the BT-handshake ↔ SoftAP dance, all observed
live:

1. **Decline retry storm** (`AaSdkBtWirelessHandshake.kt`): closing a
   declined RFCOMM connection makes the phone reconnect within ~50-100ms —
   an accept/decline/close storm at ~15Hz for as long as a sticky decline
   held. Declined connections are now **parked** (held open, draining) so
   the phone waits quietly; the park ends on ACL drop, USB attach/detach, or
   the user reopening the app (`clearWirelessDecline()` now cuts a parked
   link via `disconnectIfParked()` so a fresh prompt appears immediately).

2. **AP torn down by the connection that started it**
   (`AaSdkBtWirelessHandshake.kt`): a 5GHz bring-up takes ~8-9s (hostapd HT
   scan) but the phone only waits ~5-8s on a silent RFCOMM link — so the
   connection that starts the AP often dies ("Broken pipe") right as the AP
   becomes ready, and its cleanup used to stop the hotspot, feeding a
   livelock (stop racing the reconnect's start → tethering error=5 on both
   bands → dead end → repeat every ~34s with zero completed handshakes).
   Now only a connection whose **handshake completed (stage 5/5)** owns AP
   teardown; failed/aborted/declined handshakes leave the AP warming, so the
   phone's instant retry finds it ready and gets its `WifiStartRequest`
   within milliseconds. Verified live: retry completed the full handshake
   82ms after a Broken pipe.

3. **Permanent wedge: `isActive` true with no AP**
   (`AaSdkSoftApHotspot.kt`): a stop/start race could fail both bands and
   leave the hotspot object believing an AP existed — every later `start()`
   no-oped, every handshake timed out, forever (wlan0 sat in STA mode).
   `start()` now self-heals: if the AP still isn't broadcasting 15s after
   the last tethering attempt, restart tethering from scratch. Additionally
   `SoftApConfiguration.setAutoShutdownEnabled(false)` stops the framework's
   10-min idle shutdown from disabling the AP behind the app's back.

4. **Stale failure callbacks condemning the wrong attempt**
   (`AaSdkSoftApHotspot.kt`): one failed attempt reports through two
   callbacks, and a late 5GHz failure could arrive after the 2.4GHz fallback
   was already in flight. Failure handling is now band-tagged and ignores
   reports for a band that is no longer the current attempt. A true
   both-bands dead end marks the attempt clock expired so the next
   reconnect heals instantly instead of waiting out the full window
   (`waitUntilReady` also bails early on that marker).

5. **Reopening the app killed live handshakes**
   (`AaSdkUsbService.kt`): `resetWirelessSessionForReentry()` used to fire on
   every AA-screen re-entry with no USB attached — including when no session
   existed — force-closing whatever the handshake thread was doing (observed
   killing a just-approved handshake mid-flight). It is now gated on
   `wirelessTcpSessionActive`, a flag tracking whether a wireless TCP session
   is actually connected.

## 6. Older unpushed correctness fixes included in this series

- **Channel receive re-arm** (`aasdk/session/src/{AVInput,Audio,Bluetooth,
  Input,Sensor}Service.cpp`): `onChannelError` never re-called `receive()`,
  so one transient error deafened a channel forever (session alive, sends OK,
  nothing received again). Each service now re-arms its receive after logging
  the error.
- **proto3 zero-omission on touch fields**
  (`aasdk/aasdk_proto/Touch{Event,Location}Data.proto`): plain proto3 fields
  omit zero values from the wire; `action_index`, `x`, `y` and `pointer_id`
  are all legitimately 0 (single pointer, top/left-edge taps), so they are
  marked `optional` to force explicit presence, like `touch_action` before
  them.

---

## Build & flash

```bash
lunch aosp_rpi4_car bp4a userdebug   # 3-arg form
m systemimage vendorimage
```

| Change | Image | SD partition |
|---|---|---|
| App + aasdk (APK embeds `libaasdk_jni.so`) | `system.img` | `sda5` (label `_`) |
| `vendor.prop`, `aosp_rpi4_car.mk` props, WiFi HAL | `vendor.img` | `sda6` (label `vendor`) |

Both images are raw ext4 — `dd` them directly. **Flash both**: several fixes
here were invisible for a full test cycle because only `system.img` had been
flashed while the country-code and HAL fixes sat in an unflashed
`vendor.img`.

Post-flash verification checklist:

- `getprop ro.boot.wificountrycode` → `VN`
- logcat on wireless connect: `REQ_SET_REG(VN) result: 0`, then
  `AaSdk_SoftAp: AP info: ... freq=5180MHz`
- no `MceStateMachine` crash loops
- a wireless session that keeps running past 20s with video + audio + touch
- wired USB AA still works (it shares the service, session slot and teardown
  paths with everything above)
