#include "net_client.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>

#include "../config.h"

namespace {

constexpr const char* NVS_NS = "concresense_net";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

NetConfig gCfg;
NetState gState = NetState::UNCONFIGURED;

uint32_t gLastAttemptMs = 0;
uint32_t gBackoffMs = 2000;
bool gTimeSynced = false;

// PubSubClient's default buffer is 256 bytes (MQTT_MAX_PACKET_SIZE in
// PubSubClient.h). Our payload carries GPS + 7 features + class probabilities
// and comfortably exceeds that. Verified behaviour: publish() returns false and
// sends NOTHING rather than truncating, so without this the dashboard would
// simply never receive a message and the cause would be invisible.
//
// The buffer must hold the whole MQTT packet, not just the payload: topic
// string ("concresense/site/<DEVICE_ID>/test", ~44 bytes here) and protocol
// framing sit on top of the JSON body. 768 was sized to the payload alone and
// measurably failed against the real topic+payload combination on real
// hardware -- 1024 leaves genuine headroom instead of chasing the exact byte.
constexpr uint16_t MQTT_BUFFER = 1024;

// Bangalore is UTC+5:30, but timestamps are published as UTC and the offset is
// left to the dashboard. Passing 0 here keeps gmtime() honest.
constexpr long GMT_OFFSET_SEC = 0;
constexpr int DST_OFFSET_SEC = 0;

}  // namespace

const char* netStateName(NetState s) {
  switch (s) {
    case NetState::UNCONFIGURED: return "UNCONFIGURED";
    case NetState::LINK_CONNECTING: return "WIFI_CONNECTING";
    case NetState::LINK_UP: return "WIFI_CONNECTED";
    case NetState::BROKER_UP: return "MQTT_CONNECTED";
    case NetState::FAILED: return "FAILED";
  }
  return "?";
}

namespace net {

void loadConfig(NetConfig& cfg) {
  Preferences p;
  p.begin(NVS_NS, true);
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  String host = p.getString("host", "");
  cfg.mqttPort = p.getUShort("port", 1883);
  p.end();

  strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
  strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);
  strncpy(cfg.mqttHost, host.c_str(), sizeof(cfg.mqttHost) - 1);
  cfg.configured = ssid.length() > 0 && host.length() > 0;
}

void saveConfig(const NetConfig& cfg) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("ssid", cfg.ssid);
  p.putString("pass", cfg.pass);
  p.putString("host", cfg.mqttHost);
  p.putUShort("port", cfg.mqttPort);
  p.end();
}

void clearConfig() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.clear();
  p.end();
}

