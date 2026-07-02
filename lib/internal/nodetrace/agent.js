'use strict';
// Log-shipping agent baked into the nodetrace fork.
//
// On startup, if `~/.nodetrace/agent.json` exists, we try to reach the
// configured host:port.  If the initial connect fails, we retry every 60 s.
// Once connected, we send a `hello` frame and then sit ready to ship traces
// / logs and to receive throttling / nested-tracking instructions.
//
// The agent is opt-in via the presence of the config file: if the file is
// missing, no work happens and the module has zero footprint.  All sockets
// and timers are `.unref()`'d so the agent never keeps the process alive.
//
// Wire format: length-prefixed JSON frames.
//   [uint32 LE payload length][utf-8 JSON payload]
//
// Frame types (client → server / server → client):
//   { type: "hello", machineId, pid, argv, cwd, version }
//   { type: "hello-ack", ok, throttleDefaults? }
//   { type: "log",   level, msg, ts }
//   { type: "trace", chunkB64, seq }
//   { type: "throttle-set", stableFuncId?, paramMatch?, maxPerSecond, trackNested }
//   { type: "pong", ts } / { type: "ping", ts }

const { ObjectFreeze } = primordials;

const RETRY_MS   = 60_000;
const HELLO_ONCE = Symbol('nodetrace.hello.sent');

let started = false;
let currentSocket = null;
let currentRetryTimer = null;

// Read + validate config file.  Returns null if not present / not valid.
function readConfig() {
  // Deferred requires: keep the fast-path (no config) fully lazy.
  const fs = require('fs');
  const os = require('os');
  const path = require('path');

  const configPath = process.env.NODETRACE_AGENT_CONFIG
                    ?? path.join(os.homedir(), '.nodetrace', 'agent.json');
  let raw;
  try {
    raw = fs.readFileSync(configPath, 'utf8');
  } catch {
    return null;
  }
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch (e) {
    process.stderr.write(`[nodetrace-agent] bad config ${configPath}: ${e.message}\n`);
    return null;
  }
  if (parsed.enabled === false) return null;
  if (typeof parsed.host !== 'string' || !parsed.host) {
    process.stderr.write(`[nodetrace-agent] config missing "host"\n`);
    return null;
  }
  if (!Number.isInteger(parsed.port) || parsed.port <= 0 || parsed.port > 65535) {
    process.stderr.write(`[nodetrace-agent] config missing/invalid "port"\n`);
    return null;
  }

  // Machine ID: prefer explicit config, else /etc/machine-id, else hostname hash.
  let machineId = parsed.machineId;
  if (!machineId) {
    try {
      machineId = fs.readFileSync('/etc/machine-id', 'utf8').trim();
    } catch { /* ignore */ }
  }
  if (!machineId) {
    machineId = require('crypto').createHash('sha256')
      .update(String(os.hostname() ?? 'unknown'))
      .digest('hex');
  }

  return ObjectFreeze({
    configPath,
    host: parsed.host,
    port: parsed.port,
    machineId,
    serverKey: typeof parsed.serverKey === 'string' ? parsed.serverKey : null,
  });
}

function encodeFrame(obj) {
  const body = Buffer.from(JSON.stringify(obj), 'utf8');
  const out = Buffer.alloc(4 + body.length);
  out.writeUInt32LE(body.length, 0);
  body.copy(out, 4);
  return out;
}

function makeFrameParser(onFrame) {
  let acc = Buffer.alloc(0);
  return (chunk) => {
    acc = acc.length === 0 ? chunk : Buffer.concat([acc, chunk]);
    while (acc.length >= 4) {
      const len = acc.readUInt32LE(0);
      if (len > 16 * 1024 * 1024) throw new Error('agent frame too large');
      if (acc.length < 4 + len) return;
      const body = acc.subarray(4, 4 + len);
      acc = acc.subarray(4 + len);
      let obj;
      try { obj = JSON.parse(body.toString('utf8')); }
      catch { throw new Error('agent bad JSON frame'); }
      onFrame(obj);
    }
  };
}

