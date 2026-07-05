# SystemUI crash on Bluetooth connect (BT-only subscription, no telephony data HAL)

## Symptom

On this RPi4 AOSP Automotive build (no modem, no `FEATURE_TELEPHONY_DATA`),
connecting a Bluetooth device (e.g. during Wireless Android Auto bring-up)
crashes SystemUI.

## Root cause

`MobileConnectionRepositoryImpl` (SystemUI's mobile-signal-pipeline code)
unconditionally read `telephonyManager.isDataConnectionAllowed` and called
`telephonyManager.setDataEnabledForReason(...)`. A Bluetooth HFP connection
can register a phone-account-backed telephony subscription -- so a valid
`subId` exists and this repository gets instantiated for it -- **without**
the device actually having `FEATURE_TELEPHONY_DATA` (no cellular data HAL,
as on this build). Both `TelephonyManager` calls throw
`UnsupportedOperationException` in that case, which is fatal inside
SystemUI's process.

This is the same class of bug as the earlier boot crash-loop fix in
`NetworkPolicyManagerService.updateSubscriptions()` (see `platform-patches/NetworkPolicyManagerService.java.patch`
on `main`): AOSP telephony-adjacent code across the framework assumes that
"a subscription exists" implies "the telephony data HAL is present", which
doesn't hold on a headless-modem automotive build that still supports BT-HFP.

## Fix

`packages/SystemUI/src/com/android/systemui/statusbar/pipeline/mobile/data/repository/prod/MobileConnectionRepositoryImpl.kt`
(patch: `platform-patches/MobileConnectionRepositoryImpl.kt.patch`):

- Added `isDataCapable = context.packageManager.hasSystemFeature(PackageManager.FEATURE_TELEPHONY_DATA)`.
- `dataEnabled`'s initial value: only reads `telephonyManager.isDataConnectionAllowed` when `isDataCapable`, else `false`.
- `setDataEnabled()`: no-ops when `!isDataCapable` instead of calling `setDataEnabledForReason()`.

## Apply

```
cd <AOSP_ROOT>/frameworks/base
git apply <THIS_REPO>/platform-patches/MobileConnectionRepositoryImpl.kt.patch
```

## Watch for elsewhere

If SystemUI (or another system-server component) crashes again on a BT
connect/disconnect with an `UnsupportedOperationException` from a
`TelephonyManager` call, it's very likely another call site with the same
unguarded pattern -- check for a `FEATURE_TELEPHONY_DATA` /
`FEATURE_TELEPHONY_SUBSCRIPTION` guard before re-diagnosing from scratch.
