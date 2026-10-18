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
| 16 | Sendspin buffer 150 KB, TCP window 16 KB | no buffer underruns | fewer dropouts (later found too small for dense music, see phase 7) |
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
| After "Re-pair speaker": 80–100 lost syncs and 100–146 dropped chunks per minute, each gap 104.5 ms (= one 4608-sample FLAC block), until the next reboot | suspected: after the inquiry (discovery) the A2DP stack pulls less than real time. **Wrong diagnosis**, see phase 7 | restart the Bluetooth stack after a discovery, before connecting (kept, but did not fix the dropouts) |
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
| "Re-pair speaker" | "Speaker found, restarting Bluetooth before connecting", paired and connected ✔; **dropouts continued** (lost syncs for minutes). An earlier version of this report claimed they stopped after one minute; that came from evaluating a log that was still being written |
| No reboot | no "No clients; rebooting" during the test ✔ |

The native firmware received the same fixes for discovery and play/pause (compiled, not yet tested on the device;
it has no API and therefore no reboot timeout).

## Phase 7: Real cause of the 104.5 ms dropouts (2026-10-06)

The dropouts came back without any re-pairing, also right after a fresh boot and after switching the speaker off
and on (log: 959 lost syncs in 13 min).

| Observation | Finding |
|---|---|
| Pull rate of the A2DP stack during the dropouts | 44,040–44,500 Hz, i.e. real time: Bluetooth was not the cause |
| `Failed to send audio chunk` before every `Lost sync` | Sendspin's encoded audio buffer (150 KB) was full and incoming chunks were dropped |
| `Lost sync` of +104.5 ms (or multiples) | positive error = gap in the timeline: exactly the dropped chunk, filled with silence |
| Clean runs before (radio stream) vs. dropouts (music) | Music Assistant fills the buffer ahead of time; dense music compresses worse (FLAC ~1 Mbit/s) and overflowed 150 KB |

