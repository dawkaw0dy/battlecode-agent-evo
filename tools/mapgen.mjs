import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { WASI } from 'node:wasi';

// The CLI unpacks its assets into one hash-named directory per release; use the newest map-editor engine.
function newestMapEditor() {
  const roots = [process.env.CAMBC_CACHE_DIR, join(homedir(), '.cache', 'cambc'), process.env.LOCALAPPDATA && join(process.env.LOCALAPPDATA, 'cambc')]
    .filter(Boolean).map(r => join(r, 'assets'));
  const version = f => f.match(/^v(\d+)_(\d+)_(\d+)-/).slice(1).map(Number);
  const newer = (a, b) => { const x = version(a), y = version(b); for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] > y[i]; return false; };
  let best = null;
  for (const root of roots) {
    let dirs; try { dirs = readdirSync(root); } catch { continue; }
    for (const d of dirs) {
      const dir = join(root, d, 'mapeditor', 'assets');
      let files; try { files = readdirSync(dir); } catch { continue; }
      for (const f of files) if (/^v\d+_\d+_\d+-.*\.wasm$/.test(f) && (!best || newer(f, best.f))) best = { f, path: join(dir, f) };
    }
  }
  if (!best) throw new Error(`no mapeditor/assets/v*.wasm under ${roots.join(', ')}; run any cambc command once`);
  return best.path;
}
const WASM = newestMapEditor();
const wasi = new WASI({ version: 'preview1', args: [], env: {} });
const mod = await WebAssembly.compile(readFileSync(WASM));
const imports = { ...wasi.getImportObject(), deep_wasm: { on_assert_fail_impl: (a,b,c,d)=>{ console.error('assert fail'); return 1; } } };
const inst = await WebAssembly.instantiate(mod, imports);
wasi.initialize(inst);
const E = inst.exports;
const out = E.Engine_Malloc(64);
const mem = () => new DataView(E.memory.buffer);
const chk = (c, what) => { if (c !== 0) throw new Error(`${what} failed with code ${c}`); };
export function loadMap(path) {
  const b = readFileSync(path); const p = E.Engine_Malloc(b.length);
  new Uint8Array(E.memory.buffer, p, b.length).set(b);
  chk(E.MapBuilder_FromFileBytes(p, b.length, out), 'FromFileBytes'); E.Engine_Free(p);
  return mem().getBigUint64(out, true);
}
export function info(h) {
  chk(E.MapBuilder_GetBitboardMetadata(h, out), 'meta'); const w = mem().getInt32(out, true), hh = mem().getInt32(out+4, true);
  chk(E.MapBuilder_GetTeamASpawnArea(h, out), 'spawn'); const sp = [0,1,2,3].map(i => mem().getInt32(out+4*i, true));
  chk(E.MapBuilder_GetSymmetry(h, out), 'sym'); const sym = mem().getInt32(out, true);
  chk(E.MapBuilder_GetNumChampsPerTeam(h, out), 'nc'); const nc = mem().getUint32(out, true);
  chk(E.MapBuilder_GetNumCamps(h, out), 'ncamps'); const n = mem().getUint32(out, true);
  const camps = [];
  for (let i = 0; i < n; i++) { chk(E.MapBuilder_GetCampHandle(h, i, out), 'ch'); const ch = mem().getBigUint64(out, true); chk(E.MapBuilder_GetCamp(h, ch, out), 'gc'); camps.push([mem().getInt32(out,true), mem().getInt32(out+4,true), mem().getInt32(out+8,true)]); }
  return { w, h: hh, spawn: sp, sym, nc, camps };
}
if (process.argv[2] === 'info') console.log(JSON.stringify(info(loadMap(process.argv[3]))));

// ---- arena generation ----
// spec: { w, h, sym (0 horiz,1 vert,2 rot), champs, spawn:[minX,minY,maxX,maxY], walls:[[x,y]], bushes, holes, altars:[[x,y]], camps:[[type,x,y]] }
export function build(spec) {
  chk(E.MapBuilder_Create(spec.w, spec.h, spec.sym, out), 'Create');
  const h = mem().getBigUint64(out, true);
  chk(E.MapBuilder_SetNumChampsPerTeam(h, spec.champs), 'champs');
  const sp = spec.spawn; [0,1,2,3].forEach(i => mem().setInt32(out+4*i, sp[i], true));
  chk(E.MapBuilder_SetTeamASpawnArea(h, out), 'spawn');
  for (const [x,y] of spec.walls||[]) chk(E.MapBuilder_AddWall(h, x, y), `wall ${x},${y}`);
  for (const [x,y] of spec.bushes||[]) chk(E.MapBuilder_AddBush(h, x, y), `bush ${x},${y}`);
  for (const [x,y] of spec.holes||[]) chk(E.MapBuilder_AddHole(h, x, y), `hole ${x},${y}`);
  for (const [x,y] of spec.altars||[]) chk(E.MapBuilder_AddAltar(h, x, y), `altar ${x},${y}`);
  for (const [t,x,y] of spec.camps||[]) chk(E.MapBuilder_AddCamp(h, t, x, y, out), `camp ${t} ${x},${y}`);
  chk(E.MapBuilder_VerifyIntegrity(h), 'VerifyIntegrity');
  chk(E.MapBuilder_Encode(h, out, out+4), 'Encode');
  const p = mem().getUint32(out, true), n = mem().getUint32(out+4, true);
  return Buffer.from(new Uint8Array(E.memory.buffer, p, n));
}
if (process.argv[2] === 'build') {
  const spec = JSON.parse(readFileSync(process.argv[3], 'utf8'));
  writeFileSync(process.argv[4], build(spec));
  console.log('wrote', process.argv[4], JSON.stringify(info(loadMap(process.argv[4]))));
}
