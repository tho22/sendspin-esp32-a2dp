# Sendspin → ESP32 → Bluetooth-Box

Firmware, die einen ESP32 (WROVER) als Sendspin-Player in Music Assistant anmeldet und das Audio per Bluetooth
A2DP an eine Box (getestet mit Sony SRS-XB100) weitergibt.

```
Music Assistant ──Sendspin (WLAN, FLAC 44,1 kHz)──▶ ESP32-WROVER ──A2DP/SBC 229 kbit/s──▶ SRS-XB100
```

Es gibt zwei Varianten:

| Variante | Ordner | Stand |
|---|---|---|
| **Native ESP-IDF 5.5 (empfohlen)** | `native/` | läuft ohne hörbare Aussetzer |
| ESPHome | `sendspin-bt-wrover.yaml`, `components/a2dp_source/` | läuft, aber mit gelegentlichen Aussetzern (alte Netzwerk-Einstellungen) |

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
- Die ESPHome-Variante nutzt noch die alten Netzwerk-Einstellungen und hat keine Tastensteuerung.

## Bedienung über die Box (native Variante)

Über AVRCP: Play/Pause an der Box startet bzw. pausiert die Wiedergabe in Music Assistant, +/− regeln die
Lautstärke. Unterstützt die Box „Absolute Volume“ (z. B. SRS-XB100), regelt die Box selbst und Lautstärke-
änderungen werden in beide Richtungen mit Music Assistant abgeglichen; sonst regelt der ESP digital in 5-%-Schritten.

Alle Messungen und Hörtests: [docs/TESTPROTOKOLL.md](docs/TESTPROTOKOLL.md)

## Lizenz

Apache License 2.0, siehe [LICENSE](LICENSE). Copyright 2026 tho22.
