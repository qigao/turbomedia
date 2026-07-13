#!/usr/bin/env node

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const EXAMPLES_DIR = __dirname;
const REPO_ROOT = path.resolve(EXAMPLES_DIR, '..', '..');
const HTML_PATH = path.join(EXAMPLES_DIR, 'browser_media_interop.html');

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function usage() {
  console.log(`Usage: node webrtc/examples/browser_media_interop_smoke.js [options]

Options:
  --chrome <path>     Browser executable path
  --native <path>     browser_media_interop executable path
  --codec <name>      Browser codec preference: auto, vp8, vp9, h264
  --timeout <ms>      Overall wait budget per phase (default: 45000)
  --debug-port <n>    Chrome DevTools port (default: 9227)
  --with-stun         Run native example without --no-stun override
  --browser-send      Browser sends audio/video, native receives and decodes
  --matrix            Run vp8/vp9/h264 in both directions and print a summary
  --matrix-codecs     Comma-separated codec list for --matrix (default: vp8,vp9,h264)
  --help              Show this help
`);
}

function parseArgs(argv) {
  const options = {
    chrome: process.env.CHROME_PATH || '',
    native: process.env.BROWSER_MEDIA_INTEROP_BIN || '',
    codec: 'auto',
    timeoutMs: 45000,
    debugPort: 9227,
    useStun: false,
    browserSend: false,
    matrix: false,
    matrixCodecs: ['vp8', 'vp9', 'h264'],
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
    if (arg === '--browser-send') {
      options.browserSend = true;
      continue;
    }
    if (arg === '--matrix') {
      options.matrix = true;
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
    if (arg === '--codec' && i + 1 < argv.length) {
      options.codec = String(argv[++i]).toLowerCase();
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
    if (arg === '--matrix-codecs' && i + 1 < argv.length) {
      options.matrixCodecs = String(argv[++i])
        .split(',')
        .map((value) => value.trim().toLowerCase())
        .filter(Boolean);
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
          path.join(REPO_ROOT, 'build', 'Msvc-Release', 'bin', 'browser_media_interop.exe'),
          path.join(REPO_ROOT, 'build', 'Msvc', 'bin', 'browser_media_interop.exe'),
        ]
      : [
          path.join(REPO_ROOT, 'build', 'bin', 'browser_media_interop'),
          path.join(REPO_ROOT, 'build', 'linux-release', 'bin', 'browser_media_interop'),
        ];
    options.native = candidates.find((candidate) => fs.existsSync(candidate)) || '';
  }

  if (!options.chrome) {
    throw new Error('browser executable not found; pass --chrome <path> or set CHROME_PATH');
  }
  if (!options.native) {
    throw new Error('browser_media_interop executable not found; pass --native <path> or set BROWSER_MEDIA_INTEROP_BIN');
  }
  if (!['auto', 'vp8', 'vp9', 'h264'].includes(options.codec)) {
    throw new Error(`unsupported codec preference: ${options.codec}`);
  }
  if (options.matrixCodecs.length === 0) {
    throw new Error('matrix codec list is empty');
  }
  for (const codec of options.matrixCodecs) {
    if (!['vp8', 'vp9', 'h264'].includes(codec)) {
      throw new Error(`unsupported matrix codec: ${codec}`);
    }
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

function spawnPromise(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, options);
    let stdout = '';
    let stderr = '';

    child.stdout.on('data', (data) => {
      const text = data.toString();
      stdout += text;
      process.stdout.write(text);
    });
    child.stderr.on('data', (data) => {
      const text = data.toString();
      stderr += text;
      process.stderr.write(text);
    });
    child.on('error', reject);
    child.on('exit', (code, signal) => {
      if (code === 0) {
        resolve({ code, signal, stdout, stderr });
        return;
      }
      const error = new Error(`child exited with code=${code} signal=${signal || ''}`.trim());
      error.code = code;
      error.signal = signal;
      error.stdout = stdout;
      error.stderr = stderr;
      reject(error);
    });
  });
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

async function runMatrix(options) {
  const entries = [];
  const scriptPath = __filename;
  let debugPort = options.debugPort;

  for (const codec of options.matrixCodecs) {
    for (const browserSend of [false, true]) {
      const label = `${codec} ${browserSend ? 'browser-send' : 'native-send'}`;
      const args = [
        scriptPath,
        '--codec', codec,
        '--timeout', String(options.timeoutMs),
        '--debug-port', String(debugPort),
      ];

      if (options.chrome) {
        args.push('--chrome', options.chrome);
      }
      if (options.native) {
        args.push('--native', options.native);
      }
      if (options.useStun) {
        args.push('--with-stun');
      }
      if (browserSend) {
        args.push('--browser-send');
      }

      console.log(`\n=== Matrix Case: ${label} ===`);
      const startedAt = Date.now();
      try {
        await spawnPromise(process.execPath, args, { cwd: REPO_ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
        entries.push({
          codec,
          direction: browserSend ? 'browser-send' : 'native-send',
          ok: true,
          durationMs: Date.now() - startedAt,
        });
      } catch (error) {
        entries.push({
          codec,
          direction: browserSend ? 'browser-send' : 'native-send',
          ok: false,
          durationMs: Date.now() - startedAt,
          error: error.stderr || error.message,
        });
      }
      debugPort += 1;
    }
  }

  console.log('\n=== Browser Media Matrix Summary ===');
  for (const entry of entries) {
    const seconds = (entry.durationMs / 1000).toFixed(1);
    console.log(`${entry.ok ? 'PASS' : 'FAIL'} ${entry.codec} ${entry.direction} ${seconds}s`);
    if (!entry.ok && entry.error) {
      const summary = String(entry.error).trim().split(/\r?\n/).slice(-3).join(' | ');
      console.log(`  ${summary}`);
    }
  }

  const failed = entries.filter((entry) => !entry.ok);
  if (failed.length > 0) {
    throw new Error(`${failed.length} browser media matrix case(s) failed`);
  }
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.matrix) {
    await runMatrix(options);
    return;
  }
  const chromeUserData = fs.mkdtempSync(path.join(os.tmpdir(), 'turbonet-media-chrome-'));
  const codecArg = JSON.stringify(options.codec);

  let chrome = null;
  let native = null;
  let cdp = null;
  let nativeStdout = '';
  let nativeStderr = '';
  let offer = '';
  let answer = '';

  const printH264SdpLines = (label, sdp) => {
    if (!sdp) {
      return;
    }
    const lines = String(sdp)
      .split(/\r?\n/)
      .filter((line) => /^(a=rtpmap:|a=fmtp:|a=rtcp-fb:)/.test(line) &&
        /(?:H264|packetization-mode|profile-level-id)/i.test(line));
    if (lines.length > 0) {
      console.log(`\n=== ${label} H264 SDP ===`);
      console.log(lines.join('\n'));
    }
  };

  const collectDebugSnapshot = async () => {
    if (!cdp) {
      return null;
    }
    const result = await cdp.send('Runtime.evaluate', {
      expression: `Promise.all([
        collectSnapshot(),
        Promise.resolve(document.getElementById('debugLog')?.innerText || ''),
        Promise.resolve(document.getElementById('track-status')?.textContent || ''),
        Promise.resolve(document.getElementById('remoteVideo')?.readyState || 0)
      ]).then(([snapshot, logText, trackStatus, readyState]) => ({
        snapshot,
        logText,
        trackStatus,
        readyState
      }))`,
      awaitPromise: true,
      returnByValue: true,
    });
    return result?.result?.value || null;
  };

  try {
    chrome = spawn(options.chrome, [
      '--headless=new',
      `--remote-debugging-port=${options.debugPort}`,
      `--user-data-dir=${chromeUserData}`,
      '--no-first-run',
      '--no-default-browser-check',
      '--disable-background-networking',
      '--disable-component-update',
      '--disable-default-apps',
      '--disable-sync',
      '--metrics-recording-only',
      '--allow-file-access-from-files',
      '--autoplay-policy=no-user-gesture-required',
      `file:///${HTML_PATH.replace(/\\/g, '/')}`,
    ], { stdio: ['ignore', 'pipe', 'pipe'] });
    chrome.stderr.on('data', (data) => {
      const text = data.toString();
      if (text.includes('google_apis\\gcm\\engine\\registration_request.cc') ||
          text.includes('DevTools listening on') ||
          text.includes('Created TensorFlow Lite XNNPACK delegate for CPU.')) {
        return;
      }
      process.stderr.write(text);
    });

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

    const nativeArgs = [];
    if (!options.useStun) {
      nativeArgs.push('--no-stun');
    }
    if (options.browserSend) {
      nativeArgs.push('--receive');
    }
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
      expression: options.browserSend
        ? `createSendOffer(${codecArg})`
        : `createOffer(${codecArg})`,
      awaitPromise: true,
      returnByValue: true,
    });

    offer = await waitFor(async () => {
      const result = await cdp.send('Runtime.evaluate', {
        expression: `(() => document.getElementById('localSdp').value)()`,
        returnByValue: true,
      });
      const value = result?.result?.value;
      return value &&
        value.includes('m=audio') &&
        value.includes('m=video') &&
        value.includes('a=candidate:') ? value : null;
    }, options.timeoutMs, 'browser offer SDP with candidates');
    printH264SdpLines('Browser Offer', offer);

    native.stdin.write(offer.replace(/\r\n/g, '\n'));
    native.stdin.write('\n');

    answer = await waitFor(() => {
      const match = nativeStdout.match(/-----BEGIN SDP ANSWER-----\r?\n([\s\S]*?)-----END SDP ANSWER-----/);
      return match ? match[1] : null;
    }, options.timeoutMs, 'native SDP answer');
    printH264SdpLines('Native Answer', answer);

    await cdp.send('Runtime.evaluate', {
      expression: `(() => {
        document.getElementById('remoteSdp').value = ${JSON.stringify(answer)};
        return setRemoteAnswer();
      })()`,
      awaitPromise: true,
      returnByValue: true,
    });

    if (options.browserSend) {
      await waitFor(
        () => nativeStdout.includes('[Media] Audio receiver active') &&
          nativeStdout.includes('[Media] Video receiver active'),
        options.timeoutMs,
        'native media audio/video receiver activation'
      );
      const outboundSnapshot = await waitFor(async () => {
        const result = await cdp.send('Runtime.evaluate', {
          expression: 'collectSnapshot()',
          awaitPromise: true,
          returnByValue: true,
        });
        const value = result?.result?.value;
        if (value?.outboundAudio?.packetsSent > 0 &&
            value?.outboundVideo?.packetsSent > 0 &&
            value?.outboundVideo?.framesEncoded > 0) {
          return value;
        }
        return null;
      }, options.timeoutMs, 'browser encoded outbound audio/video');
      console.log('\n=== Browser Outbound A/V Snapshot ===');
      console.log(JSON.stringify(outboundSnapshot, null, 2));
      await waitFor(
        () => nativeStdout.includes('audio frames') &&
          nativeStdout.includes('video frames'),
        options.timeoutMs,
        'native decoded inbound audio/video'
      );
    } else {
      await waitFor(
        () => nativeStdout.includes('[Media] Audio sender active') &&
          nativeStdout.includes('[Media] Video sender active'),
        options.timeoutMs,
        'native media audio/video sender activation'
      );
    }

    let snapshot;
    try {
      snapshot = await waitFor(async () => {
        const result = await cdp.send('Runtime.evaluate', {
          expression: 'collectSnapshot()',
          awaitPromise: true,
          returnByValue: true,
        });
        const value = result?.result?.value;
        if (!value) {
          return null;
        }
        if (options.browserSend &&
            value.outboundAudio &&
            value.outboundAudio.packetsSent > 0 &&
            value.outboundVideo &&
            value.outboundVideo.packetsSent > 0 &&
            value.outboundVideo.framesEncoded > 0) {
          return value;
        }
        if (value.inboundAudio &&
            value.inboundAudio.packetsReceived > 0 &&
            value.inboundVideo &&
            value.inboundVideo.packetsReceived > 0 &&
            value.inboundVideo.framesDecoded > 0) {
          return value;
        }
        return null;
      }, options.timeoutMs, 'browser audio/video media flow');
    } catch (error) {
      const debugState = await collectDebugSnapshot();
      if (debugState) {
        console.log('\n=== Browser Debug State ===');
        console.log(JSON.stringify(debugState.snapshot, null, 2));
        console.log('\n=== Browser Track State ===');
        console.log(`track=${debugState.trackStatus} readyState=${debugState.readyState}`);
        if (debugState.logText) {
          console.log('\n=== Browser Debug Log ===');
          console.log(debugState.logText.trim());
        }
      }
      throw error;
    }

    console.log('\n=== Browser Snapshot ===');
    console.log(JSON.stringify(snapshot, null, 2));

    if (options.browserSend) {
      if (!snapshot.outboundAudio || snapshot.outboundAudio.packetsSent <= 0) {
        throw new Error('browser never sent outbound audio packets');
      }
      if (!snapshot.outboundVideo || snapshot.outboundVideo.framesEncoded <= 0) {
        throw new Error('browser never encoded outbound video frames');
      }
    } else {
      if (!snapshot.inboundAudio || snapshot.inboundAudio.packetsReceived <= 0) {
        throw new Error('browser never received inbound audio packets');
      }
      if (!snapshot.inboundVideo || snapshot.inboundVideo.framesDecoded <= 0) {
        throw new Error('browser never decoded inbound video frames');
      }
    }

    if (nativeStderr.trim()) {
      console.log('\n=== Native stderr ===');
      console.log(nativeStderr.trim());
    }

    console.log(`\nRESULT: browser media interop smoke passed (${options.browserSend ? 'browser-send/native-receive' : 'native-send/browser-receive'}, codec=${options.codec}, av=audio+video)`);
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
