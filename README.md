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
| Native ESP-IDF 5.5 | `native/` | plays without audible dropouts, speaker buttons, web interface, more CPU headroom |

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
3. Play to "Sendspin BT 1" in Music Assistant. Static delay about 250 ms (initial value, reported by the speaker). The volume starts at 10 % after a fresh flash.

### Web interface

`http://sendspin-bt-1.local` shows:

- **Bluetooth status**: connected/disconnected, name and address of the speaker
- **WiFi signal** (diagnostic)
- **Title, artist, album** of the current track (Sendspin metadata role)
- **Volume** slider and **Play/Pause**, **Next track**, **Previous track** buttons
- **Search speakers**: searches about 13 s for audio devices ready to pair (put the speaker into pairing mode
  first) and lists them under **Speaker**. A connected speaker is disconnected for the search and reconnected
  afterwards. The page reloads itself when the search is done (`web/pairing.js`): the web interface reads the
  options of a select only when loading.
- **Speaker**: choosing a found speaker drops the old pairing and pairs the chosen one
- **Unpair speaker**: disconnects and forgets the speaker; no connection attempts (also after a reboot) until a
  speaker is chosen again
- **Re-pair speaker**: drops the pairing and the stored address and searches for the speaker by name
  (`bt_device_name`, put the speaker into pairing mode first)
- **Log level**: INFO after every boot (each log line costs CPU and is streamed to every open page); choose DEBUG
  for the statistics and per-task load lines. DEBUG returns to INFO after 10 min: streamed to an open page it takes
  enough WiFi airtime for audible Bluetooth dropouts

### NFC reader (optional)

A PN7160 on I2C reports scanned tags to Home Assistant as tag scans (`homeassistant.tag_scanned`). Tags written by
the Companion app are reported with their HA tag ID (from the NDEF record), all other tags with their UID. The web
interface shows the tag on the reader under **NFC tag** (the ID sent to Home Assistant, or "No tag").

Wiring (tested with the Elechouse PN7160 MINI V1, I2C):

| Module | ESP32 | Note |
|---|---|---|
| SDA | GPIO21 | |
| SCL | GPIO22 | 400 kHz |
| IRQ | GPIO34 | input only, driven by the PN7160 |
| VEN | GPIO25 | |
| VCC | separate 5 V supply | 3.3–5.5 V; I/O stays at 3.3 V. The dev board's 5V pin is not enough (see below) |
| GND | GND | also the GND of a separate supply. Not CMD next to 5V: without a common ground I2C fails with random NACKs and garbage |

The small antenna has a short range: the tag has to lie on it. Test_1 (an NTAG215) was only detected with the larger
antenna of the kit.

`components/pn7160/` and `components/pn7160_i2c/` are copies of ESPHome's components (2026.9.1) with these changes:

- **ISO 15693** (e.g. Tonies): detected by UID; their memory (NDEF) is not read, cleaning/formatting/writing is refused
- **Late responses are waited for, not requested again**: resending a command made the NFCC answer twice, and every
  later response then matched the wrong command
- **Messages around a command**: credits notifications, a tag activation or a stale response arriving before a
  command's response are skipped instead of failing the command (a deactivation notification is kept for the state
  machine); a data exchange accepts the credits notification and the tag data in either order
- **IRQ still high after a read** means the next message is queued, which is no error
- **Failed activation during discovery**: the NFCC usually activates the tag on one of its next attempts, so discovery
  is only restarted if no activation follows within 500 ms. Stopping it right away raced with the activation: the tag
  dropped out while it lay on the reader, and an activation arriving during the stop could hang the reader
- **Stale deactivation dropped when discovery starts**: a deactivation notification arriving while discovery is being
  restarted belongs to the discovery before. Processing it later started discovery a second time; the NFCC had
  activated the tag by then, that notification was skipped, and both sides waited for each other (silent for hours)
