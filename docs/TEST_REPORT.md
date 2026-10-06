# Test report: Sendspin → ESP32 → Bluetooth speaker

As of 2026-10-06. All tests with the same hardware and environment.

## Test environment

| | |
|---|---|
| ESP | ESP32-WROVER-B, chip ESP32-D0WDQ6 **revision 1.0**, 4 MB flash, 4 MB PSRAM (cache workaround active) |
| Speaker | Sony SRS-XB100 (A2DP sink, SBC, bitpool 2–49, reports 250 ms delay, supports AVRCP absolute volume) |
| Server | Music Assistant with Sendspin, source: FLAC 44.1 kHz/16 bit (music or radio stream) |
| Radio | ESP next to the PC (no USB 3), speaker 20 cm away, WiFi AP in line of sight < 5 m |
| Software | ESPHome 2026.9.1 or native ESP-IDF 5.5.5, sendspin-cpp 0.7.2 |
| Reference | squeezelite-esp32 (ESP-IDF 4.3.5) on identical hardware: no dropouts |

## Method

- Serial log with timestamps (`native/serial_log.py`), evaluated with `native/analyze_log.py` from 120 s after the
  stream start (settling excluded).
- Key figures: Sendspin `Lost sync`/`Hard sync`, buffer underruns towards the speaker, pull rate of the A2DP stack
  (target 44100 Hz), half round trip of the clock sync (`best max_error`), peak level at input/output, CPU load
  per task.
- Listening test by the user: dropouts on the radio link do not show up in the logs.

## Phase 1: ESPHome variant

| # | Measure | Measurement | Listening |
|---|---|---|---|
| 1 | First version (A2DP speaker component) | crashes at playback start, pipeline hangs after a stream change | short audio, then off |
| 2 | Playback progress from a steady clock instead of pull times | before: ~190 `Lost sync` in 5 min (10–90 ms jitter) | |
| 3 | No task stacks in PSRAM (rev1) | crash in `SpscRingBuffer::acquire` fixed | |
| 4 | SBC 328 kbit/s (default) | pull rate 34,490 Hz (78 %) | very choppy |
| 5 | SBC 229 kbit/s | 43,500 Hz | better |
| 6 | SBC 200 / 170 kbit/s | 43,300–44,200 / 42,200–44,500 Hz | no further gain |
| 7 | Trace "Limit frms to send from 40 to 21" (1581× in 2 min) → catch-up limit 21 → 27 | afterwards > 3 min without correction, 44,100–44,300 Hz | much better, small dropouts |
| 8 | Keep alive (stream silence) | speaker stays on | |
| 9 | CPU load per task | core 0 idle **3–4 %** (WiFi 29 %, BT stack 23 %, BT controller 20 %, BTU 14 %) | |
| 10 | Callback profiling | avg 500 µs, max 72 ms per call | |
| 11 | Playback progress reported from a separate task (core 1) | avg 300 µs | |
| 12 | Ring buffer in internal RAM (200 ms) | avg 175 µs, spikes of 50–70 ms remain (preemption) | unchanged |
| 13 | WiFi driver on core 1 | core 0 idle 11–17 %, but the decoder starves (gaps up to 3.4 s) | worse |
| 14 | WiFi buffers in internal RAM | core 0 idle **50 %**, but RAM exhausted → crash in `fixed_queue_new` | — |
| 15 | **441 Hz test tone without WiFi traffic** | — | **clean** → cause is the WiFi coexistence |
| 16 | Sendspin buffer 150 KB, TCP window 16 KB | no buffer underruns | fewer dropouts |
| 17 | Sendspin buffer 75 KB, TCP window 8 KB | 60–70 `Lost sync`/min, 39 underruns | worse |

Result of phase 1: clean on the ESP side, small dropouts remained on the radio link.

## Phase 2: Native ESP-IDF variant (without ESPHome)

