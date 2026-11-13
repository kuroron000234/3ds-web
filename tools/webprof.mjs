// ワーカー（エミュレーションスレッド）の CPU プロファイルを取り、自己時間の多い関数を出す
import { createRequire } from 'node:module';
const require = createRequire(process.env.HOME + '/.npm-packages/lib/node_modules/@playwright/mcp/');
const { chromium } = require('playwright-core');
const [rom, port, jit] = process.argv.slice(2);
const browser = await chromium.launch({ executablePath: '/tmp/pw/chromium-1247/chrome-linux64/chrome', headless: true });
const page = await browser.newPage();
const cdp = await page.context().newCDPSession(page);
const sessions = new Map(); let seq = 1; const pending = new Map();
cdp.on('Target.attachedToTarget', e => sessions.set(e.sessionId, e.targetInfo));
cdp.on('Target.receivedMessageFromTarget', e => { const m = JSON.parse(e.message); if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result || m); pending.delete(m.id); } });
const send = (sessionId, method, params = {}) => new Promise(r => { const id = seq++; pending.set(id, r); cdp.send('Target.sendMessageToTarget', { sessionId, message: JSON.stringify({ id, method, params }) }).catch(() => r(null)); });
await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: false });
await page.goto(`http://127.0.0.1:${port}/`);
await page.waitForFunction(() => window.__ready, null, { timeout: 60000 });
await page.evaluate(async ({ rom, jit }) => {
  const o = (k, v) => Module.ccall('web_set_option', null, ['string', 'string'], [k, v]);
  o('citra_graphics_api', 'Software'); o('web_jit', jit);
  const blob = await (await fetch('/' + rom)).blob();
  await window.mountRom('/roms/' + rom, new File([blob], rom));
  Module.ccall('web_load_game', 'boolean', ['string'], ['/roms/' + rom]);
}, { rom, jit });
await new Promise(r => setTimeout(r, 8000));
for (const id of sessions.keys()) { await send(id, 'Profiler.enable'); await send(id, 'Profiler.setSamplingInterval', { interval: 2000 }); await send(id, 'Profiler.start'); }
await new Promise(r => setTimeout(r, 5000));
let best = null;
for (const id of sessions.keys()) {
  const res = await Promise.race([send(id, 'Profiler.stop'), new Promise(r => setTimeout(() => r(null), 8000))]);
  const p = res && res.profile; if (!p) continue;
  const self = new Map(); const byId = new Map(p.nodes.map(n => [n.id, n]));
  for (const s of p.samples) { const n = byId.get(s); const k = n.callFrame.functionName || '(anon ' + n.callFrame.url.slice(-20) + ')'; self.set(k, (self.get(k) || 0) + 1); }
  let busy = p.samples.length; for (const [k, v] of self) if (/idle|program|futex|wait|garbage/i.test(k)) busy -= v;
  if (!best || busy > best.busy) best = { busy, total: p.samples.length, self };
}
if (best) { console.log('samples', best.total, 'busy', best.busy); for (const [k, v] of [...best.self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 22)) console.log((v / best.total * 100).toFixed(1).padStart(5) + '%  ' + k.slice(0, 110)); }
await browser.close(); process.exit(0);
