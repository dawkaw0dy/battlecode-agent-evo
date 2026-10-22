// Replay reader: drives the visualiser engine WASM from Node and dumps a match as JSON.
// usage: node --no-warnings tools/replay.mjs replay.repl26a out.json
// The JSON has: meta (rounds, outcome, drafts, gems), rounds[] (per-round champion state),
// events[] (deaths, submits, captures, recalls) and actions (per-champion step histogram).
import { readFileSync, writeFileSync, readdirSync, statSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { inflateRawSync } from 'node:zlib';
import { WASI } from 'node:wasi';
const TYPES = ['Ryth', 'VesperVale', 'Vendetta', 'Cassi', 'Issac', 'Stellar', 'Prism', 'Marcella', 'Willow', 'Ren', 'Varg', 'Tavi', 'HeartOfRuin'];
const KINDS = 'Phase.Respawn.Capture.Tick.Move.Attack.Cast.Input.EndTurn.SetPriority..Recall.SubmitGems.Shop.PlaceWard.Consumable.CancelChannel.Sandbox.GemsSubmitted.Draft.AltarLock.TeleportChannel.TeleportComplete.MovementBlocked.Resign.SellItem.TurnRollover.BotError'.split('.');

// The CLI unpacks its assets into one hash-named directory per release; search them all, newest first.
function findAsset(sub, prefix) {
  const roots = [process.env.CAMBC_CACHE_DIR, join(homedir(), '.cache', 'cambc'), process.env.LOCALAPPDATA && join(process.env.LOCALAPPDATA, 'cambc')]
    .filter(Boolean).map(r => join(r, 'assets'));
  const hits = [];
  for (const root of roots) {
    let dirs; try { dirs = readdirSync(root); } catch { continue; }
    for (const d of dirs) {
      const dir = join(root, d, sub);
      let files; try { files = readdirSync(dir); } catch { continue; }
      for (const f of files) if (f.startsWith(prefix) && f.endsWith('.wasm')) hits.push(join(dir, f));
    }
  }
  if (!hits.length) throw new Error(`no ${sub}/${prefix}*.wasm under ${roots.join(', ')}; run any cambc command once`);
  return hits.sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs)[0];
}

// Read one stored or deflated member from a zip, without an external unzip.
function zipMember(buf, name) {
  let eocd = buf.length - 22;
  while (eocd >= 0 && buf.readUInt32LE(eocd) !== 0x06054b50) eocd--;
  if (eocd < 0) throw new Error('not a zip file');
  let at = buf.readUInt32LE(eocd + 16);
  for (let i = 0, n = buf.readUInt16LE(eocd + 10); i < n; i++) {
    const method = buf.readUInt16LE(at + 10), size = buf.readUInt32LE(at + 20);
    const nameLen = buf.readUInt16LE(at + 28), extra = buf.readUInt16LE(at + 30), comment = buf.readUInt16LE(at + 32);
    const local = buf.readUInt32LE(at + 42);
    if (buf.toString('utf8', at + 46, at + 46 + nameLen) === name) {
      const start = local + 30 + buf.readUInt16LE(local + 26) + buf.readUInt16LE(local + 28);
      const data = buf.subarray(start, start + size);
      if (method === 0) return data;
      if (method === 8) return inflateRawSync(data);
      throw new Error('unsupported zip method ' + method);
    }
    at += 46 + nameLen + extra + comment;
  }
  throw new Error(`${name} not found in zip`);
}

let bytes = readFileSync(process.argv[2]);
// A match bundle from `cambc run` is a zip holding replay.repl26a; a downloaded match is the raw file.
if (bytes[0] === 0x50 && bytes[1] === 0x4b) bytes = zipMember(bytes, 'replay.repl26a');
const dv0 = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
const ver = `v${bytes[8]}_${bytes[9]}_${bytes[10]}`;
const wasmPath = findAsset(join('visualiser', 'assets'), ver + '-');
const wasi = new WASI({ version: 'preview1', args: [], env: {} });
const mod = await WebAssembly.compile(readFileSync(wasmPath));
const inst = await WebAssembly.instantiate(mod, { ...wasi.getImportObject(), deep_wasm: { on_assert_fail_impl: () => 1 } });
wasi.initialize(inst);
const E = inst.exports;
const mem = () => new DataView(E.memory.buffer);
const chk = (c, what) => { if (c !== 0) throw new Error(`${what} failed with code ${c}`); };
const out = E.Engine_Malloc(64);
const p = E.Engine_Malloc(bytes.length);
new Uint8Array(E.memory.buffer, p, bytes.length).set(bytes);
chk(E.Game_FromReplayFileBytes(p, bytes.length, out), 'FromReplayFileBytes');
const game = mem().getBigUint64(out, true);
E.Engine_Free(p);
if (E.Game_Visualiser_SnapshotSize() !== 184696) throw new Error('snapshot layout');
const snap = E.Engine_Malloc(184696);
const args = E.Engine_Malloc(128);
const persp = args + 48;
mem().setUint32(persp, 2, true); mem().setUint32(persp + 4, 0xffffffff, true);
chk(E.Game_Visualiser_Timeline_GetMain(game, out), 'GetMain');
const tl = mem().getBigUint64(out, true);
chk(E.Game_Visualiser_Timeline_Info(game, tl, args, args + 8, args + 16), 'Info');
const size = mem().getUint32(args, true);

