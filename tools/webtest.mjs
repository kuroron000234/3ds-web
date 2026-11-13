#!/usr/bin/env node
// ラップトップ側のブラウザテスト。tools/laptop-agent.sh test から呼ばれる。
// エージェントの成果物を COOP/COEP 付きで配信し、ヘッドレスの Chrome で ROM を起動して
// ログ・速度・スクリーンショットを JSON で標準出力に返す。
//
// オプション:
//   --agent <name>          必須。~/3ds-web-build/agents/<name>/ の成果物を使う
//   --rom <file>            ~/3ds-web-build/roms/ のファイル名（既定 boot.3dsx）
//   --graphics <api>        OpenGL | Software（既定 OpenGL）
//   --option key=value      コア設定の上書き（何度でも）
//   --seconds <n>           ROM 読み込み後に動かす秒数（既定 10）
//   --log-lines <n>         ログの末尾を何行返すか（既定 40）
//   --chromebook            Chromebook 相当の計測（CPU・メモリの制限は laptop-agent.sh が掛ける）
//
// 出力の speedPercent は実機比の速度（web_get_speed、旧ビルドでは null）、emuBusyPercent は
// エミュレーションスレッドが retro_run にいた割合、mainThread* はブラウザのメインスレッドの空き具合。
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { readFile, stat } from 'node:fs/promises';
import { homedir } from 'node:os';
import path from 'node:path';

// 既定はラップトップの配置。Cloud Shell では tools/local.sh が環境変数で切り替える
const ROOT = process.env.WEBTEST_ROOT || path.join(homedir(), '3ds-web-build');
const require = createRequire((process.env.WEBTEST_NODE || path.join(ROOT, 'node')) + '/');
const { chromium } = require('playwright-core');

const opts = { rom: 'boot.3dsx', graphics: 'OpenGL', seconds: 10, logLines: 40, options: [], chromebook: false };
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--agent') opts.agent = argv[++i];
  else if (a === '--rom') opts.rom = argv[++i];
  else if (a === '--graphics') opts.graphics = argv[++i];
  else if (a === '--option') opts.options.push(argv[++i]);
  else if (a === '--seconds') opts.seconds = Number(argv[++i]);
  else if (a === '--log-lines') opts.logLines = Number(argv[++i]);
  else if (a === '--chromebook') opts.chromebook = true;
  else throw new Error('不明なオプション: ' + a);
}
if (!opts.agent) throw new Error('--agent が必要');

const A = path.join(ROOT, 'agents', opts.agent);
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };

// index.html はエージェントの web/、成果物は out/、ROM は共有の roms/ から返す
async function resolveFile(name) {
  for (const dir of [path.join(A, 'out'), path.join(A, 'web'), path.join(ROOT, 'roms')]) {
    const p = path.join(dir, name);
    try { if ((await stat(p)).isFile()) return p; } catch {}
  }
  return null;
}

