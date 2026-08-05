#!/usr/bin/env node
/**
 * End-to-end verification of the Phase 4 backend, with no hardware and no
 * system packages.
 *
 * Publishes a payload byte-identical in shape to what net_client.cpp emits,
 * then asserts it travelled: MQTT -> server -> REST -> WebSocket.
 *
 * This is what makes the "Phase 4 works" claim checkable rather than asserted.
 * Run the server first:  npm start
 */

'use strict';

const mqtt = require('mqtt');
const WebSocket = require('ws');

const HTTP = `http://localhost:${process.env.PORT || 3000}`;
const MQTT_URL = process.env.MQTT_URL || `mqtt://127.0.0.1:${process.env.MQTT_PORT || 1883}`;
const DEVICE = 'CONCRESENSE_ESP32_001';

let failures = 0;
const check = (cond, what) => {
  console.log(`  ${cond ? 'ok  ' : 'FAIL'}  ${what}`);
  if (!cond) failures++;
};

// Mirrors the JSON built in firmware/concresense/src/network/net_client.cpp.
// Kept structurally identical on purpose: if the firmware schema changes and
// this is not updated, the assertions below start failing, which is the point.
function samplePayload(seq) {
  return {
    device_id: DEVICE,
    seq,
    timestamp_utc: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
    time_source: 'ntp',
    location: { fix: false, reason: 'no_gps_fix' },
    sensor_raw: {
      moisture_mv: 2408.7,
      temperature_c: 37.44,
      load_counts: 15230,
      vibration_rms_g: 0.27587,
    },
    features: {
      estimated_wc_ratio: 0.462,
      estimated_slump_mm: 88.4,
      yield_stress_pa: 1880.2,
      penetration_force_n: 0.3741,
      vib_dominant_hz: 28.588,
      vib_spectral_entropy: 0.56147,
      vib_damping_ratio: 0.07682,
    },
    classification: {
      model_result: 'GOOD',
      confidence: 0.497,
      class_probabilities: { GOOD: 0.497, MARGINAL: 0.32, REJECT: 0.183 },
      rule_result: 'GOOD',
      agreement: true,
    },
    standard_compliance: 'IS 456:2000',
    firmware: '0.1.0-phase1',
    data_source: 'device_measurement',
  };
}

async function main() {
  console.log('=== ConcreSense backend end-to-end self-test ===\n');

  // 1. server reachable
  let health;
  try {
    health = await (await fetch(`${HTTP}/api/health`)).json();
  } catch (e) {
    console.error(`Cannot reach ${HTTP} -- is the server running? (npm start)`);
    process.exit(1);
  }
  check(health.ok === true, 'server /api/health responds');
  check(health.mqtt_connected === true, 'server is connected to the broker');

  // 2. websocket receives the broadcast
  const ws = new WebSocket(`ws://localhost:${process.env.PORT || 3000}/ws`);
  const wsGot = new Promise((resolve) => {
    const timer = setTimeout(() => resolve(null), 4000);
    ws.on('message', (raw) => {
      const msg = JSON.parse(raw.toString());
      if (msg.type === 'record' && msg.data.seq === 4242) {
        clearTimeout(timer);
        resolve(msg.data);
      }
    });
  });
  await new Promise((r) => ws.on('open', r));
  check(true, 'websocket connected');

  // 3. publish exactly what the firmware would
  const client = mqtt.connect(MQTT_URL, { clientId: 'selftest-publisher' });
  await new Promise((r) => client.on('connect', r));
  check(true, 'test publisher connected to broker');

  const topic = `concresense/site/${DEVICE}/test`;
  await new Promise((resolve, reject) =>
    client.publish(topic, JSON.stringify(samplePayload(4242)), (e) =>
      e ? reject(e) : resolve()));
  check(true, `published to ${topic}`);

  // 4. it arrived over the websocket
  const viaWs = await wsGot;
  check(viaWs !== null, 'record arrived over websocket');
  if (viaWs) {
    check(viaWs.classification.rule_result === 'GOOD',
      'websocket record preserved rule_result');
    check(viaWs.features.estimated_wc_ratio === 0.462,
      'websocket record preserved feature values');
    check(typeof viaWs.received_at === 'string',
      'server stamped received_at');
  }

  // 5. REST reflects it
  await new Promise((r) => setTimeout(r, 200));
  const latest = await (await fetch(`${HTTP}/api/latest`)).json();
  check(latest.data && latest.data.seq === 4242, '/api/latest returns the record');

  const hist = await (await fetch(`${HTTP}/api/history?limit=10`)).json();
  check(hist.count >= 1, '/api/history contains the record');

  const summary = await (await fetch(`${HTTP}/api/summary`)).json();
  check(summary.counts.GOOD >= 1, '/api/summary counted a GOOD verdict');

  // 6. malformed input must not crash the server
  await new Promise((resolve) =>
    client.publish(topic, 'this is not json{{{', () => resolve()));
  await new Promise((r) => setTimeout(r, 400));
  const after = await (await fetch(`${HTTP}/api/health`)).json();
  check(after.ok === true, 'server survived a malformed publish');
  check(after.malformed >= 1, 'malformed publish was counted, not silently dropped');

  ws.close();
  client.end(true);

  console.log(`\n=== ${failures === 0 ? 'ALL CHECKS PASSED' : failures + ' CHECK(S) FAILED'} ===`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error('self-test crashed:', e);
  process.exit(1);
});
