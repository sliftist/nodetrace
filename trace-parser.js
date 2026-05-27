'use strict';
// Canonical parser for the nodetrace binary format.
// See TRACING.md for the full wire-format spec.
//
// Usage:
//   const { readTrace } = require('./trace-parser');
//   const { events, names } = readTrace(fs.readFileSync('trace.bin'));
//
// Each event object has at minimum: { type, ts }
//   type  — one of the EV_TYPE_* strings below
//   ts    — BigInt nanoseconds since Unix epoch
//
// ENTER events additionally have:
//   { func, isAsync, callId, params }
//   params — array of { name, tag, value }; value present for tag 2/3/4 only
//
// EXIT / SUSPEND / RESUME / ON_STACK_REPLACEMENT events additionally have:
//   { func, callId }
//
// OPTIMIZED_BATCH events additionally have:
//   { count, minTs, maxTs }  (all BigInt)

// Wire-format type codes (bottom 6 bits of the header byte).
const EV_ENTER                = 0x00;
const EV_EXIT                 = 0x01;
const EV_SUSPEND              = 0x02;
const EV_RESUME               = 0x03;
const EV_ON_STACK_REPLACEMENT = 0x04;
const EV_OPTIMIZED_BATCH      = 0x05;
const EV_NEWNAME              = 0x06;

// String names used in event.type.
const EV_TYPE_ENTER                = 'ENTER';
const EV_TYPE_EXIT                 = 'EXIT';
const EV_TYPE_SUSPEND              = 'SUSPEND';
const EV_TYPE_RESUME               = 'RESUME';
const EV_TYPE_ON_STACK_REPLACEMENT = 'ON_STACK_REPLACEMENT';
const EV_TYPE_OPTIMIZED_BATCH      = 'OPTIMIZED_BATCH';

// Human-readable names for param type tags (index = tag value).
const PARAM_TYPE_NAMES = [
  'undefined', 'null', 'boolean', 'integer', 'float',
  'string', 'object', 'array', 'function', 'symbol', 'bigint',
];

// Parse a Buffer containing a binary trace.
// Returns { events, names } where names[idx] is the interned string for that index.
// Throws on malformed input.
function readTrace(buf) {
  let pos = 0;
  const names  = [];
  const events = [];
  let lastTs   = 0n;

  const u8  = () => buf[pos++];
  const u16 = () => { const v = buf.readUInt16LE(pos); pos += 2; return v; };
  const u32 = () => { const v = buf.readUInt32LE(pos); pos += 4; return v; };
  const i32 = () => { const v = buf.readInt32LE(pos);  pos += 4; return v; };
  const u64 = () => {
    const lo = buf.readUInt32LE(pos);
    const hi = buf.readUInt32LE(pos + 4);
    pos += 8;
    return BigInt(hi) * 0x100000000n + BigInt(lo);
  };

  const readDelta = (ss) => {
    switch (ss) {
      case 0: return BigInt(u8());
      case 1: return BigInt(u16());
      case 2: return BigInt(u32());
      case 3: return u64();
    }
  };

  while (pos < buf.length) {
    const header = u8();
    const ss     = (header >> 6) & 0x03;
    const type   = header & 0x3F;

    lastTs += readDelta(ss);
    const ts = lastTs;

    if (type === EV_NEWNAME) {
      const idx = u32();
      const len = u16();
      names[idx] = buf.toString('utf8', pos, pos + len);
      pos += len;

    } else if (type === EV_ENTER) {
      const func       = names[u32()] ?? '(unknown)';
      const isAsync    = !!u8();
      const callId     = u32();
      const paramCount = u8();
      const params     = [];
      for (let i = 0; i < paramCount; i++) {
        const name  = names[u32()] ?? `arg${i}`;
        const tag   = u8();
        const value = tag === 2 || tag === 4 ? u64()
                    : tag === 3              ? i32()
                    : undefined;
        params.push({ name, tag, value });
      }
      events.push({ type: EV_TYPE_ENTER, ts, func, isAsync, callId, params });

    } else if (type === EV_OPTIMIZED_BATCH) {
      const count = BigInt(u32());
      const minTs = u64();
      const maxTs = u64();
      events.push({ type: EV_TYPE_OPTIMIZED_BATCH, ts, count, minTs, maxTs });

    } else if (type <= EV_ON_STACK_REPLACEMENT) {
      const func   = names[u32()] ?? '(unknown)';
      const callId = u32();
      const name   = [
        EV_TYPE_ENTER, EV_TYPE_EXIT, EV_TYPE_SUSPEND,
        EV_TYPE_RESUME, EV_TYPE_ON_STACK_REPLACEMENT,
      ][type];
      events.push({ type: name, ts, func, callId });

    } else {
      throw new Error(`Unknown event type 0x${type.toString(16)} at offset ${pos}`);
    }
  }

  return { events, names };
}

module.exports = {
  readTrace,
  PARAM_TYPE_NAMES,
  EV_ENTER,
  EV_EXIT,
  EV_SUSPEND,
  EV_RESUME,
  EV_ON_STACK_REPLACEMENT,
  EV_OPTIMIZED_BATCH,
  EV_NEWNAME,
  EV_TYPE_ENTER,
  EV_TYPE_EXIT,
  EV_TYPE_SUSPEND,
  EV_TYPE_RESUME,
  EV_TYPE_ON_STACK_REPLACEMENT,
  EV_TYPE_OPTIMIZED_BATCH,
};