const server = createServer(async (req, res) => {
  const name = path.basename(decodeURIComponent(req.url.split('?')[0])) || 'index.html';
  const file = await resolveFile(name === '/' ? 'index.html' : name);
  const headers = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cache-Control': 'no-store',
  };
  if (!file) { res.writeHead(404, headers); res.end(); return; }
  headers['Content-Type'] = MIME[path.extname(file)] || 'application/octet-stream';
  res.writeHead(200, headers);
  res.end(await readFile(file));
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/`;

// GPU は内蔵の Intel UHD を使う（ハイブリッド構成の既定。NVIDIA は使わない）
const browser = await chromium.launch({
  executablePath: process.env.WEBTEST_CHROME || '/usr/bin/google-chrome',
  headless: true,
  // Cloud Shell には GPU が無いので、既定の SwiftShader（CPU 描画）に任せる
  args: process.env.WEBTEST_NO_GPU ? [] : ['--enable-gpu', '--ignore-gpu-blocklist', '--use-angle=gl-egl'],
});
const result = { agent: opts.agent, rom: opts.rom, graphics: opts.graphics, chromebook: opts.chromebook };
try {
  const page = await browser.newPage({ viewport: { width: 900, height: 900 } });
  const pageErrors = [];
  const consoleErrors = [];
  page.on('pageerror', e => pageErrors.push(String(e.stack || e).slice(0, 1500)));
  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text().slice(0, 300)); });
  page.on('crash', () => pageErrors.push('ページがクラッシュした'));

  const t0 = Date.now();
  await page.goto(url);
  await page.waitForFunction(() => window.__ready, null, { timeout: 60000 });
  result.readyMs = Date.now() - t0;

  const run = await page.evaluate(async ({ rom, graphics, options, seconds }) => {
    const out = {};
    const gl = document.createElement('canvas').getContext('webgl2');
    const dbg = gl && gl.getExtension('WEBGL_debug_renderer_info');
    out.webgl = gl ? (dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER)) : 'WebGL2 なし';
    out.cores = navigator.hardwareConcurrency;
    try {
      Module.ccall('web_set_option', null, ['string', 'string'], ['citra_graphics_api', graphics]);
      for (const kv of options) {
        const i = kv.indexOf('=');
        Module.ccall('web_set_option', null, ['string', 'string'], [kv.slice(0, i), kv.slice(i + 1)]);
      }
      const blob = await (await fetch('/' + rom)).blob();
      if (window.mountRom) await window.mountRom('/roms/' + rom, new File([blob], rom));
      else Module.FS.writeFile('/roms/' + rom, new Uint8Array(await blob.arrayBuffer()));
      const t = performance.now();
      out.loaded = await (async (path) => {
      const started = Module.ccall('web_load_game', 'boolean', ['string'], [path]);
      if (!Module._web_load_state) return started; // 旧ビルドは同期で結果を返す
      if (!started) return false;
      for (;;) {
        const s = Module._web_load_state();
        if (s >= 2) return s === 2;
        await new Promise(r => setTimeout(r, 50));
      }
    })('/roms/' + rom);
      out.loadMs = Math.round(performance.now() - t);
    } catch (e) {
      out.exception = String(e && e.stack || e).slice(0, 2000);
    }
    const f0 = (() => { try { return Module.ccall('web_get_frame_count', 'number', [], []); } catch { return 0; } })();
    // メインスレッドの空き具合: rAF の回数と、50ms を超えたタスク（longtask）の合計時間
    let rafs = 0, longMs = 0, rafOn = true;
    const tick = () => { rafs++; if (rafOn) requestAnimationFrame(tick); };
    requestAnimationFrame(tick);
    const po = new PerformanceObserver(l => { for (const e of l.getEntries()) longMs += e.duration; });
    try { po.observe({ entryTypes: ['longtask'] }); } catch {}
    const s0 = performance.now();
    await new Promise(r => setTimeout(r, seconds * 1000));
    rafOn = false;
    po.disconnect();
    const sec = (performance.now() - s0) / 1000;
    out.mainThreadRafPerSec = Math.round(rafs / sec * 10) / 10;
    out.mainThreadLongTaskPercent = Math.round(longMs / (sec * 10));
    const opt = (name) => { try { return Math.round(Module.ccall(name, 'number', [], []) * 10) / 10; } catch { return null; } };
    out.speedPercent = opt('web_get_speed');
    out.emuBusyPercent = opt('web_get_busy');
    // 内訳（%）: GPU コマンド処理・画面出力・音声・処理の外で失った分。JIT で変換した関数の数
    try {
      out.profPercent = [0, 1, 2, 3].map(i => Math.round(Module.ccall('web_get_prof', 'number', ['number'], [i]) * 10) / 10);
      out.jitFunctions = Module.ccall('web_jit_function_count', 'number', [], []);
      // tools/cpubench の記録: 周回数（速度の指標）と途中のチェックサム（正しさの確認）
      if (Module.ccall('web_peek32', 'number', ['number'], [0x00102000]) === 0x42454E43) {
        out.benchIterations = Module.ccall('web_peek32', 'number', ['number'], [0x00102004]);
        out.benchChecksum = (Module.ccall('web_peek32', 'number', ['number'], [0x00102008]) >>> 0).toString(16);
      }
    } catch {}
    try {
      const frames = Module.ccall('web_get_frame_count', 'number', [], []) - f0;
      out.frames = frames;
      out.fps = Math.round(frames / ((performance.now() - s0) / 1000) * 10) / 10;
      out.avgFrameMs = Math.round(Module.ccall('web_get_frame_ms', 'number', [], []) * 10) / 10;
    } catch (e) {
      out.statsError = String(e).slice(0, 300);
    }
    const lines = document.getElementById('log').textContent.split('\n');
    const count = (arr, norm) => {
      const m = new Map();
      for (const l of arr) { const k = norm(l); m.set(k, (m.get(k) || 0) + 1); }
      return [...m.entries()].sort((a, b) => b[1] - a[1]).slice(0, 30).map(([k, n]) => `${n}x ${k}`);
    };
    out.errors = count(
      lines.filter(l => /\[(ERROR|CRITICAL)\]|<Critical>|<Error>/.test(l) && !l.includes('FetchVariable')),
      l => l.replace(/^\[[A-Z]+\] \[\s*[\d.]+\] /, '').slice(0, 220));
    out.glslErrors = count(lines.filter(l => /^(ERROR|WARNING): \d+:/.test(l)), l => l.replace(/\d+:\d+/, 'N:N').slice(0, 200));
    out.logTail = lines.filter(l => !l.includes('FetchVariable')).slice(-40);
    return out;
  }, { rom: opts.rom, graphics: opts.graphics, options: opts.options, seconds: opts.seconds });
  Object.assign(result, run);
  result.logTail = run.logTail.slice(-opts.logLines);

  const shot = path.join(A, 'test', 'shot.png');
  await page.locator('#canvas').screenshot({ path: shot, timeout: 10000 }).then(
    () => { result.screenshot = shot; },
    e => { result.screenshotError = String(e).slice(0, 200); });
  result.pageErrors = pageErrors;
  result.consoleErrors = consoleErrors.slice(0, 20);
} catch (e) {
  result.harnessError = String(e && e.stack || e).slice(0, 2000);
} finally {
  await browser.close();
  server.close();
}
console.log(JSON.stringify(result, null, 1));