| # | Network configuration | Measurement | Listening |
|---|---|---|---|
| 1 | Lean: WiFi buffers internal, no AMPDU, TCP 16 KB | data rate from Sendspin only 35–40 KB/s instead of 176 KB/s | fragments |
| 2 | as 1, TCP 32 KB | 176 KB/s, but level **−90 dBFS**, clock sync error ~440 ms | nothing |
| 3 | + modem sleep off while streaming | clock sync error 440–560 ms | nothing |
| 4 | + coexistence hint `A2DP_STREAMING` removed | round trip grows 89 → 268 → 357 → 672 ms | nothing |
| 5 | Sendspin buffer 64 KB | error 135–270 ms, input −90 dBFS, "45 s late" | nothing |
| 6 | **Generous (like ESPHome high performance)**: AMPDU on, BA 32, 16/64, TCP 64 KB, WiFi buffers in PSRAM | round trip 7–27 ms, input −2 dBFS | **music**, small dropouts |
| 7 | squeezelite network (AMPDU off, 12/40, 32 KB) + PSRAM | round trip 300–730 ms, 146 hard syncs | silence |
| 8 | as 7 + AMPDU RX with BA 6 | 250–510 ms, 98 `Lost sync` | silence |
| 9 | as 8 + lwIP mailboxes 64 (default 6) | 12–41 ms at first, timeouts after ~6 min, jumps of ±3/12 s | breaks down |

Finding: without receive aggregation and with the default lwIP mailbox (6) the TCP stream backs up, Sendspin's
clock sync (over the same connection) falls apart and Sendspin discards all audio as late.

### A/B series starting from no. 6 (13 min each, evaluated from 2 min after start)

| Run | Change (cumulative) | Lost sync | Underruns | Sync rtt/2 median/max | Core 0 idle | Min pull rate |
|---|---|---|---|---|---|---|
| Reference | generous (no. 6) | 0 | 0 | 24 / 45 ms | 13 % | 44,051 Hz |
| run1 | TX AMPDU off, RX BA 32 → 6 | 0 | 0 | 16 / 43 ms | 14 % | 44,050 Hz |
| **run2** | + WiFi buffers 12/40, TCP 32 KB, send buffer 8 KB | **0** | **0** | 25 / 43 ms | 14 % | 44,043 Hz |
| run3 | + Sendspin buffer 64 KB | 2 | 3 | 22 / 43 ms | 14 % | 44,057 Hz |

**Listening test run2: no more dropouts.** run2 is the final configuration (`native/sdkconfig.defaults`).
Check run after the cleanup: 0 lost sync, 0 underruns, pull rate ≥ 44,068 Hz.

## Phase 3: Speaker controls (AVRCP), native variant

| Test | Result |
|---|---|
| Absolute volume detected | yes ("Speaker supports absolute volume"), the speaker applies the volume, no digital gain |
| − on the speaker | steps 48 % → 19 % each reported to Music Assistant ✔ |
| + on the speaker | ✔ (confirmed by the user) |
| Volume from Music Assistant → speaker | ✔ (confirmed by the user) |
| Play/pause button | code 0x46 (PAUSE) → Sendspin command PAUSE, stream ends ✔; pressing again resumes ✔ (confirmed by the user) |
| Play on connect | ignored during the first 3 s (the XB100 sends play by itself) |

## Phase 4: ESPHome variant with the new network settings (2026-10-06)

Taken over from the native variant: WiFi compromise of run2, modem sleep no longer forced, no coexistence hint
`A2DP_STREAMING`, AVRCP controls (triggers `on_play_pause`, `on_next`, `on_previous`, `on_stop`, `on_volume` →
media player actions).

| Test | Result |
|---|---|
| Switch native → ESPHome | speaker refuses the A2DP channel (`BTA_AV_OPEN_EVT::FAILED status: 3`, 28×): stale link key |
| Recovery: link up but A2DP refused → drop the old pairing after 2 attempts | paired again **without** pairing mode, connected, stream running ✔ |
| First listening test (without absolute volume) | nothing audible: the speaker was still at 19 % (from the native variant) plus digital attenuation → audio after "+" on the speaker ✔ |
| Absolute volume detected | yes, the speaker applies the volume ✔ |
| − on the speaker | 28 % → 11 % each passed to media player/Music Assistant ✔ |
| Play/pause button | code 0x46 → media player IDLE (pause), 4 s later 0x44 → PLAYING (via `media_player.toggle`) ✔ |
| + on the speaker, volume from Music Assistant | ✔ (confirmed by the user) |
| Listening test | **no dropouts** (confirmed by the user); log: 0 lost sync, 0 underruns, pull rate 44,076 Hz. Observed for only ~1.5 min after the stream start |
| CPU | core 0: WiFi 29 %, BT stack 22 %, BT controller 19 %, BTU 17 % (more than native), core 1 idle 39 % |

