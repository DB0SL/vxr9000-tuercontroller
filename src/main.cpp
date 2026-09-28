#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "relay_id_schedule.h"

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

#ifndef DOOR_REPEAT_INTERVAL_MS
#define DOOR_REPEAT_INTERVAL_MS 30000UL
#endif
#ifndef RELAY_ID_INTERVAL_MS
#define RELAY_ID_INTERVAL_MS 600000UL
#endif
#ifndef DOOR_ANNOUNCEMENT_HOLD_MS
#define DOOR_ANNOUNCEMENT_HOLD_MS 11000UL
#endif
#ifndef MQTT_STATUS_INTERVAL_MS
#define MQTT_STATUS_INTERVAL_MS 600000UL
#endif
#ifndef WATCHDOG_TIMEOUT_MS
#define WATCHDOG_TIMEOUT_MS 30000UL
#endif
#ifndef WIFI_MAX_ATTEMPTS
#define WIFI_MAX_ATTEMPTS 20U
#endif
#ifndef MQTT_MAX_ATTEMPTS
#define MQTT_MAX_ATTEMPTS 20U
#endif

namespace {

// ESP32-DevKitC V4 mit WROOM-32: herausgefuehrte GPIO, keine Boot-/Flash-Pins.
constexpr uint8_t PIN_DOOR_INSIDE = 17;   // IO17
constexpr uint8_t PIN_DOOR_OUTSIDE = 16;  // IO16
constexpr uint8_t PIN_TX_ATT = 32;        // IO32
constexpr uint8_t PIN_PIO_2 = 25;         // IO25: "INNEN"
constexpr uint8_t PIN_PIO_1 = 26;         // IO26: "AUSSEN"
constexpr uint8_t PIN_PIO_3 = 27;         // IO27: "DB0SL"
constexpr uint8_t PIN_EXT_PTT = 33;       // IO33: DB25-12, aktiv fuer Tueransagen

constexpr uint32_t DEBOUNCE_MS = 40;
constexpr uint32_t GPIO_DEBUG_INTERVAL_MS = 1000;
constexpr uint32_t ID_TRIGGER_PULSE_MS = 500;
constexpr uint32_t TX_IDLE_MS = 500;
constexpr uint32_t TX_START_TIMEOUT_MS = 10000;
constexpr uint32_t FAILED_TRIGGER_GAP_MS = 1000;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t MQTT_RETRY_MS = 5000;
constexpr uint32_t PUBLISH_RETRY_MS = 1000;
constexpr char MQTT_TOPIC[] = "tuerstatus";
constexpr uint32_t RESTART_WIFI_MARKER = 0xDB051001;
constexpr uint32_t RESTART_MQTT_MARKER = 0xDB051002;
RTC_NOINIT_ATTR uint32_t plannedRestart;
static_assert(DOOR_REPEAT_INTERVAL_MS > 0,
              "DOOR_REPEAT_INTERVAL_MS muss groesser als 0 sein");
static_assert(RELAY_ID_INTERVAL_MS > 0,
              "RELAY_ID_INTERVAL_MS muss groesser als 0 sein");
static_assert(DOOR_ANNOUNCEMENT_HOLD_MS > 0,
              "DOOR_ANNOUNCEMENT_HOLD_MS muss groesser als 0 sein");
static_assert(MQTT_STATUS_INTERVAL_MS > 0,
              "MQTT_STATUS_INTERVAL_MS muss groesser als 0 sein");
static_assert(WATCHDOG_TIMEOUT_MS >= 10000,
              "WATCHDOG_TIMEOUT_MS muss mindestens 10000 sein");
static_assert(WIFI_MAX_ATTEMPTS > 0 && MQTT_MAX_ATTEMPTS > 0,
              "Die maximalen Verbindungsversuche muessen groesser als 0 sein");

constexpr uint8_t WANT_INSIDE = 1u << 0;
constexpr uint8_t WANT_OUTSIDE = 1u << 1;
constexpr uint8_t WANT_ID = 1u << 2;

bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

struct DebouncedDoor {
  uint8_t pin;
  bool rawClosed = false;
  bool closed = false;
  uint32_t rawChangedAt = 0;

  explicit DebouncedDoor(uint8_t gpio) : pin(gpio) {}