| # | Measure | Result |
|---|---|---|
| 1 | Sendspin buffer 150 KB → 1 MB (ESPHome default, allocated in PSRAM) | no dropouts, but reboots 1–20 s after every stream start: abort in `operator new` (from Sendspin's state message, triggered among others by volume changes). Internal heap down to ~37 KB: the larger read-ahead kept more incoming chunks in internal RAM, since ESPHome only uses PSRAM on explicit request |
| 2 | + `CONFIG_SPIRAM_USE_MALLOC` (`malloc()` above 4 KB may use PSRAM, 32 KB internal reserved), as in the native variant | **no reboots, no dropouts** (confirmed by the user): 11 min of music with 0 lost sync, 0 dropped chunks, pull rate 43,914–44,435 Hz, internal heap 44–48 KB free; volume buttons and play/pause on the speaker ✔ |

The native variant received the 1 MB buffer as well (it already used `CONFIG_SPIRAM_USE_MALLOC`; compiled, not yet
tested on the device).

Still open: when switching the speaker off, the SRS-XB100 once sent pause 80 s before it disconnected, so the 1 s
confirmation window did not filter it and Music Assistant stopped.

## Phase 8: Stalls in the ESPHome variant, IDF patch re-check (2026-10-06)

After phase 7 the ESPHome variant ran cleanly for ~20 min, then the stream stalled repeatedly: no data from the
server for 5–20 s (time sync messages timed out as well), buffer underruns of up to 5 s, Music Assistant reopened
the connection every 30–60 s.

| Test | Result |
|---|---|
| WiFi signal sensor (new, diagnostic) | −57 to −82 dBm without any correlation to the stalls (clean phases at −82 dBm, stalls at −57 dBm) |
| Ping PC → ESP | 50–120 ms in clean phases, 300–500 ms during stalls; PC → Music Assistant 3–10 ms |
| 2.4 GHz channel 11 → 1 (a strong foreign network shared channel 11) | stalls continued |
| **Native variant in the same environment** (13 min) | **0 lost sync, 0 underruns**, ping 10–154 ms → the cause is specific to ESPHome |
| sdkconfig comparison | ESPHome sets `CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4` (IDF default 0 = unlimited): with a 32 KB window a single lost WiFi frame discards most segments behind it |
| ESPHome with `CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS: "0"` | 9.5 min: 1 lost sync together with 1 clock resync (Bluetooth hiccup), otherwise clean, pull rate 43,042–44,332 Hz |

### IDF patch re-check (native variant, current WiFi settings, 10 min each)

| | With patch (229 kbit/s, 27 frames/tick) | Without patch (328 kbit/s, 21 frames/tick) |
|---|---|---|
| Pull rate | 43,919–44,277 Hz | 43,771–44,295 Hz |
| Lost sync / underruns | 0 / 0 | 0 / 0 |
| Pull jitter (clock error max) | up to 268 ms | up to 521 ms |
| Listening | no dropouts | **dropouts audible** |

The ~78 % pull rate at 328 kbit/s from phase 1 came from the old WiFi settings. With the current settings the stack
keeps up, but the radio link still drops audio that no log shows → the patch stays. To be re-checked on ESP32
revision 3 boards (no PSRAM cache workaround, more headroom on core 0).

Pitfall found: `patch-idf-sbc-bitrate.sh --revert` restored the backup with its old timestamp, so builds kept the
patched object file. The script now touches the file.

## Phase 9: Bluetooth stream stall and watchdog (2026-10-06)

ESPHome variant, 9 min of clean playback, then the music stopped (speaker on, nothing pressed):

| Observation | Finding |
|---|---|
| Pull rate 44,206 Hz → 34,107 Hz → 0 Hz within ~20 s | the A2DP stack stopped calling the data callback |
| No `Disconnected`, no `Media stream suspended` | the link stayed up from the ESP's point of view |
| BTC_TASK 25 % → 1.4 %, Sendspin 31 % → 1.3 % | Bluetooth idle, Sendspin blocked on the full buffer and dropping every chunk (`Failed to send audio chunk`) |
| WiFi, heap | fine; Music Assistant kept streaming |

Cause unknown (speaker or Bluedroid). Fix in both variants: a stream watchdog. If the stream is started but no audio
was pulled for 3 s, the ESP disconnects and reconnects right away; if the disconnect is not confirmed within 5 s,
or a second stall follows within 5 min, it restarts the Bluetooth stack. Checked: no false triggers in normal
operation; the recovery itself has not been observed yet (the stall could not be provoked).

## Phase 10: ESP32 revision 3 board (2026-10-16)

New board ESP32-DevKitC-VE: ESP32-D0WD-V3 **revision 3.1**, 8 MB flash, 8 MB PSRAM (4 MB mapped). WiFi back on
channel 11. Native variant, 10 min of music each, same speaker:

| Run | Lost sync / underruns / stalls | Pull rate | Core 0 idle | Core 1 idle | Internal heap free | Listening |
|---|---|---|---|---|---|---|
| Old WROVER rev1 (2026-10-06, for reference) | 0 / 0 / 0 | 43,919–44,277 Hz | 21–24 % | 46–49 % | 27 KB | clean |
| Rev3 board, build for rev1 (PSRAM workaround on), IDF patch | 0 / 0 / 0 | 43,975–44,251 Hz | 32–33 % | 58 % | 27 KB | clean |
| Rev3 board, build for rev3 (`CONFIG_ESP32_REV_MIN_3`, no workaround), IDF patch | 0 / 0 / 0 | 44,023–44,185 Hz | 21–22 % | 47 % | 36 KB | clean |
| Rev3 board, build for rev3, IDF patch with **200 kbit/s** (27 frames/tick), ~2 min | 0 / 0 / 0 | 44,018–44,076 Hz | 24 % | 46 % | — | **slight crackling** |
| Rev3 board, build for rev3, **without** IDF patch (328 kbit/s, 21 frames/tick) | 3 lost sync in the first 1.5 min, then 0 | 43,824–44,427 Hz | 13–14 % | 42–44 % | — | **small dropouts audible** |

Findings:
- The rev3 board plays cleanly with the IDF patch.
- No measurable CPU gain from dropping the PSRAM workaround: the difference between the two patched runs sits almost
  entirely in the WiFi task and the Sendspin receive task (httpd), i.e. varying network traffic, not in the
  Bluetooth tasks. A repeat run would be needed to separate this. The rev3 build leaves 9 KB more internal RAM.
- Going below 229 kbit/s does not help: at 200 kbit/s slight crackling was audible while 229 kbit/s sounded
  clean in the same session. 229 kbit/s stays (single short listening tests, so the difference may be chance).
- The IDF patch is still needed on revision 3: at 328 kbit/s the radio link drops audio audibly, and Bluetooth
  takes noticeably more CPU on core 0 (idle 13 % instead of 21 %).

## Phase 11: Pairing from the web interface, ESPHome on the rev3 board (2026-10-17)

ESPHome variant flashed on the rev3 board for the first time (build for rev1, IDF patch 229 kbit/s, 27 frames/tick).
New: "Search speakers" button, "Speaker" select with the speakers found, "Unpair speaker" button, "Log level"
select (INFO after boot).

| Test | Result |
|---|---|
| Search with a JBL GO and a Sony SRS-XB100 in pairing mode | found ✔; a phone nearby is filtered out (class of device not audio/video) ✔ |
| Choose the JBL GO, then unpair, then choose it again | paired and connected in ~4 s ✔; unpair disconnects, no reconnect ✔ |
| Choose the Sony | paired and connected ✔ (24 s: the first connect after the Bluetooth restart fails with `ESP_ERR_INVALID_STATE`) |
| Found speakers in the web interface | only after a page reload (the web interface reads select options when loading) → `web/pairing.js` reloads the page when the search ends ✔ |
| Volume buttons on the JBL GO | reported value stuck at 13 %: the immediate echo from the AVRCP callback pinned the speaker's volume. Without it the steps 6 → 0 → 6 → 13 → 20 % reach the media player ✔. The echo now only goes through `on_volume` → media player. **Not yet re-tested on the Sony**, which needs the echo |
| JBL GO, ~25 min of music | 9 stream stalls (watchdog), pull rate partly 30–44 kHz; one cause found: choosing a speaker right after a search by name connected without the Bluetooth restart that an inquiry needs → fixed. Stalls also occur without a preceding inquiry |
| Sony XB100, ~70 min of music, same build | 0 stalls, 0 underruns, 0 lost sync, pull rate 43,523–44,604 Hz → the stalls are specific to the JBL GO |
| Listening, Sony | **short dropouts**, nothing in the log |
| Same, browser closed and log level ERROR | dropouts remain |
| Same, compiled for speed (`-O2`) and `post_connect_roaming: false` | dropouts remain (flash 98 % full → `-O2` reverted, roaming stays off) |
| Native build of 2026-10-16 that played cleanly then, same board, same speaker | **dropouts as well** |

Findings:
- The dropouts are not caused by ESPHome or by the pairing changes: the native build that was clean on 2026-10-16
  (21:00, WiFi channel 11, RSSI -66 dBm) drops out today as well (13:00, channel 11, RSSI -68 dBm). Most likely
  the 2.4 GHz band is busier during the day; losses on the Bluetooth link show in no log. Not verified.
- Core 0 idle with ESPHome on the rev3 board: 7–12 % (native: 21–29 %); the Bluetooth controller takes 16 % instead
  of 11–12 % with identical Bluetooth settings.
- After a Bluetooth restart the first connect fails with `ESP_ERR_INVALID_STATE`; it is now retried after 1 s
  instead of the 15 s reconnect interval.

## Phase 12: Native variant brought to feature parity (2026-10-17/18)

Both variants are to keep the same feature set. The native variant got the Bluetooth changes of phase 11 (search
with audio device filter, pairing a found speaker with a Bluetooth restart first, unpair persisted, re-pair by
name, quick retry after `ESP_ERR_INVALID_STATE`, no immediate volume echo) and its own web interface on port 80
(`main/web_ui.*`): track info (Sendspin metadata role), play/pause/next/previous, volume, Bluetooth status and
pairing, WiFi signal, free internal RAM, log level, log. The volume is now stored like in ESPHome. Rev3 board, Sony
SRS-XB100.

| Test | Result |
|---|---|
| Web server next to Sendspin's httpd | first start: Sendspin failed (`error in creating ctrl socket`), because the web interface took control port 32769, which Sendspin uses to make room for a web server → web interface on the default port, both run ✔ |
| Search via the web interface | runs, Bluetooth restart afterwards ✔ |
| Status: title/artist/album, volume, RSSI, free RAM | ✔ |
| Log level DEBUG for all tags | floods the log with httpd per-packet lines, followed by `BT_L2CAP: l2cab is_cong_cback_context` → DEBUG only for the application's tags (`a2dp`, `main`, `web_ui`), libraries stay at INFO ✔ |
| Playback buttons, volume slider (also following the speaker's buttons), search and pair from the page | ✔ (confirmed by the user) |
| Free internal RAM | ~36–41 KB → 26–28 KB (min 25 KB) with the web server; about the old WROVER's level. Task stack in PSRAM rejected (risky on rev1) |
| Long run | ~20 h without crash; pull rate 44,302 Hz at the end |

## Phase 13: JBL GO stream stalls caused by WiFi/Bluetooth coexistence (2026-10-18)

Native variant, rev3 board, JBL GO. The stream watchdog (phase 9) fired every 2.5–6 min, while playing and
while idle (keep alive silence). Each time the same pattern: L2CAP congestion (the speaker stops acknowledging),
no audio pulled for 3 s, then HCI disconnect reason 0x08 (connection timeout, not the local 0x16), so the speaker
really stopped answering on the air. It answered the next page right away; the watchdog reconnected within
2–4 s (a second stall within 5 min restarts the Bluetooth stack, 4–6 s). Once a page timed out and the next
attempt only came after the 15 s reconnect interval (~25 s of silence).

| Test (JBL idle unless noted) | Stalls |
|---|---|
| Normal build (IDF patch 229 kbit/s, 27 frames/tick), long uptime | every 2.5–6 min, also during playback |
| Speaker 10 cm from the board, right after a power-on reset | first after 3 min, same pattern |
| Without IDF patch (328 kbit/s, 21 frames/tick) | after 6 min, then 2.5 min and 0.5 min |
| **WiFi stopped** 30 s after connecting (`esp_wifi_stop`) | **none in 23.5 min**; callback max 26 µs instead of 1–5 ms |
| `esp_coex_preference_set(ESP_COEX_PREFER_BT)` | worse: 2 within 3 min |
| `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` | first after 12.5 min, the next 24 s later |

Findings:

- The trigger is the ESP32 sharing its radio between WiFi and Bluetooth. Not distance, uptime, stack state, the
  IDF patch or CPU load. The Sony SRS-XB100 handles the same build without stalls.
- The stalls start evenly spread over the 10 s Sendspin time sync cycle, not during its high performance bursts,
  so syncing less often would not help; any WiFi activity seems to be enough.
- The coexistence preference is not a fix in either direction. The time sync error is ~2 ms right after boot and
  settles to ~0.7 ms within 10 min in both cases (boot settling, not the preference).
- No simple fix found; the watchdog keeps the JBL usable, the Sony is the recommended speaker. All test code was
  removed again.
- Side fix: the task load line printed loads of over 14000 % after a Bluetooth restart (recreated tasks reuse
  the old address while their run time counter restarts at zero).

## Phase 14: Faster recovery, Rapoo A300, log streaming as a dropout source (2026-10-18)

Recovery after a lost connection (both variants, tested on the rev3 board with the JBL GO, ESPHome and native):

| Change | Result |
|---|---|
| Quick reconnect: after an established connection drops, up to 3 attempts every 3 s, then the 15 s interval | speaker off/on: attempt at +3 s, page timeout, next attempt 3 s later, connected; no more ~25 s gaps |
| Stack restart only if the disconnect is not confirmed, a restarted stream never pulls or connecting fails repeatedly (instead of on every second stall within 5 min) | JBL: 7 stalls in 21 min, all recovered with a plain reconnect, 7–9.5 s of silence (before: 7–10 s, and each restart blocked ESPHome's main loop for 2.8 s) |
| Watchdog 2 s instead of 3 s | no gain: from the last pull to the confirmed disconnect it takes ~4 s either way (the controller waits for the speaker), the disconnect just took 2 s instead of 1–1.5 s → back to 3 s |

On ESPHome the JBL GO stalls more often than on native (every ~2–6 min, clustered right after reconnects), which
fits ESPHome's additional WiFi traffic (API, web server).

New speaker Rapoo A300 (ESPHome): no stalls. Short audible dropouts while the web interface was open with the log
at DEBUG, with a clean log (no underruns, pull rate ~44.1 kHz):

| Condition | WiFi task | httpd | Callback max | Listening |
|---|---|---|---|---|
| Web page open, log DEBUG | 32 % | 15 % | 218–229 ms | short dropouts |
| Page closed, log INFO | – | – | 57–97 ms (DEBUG stats right after closing) | no dropouts, no reconnects |

Streaming every log line over WiFi takes enough airtime from Bluetooth for audible dropouts. Both variants now set
the log level back to INFO 10 min after DEBUG was chosen (ESPHome: script on the logger select; native: esp_timer
in the web interface, with a hint on the page).

## Quirks of the JBL GO

- Supports absolute volume, but an immediate echo of its volume notification keeps its volume from changing.
- Frequent stream stalls (the stack stops pulling audio for 3 s) with the same build that runs cleanly with the Sony;
  caused by WiFi/Bluetooth coexistence on the ESP32, none with WiFi off (phase 13).

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

## Quirks of the Rapoo A300

- No stream stalls with WiFi/Bluetooth coexistence (unlike the JBL GO).
- Audible dropouts while the web interface streams the DEBUG log (phase 14).
