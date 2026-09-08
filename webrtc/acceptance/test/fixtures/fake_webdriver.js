'use strict';

const vm = require('node:vm');
const http = require('node:http');
const { webcrypto, createHash } = require('node:crypto');
const { Capabilities } = require('selenium-webdriver');
const PAGE = '<!doctype html><title>acceptance fixture</title>';
const PAGE_HASH = createHash('sha256').update(PAGE).digest('hex');
const METHODS = ['configure', 'startPublisher', 'startViewer', 'restartIce', 'snapshot', 'close'];

function snapshot(role = 'publisher') {
  return { schema_version: 1, phase: 'active', role, generation: 1, timestamp_ms: 100,
    ice: { generation: 1, local_sha256: 'a'.repeat(64), remote_sha256: 'b'.repeat(64) },
    connection_state: 'connected', ice_connection_state: 'connected', signaling_state: 'stable',
    stats: [
      { id: 'transport', type: 'transport', selectedCandidatePairId: 'pair' },
      { id: 'pair', type: 'candidate-pair', localCandidateId: 'local', remoteCandidateId: 'remote', state: 'succeeded' },
      { id: 'local', type: 'local-candidate', candidateType: 'relay', protocol: 'udp', relayProtocol: 'tcp', address: '192.0.2.1' },
      { id: 'remote', type: 'remote-candidate', candidateType: 'host', protocol: 'udp', address: '192.0.2.2' },
      { id: 'rtp', type: 'inbound-rtp', kind: 'video', packetsReceived: 0, framesDecoded: 0 } ],
    api: { post_status: 201, trickle_status: 204, restart_status: null, stale_etag_status: null,
      delete_status: null, local_media_order: ['audio', 'video'], remote_answer_applied: true,
      location: 'https://sfu.example.test/whip/room/publisher/sessions/session', etag: '"etag"', session_id: 'session' },
    media: { remote_tracks: [], presented_frames: null }, cleanup_errors: [] };
}

// Only browser/Grid side effects are substituted. Actual adapter scripts execute
// in a VM, including API descriptor checks, hashing, async callback and cloning.
function fakeWebDriver(options = {}) {
  const drivers = [], events = [], builders = [];
  function builderFactory() {
    const index = builders.length, settings = options.sessions?.[index] ?? options;
    const builder = {
      disableEnvironmentOverrides() { this.ignoreEnvironment = true; return this; },
      usingServer(url) { this.url = url; return this; },
      usingHttpAgent(agent) { this.agent = agent; return this; },
      withCapabilities(value) { this.capabilities = value; return this; },
      build() {
        events.push([index, 'build']);
        if (settings.build) return settings.build();
        const pageWindow = {};
        const state = { phase: 'idle', role: index === 1 ? 'viewer' : 'publisher' };
        const api = Object.fromEntries(METHODS.map((method) => [method, async (argument) => {
          events.push([index, method]);
          if (settings.methods?.[method]) return settings.methods[method](argument);
          if (method === 'configure') { state.phase = 'configured'; return { phase: state.phase }; }
          if (method === 'close') return { phase: 'closed', cleanup_errors: [] };
          return settings.snapshot ? settings.snapshot() : snapshot(state.role);
        }]));
        pageWindow.turboAcceptance = Object.freeze(api);
        settings.page?.(pageWindow);
        const sandbox = vm.createContext({ window: pageWindow, location: { href: '' },
          crypto: webcrypto, TextEncoder, Uint8Array, URL, AbortController, setTimeout, clearTimeout,
          fetch: async () => new Response(settings.pageBody ?? PAGE, { headers: { 'Content-Type': 'text/html' } }) });
        const driver = {
          sandbox,
          async getCapabilities() {
            if (settings.getCapabilities) return settings.getCapabilities();
            return new Capabilities(settings.capabilities ?? { browserName: 'chrome', browserVersion: '127.0.0', platformName: 'Windows 11' });
          },
          async getSession() { return { getId: () => settings.sessionId ?? `session-${index}` }; },
          getExecutor() { return { execute: async () => { events.push([index, 'status']); return settings.status ?? { ready: true }; } }; },
          manage() { return { setTimeouts: async (value) => { driver.timeouts = value; await settings.timeouts?.(); } }; },
          async get(url) { events.push([index, 'get', url]); sandbox.location.href = settings.redirect ?? url; await settings.get?.(); },
          async executeAsyncScript(script, ...args) {
            if (settings.execute) return settings.execute(script, ...args);
            sandbox.scriptArguments = [...args];
            return new Promise((resolve, reject) => {
              sandbox.scriptArguments.push(resolve);
              try { vm.runInContext(`(function(){${typeof script === 'function' ? `return (${script}).apply(null, arguments)` : script}}).apply(null, scriptArguments)`, sandbox); }
              catch (error) { reject(error); }
            });
          },
          async quit() { events.push([index, 'quit']); await settings.quit?.(); },
        };
        drivers.push(driver);
        return driver;
      },
    };
    builders.push(builder);
    return builder;
  }
  return { builderFactory, drivers, events, builders };
}

// Protocol fixture is test-only. Production always uses Selenium's own Builder,
// command mapping and JSON serialization against this HTTP boundary.
async function startFakeGrid(options = {}) {
  const requests = [], sockets = new Set();
  const fake = fakeWebDriver();
  const drivers = new Map();
  const server = http.createServer(async (req, res) => {
    const chunks = [];
    for await (const chunk of req) chunks.push(chunk);
    const body = chunks.length ? JSON.parse(Buffer.concat(chunks).toString()) : null;
    requests.push({ method: req.method, url: req.url, body });
    if (options.redirect) { res.writeHead(302, { Location: options.redirect }); res.end(); return; }
    if (options.hang === req.url) return;
    let value = null;
    if (req.method === 'POST' && req.url === '/session') {
      const driver = fake.builderFactory().build(), id = `grid-${drivers.size}`;
      drivers.set(id, driver);
      value = { sessionId: id, capabilities: { browserName: 'chrome', browserVersion: '127.0.0', platformName: 'Windows 11' } };
    } else if (req.url === '/status') value = { ready: true };
    else {
      const match = /^\/session\/([^/]+)(.*)$/.exec(req.url);
      const driver = match && drivers.get(match[1]);
      if (!driver) { res.writeHead(404); res.end(JSON.stringify({ value: { error: 'invalid session id', message: 'unknown' } })); return; }
      if (match[2] === '/url') await driver.get(body.url);
      else if (match[2] === '/execute/async') value = await driver.executeAsyncScript(body.script, ...body.args);
      else if (req.method === 'DELETE') { await driver.quit(); drivers.delete(match[1]); }
    }
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify({ value }));
  });
  server.on('connection', (socket) => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return { gridUrl: `http://127.0.0.1:${server.address().port}`, requests, drivers,
    close: async () => { for (const socket of sockets) socket.destroy(); await new Promise((resolve) => server.close(resolve)); } };
}

module.exports = { fakeWebDriver, startFakeGrid, snapshot, PAGE, PAGE_HASH };
