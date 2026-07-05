# Android Auto (wired) head unit for Raspberry Pi 4 / AOSP Automotive

Turns a Raspberry Pi 4 running **AOSP Automotive** (`aosp_rpi4_car`) into a
wired **Android Auto head unit**: plug a phone into a USB-A port and it
projects Android Auto onto the car's screen, the same way Crankshaft/OpenAuto
do on bare Debian -- except this runs natively on Android itself, on top of
the platform's own USB/AOAP stack.

It's built around [f1xpl/aasdk](https://github.com/f1xpl/aasdk) (the AOAP
protocol core also used by Crankshaft/OpenAuto), ported to build against
bionic/Soong and to talk to Android-native video/audio/input APIs instead of
Qt/RtAudio/OMX.

## Status

**Working:**
- USB attach -> AOAP mode-switch -> SSL handshake -> service discovery ->
  video/audio/sensor channels, all the way to a rendered Android Auto screen.
- Zero-tap auto-launch: plugging the phone in is enough, no dialog to tap
  and no need to have the app already open (see [Auto-launch](#auto-launch-no-dialog)).
- Auto-return to the car's home screen when the phone is unplugged.

**Known issues (unresolved):**
- **Black screen on the very first plug-in after boot.** Traced to a
  Raspberry Pi 4 kernel-level `bcm2835-codec` V4L2 driver log
  (`buffer size mismatch sizeimage 0 < min size 691200`) that appears to
  correlate with, but was not conclusively proven to *cause*, the first
  decoder instance silently producing no output. A mitigation (`AndroidVideoOutput::warmup()`,
  a throwaway decoder cycle at service startup) is in place but was **not**
  confirmed to fix it on hardware -- unplugging and replugging always works.
- **Touch input on the head-unit screen produces no reaction on the phone.**
  Extensively investigated: the touch protocol (proto fields, `ChannelId`,
  coordinate math) is correct, and a `usbmon` packet capture confirmed the
  touch event bytes are sent correctly and accepted by the phone at the USB
  level. GearHead (closed-source) acknowledges touch *capability* at connect
  time but never logs any per-tap reaction. Root cause is believed to be
  phone/GearHead-side, not conclusively diagnosable without a known-working
  reference head unit (e.g. real OpenAuto) to compare against.

## Repo layout

This mirrors four separate locations in an `aosp_rpi4_car` source tree.
`external/aasdk` and `packages/apps/AaSdkUsbBridge` are standalone projects
this fork created (no upstream AOSP git history to preserve, so they're kept
as full directory copies). `device/brcm/rpi4` and `frameworks/base` are
existing AOSP projects, so only the diffs/new files are kept, as patches.

### Full-copy directories -- 1:1 with their AOSP path

| In this repo | Goes to (relative to AOSP source root) |
|---|---|
| `aasdk/` | `external/aasdk/` |
| `app/` | `packages/apps/AaSdkUsbBridge/` |

### `device-patches/` -- against `device/brcm/rpi4/`

| In this repo | Applies to / installs at |
|---|---|
| `device-patches/aosp_rpi4_car.mk.patch` | `git apply` inside `device/brcm/rpi4/`, patches `aosp_rpi4_car.mk` |
| `device-patches/sepolicy_platform_app.te.patch` | `git apply` inside `device/brcm/rpi4/`, patches `sepolicy/platform_app.te` |
| `device-patches/permissions/Android.bp` | `device/brcm/rpi4/permissions/Android.bp` (new file) |
| `device-patches/permissions/default-permissions-com.android.systemui.xml` | `device/brcm/rpi4/permissions/default-permissions-com.android.systemui.xml` (new file) |

### `platform-patches/` -- against `frameworks/base/`

| In this repo | Applies to |
|---|---|
| `platform-patches/NetworkPolicyManagerService.java.patch` | `git apply` inside `frameworks/base/`, patches `services/core/java/com/android/server/net/NetworkPolicyManagerService.java` |

## Building

```
cp -r aasdk/*   <AOSP_ROOT>/external/aasdk/
cp -r app/*     <AOSP_ROOT>/packages/apps/AaSdkUsbBridge/

cd <AOSP_ROOT>/device/brcm/rpi4
git apply <THIS_REPO>/device-patches/aosp_rpi4_car.mk.patch
git apply <THIS_REPO>/device-patches/sepolicy_platform_app.te.patch
cp -r <THIS_REPO>/device-patches/permissions .

cd <AOSP_ROOT>/frameworks/base
git apply <THIS_REPO>/platform-patches/NetworkPolicyManagerService.java.patch

cd <AOSP_ROOT>
source build/envsetup.sh
lunch aosp_rpi4_car-bp4a-userdebug
m AaSdkUsbBridge
m systemimage   # or just `m` for a full build
```

## Architecture notes

### Auto-launch, no dialog

AAOS's built-in `android.car.usb.handler` (CarUsbHandler) shows a
disambiguation dialog whenever more than one installed app matches a
just-attached USB device generically. Rather than working around that with a
hack, `AaSdkUsbBridge` registers itself the way AAOS expects an Android-Auto-style
app to: a `usb-aoap-accessory` filter entry (`app/src/main/res/xml/device_filter.xml`)
plus a `android.car.AoapService` implementation (`AaSdkAoapService`), protected
by `android.permission.MANAGE_USB` and gated on the app holding
`android.car.permission.CAR_HANDLE_USB_AOAP_DEVICE` (a `signature|privileged`
permission, granted via `device-patches/permissions/`). When exactly one app
registers as the AOAP handler for a device that supports AOAP, CarUsbHandler
skips the dialog and dispatches directly -- see
`android.car.usb.handler.UsbHostController#onHandlersResolveCompleted` /
`getSingleAoapDeviceHandlerOrNull` in the platform source for the exact logic.

### Native session layer

`aasdk/session/` ports openauto's `AndroidAutoEntity` control-channel state
machine (version/handshake/SSL/service-discovery/audio-focus/ping) and its
service implementations near-verbatim, swapping openauto's Qt/RtAudio/OMX
sinks for Android-native ones:

- `AndroidVideoOutput` -- NDK `AMediaCodec` decoding straight to an
  `ANativeWindow` from a Java `Surface`, no per-frame JNI.
- `AndroidAudioOutput` -- NDK AAudio, one instance per audio channel.
- `AndroidInputDevice` -- touch events injected via JNI from a `SurfaceView`.
- `DummyBluetoothDevice` -- reports unavailable (no Bluetooth pairing yet).

`aasdk/jni/aasdk_jni.cpp` bridges `AaSdkUsbService` (Kotlin) to this layer:
USB fd handoff on accessory attach, a dedicated thread pumping
`libusb_handle_events_timeout` (required -- aasdk's `USBTransport` submits
async libusb transfers but nothing in the upstream library ever pumps the
event loop that delivers their completions), and surface/touch passthrough.

### Unrelated boot-stability fixes bundled in

Two fixes unrelated to Android Auto itself, but required for this device to
boot/render at all, are included since without them there'd be nothing to
plug a phone into:

- `platform-patches/`: `NetworkPolicyManagerService.updateSubscriptions()`
  called `SubscriptionManager`/`TelephonyManager` APIs unconditionally, which
  throws `UnsupportedOperationException` on a device with no telephony
  subscription HAL (no modem) -- fatal inside `system_server`, crash-looping
  boot forever. Guarded with a `hasSystemFeature(FEATURE_TELEPHONY_SUBSCRIPTION)`
  check.
- `device-patches/aosp_rpi4_car.mk.patch`: inherits `carpowerpolicy.mk`
  (`android.hardware.automotive.audiocontrol-service` binds
  `ICarPowerPolicyServer/default` at startup and retries forever without it)
  and adds `default-permissions-com.android.systemui.xml` (CarSystemUI's
  `KeyguardService` crash-loops without a `BLUETOOTH_CONNECT` grant).

## License

[f1xpl/aasdk](https://github.com/f1xpl/aasdk) is GPLv3. This is a derivative
work built on it and should be treated as GPLv3 as well.
