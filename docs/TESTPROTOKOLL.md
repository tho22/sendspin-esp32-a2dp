# Testprotokoll: Sendspin → ESP32 → Bluetooth-Box

Stand: 06.10.2026. Alle Tests mit derselben Hardware und Umgebung.

## Testumgebung

| | |
|---|---|
| ESP | ESP32-WROVER-B, Chip ESP32-D0WDQ6 **Revision 1.0**, 4 MB Flash, 4 MB PSRAM (Cache-Workaround aktiv) |
| Box | Sony SRS-XB100 (A2DP-Sink, SBC, Bitpool 2–49, meldet 250 ms Verzögerung, unterstützt AVRCP Absolute Volume) |
| Server | Music Assistant mit Sendspin, Quelle: FLAC 44,1 kHz/16 bit (Musik bzw. Radiostream) |
| Funk | ESP neben dem PC (kein USB 3), Box 20 cm entfernt, WLAN-AP in Sichtweite < 5 m |
| Software | ESPHome 2026.9.1 bzw. natives ESP-IDF 5.5.5, sendspin-cpp 0.7.2 |
| Referenz | squeezelite-esp32 (ESP-IDF 4.3.5) auf identischer Hardware: ohne Aussetzer |

## Messmethodik

- Serielles Log mit Zeitstempeln (`native/serial_log.py`), Auswertung mit `native/analyze_log.py` ab 120 s nach
  Stream-Start (Einschwingen ausgeklammert).
- Kennzahlen: Sendspin `Lost sync`/`Hard sync`, Puffer-Aussetzer zur Box, Abholrate des A2DP-Stacks (Soll 44100 Hz),
  halbe Laufzeit der Zeitsynchronisierung (`best max_error`), Spitzenpegel am Ein-/Ausgang, CPU-Last pro Task.
- Hörtest durch den Nutzer: Aussetzer auf der Funkstrecke sind in den Logs nicht sichtbar.

## Phase 1: ESPHome-Variante

| # | Maßnahme | Messung | Hörbild |
|---|---|---|---|
| 1 | Erste Version (A2DP-Speaker-Komponente) | Abstürze beim Wiedergabestart, Pipeline hängt nach Stream-Wechsel | kurz Ton, dann aus |
| 2 | Abspielmeldung aus gleichmäßiger Uhr statt Abrufzeitpunkten | vorher ~190 `Lost sync` in 5 min (10–90 ms Jitter) | |
| 3 | Kein Task-Stack im PSRAM (rev1) | Absturz in `SpscRingBuffer::acquire` behoben | |
| 4 | SBC 328 kbit/s (Standard) | Abholrate 34 490 Hz (78 %) | sehr zerhackt |
| 5 | SBC 229 kbit/s | 43 500 Hz | besser |
| 6 | SBC 200 / 170 kbit/s | 43 300–44 200 / 42 200–44 500 Hz | kein weiterer Gewinn |
| 7 | Trace „Limit frms to send from 40 to 21“ (1581× in 2 min) → Nachhol-Limit 21 → 27 | danach > 3 min ohne Korrektur, 44 100–44 300 Hz | viel besser, kleine Aussetzer |
| 8 | Keep-Alive (Stille streamen) | Box bleibt an | |
| 9 | Lastmessung pro Task | Core 0 Leerlauf **3–4 %** (WLAN 29 %, BT-Stack 23 %, BT-Controller 20 %, BTU 14 %) | |
| 10 | Callback-Profiling | Ø 500 µs, max 72 ms je Aufruf | |
| 11 | Abspielmeldung über eigenen Task (Core 1) | Ø 300 µs | |
| 12 | Ringpuffer im internen RAM (200 ms) | Ø 175 µs, Spitzen 50–70 ms bleiben (Preemption) | unverändert |
| 13 | WLAN-Treiber auf Core 1 | Core 0 Leerlauf 11–17 %, aber Decoder verhungert (Aussetzer bis 3,4 s) | schlechter |
| 14 | WLAN-Puffer im internen RAM | Core 0 Leerlauf **50 %**, aber RAM erschöpft → Absturz `fixed_queue_new` | — |
| 15 | **Testton 441 Hz ohne WLAN-Verkehr** | — | **sauber** → Ursache ist die WLAN-Koexistenz |
| 16 | Sendspin-Puffer 150 KB, TCP-Fenster 16 KB | keine Puffer-Aussetzer | weniger Aussetzer |
| 17 | Sendspin-Puffer 75 KB, TCP-Fenster 8 KB | 60–70 `Lost sync`/min, 39 Aussetzer | schlechter |