void begin(const NetConfig& cfg) {
  gCfg = cfg;
  if (!gCfg.configured) {
    gState = NetState::UNCONFIGURED;
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep adds latency spikes to publishes
  WiFi.begin(gCfg.ssid, gCfg.pass);

  mqtt.setServer(gCfg.mqttHost, gCfg.mqttPort);
  if (!mqtt.setBufferSize(MQTT_BUFFER)) {
    Serial.println(F("[net] WARNING: could not allocate MQTT buffer; "
                     "publishes may fail"));
  }
  mqtt.setKeepAlive(30);

  gState = NetState::LINK_CONNECTING;
  gLastAttemptMs = millis();
}

void poll() {
  if (!gCfg.configured) {
    gState = NetState::UNCONFIGURED;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    gState = NetState::LINK_CONNECTING;
    // Exponential backoff, capped. Retrying a dead AP every loop iteration
    // burns power and floods the log without ever helping.
    if (millis() - gLastAttemptMs > gBackoffMs) {
      gLastAttemptMs = millis();
      if (gBackoffMs < 30000) gBackoffMs *= 2;
      WiFi.disconnect();
      WiFi.begin(gCfg.ssid, gCfg.pass);
    }
    return;
  }

  // WiFi is up.
  if (gState == NetState::LINK_CONNECTING) {
    gBackoffMs = 2000;
    gState = NetState::LINK_UP;
    Serial.printf("[net] WiFi connected, IP %s\n",
                  WiFi.localIP().toString().c_str());

    // GPS gets no fix indoors (confirmed during Phase 1 bring-up), so without
    // NTP every published timestamp would be a 1970 epoch value. This is the
    // only reliable clock source this device has in a lab.
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  }

  if (!gTimeSynced) {
    struct tm t;
    // Any year past 2016 means NTP actually replied; the RTC powers up in 1970.
    if (getLocalTime(&t, 10) && (t.tm_year + 1900) > 2016) {
      gTimeSynced = true;
      Serial.println(F("[net] NTP time synced"));
    }
  }

  if (!mqtt.connected()) {
    if (millis() - gLastAttemptMs > 2000) {
      gLastAttemptMs = millis();
      char clientId[48];
      snprintf(clientId, sizeof(clientId), "%s-%06X", DEVICE_ID,
               (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
      if (mqtt.connect(clientId)) {
        gState = NetState::BROKER_UP;
        Serial.printf("[net] MQTT connected to %s:%u\n", gCfg.mqttHost,
                      gCfg.mqttPort);
      } else {
        gState = NetState::LINK_UP;
        Serial.printf("[net] MQTT connect failed, rc=%d\n", mqtt.state());
      }
    }
    return;
  }

  gState = NetState::BROKER_UP;
  mqtt.loop();
}

bool timeSynced() { return gTimeSynced; }

void isoTimestamp(char* out, size_t len) {
  if (gTimeSynced) {
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &t);
  } else {
    // Explicitly marked as unsynced rather than emitting a plausible-looking
    // 1970 timestamp that a dashboard would silently plot.
    snprintf(out, len, "UNSYNCED+%lums", (unsigned long)millis());
  }
}

NetState state() { return gState; }

bool publish(const MeasurementRecord& rec, const Inference& inf,
             const char* ruleVerdict) {
  if (gState != NetState::BROKER_UP) return false;

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["seq"] = rec.seq;

  char ts[32];
  isoTimestamp(ts, sizeof(ts));
  doc["timestamp_utc"] = ts;
  doc["time_source"] = gTimeSynced ? "ntp" : "unsynced";

  JsonObject loc = doc["location"].to<JsonObject>();
  if (rec.gps.valid) {
    loc["latitude"] = rec.gps.latitude;
    loc["longitude"] = rec.gps.longitude;
    loc["hdop"] = rec.gps.hdop;
    loc["satellites"] = rec.gps.satellites;
    loc["fix"] = true;
  } else {
    // No coordinates at all rather than 0,0 -- null island is a real place on
    // a map and would render as a pin off the coast of Africa.
    loc["fix"] = false;
    loc["reason"] = "no_gps_fix";
  }

  JsonObject raw = doc["sensor_raw"].to<JsonObject>();
  if (rec.moistureValid) raw["moisture_mv"] = round(rec.moistureMv * 10) / 10.0;
  if (rec.tempValid) raw["temperature_c"] = round(rec.tempC * 100) / 100.0;
  if (rec.loadValid) raw["load_counts"] = rec.loadCounts;
  if (rec.vib.valid) raw["vibration_rms_g"] = rec.vib.rms;

  JsonObject feat = doc["features"].to<JsonObject>();
  if (rec.derived.wcValid) feat["estimated_wc_ratio"] = rec.derived.wcRatio;
  if (rec.derived.slumpValid) {
    feat["estimated_slump_mm"] = rec.derived.slumpMm;
    feat["yield_stress_pa"] = rec.derived.yieldStressPa;
  }
  if (!isnan(rec.derived.forceN)) feat["penetration_force_n"] = rec.derived.forceN;
  if (rec.vib.valid) {
    feat["vib_dominant_hz"] = rec.vib.dominantFreqHz;
    feat["vib_spectral_entropy"] = rec.vib.spectralEntropy;
    feat["vib_damping_ratio"] = rec.vib.dampingRatio;
  }

  JsonObject cls = doc["classification"].to<JsonObject>();
  if (inf.valid) {
    cls["model_result"] = inferenceClassName(inf.classIndex);
    cls["confidence"] = inf.confidence;
    JsonObject probs = cls["class_probabilities"].to<JsonObject>();
    for (int c = 0; c < INFERENCE_N_CLASSES; c++) {
      probs[inferenceClassName(c)] = inf.probabilities[c];
    }
  } else {
    cls["model_result"] = "UNKNOWN";
    cls["reason"] = "incomplete_feature_vector";
  }
  // Both verdicts travel together on purpose. If the model ever merely
  // re-derives the IS 456 rules, these two fields being identical on every
  // record is what makes that visible instead of impressive.
  cls["rule_result"] = ruleVerdict;
  cls["agreement"] =
      (inf.valid && strcmp(inferenceClassName(inf.classIndex), ruleVerdict) == 0);

  doc["standard_compliance"] = COMPLIANCE_STANDARD;
  doc["firmware"] = FW_VERSION;
  // Provenance follows the record all the way to the dashboard and the audit
  // PDF. A simulated reading must never be indistinguishable from a measured
  // one once it has left the device.
  doc["data_source"] = rec.simulated ? "simulated_onboard" : "device_measurement";

  char topic[96];
  snprintf(topic, sizeof(topic), "concresense/site/%s/test", DEVICE_ID);

  char payload[MQTT_BUFFER];
  const size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload)) {
    Serial.printf("[net] payload too large (%u B) - not published\n",
                  (unsigned)n);
    return false;
  }

  const bool ok = mqtt.publish(topic, payload, false);
  if (!ok) {
    Serial.printf("[net] publish FAILED (%u B, mqtt state %d)\n", (unsigned)n,
                  mqtt.state());
  }
  return ok;
}

}  // namespace net