  void begin(uint32_t now) {
    pinMode(pin, INPUT_PULLUP);
    // Gemessene Logik der Tuerkontakte: HIGH = geschlossen, LOW = offen.
    rawClosed = digitalRead(pin) == HIGH;
    closed = rawClosed;
    rawChangedAt = now;
  }

  bool update(uint32_t now) {
    const bool sampleClosed = digitalRead(pin) == HIGH;
    if (sampleClosed != rawClosed) {
      rawClosed = sampleClosed;
      rawChangedAt = now;
    }
    if (closed != rawClosed && elapsed(now, rawChangedAt, DEBOUNCE_MS)) {
      closed = rawClosed;
      return true;
    }
    return false;
  }
};

enum class RadioPhase : uint8_t {
  Idle,
  OutputActive,
  WaitForTx,
  WaitForTxEnd,
  FailedTriggerGap,
};

DebouncedDoor inside(PIN_DOOR_INSIDE);
DebouncedDoor outside(PIN_DOOR_OUTSIDE);
RadioPhase radioPhase = RadioPhase::Idle;
uint8_t pending = 0;
uint8_t nextDoorPriority = WANT_INSIDE;
uint8_t activeOutput = 0;
uint32_t activeOutputDurationMs = 0;
bool activeUsesExtPtt = false;
uint32_t phaseStartedAt = 0;
uint32_t txIdleSince = 0;
RelayIdSchedule relayId(RELAY_ID_INTERVAL_MS);
uint32_t lastInsideTriggerAt = 0;
uint32_t lastOutsideTriggerAt = 0;
bool txSeenDuringOutput = false;
int8_t lastDebugTxAtt = -1;
int8_t lastDebugOutside = -1;
int8_t lastDebugInside = -1;
uint32_t lastGpioDebugAt = 0;

WiFiClient networkClient;
PubSubClient mqtt(networkClient);
char mqttClientId[32];
uint32_t lastWifiAttemptAt = 0;
uint32_t lastMqttAttemptAt = 0;
uint32_t lastPublishAttemptAt = 0;
uint32_t lastPublishedAt = 0;
bool statusDirty = true;
bool wifiStarted = false;
bool wifiWasConnected = false;
bool watchdogEnabled = false;
uint32_t wifiAttemptCount = 0;
uint32_t mqttFailureCount = 0;

void restartAfterConnectionFailures(const char *service, uint32_t marker,
                                    uint32_t attempts) {
  plannedRestart = marker;
  Serial.printf("%s: %lu erfolglose Verbindungsversuche; Neustart\n",
                service, static_cast<unsigned long>(attempts));
  Serial.flush();
  ESP.restart();
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "Strom eingeschaltet";
    case ESP_RST_SW: return "Software-Neustart";
    case ESP_RST_PANIC: return "Programmfehler";
    case ESP_RST_INT_WDT: return "Interrupt-Watchdog";
    case ESP_RST_TASK_WDT: return "Task-Watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_BROWNOUT: return "Unterspannung";
    default: return "sonstiger Grund";
  }
}

