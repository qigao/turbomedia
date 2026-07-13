#!/usr/bin/env node

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const EXAMPLES_DIR = __dirname;
const REPO_ROOT = path.resolve(EXAMPLES_DIR, '..', '..');
const HTML_PATH = path.join(EXAMPLES_DIR, 'browser_interop.html');

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function usage() {
  console.log(`Usage: node webrtc/examples/browser_interop_smoke.js [options]

Options:
  --chrome <path>     Browser executable path
  --native <path>     browser_interop executable path
  --timeout <ms>      Overall wait budget per phase (default: 30000)
  --debug-port <n>    Chrome DevTools port (default: 9226)
  --with-stun         Run native example without --no-stun override
  --help              Show this help
`);
}

function parseArgs(argv) {
  const options = {
    chrome: process.env.CHROME_PATH || '',
    native: process.env.BROWSER_INTEROP_BIN || '',
    timeoutMs: 30000,
    debugPort: 9226,
    useStun: false,
  };

  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    }
    if (arg === '--with-stun') {
      options.useStun = true;
      continue;
    }
    if (arg === '--chrome' && i + 1 < argv.length) {
      options.chrome = argv[++i];
      continue;
    }
    if (arg === '--native' && i + 1 < argv.length) {
      options.native = argv[++i];
      continue;
    }
    if (arg === '--timeout' && i + 1 < argv.length) {
      options.timeoutMs = Number(argv[++i]) || options.timeoutMs;
      continue;
    }
    if (arg === '--debug-port' && i + 1 < argv.length) {
      options.debugPort = Number(argv[++i]) || options.debugPort;
      continue;
    }
    throw new Error(`unknown argument: ${arg}`);
  }

  if (!options.chrome) {
    const candidates = process.platform === 'win32'
      ? [
          'C:/Program Files/Google/Chrome/Application/chrome.exe',
          'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
          'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
          'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
        ]
      : [
          '/usr/bin/google-chrome',
          '/usr/bin/google-chrome-stable',
          '/usr/bin/chromium',
          '/usr/bin/chromium-browser',
        ];
    options.chrome = candidates.find((candidate) => fs.existsSync(candidate)) || '';
  }

  if (!options.native) {
    const candidates = process.platform === 'win32'
      ? [
          path.join(REPO_ROOT, 'build', 'Msvc-Release', 'bin', 'browser_interop.exe'),
          path.join(REPO_ROOT, 'build', 'Msvc', 'bin', 'browser_interop.exe'),
        ]
      : [
          path.join(REPO_ROOT, 'build', 'bin', 'browser_interop'),
          path.join(REPO_ROOT, 'build', 'linux-release', 'bin', 'browser_interop'),
        ];
    options.native = candidates.find((candidate) => fs.existsSync(candidate)) || '';
  }

  if (!options.chrome) {
    throw new Error('browser executable not found; pass --chrome <path> or set CHROME_PATH');
  }
  if (!options.native) {
    throw new Error('browser_interop executable not found; pass --native <path> or set BROWSER_INTEROP_BIN');
  }

  return options;
}

async function waitFor(fn, timeoutMs, label) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const value = await fn();
    if (value) {
      return value;
    }
    await sleep(200);
  }
  throw new Error(`timeout waiting for ${label}`);
}

async function fetchJson(url) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`${url} -> ${response.status}`);
  }
  return response.json();
}

class CdpClient {
  constructor(wsUrl) {
    this.ws = new WebSocket(wsUrl);
    this.nextId = 1;
    this.pending = new Map();
    this.ready = new Promise((resolve, reject) => {
      this.ws.onopen = resolve;
      this.ws.onerror = reject;
    });
    this.ws.onmessage = (event) => {
      const message = JSON.parse(event.data.toString());
      if (!message.id) {
        return;
      }
      const pending = this.pending.get(message.id);
      if (!pending) {
        return;
      }
      this.pending.delete(message.id);
      if (message.error) {
        pending.reject(new Error(JSON.stringify(message.error)));
      } else {
        pending.resolve(message.result);
      }
    };
  }

  async send(method, params = {}) {
    await this.ready;
    const id = this.nextId++;
    const result = new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
    });
    this.ws.send(JSON.stringify({ id, method, params }));
    return result;
  }

  close() {
    try {
      this.ws.close();
    } catch {
      /* Ignore close errors during shutdown. */
    }
  }
}