Ergebnis Phase 1: Auf ESP-Seite sauber, auf der Funkstrecke blieben kleine Aussetzer.

## Phase 2: Native ESP-IDF-Variante (ohne ESPHome)

| # | Netzwerk-Konfiguration | Messung | Hörbild |
|---|---|---|---|
| 1 | Sparsam: WLAN-Puffer intern, kein AMPDU, TCP 16 KB | Datenrate von Sendspin nur 35–40 KB/s statt 176 KB/s | Soundfetzen |
| 2 | wie 1, TCP 32 KB | 176 KB/s, aber Pegel **−90 dBFS**, Zeitsync-Fehler ~440 ms | nichts |
| 3 | + Modem-Sleep während Streaming aus | Zeitsync-Fehler 440–560 ms | nichts |
| 4 | + Koexistenz-Hinweis `A2DP_STREAMING` entfernt | Laufzeit wächst 89 → 268 → 357 → 672 ms | nichts |
| 5 | Sendspin-Puffer 64 KB | Fehler 135–270 ms, Eingang −90 dBFS, „45 s zu spät“ | nichts |
| 6 | **Großzügig (wie ESPHome High Performance)**: AMPDU an, BA 32, 16/64, TCP 64 KB, WLAN-Puffer PSRAM | Laufzeit 7–27 ms, Eingang −2 dBFS | **Musik**, kleine Aussetzer |
| 7 | squeezelite-Netz (AMPDU aus, 12/40, 32 KB) + PSRAM | Laufzeit 300–730 ms, 146 Hard Syncs | Stille |
| 8 | wie 7 + AMPDU RX mit BA 6 | 250–510 ms, 98 `Lost sync` | Stille |
| 9 | wie 8 + lwIP-Mailboxen 64 (Standard 6) | anfangs 12–41 ms, nach ~6 min Timeouts, Sprünge ±3/12 s | bricht ab |

Erkenntnis: Ohne Empfangs-Aggregation und mit der lwIP-Standard-Mailbox (6) staut sich der TCP-Strom, Sendspins
Zeitsynchronisierung (über dieselbe Verbindung) läuft aus dem Ruder und Sendspin verwirft alles Audio als „zu spät“.

### A/B-Reihe ausgehend von Nr. 6 (je 13 min, ausgewertet ab 2 min nach Start)

| Lauf | Änderung (kumulativ) | Lost sync | Aussetzer | Zeitsync rtt/2 Median/Max | Core 0 Leerlauf | Abholrate min |
|---|---|---|---|---|---|---|
| Referenz | großzügig (Nr. 6) | 0 | 0 | 24 / 45 ms | 13 % | 44 051 Hz |
| run1 | TX-AMPDU aus, RX-BA 32 → 6 | 0 | 0 | 16 / 43 ms | 14 % | 44 050 Hz |
| **run2** | + WLAN-Puffer 12/40, TCP 32 KB, Sendepuffer 8 KB | **0** | **0** | 25 / 43 ms | 14 % | 44 043 Hz |
| run3 | + Sendspin-Puffer 64 KB | 2 | 3 | 22 / 43 ms | 14 % | 44 057 Hz |

**Hörtest run2: keine Aussetzer mehr.** run2 ist die finale Konfiguration (`native/sdkconfig.defaults`).
Kontrolllauf nach dem Aufräumen: 0 Lost sync, 0 Aussetzer, Abholrate ≥ 44 068 Hz.