function read() {
  chk(E.Game_Visualiser_ReadState(game, persp, snap), 'ReadState');
  const m = mem();
  const u = (o) => m.getUint32(snap + o, true), i = (o) => m.getInt32(snap + o, true), f = (o) => m.getFloat32(snap + o, true);
  const h = {
    round: u(8), turn: u(12), team: u(16), awaitingInput: !!u(20), phase: u(24), outcome: u(28),
    gems: [Number(m.getBigUint64(snap + 32, true)), Number(m.getBigUint64(snap + 40, true))],
    inputPosition: u(56), currentChampion: u(60),
    kind: u(68), actorKind: u(72), actor: u(76), from: [i(80), i(84)], to: [i(88), i(92)], substep: u(104), slot: u(108),
  };
  const champs = [];
  const nc = u(120);
  for (let k = 0; k < nc; ++k) {
    const n = 140 + k * 456;
    const idx = u(n + 4);
    champs.push({ idx, team: u(n + 8), type: u(n + 12), x: i(n + 20), y: i(n + 24), hp: f(n + 28), max: f(n + 32), alive: !!u(n + 224), gold: u(n + 232), gems: u(n + 236), kills: u(184504 + idx * 12) });
  }
  const camps = [];
  const ncamp = u(124);
  for (let k = 0; k < ncamp; ++k) {
    const n = 7436 + k * 232;
    camps.push({ idx: u(n + 4), type: u(n + 12), x: i(n + 20), y: i(n + 24), hp: f(n + 28) });
  }
  const groups = [];
  const ng = u(128);
  for (let k = 0; k < ng; ++k) {
    const n = 123436 + k * 20;
    groups.push({ reward: !!u(n + 4), team: u(n + 8) });
  }
  const drafts = [[], []];
  for (let t = 0; t < 2; ++t) for (let k = 0; k < 5; ++k) drafts[t].push(i(179516 + 12 + t * 20 + k * 4));
  return { h, champs, camps, groups, drafts };
}

chk(E.Game_Visualiser_Timeline_Seek(game, tl, 0, 0), 'Seek');
let s = read();
const meta = { engine: ver, size, drafts: s.drafts };
const rounds = [];
const events = [];
const actions = {};
let prev = s;
let lastRound = -1;
const prevByIdx = new Map();
for (const c of s.champs) prevByIdx.set(c.idx, c);
let lastAttack = null; // {round, actorKind, actor, target}
const actionsOf = (idx) => (actions[idx] ??= {});
for (let guard = 0; guard < 5_000_000; ++guard) {
  const { h } = s;
  if (h.round !== lastRound) {
    lastRound = h.round;
    rounds.push({ r: h.round, gems: h.gems, c: s.champs.map(c => [c.idx, c.alive ? 1 : 0, c.x, c.y, Math.round(c.hp), c.gold, c.gems]), k: s.camps.map(c => Math.round(c.hp)) });
    if (rounds.length === 1) meta.camps0 = s.camps.map(c => [c.type, c.x, c.y]);
  }
  // per-champion step histogram (actor kind 1 = champion)
  if (h.actorKind === 1 && h.kind > 0) {
    const a = actionsOf(h.actor); const k = KINDS[h.kind] || String(h.kind); a[k] = (a[k] || 0) + 1;
  }
  if (h.kind === 5 || h.kind === 6) lastAttack = { round: h.round, actorKind: h.actorKind, actor: h.actor, to: h.to, kind: h.kind };
  // transitions
  for (const c of s.champs) {
    const q = prevByIdx.get(c.idx);
    if (q) {
      if (q.alive && !c.alive) events.push({ e: 'death', r: h.round, idx: c.idx, team: c.team, type: TYPES[c.type], x: q.x, y: q.y, gems: q.gems, by: lastAttack, step: KINDS[h.kind], actorKind: h.actorKind, actor: h.actor });
      if (!q.alive && c.alive) events.push({ e: 'respawn', r: h.round, idx: c.idx });
      if (c.gems < q.gems && c.alive && q.alive) events.push({ e: 'submit', r: h.round, idx: c.idx, team: c.team, gems: q.gems - c.gems, x: c.x, y: c.y });
      if (c.gems > q.gems) events.push({ e: 'gain', r: h.round, idx: c.idx, team: c.team, gems: c.gems - q.gems, x: c.x, y: c.y });
    }
    prevByIdx.set(c.idx, c);
  }
  if (h.kind === 11 && h.actorKind === 1) events.push({ e: 'recall', r: h.round, idx: h.actor });
  if (h.kind === 12 && h.actorKind === 1) events.push({ e: 'tribute', r: h.round, idx: h.actor });
  if (h.kind === 2) events.push({ e: 'capture', r: h.round, x: h.to[0], y: h.to[1] });
  if (h.outcome || (h.awaitingInput && h.inputPosition >= size)) break;
  chk(E.Game_Visualiser_Timeline_ReplayNext(game, 1), 'ReplayNext');
  const n = read();
  if (n.h.inputPosition === h.inputPosition && n.h.substep === h.substep) throw new Error('no progress');
  prev = s; s = n;
}
meta.rounds = s.h.round; meta.outcome = s.h.outcome; meta.gems = s.h.gems;
meta.champs = s.champs.map(c => ({ idx: c.idx, team: c.team, type: TYPES[c.type], kills: c.kills, gold: c.gold }));
chk(E.Game_GetBitboardMetadata(game, out), 'meta');
meta.w = mem().getInt32(out, true); meta.h = mem().getInt32(out + 4, true);
meta.camps = prev.camps.map(c => [c.type, c.x, c.y]);
writeFileSync(process.argv[3], JSON.stringify({ meta, rounds, events, actions }));
console.log(JSON.stringify({ ...meta, events: events.length }));
