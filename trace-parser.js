'use strict';
// Canonical parser for the nodetrace binary format (v2, parent-ref).
// See deps/v8/src/trace/trace-writer.h for the authoritative wire spec.
//
// Usage:
//   const { readTrace } = require('./trace-parser');
//   const { events, names, header } = readTrace(fs.readFileSync('trace.bin'));
//
// header — { version, flags, hasParams } (or null for a headerless v1 file).
// names  — flat array; names[idx] is the interned string for that index.
// events — array of event objects; every event has { type, ts }.
//   ENTER:   { type:'ENTER', ts, func, parentId, isAsync, callId, params }
//   RESUME:  { type:'RESUME', ts, func, parentId, callId }
//   EXIT / SUSPEND / ON_STACK_REPLACEMENT: { type, ts, func, callId }
//   OPTIMIZED_BATCH: { type:'OPTIMIZED_BATCH', ts, count, minTs, maxTs }
//   META:    { type:'META', ts, holderId, key, tag, value }
//     value is a decoded string / boolean / number / undefined depending on tag.
//   ts / minTs / maxTs / count are BigInt nanoseconds since Unix epoch (except
//   count which is a plain BigInt count).

const EV_ENTER                = 0x00;
const EV_EXIT                 = 0x01;
const EV_SUSPEND              = 0x02;
const EV_RESUME               = 0x03;
const EV_ON_STACK_REPLACEMENT = 0x04;
const EV_OPTIMIZED_BATCH      = 0x05;
const EV_NEWNAME              = 0x06;
const EV_META                 = 0x07;

const EV_TYPE_ENTER                = 'ENTER';
const EV_TYPE_EXIT                 = 'EXIT';
const EV_TYPE_SUSPEND              = 'SUSPEND';
const EV_TYPE_RESUME               = 'RESUME';
const EV_TYPE_ON_STACK_REPLACEMENT = 'ON_STACK_REPLACEMENT';
const EV_TYPE_OPTIMIZED_BATCH      = 'OPTIMIZED_BATCH';
const EV_TYPE_META                 = 'META';

// Sentinel used in parent_id / holder_id fields.
const NO_CALL_ID = 0xFFFFFFFF;

const PARAM_TYPE_NAMES = [
  'undefined', 'null', 'boolean', 'integer', 'float',
  'string', 'object', 'array', 'function', 'symbol', 'bigint',
];

// File-header magic and expected version.
const FILE_MAGIC   = Buffer.from('NTRC', 'utf8');
const FILE_VERSION = 2;
const FILE_FLAG_PARAMS = 0x0001;

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
  const f64 = () => { const v = buf.readDoubleLE(pos); pos += 8; return v; };

  // Optional v2 file header.
  let header = null;
  if (buf.length >= 8 && buf.slice(0, 4).equals(FILE_MAGIC)) {
    pos = 4;
    const version = u16();
    const flags   = u16();
    if (version !== FILE_VERSION) {
      throw new Error(`Unsupported trace file version ${version} (expected ${FILE_VERSION})`);
    }
    header = { version, flags, hasParams: (flags & FILE_FLAG_PARAMS) !== 0 };
  }

  const readDelta = (ss) => {
    switch (ss) {
      case 0: return BigInt(u8());
      case 1: return BigInt(u16());
      case 2: return BigInt(u32());
      case 3: return u64();
    }
  };

  const decodeMetaValue = (tag) => {
    switch (tag) {
      case 0: return undefined;
      case 1: return null;
      case 2: return u64() !== 0n;
      case 3: return i32();
      case 4: return f64();
      case 5: return names[u32()] ?? '(unknown)';
      default: return undefined;  // object/array/function/symbol/bigint — type only
    }
  };

  while (pos < buf.length) {
    const header_byte = u8();
    const ss   = (header_byte >> 6) & 0x03;
    const type = header_byte & 0x3F;

    lastTs += readDelta(ss);
    const ts = lastTs;

    if (type === EV_NEWNAME) {
      const idx = u32();
      const len = u16();
      names[idx] = buf.toString('utf8', pos, pos + len);
      pos += len;

    } else if (type === EV_ENTER) {
      const funcIdx    = u32();
      const parentRaw  = u32();
      const parentId   = parentRaw === NO_CALL_ID ? null : parentRaw;
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
      events.push({
        type: EV_TYPE_ENTER, ts,
        func: names[funcIdx] ?? '(unknown)',
        parentId, isAsync, callId, params,
      });

    } else if (type === EV_RESUME) {
      const funcIdx   = u32();
      const parentRaw = u32();
      const parentId  = parentRaw === NO_CALL_ID ? null : parentRaw;
      const callId    = u32();
      events.push({
        type: EV_TYPE_RESUME, ts,
        func: names[funcIdx] ?? '(unknown)',
        parentId, callId,
      });

    } else if (type === EV_OPTIMIZED_BATCH) {
      const count = BigInt(u32());
      const minTs = u64();
      const maxTs = u64();
      events.push({ type: EV_TYPE_OPTIMIZED_BATCH, ts, count, minTs, maxTs });

    } else if (type === EV_META) {
      const holderRaw = u32();
      const holderId  = holderRaw === NO_CALL_ID ? null : holderRaw;
      const keyIdx    = u32();
      const tag       = u8();
      const value     = decodeMetaValue(tag);
      events.push({
        type: EV_TYPE_META, ts,
        holderId,
        key: names[keyIdx] ?? '(unknown)',
        tag, value,
      });

    } else if (type === EV_EXIT || type === EV_SUSPEND ||
               type === EV_ON_STACK_REPLACEMENT) {
      const funcIdx = u32();
      const callId  = u32();
      const name = type === EV_EXIT   ? EV_TYPE_EXIT
                 : type === EV_SUSPEND ? EV_TYPE_SUSPEND
                 : EV_TYPE_ON_STACK_REPLACEMENT;
      events.push({
        type: name, ts,
        func: names[funcIdx] ?? '(unknown)',
        callId,
      });

    } else {
      throw new Error(`Unknown event type 0x${type.toString(16)} at offset ${pos}`);
    }
  }

  return { events, names, header };
}

module.exports = {
  readTrace,
  PARAM_TYPE_NAMES,
  NO_CALL_ID,
  EV_ENTER,
  EV_EXIT,
  EV_SUSPEND,
  EV_RESUME,
  EV_ON_STACK_REPLACEMENT,
  EV_OPTIMIZED_BATCH,
  EV_NEWNAME,
  EV_META,
  EV_TYPE_ENTER,
  EV_TYPE_EXIT,
  EV_TYPE_SUSPEND,
  EV_TYPE_RESUME,
  EV_TYPE_ON_STACK_REPLACEMENT,
  EV_TYPE_OPTIMIZED_BATCH,
  EV_TYPE_META,
};