function connect(cfg) {
  // Cancel any pending retry — we're actively trying now.
  if (currentRetryTimer) { clearTimeout(currentRetryTimer); currentRetryTimer = null; }

  const net = require('net');
  const sock = net.connect({ host: cfg.host, port: cfg.port, allowHalfOpen: false });
  sock.setNoDelay(true);
  sock.unref();  // never keep event loop alive
  currentSocket = sock;

  let helloSent = false;
  const parse = makeFrameParser((frame) => {
    if (!frame || typeof frame !== 'object') return;
    if (frame.type === 'ping') {
      trySend({ type: 'pong', ts: Date.now() });
      return;
    }
    // { type: "throttle-set", func: "<funcName>", level: 0|1|2 }
    // level 0 = respect global throttle; 1 = always keep; 2 = keep + subtree.
    // See deps/v8/src/trace/trace-writer.h SetBoostByNameIdx for details.
    if (frame.type === 'throttle-set' && typeof frame.func === 'string' &&
        (frame.level === 0 || frame.level === 1 || frame.level === 2)) {
      try { process.setFunctionBoost?.(frame.func, frame.level); }
      catch (e) {
        if (process.env.NODETRACE_AGENT_DEBUG) {
          process.stderr.write(`[nodetrace-agent] setFunctionBoost failed: ${e.message}\n`);
        }
      }
      return;
    }
    // paramMatch-based throttling is defined in the wire protocol but is
    // deferred: recording param-value predicates in-writer needs a per-name
    // predicate table.  When we ship that, this handler will dispatch on
    // frame.paramMatch → an as-yet-unimplemented process API.
  });

  sock.on('connect', () => {
    if (helloSent) return;
    helloSent = true;
    sock[HELLO_ONCE] = true;
    trySend({
      type: 'hello',
      machineId: cfg.machineId,
      pid: process.pid,
      ppid: process.ppid ?? 0,
      argv: process.argv,
      cwd: (() => { try { return process.cwd(); } catch { return null; } })(),
      version: process.version,
      execPath: process.execPath,
    });
  });

  sock.on('data', (chunk) => {
    try { parse(chunk); }
    catch (e) {
      process.stderr.write(`[nodetrace-agent] ${e.message}\n`);
      try { sock.destroy(); } catch { /* ignore */ }
    }
  });

  const onFail = () => {
    currentSocket = null;
    scheduleRetry(cfg);
  };
  sock.on('error', (e) => {
    // Silent by default — retry noise would pollute stderr on every 60s attempt.
    if (process.env.NODETRACE_AGENT_DEBUG) {
      process.stderr.write(`[nodetrace-agent] connect error: ${e.message}\n`);
    }
  });
  sock.on('close', onFail);
}

function trySend(obj) {
  const s = currentSocket;
  if (!s || s.destroyed || s.writableEnded) return false;
  try { s.write(encodeFrame(obj)); return true; }
  catch { return false; }
}

function scheduleRetry(cfg) {
  if (currentRetryTimer) return;
  currentRetryTimer = setTimeout(() => {
    currentRetryTimer = null;
    connect(cfg);
  }, RETRY_MS);
  currentRetryTimer.unref();
}

// Entrypoint: safe to call multiple times; only starts once per process.
function start() {
  if (started) return;
  started = true;
  const cfg = readConfig();
  if (!cfg) return;  // no config → no-op
  connect(cfg);
}

// sendLog: public helper for services to ship a structured log line.  Safe
// to call whether or not the agent is configured / connected — the frame is
// dropped silently when the socket isn't ready.  Consumers on the server
// side query these via /machine/<id>/search?q=<substring>.
function sendLog(level, msg, extra) {
  return trySend({
    type: 'log',
    ts: Date.now(),
    level: String(level ?? 'info'),
    msg: String(msg ?? ''),
    extra: extra ?? null,
  });
}

// Testing surface — used by internal integration tests and by part 5b/c code.
function _sendForTest(obj) { return trySend(obj); }
function _current()        { return currentSocket; }

module.exports = { start, sendLog, _sendForTest, _current };
