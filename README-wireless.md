# Wireless (WiFi) Android Auto for the RPi4 head unit

Adds a **wireless** projection path on top of the wired head unit documented
in [README.md](README.md) -- read that first. Once a phone has
been Bluetooth-paired with the device (e.g. from a prior wired session, or
plain OS-level BT pairing), it can launch Android Auto over WiFi with no
cable, by connecting to a WiFi access point **hosted by the Pi4 itself**
(not a shared home/car WiFi network).

**This is not a separate/standalone project.** It's built entirely on top of
the wired base: the same `AaSdkUsbBridge` app, the same `aasdk` native
session/protocol stack, the same device build. Wireless just adds a second
way to get bytes from the phone to that same stack (a TCP socket instead of
a USB accessory fd) plus the Bluetooth/WiFi machinery to negotiate it.

Confirmed working end-to-end on real hardware: BT handshake -> SoftAP up ->
TCP session -> SSL auth -> service discovery -> video decoding and rendering
on the car screen.

## Status

**Working:** BT RFCOMM handshake -> self-hosted WiFi AP -> TCP session ->
SSL handshake -> service discovery -> video/audio/sensor channels, all the
way to a rendered Android Auto screen -- the same rendering path as the
wired README, since it's the same native session/video/input stack
underneath, including its remaining known issue (black screen on the very
first boot) and its fixes (touch input, transport-dead recovery, audio
stutter on the 3.5mm jack -- see [README.md](README.md#status)). On top of
that, wireless-specific
robustness: re-entering the HU screen after backgrounding now reliably
forces a fresh BT/AP/TCP session instead of leaving a silently dead one
(see the debugging log).

**Known quirks (not currently blocking):**
- Stage 5/5 of the BT handshake (`WifiConnectStatus`) frequently times out
  waiting for the phone, even on runs where the session goes on to work
  completely -- not investigated further, doesn't seem to matter.
- Enabling the AP drops any existing WiFi station connection (e.g.
  `adb`-over-WiFi): the Pi4's `brcmfmac` WiFi chip is single-radio, this is
  a hardware limit, not a bug.

Full protocol diagrams, a breakdown of every new component, and the
debugging log of every bug found and fixed getting this to actually render
video on real hardware: **[docs/wireless-android-auto.md](docs/wireless-android-auto.md)**.

## What's new here, on top of the wired base

- **`app/src/main/java/com/android/car/aasdk/AaSdkBtWirelessHandshake.kt`** --
  a Bluetooth RFCOMM server that hands a BT-paired phone the WiFi
  credentials it needs to join this head unit's AP, using the same 5-stage
  handshake real Android Auto Wireless head units use.
- **`app/src/main/java/com/android/car/aasdk/AaSdkSoftApHotspot.kt`** -- hosts
  that WiFi AP itself, via `TetheringManager` (not the simpler
  `WifiManager.startTetheredHotspot()`, which turns out not to provision an
  IP for the interface -- see the debugging log).
- **A TCP transport wired into the existing native session stack**
  (`aasdk/session/src/AndroidAutoSession.cpp`'s `createAndroidAutoSessionTcp`),
  sharing everything (SSL, crypto, messenger, all the service channels, the
  `AndroidAutoEntity` state machine) with the USB path -- they differ only in
  how the underlying transport is constructed.
- **Two protocol-correctness fixes** in `aasdk` that wired-only sessions
  never exercised, needed for a wireless session to actually stay connected
  long enough to render anything:
  - Wireless Android Auto pings in *both* directions (the phone pings the
    head unit too, not just the reverse); the upstream `aasdk` library had
    no handling for an inbound ping request at all.
  - `VideoFocusIndication.unrequested` was mislabeled for the proactive
    post-setup focus grant, which the phone appears to silently ignore.
- **`VideoBlitter.kt`** -- decodes into a permanent `SurfaceTexture` and
  blits each frame onto whatever on-screen `Surface` is currently available,
  so the decoder's target survives the head-unit screen being exited and
  reentered without losing decode state. Not wireless-specific, but part of
  the same change set and needed either way for the screen to stay usable
  across a reconnect.
- **Forced session reset on screen re-entry** -- backgrounding the HU
  screen during a wireless session could leave it silently dead (socket
  `ESTABLISHED`, phone never responds again). `AaSdkScreenActivity` now
  force-closes and `AaSdkBtWirelessHandshake.forceDisconnect()` tears down
  the BT link and SoftAP together on a genuine re-entry (wireless only --
  USB sessions are untouched), which is what actually gets the phone to
  redo its handshake. See the debugging log for why each of the three
  layered fixes here was needed.
- **A native transport-watchdog** (`AndroidAutoEntity`, shared with the
  wired path) that stops a session that's gone fatally silent -- repeated
  SSL errors, no ping response for ~8s -- instead of freezing on the last
  frame forever with no detach signal to react to.

## Requirements

- The phone must already be Bluetooth-paired with the device before it will
  discover the RFCOMM service this exposes -- nothing here initiates BT
  pairing itself.
- Single-radio WiFi hardware (like the Pi4's `brcmfmac`) means hosting the AP
  and staying connected as a WiFi station at the same time isn't possible.

## Repo layout additions

Same [full-copy directories](README.md#repo-layout)
as the wired base (`aasdk/`, `app/`) -- this feature's files live inside
those, no new top-level directories. Two extra files, one under
`device-patches/` and one under `platform-patches/`:

| In this repo | Applies to / installs at |
|---|---|
| `device-patches/permissions/default-permissions-com.android.car.aasdk.xml` | `device/brcm/rpi4/permissions/default-permissions-com.android.car.aasdk.xml` (new file) -- grants `BLUETOOTH_ADVERTISE`, needed to register the RFCOMM SDP record |
| `platform-patches/MobileConnectionRepositoryImpl.kt.patch` | `git apply` inside `frameworks/base/`, patches `packages/SystemUI/.../MobileConnectionRepositoryImpl.kt` -- without it, SystemUI crashes as soon as the phone's BT-HFP connection registers a telephony subscription on this modem-less build. See [docs/systemui-bluetooth-crash-fix.md](docs/systemui-bluetooth-crash-fix.md) |

Permissions added beyond the wired base (`NETWORK_SETTINGS`/`TETHER_PRIVILEGED`
for the SoftAP, `BLUETOOTH_ADVERTISE` for the RFCOMM SDP record, plain WiFi/
INTERNET access) are broken down in
[docs/wireless-android-auto.md](docs/wireless-android-auto.md#permissions-added).

## Building

This feature's files are part of the same `aasdk/`, `app/`, and
`device-patches/` copies described in [README.md's Building
section](README.md#building) -- follow that procedure, plus one extra
patch this feature needs that the wired base doesn't:

```
cd <AOSP_ROOT>/frameworks/base
git apply <THIS_REPO>/platform-patches/MobileConnectionRepositoryImpl.kt.patch
```

## License

Same as the wired base: [f1xpl/aasdk](https://github.com/f1xpl/aasdk) is
GPLv3, and this is a derivative work built on it.