function synthesizeOffer(localSdp, candidateLines) {
  const base = localSdp.replace(/\r?\n/g, '\r\n').replace(/\r\n*$/, '\r\n');
  const candidates = candidateLines
    .map((line) => line.trim())
    .filter((line) => line.length > 0)
    .map((line) => `a=${line.replace(/^a=/, '')}\r\n`)
    .join('');
  return `${base}${candidates}a=end-of-candidates\r\n`;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const chromeUserData = fs.mkdtempSync(path.join(os.tmpdir(), 'turbonet-chrome-'));

  let chrome = null;
  let native = null;
  let cdp = null;
  let nativeStdout = '';
  let nativeStderr = '';

  try {
    chrome = spawn(options.chrome, [
      '--headless=new',
      `--remote-debugging-port=${options.debugPort}`,
      `--user-data-dir=${chromeUserData}`,
      '--no-first-run',
      '--no-default-browser-check',
      '--allow-file-access-from-files',
      '--autoplay-policy=no-user-gesture-required',
      `file:///${HTML_PATH.replace(/\\/g, '/')}`,
    ], { stdio: ['ignore', 'pipe', 'pipe'] });
    chrome.stderr.on('data', (data) => process.stderr.write(data.toString()));

    const page = await waitFor(async () => {
      try {
        const targets = await fetchJson(`http://127.0.0.1:${options.debugPort}/json/list`);
        return targets.find((target) => target.type === 'page' && target.url.startsWith('file:///')) || null;
      } catch {
        return null;
      }
    }, options.timeoutMs, 'chrome devtools page');

    cdp = new CdpClient(page.webSocketDebuggerUrl);
    await cdp.ready;
    await cdp.send('Runtime.enable');
    await cdp.send('Page.enable');

    await waitFor(async () => {
      const result = await cdp.send('Runtime.evaluate', {
        expression: 'document.readyState',
        returnByValue: true,
      });
      return result?.result?.value === 'complete';
    }, options.timeoutMs, 'page load');

    const nativeArgs = options.useStun ? [] : ['--no-stun'];
    native = spawn(options.native, nativeArgs, {
      cwd: REPO_ROOT,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    native.stdout.on('data', (data) => {
      const text = data.toString();
      nativeStdout += text;
      process.stdout.write(text);
    });
    native.stderr.on('data', (data) => {
      const text = data.toString();
      nativeStderr += text;
      process.stderr.write(text);
    });

    await waitFor(
      () => nativeStdout.includes('Paste browser SDP offer after ICE gathering completes'),
      options.timeoutMs,
      'native SDP prompt'
    );

    await cdp.send('Runtime.evaluate', {
      expression: 'createOffer()',
      awaitPromise: true,
      returnByValue: true,
    });

    const offerParts = await waitFor(async () => {
      const result = await cdp.send('Runtime.evaluate', {
        expression: `(() => ({
          sdp: document.getElementById('localSdp').value,
          candidates: Array.from(document.querySelectorAll('#localCandidates .log-entry')).map((entry) => entry.textContent)
        }))()`,
        returnByValue: true,
      });
      const value = result?.result?.value;
      return value && value.sdp && value.candidates && value.candidates.length > 0 ? value : null;
    }, options.timeoutMs, 'browser offer and candidates');

    native.stdin.write(synthesizeOffer(offerParts.sdp, offerParts.candidates).replace(/\r\n/g, '\n'));
    native.stdin.write('\n');

    const answer = await waitFor(() => {
      const match = nativeStdout.match(/-----BEGIN SDP ANSWER-----\r?\n([\s\S]*?)-----END SDP ANSWER-----/);
      return match ? match[1] : null;
    }, options.timeoutMs, 'native SDP answer');

    await cdp.send('Runtime.evaluate', {
      expression: `(() => {
        document.getElementById('remoteSdp').value = ${JSON.stringify(answer)};
        return setRemoteAnswer();
      })()`,
      awaitPromise: true,
      returnByValue: true,
    });

    const snapshot = await waitFor(async () => {
      const result = await cdp.send('Runtime.evaluate', {
        expression: `(() => ({
          ice: pc ? pc.iceConnectionState : null,
          conn: pc ? pc.connectionState : null,
          dc: dataChannel ? dataChannel.readyState : null,
          messages: document.getElementById('messageLog').innerText,
          debug: document.getElementById('debugLog').innerText
        }))()`,
        returnByValue: true,
      });
      const value = result?.result?.value;
      return value && value.dc === 'open' ? value : null;
    }, options.timeoutMs, 'browser datachannel open');

    const stats = await cdp.send('Runtime.evaluate', {
      expression: `(() => pc ? pc.getStats().then((report) => {
        const out = [];
        report.forEach((value) => out.push(value));
        return out.filter((value) =>
          value.type === 'candidate-pair' ||
          value.type === 'transport' ||
          value.type === 'data-channel'
        );
      }) : null)()`,
      awaitPromise: true,
      returnByValue: true,
    });

    console.log('\n=== Browser Snapshot ===');
    console.log(JSON.stringify(snapshot, null, 2));
    console.log('\n=== Browser Stats ===');
    console.log(JSON.stringify(stats?.result?.value || [], null, 2));

    if (!snapshot.messages.includes('Hello from TurboNet!')) {
      throw new Error('data channel opened but expected native greeting was not observed');
    }
    if (!snapshot.messages.includes('Hello from browser!')) {
      throw new Error('data channel opened but expected browser echo was not observed');
    }
    if (nativeStderr.trim()) {
      console.log('\n=== Native stderr ===');
      console.log(nativeStderr.trim());
    }

    console.log('\nRESULT: browser interop smoke passed');
  } finally {
    if (native && !native.killed) {
      try {
        native.kill('SIGINT');
      } catch {
        /* Ignore shutdown errors. */
      }
      await sleep(1000);
      try {
        native.kill('SIGTERM');
      } catch {
        /* Ignore shutdown errors. */
      }
    }
    if (cdp) {
      cdp.close();
    }
    if (chrome && !chrome.killed) {
      try {
        chrome.kill('SIGTERM');
      } catch {
        /* Ignore shutdown errors. */
      }
    }
  }
}

main().catch((error) => {
  console.error(error.stack || String(error));
  process.exit(1);
});