- **Rejected I2C writes are repeated**: the NFCC NACKs a write now and then (presumably the first one in standby)
- **No permanent failure**: after too many initialization failures the NFCC is reset again 10 s later instead of
  the component being marked failed until the next reboot. The NFCC sometimes stops answering on I2C for several
  seconds and garbles its first responses after that

Supply: the module needs a stable 5 V. The 5V pin of the dev board (behind its diode) delivered only 4.59 V, and
with it a tag lying still was reported as removed and read again every 5–10 s, with 200–500 failed activations a
minute and bursts of I2C errors (NFCC reset automatically). On a separate 5.0 V supply (common GND, ~30 mA drawn) a
tag lay on the reader for over 4.5 hours without a single false removal, error or NFCC reset, with ~7 failed
activations a minute. A 100 µF + 100 nF capacitor at the module might be enough with the 5V pin (not tested yet).
The module does not get warm.

Known issues with the tested module:

- A tag that stays on the reader after it was read is rarely activated again: the NFCC reports a failed activation
  (`DISCOVERY_TARGET_ACTIVATION_FAILED`, visible with VERBOSE logging) for as long as the tag lies there. Enough
  activations get through to keep it present with `tag_ttl: 5s`; a tag placed again is read at once. Placing a tag
  again within 5 s is not reported

## Native variant (`native/`)

