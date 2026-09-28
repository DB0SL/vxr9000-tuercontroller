# DB0SL Türcontroller

PlatformIO-Projekt für ein **ESP32 DevKitC V4 mit ESP32-WROOM-32** und Arduino-Framework. Die Ausgänge steuern über Optokoppler die bereits im VXR-9000 programmierten CW-Nachrichten. Der Controller erzeugt die Morsezeichen nicht selbst.

## Pinbelegung

| Funktion | DevKitC-Header | ESP32-GPIO | VXR-9000 DB25-Pin | Signal am ESP32 |
| --- | --- | ---: | ---: | --- |
| Türkontakt innen | J3-11, IO17 | 17 | – | HIGH = geschlossen, LOW = offen |
| Türkontakt außen | J3-12, IO16 | 16 | – | HIGH = geschlossen, LOW = offen |
| VXR-9000 `TX_ATT` | J2-7, IO32 | 32 | 5 | LOW = Sender aktiv |
| VXR-9000 `EXT_PTT` für Türansagen | J2-8, IO33 | 33 | 12 | HIGH = Optokoppler EIN, EXT_PTT nach GND |
| VXR-9000 PIO-2 → „INNEN“ | J2-9, IO25 | 25 | 17 | HIGH = Optokoppler EIN, PIO nach GND |
| VXR-9000 PIO-1 → „AUSSEN“ | J2-10, IO26 | 26 | 18 | HIGH = Optokoppler EIN, PIO nach GND |
| VXR-9000 PIO-3 → „DB0SL“ | J2-11, IO27 | 27 | 16 | HIGH = Optokoppler EIN, PIO nach GND |

Die Pinbelegung entspricht dem [Espressif-DevKitC-V4-Pinout](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html). GPIO 16/17 sind beim **WROOM-32** nutzbar; bei WROVER-Varianten sind sie belegt. GPIO 13 wurde gegenüber dem ersten Schaltplan durch GPIO 32 ersetzt, damit auch die Standardpins von HSPI (12/13/14/15) frei bleiben. Die Standardpins von VSPI (5/18/19/23) und I²C (21/22) bleiben ebenfalls frei. Das Eagle-Symbol heißt noch „ESP32-DEVKITC-32U“; für die hier genutzten Headeranschlüsse gilt die gleiche Belegung.

Die Eingangskanäle OK1–OK3 des [Schaltplans](Schaltplan/db0slcontroller.sch) haben je einen 470-Ω-Vorwiderstand an 5 V und einen 10-kΩ-Pull-up des Transistorausgangs nach 3,3 V. Der Firmware-Eingang nutzt zusätzlich `INPUT_PULLUP`. Die Firmware wertet bei den Türkontakten HIGH als geschlossen und LOW als offen; aktives `TX_ATT` bleibt LOW-aktiv. An ESP32-GPIO dürfen keine 5 V gelangen.

Die Ausgangskanäle für PIO-1 bis PIO-3 sind als **PC814** mit bidirektionalen Eingangs-LEDs ausgeführt. GPIO HIGH treibt über 470 Ω eine LED gegen GND; der Transistor verbindet den jeweiligen VXR-Eingang mit GND. Für `EXT_PTT` ist ein **vierter, gleich beschalteter PC814-Kanal** erforderlich: GPIO 33 über 470 Ω an den Optokoppler-Eingang, Transistor-Ausgang an DB25-Pin 12 und VXR-GND. `EXT_PTT` hat laut VXR-9000-Handbuch einen internen 5-V-Pull-up; dort ist kein externer Pull-up erforderlich. R8, R10 und R12 an den PIO-Eingängen werden ebenfalls nicht bestückt. Die bisherige NE555-Steuerung hat diese Eingänge rund 20 Jahre lang über Relaiskontakte ohne externe Pull-ups nach GND geschaltet.

PIO-1 und PIO-2 müssen am Funkgerät für die beiden Türtexte, PIO-3 für die Relaiskennung konfiguriert sein.

Im vorliegenden Schaltplan sind ESP32-GND, Optokoppler-GND und VXR-GND miteinander verbunden. Die Optokoppler bewirken hier deshalb **keine galvanische Trennung** zwischen ESP32 und Funkgerät. Das DevKitC darf entweder über USB oder über seinen 5-V-Pin versorgt werden; [Espressif warnt vor gleichzeitiger Speisung über mehrere Versorgungswege](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html).

## Verhalten

