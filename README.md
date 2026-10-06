# Sendspin → ESP32 → Bluetooth speaker

Firmware that registers an ESP32 (WROVER) as a Sendspin player in Music Assistant and forwards the audio via
Bluetooth A2DP to a speaker (tested with a Sony SRS-XB100).

```
Music Assistant ──Sendspin (WiFi, FLAC 44.1 kHz)──▶ ESP32-WROVER ──A2DP/SBC 229 kbit/s──▶ SRS-XB100
```

There are two variants:

| Variant | Location | Status |
|---|---|---|
| ESPHome | `sendspin-bt-wrover.yaml`, `components/a2dp_source/` | plays without audible dropouts, speaker buttons, web interface |
| Native ESP-IDF 5.5 | `native/` | plays without audible dropouts, speaker buttons, no web interface, more CPU headroom |

Only the original ESP32 has Bluetooth Classic; ESP32-S3/C3/C6 do **not** work.

## Common prerequisite: ESP-IDF patch

Both variants use the ESP-IDF 5.5.5 installation that ESPHome downloads, and need a patch of the A2DP stack:

```bash
.venv/bin/esphome compile sendspin-bt-wrover.yaml   # once, downloads ESP-IDF
scripts/patch-idf-sbc-bitrate.sh                    # 229 kbit/s, 27 frames per tick
```

The patch changes the shared copy in `~/.cache/esphome/idf/frameworks/5.5.5/` (affects all ESPHome projects on
the machine). Revert with `scripts/patch-idf-sbc-bitrate.sh --revert`. Re-apply after an ESPHome update that
brings a new IDF version.

## ESPHome variant

1. Fill `secrets.yaml` (template: `secrets.yaml.example`).
2. Put the speaker into pairing mode, then build and flash:
   ```bash
   .venv/bin/esphome run sendspin-bt-wrover.yaml --device /dev/ttyUSB0
   ```
   The ESP discovers the speaker by name (`bt_device_name`), pairs it and reconnects it automatically on every start.
3. Play to "Sendspin BT 1" in Music Assistant. Static delay about 250 ms (initial value, reported by the speaker).

### Web interface

`http://sendspin-bt-1.local` shows:

- **Bluetooth status**: connected/disconnected, name and address of the speaker
- **Title, artist, album** of the current track (Sendspin metadata role)
- **Volume** slider and **Play/Pause**, **Next track**, **Previous track** buttons
- **Re-pair speaker**: drops the pairing and the stored address and searches for the speaker by name
  (put the speaker into pairing mode first)

## Native variant (`native/`)

- `main/a2dp_output.*`: A2DP source (connection, playback clock, keep alive, statistics, AVRCP)
- `main/main.cpp`: WiFi, mDNS (`_sendspin._tcp`), NVS, Sendspin client with player and controller roles
- `sdkconfig.defaults`: complete configuration with rationale, `main/Kconfig.projbuild`: options

Getting started:

1. Fill `secrets.yaml` in the project folder with `wifi_ssid`/`wifi_password` (template: `secrets.yaml.example`).
2. Put the speaker into pairing mode, then build and flash:
   ```bash
   native/build.sh -p /dev/ttyUSB0 build flash
   ```
3. Play to "Sendspin BT 1" in Music Assistant.

View the log: `.venv/bin/python native/serial_log.py /dev/ttyUSB0 600 --reset`
A/B tests: override values in `native/sdkconfig.test`, then `native/run_test.sh NAME` (builds, flashes, logs for
13 min and evaluates with `native/analyze_log.py`).

## Speaker controls

Both variants, via AVRCP: play/pause on the speaker starts or pauses playback in Music Assistant, +/− change the
volume. If the speaker supports absolute volume (e.g. SRS-XB100), it applies the volume itself and changes are
synced with Music Assistant in both directions; otherwise the ESP applies digital volume in 5 % steps.
In the ESPHome variant the buttons fire triggers (`on_play_pause`, `on_next`, `on_previous`, `on_stop`,
`on_volume`) that `sendspin-bt-wrover.yaml` connects to the media player.

If the speaker is reachable but refuses the connection (e.g. because it was paired with the other variant in the
meantime), both variants drop the old pairing after 2 attempts and pair again.

## Why these settings

| Measure | Reason |
|---|---|
| SBC 229 instead of 328 kbit/s (IDF patch) | WiFi and Bluetooth share one radio; at 328 kbit/s the link only managed ~78 % of real time |
| Catch-up limit 27 instead of 21 frames per tick (IDF patch) | When the Bluetooth task runs late, the stack never caught up → too little audio → the speaker runs dry |
| WiFi: receive aggregation with BA window 6, no TX AMPDU, buffers 12/40, TCP window 32 KB | Compromise from A/B runs: without aggregation the TCP stream backs up and Sendspin's clock sync breaks (silence only); with long bursts (BA 32, 16/64, 64 KB) WiFi disturbs A2DP (dropouts) |
| lwIP receive mailboxes 64 (default 6) | otherwise lwIP drops segments and TCP retransmissions inflate the round trip of the sync messages |
| Sendspin buffer 150 KB | 64 KB already caused occasional underruns |
| Modem sleep only outside streaming | Sendspin requests "high performance" WiFi while streaming |
| No coexistence hint `A2DP_STREAMING` | gives Bluetooth priority and makes WiFi latency much worse |
| Main loop, Sendspin threads, TCP/IP on core 1 | keeps core 0 for the WiFi and Bluetooth stacks |
| Playback progress reported from a separate task, ring buffer in internal RAM | the data callback runs in the time-critical Bluetooth task and must not block |
| Steady playback clock instead of pull times | Sendspin hard-syncs at 5 ms error; the stack pulls audio in irregular bursts |
| Keep alive (stream silence) | the SRS-XB100 powers off after a while without audio |
| Outgoing Bluetooth connections only, timeout, stack restart after 4 failed attempts | prevents half-open links ("Conn Exists") |

## Diagnostics

Every 30 s the log shows `Stats:` with the pull rate (target ≈ 44100 Hz), clock deviation and internal RAM,
plus `Load:` with the CPU load per task. `Lost sync` from Sendspin shows hard corrections.

All measurements and listening tests: [docs/TEST_REPORT.md](docs/TEST_REPORT.md)

## Known limitations

- Tested on ESP32 rev1 (WROVER-B); the PSRAM workaround of this revision costs a lot of CPU. An ESP32 with chip
  revision ≥ 3 (e.g. ESP32-DevKitC-VE with WROVER-E) should have more headroom.
- The ESPHome variant needs more CPU on core 0 than the native one and has only been tested in short runs.
- No OTA updates and no web interface in the native variant.
- Switching between the variants: the speaker's own volume (absolute volume) carries over; a speaker turned down
  in one variant stays quiet in the other.

## License

Apache License 2.0, see [LICENSE](LICENSE). Copyright 2026 tho22.