## Phase 5: Web interface, ESPHome variant (2026-10-06)

ESPHome `web_server` (v3) with Bluetooth status, title/artist/album (Sendspin metadata role), volume slider,
play/pause/next/previous buttons and a "Re-pair speaker" button. Flash: 1.62 of 1.84 MB used (88 %).

| Test | Result |
|---|---|
| Web interface reachable | HTTP 200 on port 80, runs alongside Sendspin's own HTTP server (separate control port) ✔ |
| Status, track info, controls | ✔ (confirmed by the user) |
| Playback with the metadata role enabled | log: 0 lost sync, 0 underruns, pull rate ≥ 44,015 Hz (1.8 min after settling); internal heap 58 KB free (min 55 KB) |
| "Re-pair speaker" button | 1st press (speaker not in pairing mode): pairing dropped, discovery finds nothing, retries every 15 s; 2nd press with pairing mode: speaker found after 0.5 s, paired and connected 3 s after the press ✔ |

## Phase 6: Fixes for re-pairing, power off and reboots (2026-10-06)

Findings after the web interface test:

| Observation | Cause | Fix |
|---|---|---|
| After "Re-pair speaker": 80–100 lost syncs and 100–146 dropped chunks per minute, each gap 104.5 ms (= one 4608-sample FLAC block), until the next reboot | after the inquiry (discovery) the A2DP stack kept pulling 1–4 % less than real time, Sendspin's buffer overflowed. Reconnects without inquiry (speaker off/on) stayed clean | restart the Bluetooth stack after a discovery, before connecting |
| Switching the speaker off stops playback in Music Assistant | the SRS-XB100 sends pause (0x46) ~0.5 s before it disconnects | act on play/pause only if the speaker is still connected 1 s later |
| ESP rebooted every 15 min ("No clients; rebooting") | ESPHome reboots without a connected API client (Home Assistant) | `api: reboot_timeout: 0s` |
| Play on the speaker did nothing after Music Assistant had stopped | `media_player.toggle` needs an active source | play/pause/next/previous/stop sent as Sendspin controller commands |

Verification (ESPHome variant):

| Test | Result |
|---|---|
| Play on the speaker while stopped | 0x46 → Music Assistant playing 1 s later ✔ |
| Pause on the speaker | 0x44 → stopped 1 s later ✔ |
| Speaker switched off | pause arrives together with the disconnect → "Ignoring play/pause sent while the speaker disconnected" ✔, playback in Music Assistant not stopped |
| Reconnect after power on, play | connected after 16 s, play starts playback ✔ |
| "Re-pair speaker" | "Speaker found, restarting Bluetooth before connecting", paired and connected ✔; one minute with 51 lost syncs (pull rate 43,019 Hz), afterwards clean (44,081–44,146 Hz, 0 lost sync) — before the fix the dropouts lasted until the next reboot |
| No reboot | no "No clients; rebooting" during the test ✔ |

The native firmware received the same fixes for discovery and play/pause (compiled, not yet tested on the device;
it has no API and therefore no reboot timeout).

## Quirks of the Sony SRS-XB100

- Reports 250 ms latency via A2DP delay reporting (initial value for Sendspin's static delay).
- Reconnects by itself after power on; together with the ESP's own connection attempt this leads to half-open
  links ("Conn Exists") → the ESP accepts no incoming connections.
- Powers off after a while without audio → keep alive (stream silence).
- Sends play and its own volume right after connecting → ignored for 3 s.
- Sends pause right before it powers off → play/pause is only acted on if the speaker is still connected 1 s later.
- Only applies volume changes made with its buttons once the source echoes them back.
- After it was paired again with another device or firmware it refuses the old key, but accepts a new pairing
  initiated by the source even without pairing mode.
- Absolute volume is the speaker's own volume; it carries over when switching firmware.