- Beim Einschalten wird der aktuelle Türstatus gelesen. Eine offene Tür wird sofort für die Ansage vorgemerkt; ausgelöst wird sie erst, wenn das Relais frei ist.
- Solange eine Tür offen bleibt, wird ihre Ansage alle `DOOR_REPEAT_INTERVAL_MS` erneut angefordert (Vorgabe: 30 000 ms). Beide Türen haben getrennte Timer; bei gleichzeitiger Öffnung wechseln sich die Ansagen ab. Ist `TX_ATT` aktiv, wartet der Controller und fasst ausstehende Wiederholungen je Tür zusammen.
- Beim Schließen einer Tür verfallen noch ausstehende Ansagen dieser Tür. Die Relaiskennung ist unabhängig vom Türzustand: PIO-3 wird nur bei aktivem `TX_ATT` ausgelöst, sobald seit der letzten PIO-3-Auslösung `RELAY_ID_INTERVAL_MS` vergangen sind (Vorgabe: 10 Minuten). Inaktive Zeiten zählen mit. Ist die Kennung bei Wiederaufnahme des Sendebetriebs bereits fällig, wird sie sofort ausgelöst. Nach einem Controller-Neustart gilt sie bei der ersten aktiven Sendung als fällig.
- Für eine Türansage werden der zugehörige Eingang PIO-1 oder PIO-2 und `EXT_PTT` gleichzeitig aktiviert und für `DOOR_ANNOUNCEMENT_HOLD_MS` gehalten (Vorgabe: 11 000 ms). Danach werden beide Ausgänge freigegeben.
- Türansagen beginnen nur, wenn `TX_ATT` inaktiv ist und mindestens 500 ms Sendepause vergangen sind. Während Sprache, Relaiskennung oder einer anderen Sendung bleiben PIO-1, PIO-2 und `EXT_PTT` inaktiv; eine fällige Türansage bleibt vorgemerkt. Gleichzeitig wird nur ein Türtext ausgelöst.
- PIO-3 für die Relaiskennung bleibt ein 250-ms-Impuls und steuert `EXT_PTT` nicht. Die Relaiskennung darf während einer bereits laufenden Sendung ausgelöst werden.
- Auf `tuerstatus` wird ein **retained** JSON-Payload veröffentlicht, zum Beispiel `{"innen":"offen","aussen":"geschlossen"}`. Das geschieht nach jeder entprellten Statusänderung, nach MQTT-Wiederverbindung und spätestens nach `MQTT_STATUS_INTERVAL_MS` bei bestehender Verbindung (Vorgabe: 10 Minuten). Während einer Netzstörung wird der letzte aktuelle Zustand nach Wiederverbindung veröffentlicht.
- Die Türkontakte werden 40 ms entprellt. Alle laufenden Zeiten werden mit `millis()` verwaltet; es gibt kein `delay()`.

## Betriebssicherheit und Fehlersuche

- Das WLAN wird beim Start eingerichtet. Bei Verbindungsverlust versucht der Controller alle 15 Sekunden eine Wiederverbindung. Nach `WIFI_MAX_ATTEMPTS` erfolglosen Versuchen (Vorgabe: 20; etwa fünf Minuten) startet das Board neu und beginnt die Versuche erneut. Ein erneutes `WiFi.begin()` während eines laufenden Verbindungsversuchs wird vermieden.
- Bei unterbrochener MQTT-Verbindung wird alle 5 Sekunden erneut verbunden. Nach `MQTT_MAX_ATTEMPTS` fehlgeschlagenen Verbindungsversuchen (Vorgabe: 20) startet das Board neu. Der TCP-Verbindungsversuch ist auf 3 Sekunden, der MQTT-Leseversuch auf 2 Sekunden begrenzt. Nach erfolgreicher Verbindung wird der aktuelle Türstatus sofort erneut auf `tuerstatus` veröffentlicht. Während ein VXR-Ausgang aktiv gehalten wird, beginnt kein neuer MQTT-Verbindungsversuch.
- Ein Task-Watchdog überwacht den Programm-Loop. Bleibt er länger als `WATCHDOG_TIMEOUT_MS` stehen (Vorgabe: 30 Sekunden, mindestens 10 Sekunden), startet der ESP32 neu. Nach dem Neustart liest der Controller die Eingänge neu und setzt die Ausgänge zunächst auf LOW.
- Der serielle Monitor (115200 Baud) zeigt den Neustartgrund einschließlich eines durch WLAN oder MQTT ausgelösten Neustarts, den Watchdog-Status sowie die Verbindungsversuche. Für eine Vor-Ort-Prüfung WLAN oder Broker kurz abschalten und die Wiederverbindung sowie die erneute MQTT-Meldung beobachten.

## Konfiguration und Bedienung

1. `include/config.example.h` als `include/config.h` kopieren und dort WLAN-SSID, WLAN-Passwort und MQTT-Brokeradresse eintragen. Falls der Broker keine Anmeldung verlangt, bleiben `MQTT_USER` und `MQTT_PASSWORD` leer. `DOOR_REPEAT_INTERVAL_MS`, `DOOR_ANNOUNCEMENT_HOLD_MS`, `RELAY_ID_INTERVAL_MS`, `MQTT_STATUS_INTERVAL_MS` sowie optional `WATCHDOG_TIMEOUT_MS`, `WIFI_MAX_ATTEMPTS` und `MQTT_MAX_ATTEMPTS` können dort angepasst werden; Intervalle sind in Millisekunden, die Verbindungsgrenzen sind Anzahlen. `include/config.h` wird durch `.gitignore` nicht eingecheckt.
2. Mit `./build.sh build` kompilieren und linken. Solange das WLAN-Passwort leer ist, wird bei einem geschützten WLAN keine Verbindung aufgebaut.
3. Mit `./build.sh all` bauen, auf das ESP32 DevKitC V4 flashen und anschließend den seriellen Monitor mit 115200 Baud öffnen. Bei mehreren USB-Seriell-Adaptern: `PORT=/dev/ttyUSB0 ./build.sh all`.

Voraussetzung: PlatformIO CLI (`pio`) im `PATH`. Ohne angeschlossenes Board kann der Upload und Monitor nicht geprüft werden. Der MQTT-Broker verwendet hier unverschlüsseltes MQTT über TCP (Vorgabe Port 1883); er sollte im vertrauenswürdigen Netz stehen.
