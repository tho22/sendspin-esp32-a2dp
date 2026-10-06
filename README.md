# Sendspin → ESP32 → Bluetooth-Box

*[English version below](#english)*

Firmware, die einen ESP32 (WROVER) als Sendspin-Player in Music Assistant anmeldet und das Audio per Bluetooth
A2DP an eine Box (getestet mit Sony SRS-XB100) weitergibt.

```
Music Assistant ──Sendspin (WLAN, FLAC 44,1 kHz)──▶ ESP32-WROVER ──A2DP/SBC 229 kbit/s──▶ SRS-XB100
```

Es gibt zwei Varianten:

| Variante | Ordner | Stand |
|---|---|---|
| **Native ESP-IDF 5.5 (empfohlen)** | `native/` | läuft ohne hörbare Aussetzer |
| ESPHome | `sendspin-bt-wrover.yaml`, `components/a2dp_source/` | läuft ohne hörbare Aussetzer, Tastensteuerung über AVRCP (Kurztest) |

Nur der originale ESP32 hat Bluetooth Classic; ESP32-S3/C3/C6 funktionieren **nicht**.

## Gemeinsame Voraussetzung: ESP-IDF-Patch

Beide Varianten nutzen die ESP-IDF-5.5.5-Installation von ESPHome und brauchen einen Patch des A2DP-Stacks:

```bash
.venv/bin/esphome compile sendspin-bt-wrover.yaml   # einmal, lädt ESP-IDF herunter
scripts/patch-idf-sbc-bitrate.sh                    # 229 kbit/s, 27 Frames pro Takt
```

Der Patch ändert die gemeinsame Kopie unter `~/.cache/esphome/idf/frameworks/5.5.5/` (betrifft alle
ESPHome-Projekte auf dem Rechner). Rückgängig: `scripts/patch-idf-sbc-bitrate.sh --revert`. Nach einem
ESPHome-Update mit neuer IDF-Version erneut anwenden.

## Native Variante (`native/`)

- `main/a2dp_output.*`: A2DP-Source (Verbindung, Abspieluhr, Keep-Alive, Statistik)
- `main/main.cpp`: WLAN, mDNS (`_sendspin._tcp`), NVS, Sendspin-Client mit Player-Rolle
- `sdkconfig.defaults`: komplette Konfiguration mit Begründungen, `main/Kconfig.projbuild`: Optionen

Inbetriebnahme:

1. `secrets.yaml` im Projektordner mit `wifi_ssid`/`wifi_password` füllen (Vorlage: `secrets.yaml.example`).
2. SRS-XB100 in den Pairing-Modus versetzen, dann bauen und flashen:
   ```bash
   native/build.sh -p /dev/ttyUSB0 build flash
   ```
   Der ESP sucht die Box per Name, koppelt sie und verbindet sie danach bei jedem Start automatisch.
3. In Music Assistant auf „Sendspin BT 1“ abspielen. Static Delay ca. 250 ms (Startwert, die Box meldet ihn selbst).

Log ansehen: `.venv/bin/python native/serial_log.py /dev/ttyUSB0 600 --reset`
A/B-Tests: Werte in `native/sdkconfig.test` überschreiben, dann `native/run_test.sh NAME` (baut, flasht, loggt
13 min und wertet mit `native/analyze_log.py` aus).

## Warum diese Einstellungen

| Maßnahme | Grund |
|---|---|
| SBC 229 statt 328 kbit/s (IDF-Patch) | WLAN und Bluetooth teilen sich einen Funkteil; bei 328 kbit/s schaffte die Strecke nur ~78 % Echtzeit |
| Nachhol-Limit 27 statt 21 Frames/Takt (IDF-Patch) | Kommt der Bluetooth-Task verspätet dran, holte der Stack den Rückstand nie auf → zu wenig Audio → Box läuft leer |
| WLAN: Empfangs-Aggregation mit BA-Fenster 6, kein TX-AMPDU, Puffer 12/40, TCP-Fenster 32 KB | Kompromiss aus A/B-Tests: ohne Aggregation staut sich der TCP-Strom und Sendspins Zeitsynchronisierung bricht (nur Stille); mit ESPHomes langen Bursts (BA 32, 16/64, 64 KB) stört das WLAN die A2DP-Übertragung (Aussetzer) |
| lwIP-Empfangs-Mailboxen 64 (Standard 6) | sonst verwirft lwIP Segmente, TCP-Neuübertragungen blähen die Laufzeit der Zeit-Nachrichten auf |
| Sendspin-Puffer 150 KB | 64 KB führten schon zu vereinzelten Aussetzern |
| Modem-Sleep nur außerhalb des Streamings | Sendspin fordert während des Streamings „High Performance“ an |
| Kein Koexistenz-Hinweis `A2DP_STREAMING` | gibt Bluetooth Vorrang und verschlechtert die WLAN-Laufzeiten massiv |
| Hauptschleife, Sendspin-Threads, TCP/IP auf Core 1 | Core 0 bleibt für WLAN- und Bluetooth-Stack frei |
| Abspielmeldung an Sendspin aus eigenem Task, Ringpuffer im internen RAM | Der Daten-Callback läuft im zeitkritischen Bluetooth-Task und darf nicht blockieren |
| Gleichmäßige Abspieluhr statt Abrufzeitpunkte | Sendspin korrigiert ab 5 ms hart; der Stack holt in unregelmäßigen Schüben ab |
| Keep-Alive (Stille streamen) | Die SRS-XB100 schaltet sich sonst ohne Audio nach einiger Zeit ab |
| Nur ausgehende Bluetooth-Verbindungen, Timeout, Neustart des Stacks nach 4 Fehlversuchen | verhindert halb offene Verbindungen („Conn Exists“) |

## Diagnose

Alle 30 s erscheint `Stats:` mit Abholrate (Soll ≈ 44100 Hz), Uhr-Abweichung, Datenrate von Sendspin,
Spitzenpegel (−90 dBFS = nur Stille) und internem RAM, dazu `Load:` mit der CPU-Last pro Task.
`Lost sync` von Sendspin zeigt harte Korrekturen, `Time sync error` die Güte der Zeitsynchronisierung.

## Bekannte Grenzen

- Getestet auf ESP32 rev1 (WROVER-B); der PSRAM-Workaround dieser Revision kostet viel CPU. Ein ESP32 mit
  Chip-Revision ≥ 3 (z. B. ESP32-DevKitC-VE mit WROVER-E) dürfte mehr Reserve haben.
- Keine OTA-Updates in der nativen Variante (Flashen per USB).
- Die ESPHome-Variante ist bisher nur kurz getestet (ca. 1,5 min Log nach Stream-Start, Hörtest ohne Aussetzer)
  und braucht auf Core 0 mehr CPU als die native Variante.
- Nur die ESPHome-Variante erneuert eine abgelehnte Kopplung automatisch (siehe unten).

## Bedienung über die Box

Beide Varianten, über AVRCP: Play/Pause an der Box startet bzw. pausiert die Wiedergabe in Music Assistant, +/− regeln die
Lautstärke. Unterstützt die Box „Absolute Volume“ (z. B. SRS-XB100), regelt die Box selbst und Lautstärke-
änderungen werden in beide Richtungen mit Music Assistant abgeglichen; sonst regelt der ESP digital in 5-%-Schritten.
In der ESPHome-Variante lösen die Tasten Trigger aus (`on_play_pause`, `on_next`, `on_previous`, `on_stop`,
`on_volume`), die in `sendspin-bt-wrover.yaml` mit dem Media Player verbunden sind.

Lehnt die Box die Verbindung ab, obwohl sie erreichbar ist (z. B. weil sie inzwischen mit der anderen Variante
gekoppelt wurde), löscht die ESPHome-Variante nach 2 Versuchen die alte Kopplung und koppelt neu.

Alle Messungen und Hörtests: [docs/TESTPROTOKOLL.md](docs/TESTPROTOKOLL.md)

## Lizenz

Apache License 2.0, siehe [LICENSE](LICENSE). Copyright 2026 tho22.

---

<a id="english"></a>

# Sendspin → ESP32 → Bluetooth speaker (English)

Firmware that registers an ESP32 (WROVER) as a Sendspin player in Music Assistant and forwards the audio via
Bluetooth A2DP to a speaker (tested with a Sony SRS-XB100).

```
Music Assistant ──Sendspin (WiFi, FLAC 44.1 kHz)──▶ ESP32-WROVER ──A2DP/SBC 229 kbit/s──▶ SRS-XB100
```

There are two variants:

| Variant | Location | Status |
|---|---|---|
| **Native ESP-IDF 5.5 (recommended)** | `native/` | plays without audible dropouts |
| ESPHome | `sendspin-bt-wrover.yaml`, `components/a2dp_source/` | plays without audible dropouts, speaker buttons via AVRCP (short test) |

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

## Native variant (`native/`)

- `main/a2dp_output.*`: A2DP source (connection, playback clock, keep alive, statistics, AVRCP)
- `main/main.cpp`: WiFi, mDNS (`_sendspin._tcp`), NVS, Sendspin client with player and controller roles
- `sdkconfig.defaults`: complete configuration with rationale, `main/Kconfig.projbuild`: options

Getting started:

1. Fill `secrets.yaml` in the project folder with `wifi_ssid`/`wifi_password` (template: `secrets.yaml.example`).
2. Put the SRS-XB100 into pairing mode, then build and flash:
   ```bash
   native/build.sh -p /dev/ttyUSB0 build flash
   ```
   The ESP searches for the speaker by name, pairs it and reconnects it automatically on every start.
3. Play to "Sendspin BT 1" in Music Assistant. Static delay about 250 ms (initial value, reported by the speaker).

View the log: `.venv/bin/python native/serial_log.py /dev/ttyUSB0 600 --reset`
A/B tests: override values in `native/sdkconfig.test`, then `native/run_test.sh NAME` (builds, flashes, logs for
13 min and evaluates with `native/analyze_log.py`).

## Why these settings

| Measure | Reason |
|---|---|
| SBC 229 instead of 328 kbit/s (IDF patch) | WiFi and Bluetooth share one radio; at 328 kbit/s the link only managed ~78 % of real time |
| Catch-up limit 27 instead of 21 frames per tick (IDF patch) | When the Bluetooth task runs late, the stack never caught up → too little audio → the speaker runs dry |
| WiFi: receive aggregation with BA window 6, no TX AMPDU, buffers 12/40, TCP window 32 KB | Compromise from A/B runs: without aggregation the TCP stream backs up and Sendspin's clock sync breaks (silence only); with ESPHome's long bursts (BA 32, 16/64, 64 KB) WiFi disturbs A2DP (dropouts) |
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

Every 30 s the log shows `Stats:` with the pull rate (target ≈ 44100 Hz), clock deviation, data rate from Sendspin,
peak level (−90 dBFS = silence only) and internal RAM, plus `Load:` with the CPU load per task.
`Lost sync` from Sendspin shows hard corrections, `Time sync error` the quality of the clock sync.

## Known limitations

- Tested on ESP32 rev1 (WROVER-B); the PSRAM workaround of this revision costs a lot of CPU. An ESP32 with chip
  revision ≥ 3 (e.g. ESP32-DevKitC-VE with WROVER-E) should have more headroom.
- No OTA updates in the native variant (flash via USB).
- The ESPHome variant has only been tested briefly (about 1.5 min of log after stream start, listening test without
  dropouts) and needs more CPU on core 0 than the native variant.
- Only the ESPHome variant renews a rejected pairing automatically (see below).

## Speaker controls

Both variants, via AVRCP: play/pause on the speaker starts or pauses playback in Music Assistant, +/− change the volume. If the
speaker supports absolute volume (e.g. SRS-XB100), it applies the volume itself and changes are synced with
Music Assistant in both directions; otherwise the ESP applies digital volume in 5 % steps.
In the ESPHome variant the buttons fire triggers (`on_play_pause`, `on_next`, `on_previous`, `on_stop`,
`on_volume`) that `sendspin-bt-wrover.yaml` connects to the media player.

If the speaker is reachable but refuses the connection (e.g. because it was paired with the other variant in the
meantime), the ESPHome variant drops the old pairing after 2 attempts and pairs again.

All measurements and listening tests (German): [docs/TESTPROTOKOLL.md](docs/TESTPROTOKOLL.md)

## License

Apache License 2.0, see [LICENSE](LICENSE). Copyright 2026 tho22.
