#pragma once
#include <Arduino.h>

#include "../measurement.h"
#include "../tinyml/model_infer.h"

// WiFi + MQTT publisher.
//
// CREDENTIALS ARE NEVER HARDCODED. They are entered once over the serial
// console ('wifi' / 'mqtt' commands) and stored in NVS. Nothing in this
// repository contains an SSID or password, so the source can be shared,
// committed, or submitted without leaking network access.
//
// Runs entirely on CORE_INFERENCE_NET (Core 0), which is where the ESP32's
// WiFi/TCP-IP stack is already pinned by default. Keeping the network code on
// the same core as the driver avoids the cross-core contention that would
// otherwise jitter the 200Hz sampling loop.

struct NetConfig {
  char ssid[33] = "";
  char pass[65] = "";
  char mqttHost[64] = "";
  uint16_t mqttPort = 1883;
  bool configured = false;
};

enum class NetState : uint8_t {
  UNCONFIGURED,
  LINK_CONNECTING,
  LINK_UP,
  BROKER_UP,
  FAILED
};

const char* netStateName(NetState s);

namespace net {

void loadConfig(NetConfig& cfg);
void saveConfig(const NetConfig& cfg);
void clearConfig();

// Non-blocking: call frequently from the network task. Handles connect,
// reconnect with backoff, and MQTT keepalive.
void poll();

void begin(const NetConfig& cfg);

// Publishes one record. Returns false if not connected OR if the broker
// rejected the payload -- PubSubClient::publish() returns false when the
// message exceeds the buffer, and silently dropping that would make the
// dashboard look intermittently broken for no visible reason.
bool publish(const MeasurementRecord& rec, const Inference& inf,
             const char* ruleVerdict);

NetState state();
bool timeSynced();

// UTC ISO-8601. Falls back to an uptime-based marker when NTP has not synced,
// clearly flagged rather than emitting a fake 1970 timestamp.
void isoTimestamp(char* out, size_t len);

}  // namespace net