- `main/a2dp_output.*`: A2DP source (connection, playback clock, keep alive, statistics, AVRCP)
- `main/main.cpp`: WiFi, mDNS (`_sendspin._tcp`), NVS, Sendspin client with player and controller roles
- `main/web_ui.*`: web interface on port 80 (`http://sendspin-bt-1.local`), the counterpart of the ESPHome one:
  title/artist/album, play/pause, next, previous, volume, Bluetooth status, **Search speakers** (the list of found
  speakers updates itself), **Pair** a found speaker, **Unpair speaker**, **Re-pair speaker** (by name), WiFi
  signal, free internal RAM, log level (INFO after boot and 10 min after choosing DEBUG; DEBUG only for the
  application's own tags) and the log
  (only captured while "Show log" is on)
- `sdkconfig.defaults`: complete configuration with rationale, `main/Kconfig.projbuild`: options

Getting started:

1. Fill `secrets.yaml` in the project folder with `wifi_ssid`/`wifi_password` (template: `secrets.yaml.example`).
2. Put the speaker into pairing mode, then build and flash:
   ```bash
   native/build.sh -p /dev/ttyUSB0 build flash
   ```
3. Play to "Sendspin BT 1" in Music Assistant. The volume starts at 10 % after the first flash; afterwards the last
   volume is restored (stored 5 s after the last change).

View the log: `.venv/bin/python native/serial_log.py /dev/ttyUSB0 600 --reset`
A/B tests: override values in `native/sdkconfig.test`, then `native/run_test.sh NAME` (builds, flashes, logs for
13 min and evaluates with `native/analyze_log.py`).

## Speaker controls

Both variants, via AVRCP: play/pause on the speaker starts or pauses playback in Music Assistant, +/− change the
volume. If the speaker supports absolute volume (e.g. SRS-XB100), it applies the volume itself and changes are
synced with Music Assistant in both directions; otherwise the ESP applies digital volume in 5 % steps.
Play/pause, next, previous and stop are sent to Music Assistant as Sendspin controller commands, so they also work
while no stream is active. Play/pause is only acted on if the speaker is still connected 1 s later, because the
SRS-XB100 sends pause right before it powers off.
In the ESPHome variant the buttons fire triggers (`on_play_pause`, `on_next`, `on_previous`, `on_stop`,
`on_volume`) that `sendspin-bt-wrover.yaml` connects to Sendspin commands and the media player volume.

If the speaker is reachable but refuses the connection (e.g. because it was paired with the other variant in the
meantime), both variants drop the old pairing after 2 attempts and pair again. After discovering a speaker by
name, the Bluetooth stack is restarted before connecting: after an inquiry the controller otherwise delivered
slightly less than real time until the next reboot.

## Why these settings

| Measure | Reason |
|---|---|
| SBC 229 instead of 328 kbit/s (IDF patch) | WiFi and Bluetooth share one radio; with the old WiFi settings the link only managed ~78 % of real time at 328 kbit/s. With the current settings it keeps up, but dropouts are still audible without the patch |
| Catch-up limit 27 instead of 21 frames per tick (IDF patch) | When the Bluetooth task runs late, the stack never caught up → too little audio → the speaker runs dry |
| WiFi: receive aggregation with BA window 6, no TX AMPDU, buffers 12/40, TCP window 32 KB | Compromise from A/B runs: without aggregation the TCP stream backs up and Sendspin's clock sync breaks (silence only); with long bursts (BA 32, 16/64, 64 KB) WiFi disturbs A2DP (dropouts) |
| lwIP receive mailboxes 64 (default 6) | otherwise lwIP drops segments and TCP retransmissions inflate the round trip of the sync messages |
| Out-of-order TCP segments unlimited (ESPHome sets 4) | with 4, a single lost WiFi frame discards most of the 32 KB window; the retransmissions stalled the stream for 5–20 s (ESPHome only, native uses the IDF default) |
| Sendspin buffer 1 MB in PSRAM | Music Assistant fills it ahead of time; with 150 KB dense music (FLAC ~1 Mbit/s) overflowed it and every dropped chunk was a 104.5 ms gap |
| `malloc()` above 4 KB may use PSRAM (32 KB internal reserved) | otherwise Sendspin's incoming chunks exhaust the internal RAM (ESPHome: abort in `operator new`) |
| Modem sleep only outside streaming | Sendspin requests "high performance" WiFi while streaming |
| No coexistence hint `A2DP_STREAMING` | gives Bluetooth priority and makes WiFi latency much worse |
| Main loop, Sendspin threads, TCP/IP on core 1 | keeps core 0 for the WiFi and Bluetooth stacks |
| Playback progress reported from a separate task, ring buffer in internal RAM | the data callback runs in the time-critical Bluetooth task and must not block |
| Steady playback clock instead of pull times | Sendspin hard-syncs at 5 ms error; the stack pulls audio in irregular bursts |
| Keep alive (stream silence) | the SRS-XB100 powers off after a while without audio |
| Outgoing Bluetooth connections only, timeout, stack restart after 4 failed attempts | prevents half-open links ("Conn Exists") |
| Stream watchdog: no audio pulled for 3 s while the stream runs → reconnect (quick retries every 3 s); disconnect not confirmed within 5 s or a restarted stream that never pulls → restart the Bluetooth stack | the stack once stopped pulling audio while the link stayed up (no disconnect, no suspend), playback hung until a reboot |
| `api: reboot_timeout: 0s` (ESPHome) | ESPHome otherwise reboots every 15 min when no Home Assistant is connected |

## Diagnostics

Every 30 s the log shows `Stats:` with the pull rate (target ≈ 44100 Hz), clock deviation and internal RAM,
plus `Load:` with the CPU load per task. `Lost sync` from Sendspin shows hard corrections.

All measurements and listening tests: [docs/TEST_REPORT.md](docs/TEST_REPORT.md)

## Known limitations

- Tested on ESP32 rev1 (WROVER-B) and rev3 (ESP32-DevKitC-VE, native variant only). On revision ≥ 3, building with
  `CONFIG_ESP32_REV_MIN_3=y` (e.g. in `native/sdkconfig.test`) drops the PSRAM cache workaround and frees ~9 KB of
  internal RAM; a CPU gain could not be measured. The IDF patch is still needed on revision 3.
- The ESPHome variant needs more CPU on core 0 than the native one and has only been tested in short runs.
- No OTA updates in the native variant.
- Switching from ESPHome to the native variant loses the native pairing: put the speaker into pairing mode once.
- Switching between the variants: the speaker's own volume (absolute volume) carries over; a speaker turned down
  in one variant stays quiet in the other.

## License

Apache License 2.0, see [LICENSE](LICENSE). Copyright 2026 tho22.