void startWatchdog() {
  const esp_task_wdt_config_t config = {
      .timeout_ms = WATCHDOG_TIMEOUT_MS,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_err_t result = esp_task_wdt_reconfigure(&config);
  if (result == ESP_ERR_INVALID_STATE) {
    result = esp_task_wdt_init(&config);
  }
  if (result == ESP_OK) {
    result = esp_task_wdt_status(nullptr);
    if (result == ESP_ERR_NOT_FOUND) {
      result = esp_task_wdt_add(nullptr);
    }
  }
  watchdogEnabled = result == ESP_OK;
  if (watchdogEnabled) {
    Serial.printf("Loop-Watchdog aktiv: %lu ms\n",
                  static_cast<unsigned long>(WATCHDOG_TIMEOUT_MS));
  } else {
    Serial.printf("Loop-Watchdog konnte nicht aktiviert werden: %s\n",
                  esp_err_to_name(result));
  }
}

void requestOpening(uint8_t bit, bool isClosed, const char *name) {
  if (isClosed) {
    pending &= static_cast<uint8_t>(~bit);
  } else {
    pending |= bit;
  }
  Serial.printf("Tuer %s: %s\n", name, isClosed ? "geschlossen" : "offen");
  statusDirty = true;
  lastPublishAttemptAt = millis() - PUBLISH_RETRY_MS;
}

void debugGpioInputs(uint32_t now) {
  const int8_t txAtt = digitalRead(PIN_TX_ATT);
  const int8_t outsideLevel = digitalRead(PIN_DOOR_OUTSIDE);
  const int8_t insideLevel = digitalRead(PIN_DOOR_INSIDE);
  const bool changed = txAtt != lastDebugTxAtt ||
                       outsideLevel != lastDebugOutside ||
                       insideLevel != lastDebugInside;
  if (!changed && !elapsed(now, lastGpioDebugAt, GPIO_DEBUG_INTERVAL_MS)) {
    return;
  }

  lastDebugTxAtt = txAtt;
  lastDebugOutside = outsideLevel;
  lastDebugInside = insideLevel;
  lastGpioDebugAt = now;
  /*
  Serial.printf("GPIO-Rohwerte: GPIO32 TX_ATT=%d, GPIO16 TUER_AUSSEN=%d, "
                "GPIO17 TUER_INNEN=%d\n",
                txAtt, outsideLevel, insideLevel);
  */
}

void serviceInputs(uint32_t now) {
  debugGpioInputs(now);
  if (inside.update(now)) {
    requestOpening(WANT_INSIDE, inside.closed, "innen");
  }
  if (outside.update(now)) {
    requestOpening(WANT_OUTSIDE, outside.closed, "aussen");
  }

  if (!inside.closed &&
      elapsed(now, lastInsideTriggerAt, DOOR_REPEAT_INTERVAL_MS)) {
    pending |= WANT_INSIDE;
  }
  if (!outside.closed &&
      elapsed(now, lastOutsideTriggerAt, DOOR_REPEAT_INTERVAL_MS)) {
    pending |= WANT_OUTSIDE;
  }
}

void startTrigger(uint8_t bit, uint32_t now) {
  const bool doorAnnouncement = bit == WANT_INSIDE || bit == WANT_OUTSIDE;
  if (doorAnnouncement) pending &= static_cast<uint8_t>(~bit);
  if (bit == WANT_INSIDE) {
    activeOutput = PIN_PIO_2;
    lastInsideTriggerAt = now;
    nextDoorPriority = WANT_OUTSIDE;
    Serial.println("CW-Trigger: INNEN");
  } else if (bit == WANT_OUTSIDE) {
    activeOutput = PIN_PIO_1;
    lastOutsideTriggerAt = now;
    nextDoorPriority = WANT_INSIDE;
    Serial.println("CW-Trigger: AUSSEN");
  } else {
    activeOutput = PIN_PIO_3;
    relayId.markTriggered(now);
    Serial.println("CW-Trigger: DB0SL");
  }
  activeUsesExtPtt = doorAnnouncement;
  activeOutputDurationMs = doorAnnouncement ? DOOR_ANNOUNCEMENT_HOLD_MS
                                             : ID_TRIGGER_PULSE_MS;
  txSeenDuringOutput = false;
  // PIO zuerst aktivieren, unmittelbar danach bei Tueransagen EXT_PTT.
  digitalWrite(activeOutput, HIGH);  // Optokoppler EIN, VXR-PIO nach GND
  if (activeUsesExtPtt) digitalWrite(PIN_EXT_PTT, HIGH);
  phaseStartedAt = now;
  radioPhase = RadioPhase::OutputActive;
}

void serviceRadio(uint32_t now) {
  const bool transmitting = digitalRead(PIN_TX_ATT) == LOW;
  if (transmitting) {
    txIdleSince = now;
  }
  const bool txQuiet = !transmitting && elapsed(now, txIdleSince, TX_IDLE_MS);

  // Die Frist laeuft auch ohne Sendebetrieb weiter. Eine faellige Kennung
  // wird erst bei aktiver Sendung ausgeloest.
  relayId.update(now);
  // Kennung hat Vorrang und darf waehrend laufender TX ausgesendet werden.
  // Nur ein PIO-Ausgang darf gleichzeitig einen Triggerimpuls erhalten.
  if (relayId.ready(transmitting) &&
      radioPhase != RadioPhase::OutputActive) {
    startTrigger(WANT_ID, now);
    return;
  }

  switch (radioPhase) {
    case RadioPhase::Idle:
      // Tueransagen nie in laufende TX oder die 500-ms-Sendepause einspielen.
      if (!txQuiet) return;
      if ((pending & (WANT_INSIDE | WANT_OUTSIDE)) ==
          (WANT_INSIDE | WANT_OUTSIDE)) {
        startTrigger(nextDoorPriority, now);
      } else if (pending & WANT_INSIDE) {
        startTrigger(WANT_INSIDE, now);
      } else if (pending & WANT_OUTSIDE) {
        startTrigger(WANT_OUTSIDE, now);
      }
      return;

    case RadioPhase::OutputActive:
      txSeenDuringOutput |= transmitting;
      if (elapsed(now, phaseStartedAt, activeOutputDurationMs)) {
        // Bei Tueransagen PIO und EXT_PTT nach 11 s wieder freigeben.
        if (activeUsesExtPtt) digitalWrite(PIN_EXT_PTT, LOW);
        digitalWrite(activeOutput, LOW);  // Optokoppler AUS
        activeOutput = 0;
        activeUsesExtPtt = false;
        phaseStartedAt = now;
        radioPhase = txSeenDuringOutput ? RadioPhase::WaitForTxEnd
                                        : RadioPhase::WaitForTx;
      }
      return;

    case RadioPhase::WaitForTx:
      if (transmitting) {
        radioPhase = RadioPhase::WaitForTxEnd;
      } else if (elapsed(now, phaseStartedAt, TX_START_TIMEOUT_MS)) {
        Serial.println("Warnung: TX_ATT wurde nach dem Trigger nicht aktiv");
        radioPhase = RadioPhase::FailedTriggerGap;
        phaseStartedAt = now;
      }
      return;

    case RadioPhase::WaitForTxEnd:
      if (txQuiet) {
        radioPhase = RadioPhase::Idle;
      }
      return;

    case RadioPhase::FailedTriggerGap:
      if (txQuiet && elapsed(now, phaseStartedAt, FAILED_TRIGGER_GAP_MS)) {
        radioPhase = RadioPhase::Idle;
      }
      return;
  }
}

void serviceWifi(uint32_t now) {
  if (WIFI_SSID[0] == '\0') return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      wifiAttemptCount = 0;
      lastMqttAttemptAt = now - MQTT_RETRY_MS;
      Serial.printf("WLAN verbunden, IP: %s\n",
                    WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    networkClient.stop();
    mqttFailureCount = 0;
    statusDirty = true;
    Serial.println("WLAN getrennt");
  }

  if (!wifiStarted) {
    wifiStarted = true;
    lastWifiAttemptAt = now;
    wifiAttemptCount = 1;
    Serial.printf("WLAN-Verbindungsversuch %lu/%lu\n",
                  static_cast<unsigned long>(wifiAttemptCount),
                  static_cast<unsigned long>(WIFI_MAX_ATTEMPTS));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else if (elapsed(now, lastWifiAttemptAt, WIFI_RETRY_MS)) {
    if (wifiAttemptCount >= WIFI_MAX_ATTEMPTS) {
      restartAfterConnectionFailures("WLAN", RESTART_WIFI_MARKER,
                                     wifiAttemptCount);
      return;
    }
    lastWifiAttemptAt = now;
    ++wifiAttemptCount;
    Serial.printf("WLAN-Wiederverbindung %lu/%lu\n",
                  static_cast<unsigned long>(wifiAttemptCount),
                  static_cast<unsigned long>(WIFI_MAX_ATTEMPTS));
    if (!WiFi.reconnect()) {
      Serial.println("WLAN-Versuch fehlgeschlagen; erneuter Versuch folgt");
    }
  }
}

void serviceMqtt(uint32_t now) {
  if (WIFI_SSID[0] == '\0' || MQTT_HOST[0] == '\0') return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    // Ein Netzwerk-Connect kann kurz blockieren. Nie waehrend eines
    // Aktive VXR-Ausgaenge beginnen, damit deren Dauer verlaesslich bleibt.
    if (radioPhase == RadioPhase::OutputActive ||
        !elapsed(now, lastMqttAttemptAt, MQTT_RETRY_MS)) {
      return;
    }
    Serial.println("MQTT-Verbindung wird aufgebaut");
    const bool connected = MQTT_USER[0] == '\0'
                               ? mqtt.connect(mqttClientId)
                               : mqtt.connect(mqttClientId, MQTT_USER,
                                              MQTT_PASSWORD);
    lastMqttAttemptAt = millis();
    if (!connected) {
      ++mqttFailureCount;
      Serial.printf("MQTT-Fehler: %d (Versuch %lu/%lu)\n", mqtt.state(),
                    static_cast<unsigned long>(mqttFailureCount),
                    static_cast<unsigned long>(MQTT_MAX_ATTEMPTS));
      if (mqttFailureCount >= MQTT_MAX_ATTEMPTS) {
        restartAfterConnectionFailures("MQTT", RESTART_MQTT_MARKER,
                                       mqttFailureCount);
      }
      return;
    }
    mqttFailureCount = 0;
    statusDirty = true;
    lastPublishAttemptAt = now - PUBLISH_RETRY_MS;
  }

  mqtt.loop();
  if (!mqtt.connected()) return;

  now = millis();
  if (elapsed(now, lastPublishedAt, MQTT_STATUS_INTERVAL_MS)) {
    statusDirty = true;
  }
  if (!statusDirty || !elapsed(now, lastPublishAttemptAt, PUBLISH_RETRY_MS)) {
    return;
  }

  lastPublishAttemptAt = now;
  char payload[80];
  snprintf(payload, sizeof(payload),
           "{\"innen\":\"%s\",\"aussen\":\"%s\"}",
           inside.closed ? "geschlossen" : "offen",
           outside.closed ? "geschlossen" : "offen");

  if (mqtt.publish(MQTT_TOPIC, payload, true)) {
    statusDirty = false;
    lastPublishedAt = now;
    Serial.printf("MQTT %s: %s\n", MQTT_TOPIC, payload);
  } else {
    Serial.println("MQTT-Publish fehlgeschlagen; erneuter Versuch folgt");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("Neustartgrund: %s (%d)\n", resetReasonName(reason), reason);
  if (reason == ESP_RST_SW && plannedRestart == RESTART_WIFI_MARKER) {
    Serial.println("Letzter Neustart: WLAN-Verbindung wiederholt fehlgeschlagen");
  } else if (reason == ESP_RST_SW &&
             plannedRestart == RESTART_MQTT_MARKER) {
    Serial.println("Letzter Neustart: MQTT-Verbindung wiederholt fehlgeschlagen");
  }
  plannedRestart = 0;
  startWatchdog();
  const uint32_t now = millis();

  // Nach dem Konfigurieren sofort den inaktiven Pegel setzen.
  constexpr uint8_t outputPins[] = {PIN_PIO_1, PIN_PIO_2, PIN_PIO_3,
                                    PIN_EXT_PTT};
  for (uint8_t pin : outputPins) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  inside.begin(now);
  outside.begin(now);
  pinMode(PIN_TX_ATT, INPUT_PULLUP);
  txIdleSince = now;

  if (!inside.closed) pending |= WANT_INSIDE;
  if (!outside.closed) pending |= WANT_OUTSIDE;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  snprintf(mqttClientId, sizeof(mqttClientId), "db0sl-%012llX",
           static_cast<unsigned long long>(ESP.getEfuseMac()));
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  networkClient.setConnectionTimeout(3000);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(2);

  lastMqttAttemptAt = now - MQTT_RETRY_MS;
  lastPublishAttemptAt = now - PUBLISH_RETRY_MS;
  lastPublishedAt = now;

  Serial.println("DB0SL-Controller gestartet");
  Serial.printf("Tuer innen: %s, aussen: %s\n",
                inside.closed ? "geschlossen" : "offen",
                outside.closed ? "geschlossen" : "offen");
  if (WIFI_SSID[0] == '\0' || MQTT_HOST[0] == '\0') {
    Serial.println("WLAN/MQTT nicht konfiguriert (include/config.h)");
  }
}

void loop() {
  const uint32_t now = millis();
  serviceInputs(now);
  serviceRadio(now);
  if (radioPhase != RadioPhase::OutputActive) {
    serviceWifi(now);
    serviceMqtt(now);
  }
  if (watchdogEnabled) {
    esp_task_wdt_reset();
  }
}