## Phase 3: Steuerung über die Box (AVRCP)

| Test | Ergebnis |
|---|---|
| Absolute Volume erkannt | ja („Speaker supports absolute volume“), Lautstärke wird in der Box geregelt, kein Software-Gain |
| − an der Box | Stufen 48 % → 19 % jeweils an Music Assistant gemeldet ✔ |
| + an der Box | ✔ (Nutzerbestätigung) |
| Lautstärke in Music Assistant → Box | ✔ (Nutzerbestätigung) |
| Play/Pause-Taste | Code 0x46 (PAUSE) → Sendspin-Befehl PAUSE, Stream endet ✔; erneuter Druck startet Wiedergabe ✔ (Nutzerbestätigung) |
| Play beim Verbinden | wird in den ersten 3 s ignoriert (XB100 sendet von selbst Play) |

## Phase 4: ESPHome-Variante mit den neuen Netzwerk-Einstellungen (06.10.2026)

Übernommen aus der nativen Variante: WLAN-Kompromiss aus run2, Modem-Sleep nicht mehr erzwingen, kein
Koexistenz-Hinweis `A2DP_STREAMING`, AVRCP-Steuerung (Trigger `on_play_pause`, `on_next`, `on_previous`,
`on_stop`, `on_volume` → Media-Player-Aktionen).

| Test | Ergebnis |
|---|---|
| Wechsel native → ESPHome | Box lehnt den A2DP-Kanal ab (`BTA_AV_OPEN_EVT::FAILED status: 3`, 28×): veralteter Kopplungsschlüssel |
| Selbstheilung: Funkverbindung steht, A2DP abgelehnt → nach 2 Versuchen alte Kopplung löschen | neu gekoppelt **ohne** Pairing-Modus, verbunden, Stream läuft ✔ |
| Erster Hörtest (ohne Absolute Volume) | nichts zu hören: Box stand noch auf 19 % (aus der nativen Variante) plus digitale Absenkung → nach „+“ an der Box Ton ✔ |
| Absolute Volume erkannt | ja, Lautstärke wird in der Box geregelt ✔ |
| − an der Box | 28 % → 11 % jeweils an Media Player/Music Assistant ✔ |
| Play/Pause-Taste | Code 0x46 → Media Player IDLE (Pause), 4 s später 0x44 → PLAYING (über `media_player.toggle`) ✔ |
| + an der Box, Lautstärke aus Music Assistant | ✔ (Nutzerbestätigung) |
| Hörtest | **keine Aussetzer** (Nutzerbestätigung); Log: 0 Lost sync, 0 Aussetzer, Abholrate 44 076 Hz. Beobachtungsdauer nach Stream-Start nur ca. 1,5 min |
| CPU | Core 0: WLAN 29 %, BT-Stack 22 %, BT-Controller 19 %, BTU 17 % (höher als nativ), Core 1 Leerlauf 39 % |

## Eigenheiten der Sony SRS-XB100

- Meldet über A2DP Delay Reporting 250 ms Latenz (Startwert für Sendspins Static Delay).
- Verbindet sich nach dem Einschalten selbst; parallel zum eigenen Verbindungsaufbau des ESP führt das zu halb
  offenen Verbindungen („Conn Exists“) → ESP nimmt keine eingehenden Verbindungen an.
- Schaltet sich ohne Audio nach einiger Zeit ab → Keep-Alive (Stille streamen).
- Sendet direkt nach dem Verbinden Play und die eigene Lautstärke → 3 s ignorieren.
- Übernimmt Lautstärkeänderungen per Taste erst, wenn die Quelle sie zurückmeldet.
- Wurde sie mit einem anderen Gerät bzw. einer anderen Firmware neu gekoppelt, lehnt sie den alten Schlüssel ab,
  akzeptiert aber eine erneute Kopplung durch die Quelle auch ohne Pairing-Modus.
- Absolute Volume ist die Lautstärke der Box selbst; sie bleibt beim Wechsel der Firmware erhalten.
