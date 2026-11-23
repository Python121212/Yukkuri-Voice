/**
 * 読み辞書の生成スクリプト (kuromoji の IPADIC から抽出)
 *
 *   node scripts/build-dict.mjs
 *
 * 出力: app/src/main/assets/reading_dict.bin
 *
 * 形式 (little-endian):
 *   magic  = "YKR1" (0x31524B59)
 *   count  : uint32
 *   index  : count * (uint32 keyOffset, uint16 keyLen, uint16 valueLen)
 *   pool   : UTF-8 文字列 (キーと値が交互)
 *
 * 辞書は「表層形 -> 読み (カタカナ)」のみ。同じ表層形に複数の読みがある場合は
 * 出現コストが最小のものを採用する。
 */
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const DICT_DIR = process.env.KUROMOJI_DICT || 'node_modules/kuromoji/dict';
const OUT = process.env.OUT_DICT || 'app/src/main/assets/reading_dict.bin';

const gunzip = (f) => {
  const b = zlib.gunzipSync(fs.readFileSync(path.join(DICT_DIR, f)));
  return new Uint8Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
};

const tid = gunzip('tid.dat.gz');
const posBuf = gunzip('tid_pos.dat.gz');
const tidView = new DataView(tid.buffer);
const posView = new DataView(posBuf.buffer);
const dec = new TextDecoder();

function cstr(view, base, off) {
  let end = off;
  while (end < view.byteLength && view.getUint8(end) !== 0) end++;
  return dec.decode(new Uint8Array(base, off, end - off));
}

const n = tid.byteLength / 10;
const best = new Map();          // surface -> {reading, cost}
const KANJI = /[\u3400-\u4DBF\u4E00-\u9FFF\u3005\u3006]/;
const KATA = /[\u30A1-\u30FA\u30FC]/;

for (let i = 0; i < n; i++) {
  const o = i * 10;
  const cost = tidView.getInt16(o + 4, true);
  const posOff = tidView.getUint32(o + 6, true);
  const raw = cstr(posView, posBuf.buffer, posOff);
  const parts = raw.split(',');
  const surface = parts[0];
  const reading = parts[parts.length - 2];   // 発音 (カタカナ)
  if (!surface || !reading) continue;
  if (!(KANJI.test(surface) || KATA.test(surface))) continue;  // かな表記はそのまま使える
  if (!/^[\u30A1-\u30FA\u30FC]+$/.test(reading)) continue;     // 読みがカタカナでないものは捨てる
  if (surface.length > 24) continue;
  const prev = best.get(surface);
  if (!prev || cost < prev.cost) best.set(surface, { reading, cost });
}

// 表層形でソートして index を作る (二分探索用)
const entries = [...best.entries()].map(([k, v]) => [k, v.reading])
  .filter(([k, v]) => k !== v)
  .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));

const enc = new TextEncoder();
const pool = [];
let poolLen = 0;
const index = [];
for (const [k, v] of entries) {
  const kb = enc.encode(k), vb = enc.encode(v);
  index.push([poolLen, kb.length, vb.length]);
  pool.push(kb, vb);
  poolLen += kb.length + vb.length;
}
const total = 12 + index.length * 8 + poolLen;
const out = new Int32Array();            // placeholder
const buf = Buffer.alloc(total);
let p = 0;
buf.write('YKR1', 0, 'ascii'); p = 4;
buf.writeUInt32LE(entries.length, p); p += 4;
p += 4;                                   // reserved
for (const [off, kl, vl] of index) {
  buf.writeUInt32LE(off, p); p += 4;
  buf.writeUInt16LE(kl, p); p += 2;
  buf.writeUInt16LE(vl, p); p += 2;
}
for (const b of pool) { Buffer.from(b).copy(buf, p); p += b.length; }

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, buf);
console.log(`entries=${entries.length} bytes=${total} (${(total / 1048576).toFixed(2)} MB) -> ${OUT}`);
