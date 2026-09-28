#pragma once

#include <cstdint>

// Eine faellige Kennung bleibt auch waehrend langer Sendepausen faellig.
class RelayIdSchedule {
 public:
  explicit RelayIdSchedule(uint32_t intervalMs) : intervalMs_(intervalMs) {}

  void update(uint32_t now) {
    if (!due_ && static_cast<uint32_t>(now - lastTriggerAt_) >= intervalMs_) {
      due_ = true;
    }
  }

  bool ready(bool transmitting) const { return due_ && transmitting; }

  void markTriggered(uint32_t now) {
    lastTriggerAt_ = now;
    due_ = false;
  }

 private:
  const uint32_t intervalMs_;
  uint32_t lastTriggerAt_ = 0;
  bool due_ = true;  // Nach einem Neustart ist die letzte Kennung unbekannt.
};
