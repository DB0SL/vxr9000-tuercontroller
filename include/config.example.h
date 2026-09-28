#pragma once

// Als include/config.h kopieren und dort die lokalen Zugangsdaten eintragen.
#define WIFI_SSID "WLAN-NAME"
#define WIFI_PASSWORD ""
#define MQTT_HOST "192.0.2.1"
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASSWORD ""

// Wiederholung der Ansage, solange die jeweilige Tuer offen ist.
#define DOOR_REPEAT_INTERVAL_MS 30000UL

// PIO-1/PIO-2 und EXT_PTT fuer die komplette Tueransage aktiv halten.
#define DOOR_ANNOUNCEMENT_HOLD_MS 11000UL

// 600000 ms = 10 Minuten. Die Kennungsfrist laeuft ab der letzten
// PIO-3-Ausloesung, auch wenn der Sender zwischendurch inaktiv ist.
// Beide Intervalle koennen getrennt eingestellt werden.
#define RELAY_ID_INTERVAL_MS 600000UL
#define MQTT_STATUS_INTERVAL_MS 600000UL

// Loop-Watchdog: blockiert der Programm-Loop laenger, startet der ESP32 neu.
#define WATCHDOG_TIMEOUT_MS 30000UL

// Nach so vielen erfolglosen Versuchen startet das Board neu.
#define WIFI_MAX_ATTEMPTS 20U
#define MQTT_MAX_ATTEMPTS 20U
