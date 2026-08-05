#!/usr/bin/env node
/**
 * ConcreSense backend — Phase 4.
 *
 *   ESP32 --MQTT--> [broker] --> this server --WebSocket--> dashboard
 *                                     |
 *                                     +--> REST (/api/*)
 *
 * Runs an EMBEDDED MQTT broker (aedes) by default. That is a deliberate
 * choice: it means the entire stack runs with `npm install && npm start` and
 * no system packages, no sudo, and no cloud account. Point the ESP32 at this
 * machine's IP on port 1883 and it works. Pass --no-broker to use an external
 * broker (mosquitto, HiveMQ, ...) instead.
 *
 * Nothing here fabricates data. If no device has published, /api/latest
 * returns null and the dashboard says so, rather than showing placeholder
 * readings that look real.
 */

'use strict';

const http = require('http');
const path = require('path');

const express = require('express');
const mqtt = require('mqtt');
const { WebSocketServer } = require('ws');

// ----------------------------------------------------------------- config
const args = process.argv.slice(2);
const useEmbeddedBroker = !args.includes('--no-broker');
const HTTP_PORT = Number(process.env.PORT || 3000);
const MQTT_PORT = Number(process.env.MQTT_PORT || 1883);
const MQTT_URL = process.env.MQTT_URL || `mqtt://127.0.0.1:${MQTT_PORT}`;
const TOPIC = 'concresense/site/+/test';

// Ring buffer. Bounded on purpose: an unbounded array would grow without
// limit on a long deployment and eventually take the process down.
const MAX_HISTORY = 500;
const history = [];
let lastRecord = null;
const stats = { received: 0, malformed: 0, startedAt: new Date().toISOString() };

// ------------------------------------------------------- embedded broker
let brokerServer = null;
if (useEmbeddedBroker) {
  const aedes = require('aedes')();
  brokerServer = require('net').createServer(aedes.handle);
  brokerServer.listen(MQTT_PORT, () => {
    console.log(`[broker] embedded MQTT broker listening on :${MQTT_PORT}`);
  });
  aedes.on('client', (c) => console.log(`[broker] client connected: ${c.id}`));
  aedes.on('clientDisconnect', (c) =>
    console.log(`[broker] client disconnected: ${c.id}`));
  brokerServer.on('error', (err) => {
    if (err.code === 'EADDRINUSE') {
      console.error(
        `[broker] port ${MQTT_PORT} already in use -- another broker is ` +
        `probably running. Start with --no-broker to use it instead.`);
      process.exit(1);
    }
    throw err;
  });
}

// ------------------------------------------------------------ http + ws
const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname, '..', 'public')));

const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });

function broadcast(obj) {
  const msg = JSON.stringify(obj);
  for (const client of wss.clients) {
    if (client.readyState === 1) client.send(msg);
  }
}

wss.on('connection', (ws) => {
  console.log(`[ws] client connected (${wss.clients.size} total)`);
  // Replay the latest record immediately so a page that connects between
  // measurements is not blank for the next 5 seconds.
  if (lastRecord) ws.send(JSON.stringify({ type: 'record', data: lastRecord }));
  ws.on('close', () => console.log(`[ws] client disconnected`));
});

// ------------------------------------------------------------ mqtt client
const client = mqtt.connect(MQTT_URL, {
  clientId: `concresense-server-${Math.random().toString(16).slice(2, 8)}`,
  reconnectPeriod: 2000,
});

client.on('connect', () => {
  console.log(`[mqtt] connected to ${MQTT_URL}`);
  client.subscribe(TOPIC, { qos: 0 }, (err) => {
    if (err) console.error('[mqtt] subscribe failed:', err.message);
    else console.log(`[mqtt] subscribed to ${TOPIC}`);
  });
});

client.on('error', (err) => console.error('[mqtt] error:', err.message));
client.on('reconnect', () => console.log('[mqtt] reconnecting...'));

client.on('message', (topic, payload) => {
  let doc;
  try {
    doc = JSON.parse(payload.toString());
  } catch (e) {
    // Count and log rather than crash. A malformed publish from a
    // half-flashed device must not take the dashboard down.
    stats.malformed++;
    console.warn(`[mqtt] malformed JSON on ${topic}: ${e.message}`);
    return;
  }

  const record = {
    topic,
    received_at: new Date().toISOString(),
    ...doc,
  };

  stats.received++;
  lastRecord = record;
  history.push(record);
  if (history.length > MAX_HISTORY) history.shift();

  const cls = record.classification || {};
  console.log(
    `[mqtt] #${record.seq ?? '?'} rules=${cls.rule_result ?? '-'} ` +
    `model=${cls.model_result ?? '-'} agree=${cls.agreement ?? '-'}`);

  broadcast({ type: 'record', data: record });
});

// ------------------------------------------------------------------- api
app.get('/api/health', (req, res) => {
  res.json({
    ok: true,
    mqtt_connected: client.connected,
    embedded_broker: useEmbeddedBroker,
    websocket_clients: wss.clients.size,
    ...stats,
  });
});

app.get('/api/latest', (req, res) => {
  // Explicit null + a reason, never a fabricated placeholder reading.
  if (!lastRecord) {
    return res.json({ data: null, reason: 'no device has published yet' });
  }
  res.json({ data: lastRecord });
});

app.get('/api/history', (req, res) => {
  const limit = Math.min(Number(req.query.limit) || 100, MAX_HISTORY);
  res.json({ count: history.length, data: history.slice(-limit) });
});

// Aggregate for the dashboard's summary tiles.
app.get('/api/summary', (req, res) => {
  const counts = { GOOD: 0, MARGINAL: 0, REJECT: 0, UNKNOWN: 0 };
  let disagreements = 0;
  for (const r of history) {
    const c = r.classification || {};
    const v = c.rule_result || 'UNKNOWN';
    if (counts[v] !== undefined) counts[v]++;
    if (c.agreement === false) disagreements++;
  }
  res.json({
    total: history.length,
    counts,
    // Surfaced deliberately: if the model and the IS 456 rules never
    // disagree, the model is adding nothing over the rule engine.
    model_rule_disagreements: disagreements,
  });
});

server.listen(HTTP_PORT, () => {
  console.log(`[http] dashboard + API on http://localhost:${HTTP_PORT}`);
  console.log(`[ws]   stream on ws://localhost:${HTTP_PORT}/ws`);
  if (useEmbeddedBroker) {
    console.log(`\nPoint the ESP32 at this machine:`);
    console.log(`  mqtt <this-machine-ip> ${MQTT_PORT}`);
  }
});

function shutdown() {
  console.log('\nshutting down...');
  client.end(true);
  wss.close();
  server.close();
  if (brokerServer) brokerServer.close();
  setTimeout(() => process.exit(0), 300);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
