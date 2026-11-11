# RD-88 iPad Driver

An unofficial DriverKit driver that lets a Roland RD-88 work as a USB audio and MIDI interface on an iPad, over a single USB cable.

Out of the box the RD-88 only does USB audio through Roland's own driver, which exists for macOS and Windows but not iPadOS. This project is a small iPad app that carries a driver extension for it.

This is not affiliated with, endorsed by, or supported by Roland. "Roland" and "RD-88" are trademarks of Roland Corporation, used here only to say which hardware the driver is for.

## What works

- Audio out from the iPad to the RD-88 (stereo, 48 kHz).
- MIDI in both directions, including SysEx.
- Unplugging and replugging.
- A settings screen in the app that reads and changes some of the keyboard's system settings over MIDI: local control, key touch curve and offset, fixed velocity, transpose and master tune.

## What hasn't been tested or built

- Recording the RD-88's own audio into the iPad. The driver sends it to the system, but no app has been pointed at it yet.
- Any keyboard other than one RD-88 (not the RD-88 EX), and any iPad other than an M5 iPad Pro on iPadOS 26.
- Scene backup and editing (planned).
- Latency tuning. The output delay is set conservatively.

## Warnings

- This is a hobby project that has had one evening of testing. An early build crash-looped and restarted the iPad. That bug is fixed, but treat this as experimental.
- If the iPad misbehaves, unplug the keyboard. The driver only runs while the RD-88 is connected.
- You can only run it as a development build on your own iPad. Distributing a DriverKit USB driver needs an entitlement from Apple for the device's USB vendor ID, which is Roland's.

## Requirements

- An iPad with an M-series chip (DriverKit on iPadOS needs one), with Developer Mode on.
- A Mac with Xcode 26 or later and [XcodeGen](https://github.com/yonaskolb/XcodeGen).
- A paid Apple Developer Program membership. Development signing for DriverKit works with automatic signing; no special approval is needed for your own devices.
- The RD-88's USB Driver setting on VENDOR (the factory setting), and Rx Exclusive on if you want the settings screen to work.

## Building

1. Copy `Local.xcconfig.example` to `Local.xcconfig` and put in your team ID and a bundle ID prefix you own.
2. Run `xcodegen generate`.
3. Open `RD88.xcodeproj`, pick the RD88 scheme and your iPad, and run.
4. On the iPad, go to Settings → RD-88 Driver → Drivers and switch on the driver.
5. Plug the RD-88 into the iPad. If the driver was updated while the keyboard was connected, unplug and replug it.

## How it works

At USB high speed the RD-88 exposes four vendor-specific interfaces. (At full speed it only shows empty placeholders, which is what you see if a VM or hub drops the connection to USB 1.1.)

| Interface | Endpoints | Use |
|---|---|---|
| 1, alt 1 | isochronous OUT `0x0D` | audio to the keyboard |
| 2, alt 1 | isochronous IN `0x8E` | audio from the keyboard |
| 3 | bulk OUT `0x03`, bulk IN `0x84` | MIDI |

Both audio streams are 2 channels of 32-bit float at 48 kHz, one packet per microframe. The IN stream is the clock: the driver sends back exactly as many frames as it receives. MIDI uses standard 4-byte USB-MIDI event packets.

- `Driver/RD88Driver.*` is the audio driver (AudioDriverKit). It matches the USB device.
- `Driver/RD88MIDIDriver.*` is the MIDI driver (MIDIDriverKit). It matches interface 3.
- `App/` is the host app with the settings screen and a MIDI monitor.

Two things that cost time and may save you some:

- The audio device advertises its transport type as Thunderbolt. A developer forum report says iPadOS does not surface AudioDriverKit devices that advertise USB.
- Every `OSAction`, and the audio and MIDI objects, must be released in `Stop`. Otherwise the driver process never exits after an unplug, and stale copies pile up.

## Credits

- The MIDI driver's start-up sequence follows the approach in [ASFireWire](https://github.com/mrmidi/ASFireWire).
- SysEx addresses come from Roland's published "RD-88 MIDI Implementation" document.

## License

Public domain, under [The Unlicense](LICENSE). Take it, change it, ship something better; no credit needed.
