# Room Monitor — M5StickC Plus 1.1

A privacy-first, always-on room sound monitor using the **built-in SPM1423 microphone** on the M5StickC Plus 1.1.

## What V0.1 does

- Learns the room's background sound floor after boot.
- Detects sustained sound relative to that learned baseline.
- Shows state, level-above-baseline, IP address, and remote-listening status on the M5Stick display.
- Hosts a mobile-friendly web dashboard at the device IP.
- Streams **16 kHz / 16-bit / mono PCM** live audio on demand through WebSocket port 81.
- Requires an access PIN for live listening.
- Does **not** save or retain audio.
- Optional MQTT + Home Assistant discovery.
- Button A toggles microphone privacy/mute.
- Hold Button B for 5 seconds to erase Wi-Fi + Room Monitor settings.

## Hardware target

M5StickC Plus 1.1 (ESP32-PICO-D4), using its built-in microphone. M5Stack documents the SPM1423 microphone on GPIO0 (clock) and GPIO34 (data); M5Unified configures the board hardware automatically.

## First boot

1. Flash the firmware.
2. The device tries the last saved Wi-Fi network.
3. If none is configured, connect your phone/laptop to `RoomMonitor-XXXXXX`.
4. Complete the captive portal. Set:
   - Room name
   - Listening PIN
   - Detection threshold (default 12 dB above learned baseline)
   - Optional MQTT details
5. The display shows the device IP after joining Wi-Fi.
6. Browse to `http://DEVICE-IP/`.

The first ~12 seconds after boot are used to learn the ambient sound floor. Keep the room reasonably quiet during this calibration.

## Home Assistant

If you already run an MQTT broker (for example the Mosquitto add-on), enter its LAN address and credentials during setup. The firmware publishes Home Assistant MQTT discovery entities for:

- sound level (dBFS)
- dB above learned baseline
- noise floor
- active-sound duration
- sound-active binary sensor
- remote-listening binary sensor
- Wi-Fi RSSI

If you do not use MQTT, leave the broker blank. The Room Monitor web interface works independently.

## Tailscale

The M5Stick does **not** run Tailscale. Use Tailscale on Home Assistant (or another always-on machine) as a **subnet router** for your home LAN. Then browse directly to the Room Monitor's LAN IP while connected to your tailnet. Do not expose ports 80 or 81 directly to the public internet.

## Chrome one-click installer

The `web-installer/` folder is designed for ESP Web Tools. A compiled, merged firmware image is required next to `manifest.json`.

The included GitHub Actions workflow:

1. builds the PlatformIO project,
2. merges the ESP32 bootloader, partition table, boot app, and application into a single image,
3. places it at `web-installer/room-monitor-m5stickc-plus.bin`, and
4. publishes `web-installer/` to GitHub Pages.

After enabling **Settings → Pages → Source: GitHub Actions**, the Pages URL becomes a Chrome/Edge one-click firmware installer.

## Build locally

Install PlatformIO, then:

```bash
pio run -e m5stick-c-plus
```

A local build produces the individual ESP32 images under `.pio/build/m5stick-c-plus/`. The GitHub workflow demonstrates the `esptool merge_bin` command needed for ESP Web Tools.

## Important notes

- The displayed level is **dBFS**, plus a relative dB-above-baseline measurement. It is not calibrated dB SPL.
- V0.1 detects persistent sound. It does not yet classify music vs. speech vs. appliance noise.
- Live audio is protected by a PIN but ordinary HTTP is not encrypted on the LAN. Tailscale protects the remote network path; LAN users still share the local network trust boundary.
- Live audio requires browser access to TCP ports 80 and 81 on the device.
- Use a data-capable USB cable for flashing.

## Next iteration

Planned V0.2 improvements:

- frequency-domain features for `music-like`, `speech/TV-like`, and `steady mechanical` classification;
- Home Assistant notification blueprint;
- optional HTTPS/reverse proxy through Home Assistant;
- configurable quiet hours and away-mode behavior;
- OTA firmware updates.
