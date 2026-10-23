# Tonverk Bridge

Streams an (un-rooted) Android phone's **system media audio** (YouTube, browser,
games, music apps) to the **Elektron Tonverk** over USB — as a 48 kHz / 24-bit
class-compliant audio stream. It can also **record the Tonverk's USB audio
output to a WAV file** on the phone (the reverse direction).

## How it works

1. `AudioPlaybackCapture` grabs the phone's media audio (the app becomes the
   audio producer itself — the system "start recording/casting" dialog covers it).
2. A built-in userspace **UAC2 driver** (libusb, `app/src/main/cpp/usb_audio.c`)
   sends the audio to the Tonverk's isochronous USB OUT endpoint:
   - output packet sizing follows the device's **asynchronous feedback** endpoint,
   - content is produced by a **16-tap Kaiser-windowed sinc resampler** (64 phases)
     with a proportional controller keeping the ring buffer stable,
   - packets are written contiguously (usbfs layout), with fade-in after priming
     and decaying underflow fill — no clicks.
3. Captured audio is converted from the mixer's float output to 24-bit (with a
   16-bit fallback if float capture is unavailable).

### Recording (Tonverk → phone)

- The capture path finds the Tonverk's isochronous **IN** interface dynamically
  from the USB descriptors (channel count / subslot size included), harvests the
  ISO packets into a ring buffer and streams them into a WAV file — **bit-exact**,
  no resampling: the file follows the Tonverk's own clock.
- Files land in `Music/Tonverk Bridge` by default; a different folder can be
  picked (Android folder picker) and a file name set in the app. The default
  name is the current date & time. The WAV header is re-patched every few
  seconds, so even an interrupted recording stays playable.
- A **live level meter** shows the Tonverk's signal as soon as it is connected
  (the capture path doubles as a monitor), and a **recording gain** (−24…0 dB)
  can tame hot signals — at 0 dB the file stays bit-exact.
- Bridge and recording are **mutually exclusive** — running both would make the
  two isochronous USB streams fight for bandwidth, so only one is active at a time.
- The ongoing notification shows whether you're streaming or recording, carries a
  **Stop** action and opens the app when tapped.

## Usage

- Plug in the Tonverk (audio/class-compliant mode), open the app, press **Start**,
  confirm the permissions and the system capture dialog. Done.
- Press **Record** to capture the Tonverk's USB output into a WAV file. Stop the
  bridge first if it's running (the two directions can't run at once).
- The **Diagnostics** section has a USB test tone, descriptor dump and live
  driver stats.

## Building

1. Open the project in **Android Studio** (Gradle 8.7 + AGP 8.5.2, NDK 26).
2. Run on a device with Android 10+ (API 29, required for AudioPlaybackCapture).

Or from the command line: `gradle assembleDebug` (requires JDK 17+).

There is also a host-side test for the native driver (no Android needed):
see `hosttest/README.txt`.

## Known limitations

- Apps that opt out of audio capture (e.g. **Spotify**) stay silent by design.
- Only runs while the foreground service is active (notification visible).
- Sample rate is fixed at 48 kHz (`AudioBridgeService.SR`).

## Support

If this is useful to you: ☕ [Buy me a beer on Ko-fi](https://ko-fi.com/buyabeerforthos)

## License

[GPL-3.0](LICENSE). Bundles [libusb](https://libusb.info/) (LGPL-2.1).
This is an unofficial tool, not affiliated with Elektron.

## Disclaimer

This app — including the USB driver code, the icon and this documentation — was
largely written by Claude (Anthropic's AI assistant) under human direction.
It was debugged and verified against real hardware (Elektron Tonverk), but
review it yourself before trusting it with anything critical.
