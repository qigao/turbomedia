'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { webcrypto, createHash } = require('node:crypto');
const test = require('node:test');
const keys = ['configure', 'startPublisher', 'startViewer', 'restartIce', 'snapshot', 'close'];
const secret = 'SECRET-never-export';
const config = () => ({ sfu_origin: 'https://sfu.example.test',
  whip_url: 'https://sfu.example.test/whip/room/publisher',
  whep_url: 'https://sfu.example.test/whep/room/viewer', token: secret,
  turn: { urls: ['turns:turn.example.test:5349?transport=tcp'], username: 'SECRET-user', credential: 'SECRET-turn' },
  run_id: 'run-test', case_id: 'case-test', participant_id: 'participant-test',
  limits: { request_timeout_ms: 100, operation_timeout_ms: 300, close_timeout_ms: 100 } });
const sdp = (generation = 1, direction = 'sendonly', order = ['audio', 'video']) =>
  'v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n' +
  order.map((kind, i) => `m=${kind} 9 UDP/TLS/RTP/SAVPF ${kind === 'audio' ? 111 : 96}\r\nc=IN IP4 0.0.0.0\r\na=mid:${i}\r\na=${direction}\r\na=ice-ufrag:gen${generation}\r\na=ice-pwd:abcdefghijklmnopqrstuv${generation}\r\n`).join('');
const restartFragment = 'a=ice-ufrag:remote2\r\na=ice-pwd:remoteabcdefghijklmnopqr2\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\na=candidate:1 1 UDP 123 192.0.2.2 50000 typ relay\r\na=end-of-candidates\r\n';
const response = (status, body = null, headers = {}) => new Response(body, { status, headers });
const json = (value) => JSON.parse(JSON.stringify(value));
const livePages = new Set();
test.afterEach(async () => { for (const api of livePages) await api.close(); livePages.clear(); });

// Browser media APIs are external here. The page and fragment parser run unmodified;
// assertions inspect their requests, ownership and output, not simulated relay success.
function page(options = {}) {
  const web = path.join(__dirname, '../web');
  for (const file of ['client.html', 'client.js']) assert.ok(fs.existsSync(path.join(web, file)), `${file} must exist`);
  const requests = [], pcs = [], tracks = [], contexts = [], timers = new Set();
  const track = (kind) => {
    const value = { kind, id: `${kind}-track`, readyState: 'live', muted: false,
      stop() { this.readyState = 'ended'; } };
    tracks.push(value); return value;
  };
  class Stream {
    constructor(list = []) { this.list = list; }
    addTrack(value) { this.list.push(value); }
    getTracks() { return this.list; }
    getAudioTracks() { return this.list.filter((value) => value.kind === 'audio'); }
    getVideoTracks() { return this.list.filter((value) => value.kind === 'video'); }
  }
  class Peer extends EventTarget {
    constructor(configuration) { super(); this.configuration = configuration; this.transceivers = [];
      this.generation = 1; this.connectionState = 'new'; this.iceConnectionState = 'new';
      this.signalingState = 'stable'; this.iceGatheringState = 'new'; pcs.push(this); }
    addTransceiver(value, settings) { this.transceivers.push({ kind: typeof value === 'string' ? value : value.kind, ...settings }); }
    async createOffer(settings) { if (settings?.iceRestart) this.generation++;
      return { type: 'offer', sdp: sdp(this.generation, this.transceivers[0].direction, options.reverse ? ['video', 'audio'] : undefined) }; }
    async setLocalDescription(value) { this.localDescription = value; this.signalingState = 'have-local-offer';
      if (options.hangGather) return;
      this.iceGatheringState = 'complete';
      this.localDescription = { ...value, sdp: value.sdp.replace('a=sendonly\r\n', 'a=sendonly\r\na=candidate:1 1 UDP 123 192.0.2.1 50000 typ relay\r\na=end-of-candidates\r\n') };
      this.dispatchEvent(new Event('icegatheringstatechange')); }
    async setRemoteDescription(value) { this.remoteDescription = value; this.signalingState = 'stable';
      this.connectionState = 'connected'; this.iceConnectionState = 'connected';
      if (options.remoteTracks) for (const kind of ['audio', 'video']) this.ontrack?.({ track: track(kind), streams: [] }); }
    setConfiguration(value) { this.configuration = value; }
    async getStats() { if (options.statsHang) return new Promise(() => {});
      if (options.getStats) return options.getStats();
      return new Map((options.stats || []).map((value) => [value.id, value])); }
    close() { this.connectionState = 'closed'; this.iceConnectionState = 'closed'; this.signalingState = 'closed'; }
  }
  const canvas = { width: 0, height: 0, getContext: () => ({ fillRect() {}, fillText() {}, fillStyle: '' }),
    captureStream: () => new Stream([track('video')]) };
  const video = { srcObject: null, currentTime: 0, videoWidth: 0, videoHeight: 0,
    async play() { await options.play?.(); }, pause() {}, requestVideoFrameCallback(fn) { this.frameCallback = fn; return 1; },
    cancelVideoFrameCallback() { this.frameCallback = null; } };
  class AudioContext {
    constructor() { this.state = 'suspended'; contexts.push(this); }
    createOscillator() { return { frequency: { value: 0 }, connect() {}, disconnect() {}, start() {}, stop() {} }; }
    createMediaStreamDestination() { return { stream: new Stream([track('audio')]), disconnect() {} }; }
    async resume() { this.state = 'running'; }
    async close() { this.state = 'closed'; }
  }
  const sandbox = { window: {}, URL, TextEncoder, TextDecoder, crypto: options.crypto ?? webcrypto,
    AbortController: options.AbortController ?? AbortController, Event, EventTarget,
    performance, MediaStream: Stream, RTCPeerConnection: Peer, AudioContext,
    setTimeout, clearTimeout, setInterval(fn, ms) { const id = setInterval(fn, ms); timers.add(id); return id; },
    clearInterval(id) { clearInterval(id); timers.delete(id); },
    document: { getElementById: () => video, createElement: (tag) => tag === 'canvas' ? canvas : video },
    fetch: async (url, init) => {
      requests.push({ url, ...init });
      if (options.fetch) return options.fetch(url, init, requests.length);
      if (init.method === 'POST') return response(201, sdp(9, 'recvonly'), {
        'Content-Type': 'application/sdp', Location: `${new URL(url).pathname}/sessions/session1`, ETag: '"1"' });
      if (init.method === 'DELETE') return response(204);
      if (requests.filter((r) => r.method === 'PATCH').length === 1) return response(204, null, { ETag: '"1"' });
      if (init.headers['If-Match'] === '"1"' && requests.filter((r) => r.method === 'PATCH').length > 2) return response(412, 'old');
      return response(200, restartFragment, { 'Content-Type': 'application/trickle-ice-sdpfrag', ETag: '"2"' });
    },
  };
  for (const name of ['localStorage', 'sessionStorage']) Object.defineProperty(sandbox, name,
    { get() { throw new Error('storage access forbidden'); } });
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(web, '../src/sdpfrag.js'), 'utf8'), sandbox);
  vm.runInContext(fs.readFileSync(path.join(web, 'client.js'), 'utf8'), sandbox);
  livePages.add(sandbox.window.turboAcceptance);
  return { api: sandbox.window.turboAcceptance, requests, pcs, tracks, contexts, timers, video, sandbox };
}

test('page exposes exactly six frozen methods and never persists configuration secrets', async () => {
  const p = page();
  assert.deepEqual(Object.keys(p.api), keys); assert.ok(Object.isFrozen(p.api));
  assert.deepEqual(Object.keys(p.sandbox.window), ['turboAcceptance']);
  p.api.configure(config());
  const snapshot = await p.api.snapshot();
  assert.equal(snapshot.phase, 'configured');
  assert.doesNotMatch(JSON.stringify(snapshot), /SECRET|token|credential|username|ice-pwd|ice-ufrag/);
  assert.deepEqual(await p.api.close(), await p.api.close());
  assert.equal((await p.api.snapshot()).phase, 'closed');
  const source = fs.readFileSync(path.join(__dirname, '../web/client.js'), 'utf8');
  assert.doesNotMatch(source, /localStorage|sessionStorage|URLSearchParams|console\.|innerHTML/);
});

test('viewer delivers trickle before awaiting playback that depends on remote ICE candidates', async () => {
  const calls = [];
  const delivered = Promise.withResolvers();
  const p = page({
    play: async () => { calls.push('play'); await delivered.promise; },
    fetch: (url, init) => {
      calls.push(init.method);
      if (init.method === 'POST') return response(201, sdp(9, 'sendonly'), {
        'Content-Type': 'application/sdp', Location: `${new URL(url).pathname}/sessions/x`, ETag: '"1"' });
      if (init.method === 'PATCH') { delivered.resolve(); return response(204, null, { ETag: '"1"' }); }
      return response(204);
    },
  });
  p.api.configure(config());
  const started = await p.api.startViewer();
  assert.deepEqual(calls, ['POST', 'PATCH', 'play']);
  assert.equal(started.phase, 'active'); await p.api.close();
});

test('configuration rejects endpoint/query/unknown/policy errors before allocating resources', async () => {
  for (const patch of [ { whip_url: config().whip_url + '?token=SECRET' }, { sfu_origin: 'http://sfu.example.test' },
    { whep_url: 'https://evil.example.test/whep/x/y' }, { unexpected: true },
    { iceTransportPolicy: 'all' }, { turn: { ...config().turn, urls: ['stun:example.test'] } } ]) {
    const p = page(); assert.throws(() => p.api.configure({ ...config(), ...patch }), (e) => {
      assert.equal(e.code, 'INVALID_CONFIGURATION'); assert.doesNotMatch(String(e), /SECRET|evil/); return true;
    });
    assert.equal(p.pcs.length, 0); await p.api.close();
  }
});

test('publisher creates audio before video, verifies actual offer, trickles and releases every resource', async () => {
  const p = page(); p.api.configure(config());
  const started = await p.api.startPublisher();
  assert.equal(started.phase, 'active');
  assert.equal(started.api.location, 'https://sfu.example.test/whip/room/publisher/sessions/session1');
  assert.equal(started.api.session_id, 'session1'); assert.equal(started.api.etag, '"1"');
  assert.deepEqual(json(p.pcs[0].transceivers.map((t) => [t.kind, t.direction])), [['audio', 'sendonly'], ['video', 'sendonly']]);
  assert.equal(p.pcs[0].configuration.iceTransportPolicy, 'relay');
  assert.equal(p.requests[0].method, 'POST');
  assert.ok(p.requests[0].body.indexOf('m=audio') < p.requests[0].body.indexOf('m=video'));
  assert.equal(p.requests[1].method, 'PATCH'); assert.equal(p.requests[1].headers['If-Match'], '"1"');
  assert.match(p.requests[1].body, /a=candidate:/);
  await p.api.close(); await p.api.close();
  assert.equal(p.requests.filter((r) => r.method === 'DELETE').length, 1);
  assert.ok(p.tracks.every((t) => t.readyState === 'ended'));
  assert.ok(p.contexts.every((c) => c.state === 'closed')); assert.equal(p.timers.size, 0);
  assert.equal(p.pcs[0].connectionState, 'closed');
  assert.doesNotMatch(JSON.stringify(await p.api.snapshot()), /SECRET/);
});

test('publisher rejects reversed generated m-lines before WHIP POST', async () => {
  const p = page({ reverse: true }); p.api.configure(config());
  await assert.rejects(p.api.startPublisher(), { code: 'INVALID_MEDIA_ORDER' });
  assert.equal(p.requests.length, 0); await p.api.close();
});

test('viewer evidence distinguishes track events and raw RTP/frame stats from HTTP success', async () => {
  const p = page({ remoteTracks: true, stats: [
    { id: 'in-video', type: 'inbound-rtp', timestamp: 100, kind: 'video', packetsReceived: 7, framesDecoded: 3 },
    { id: 'local', type: 'local-candidate', timestamp: 100, candidateType: 'relay', usernameFragment: secret,
      url: `turns:SECRET-user:SECRET-turn@turn.example.test`, address: '192.0.2.1', protocol: 'udp' },
  ] });
  p.api.configure(config()); await p.api.startViewer();
  assert.deepEqual(json(p.pcs[0].transceivers.map((t) => [t.kind, t.direction])), [['audio', 'recvonly'], ['video', 'recvonly']]);
  p.video.frameCallback(0, { presentedFrames: 3, mediaTime: 0.1 });
  const snapshot = await p.api.snapshot();
  assert.equal(snapshot.media.remote_tracks.length, 2); assert.equal(snapshot.media.presented_frames, 3);
  assert.equal(snapshot.stats.find((s) => s.id === 'in-video').framesDecoded, 3);
  assert.equal(snapshot.stats.find((s) => s.id === 'local').candidateType, 'relay');
  assert.doesNotMatch(JSON.stringify(snapshot), /SECRET|usernameFragment|turns:/);
  await p.api.close(); assert.equal(p.video.srcObject, null); assert.equal(p.video.frameCallback, null);
  const empty = page(); empty.api.configure(config()); await empty.api.startViewer();
  assert.equal((await empty.api.snapshot()).media.remote_tracks.length, 0); await empty.api.close();
});

test('restart validates a new generation, uses current ETag, applies remote credentials and proves stale 412', async () => {
  const p = page(); p.api.configure(config()); await p.api.startPublisher();
  await assert.rejects(p.api.restartIce({ generation: 1 }), { code: 'INVALID_GENERATION' });
  const restarted = await p.api.restartIce({ generation: 2, turn: config().turn });
  assert.equal(restarted.generation, 2); assert.equal(restarted.api.restart_status, 200);
  assert.equal(restarted.api.stale_etag_status, 412);
  assert.equal(restarted.api.etag, '"2"');
  const patches = p.requests.filter((r) => r.method === 'PATCH');
  assert.equal(patches[1].headers['If-Match'], '"1"'); assert.match(patches[1].body, /ice-ufrag:gen2/);
  assert.equal(patches[2].headers['If-Match'], '"1"');
  assert.equal(p.pcs[0].remoteDescription.sdp.match(/a=ice-ufrag:remote2/g).length, 2);
  assert.doesNotMatch(p.pcs[0].remoteDescription.sdp, /ice-ufrag:gen9/);
  assert.doesNotMatch(JSON.stringify(restarted), /SECRET|remoteabcdefghijkl|gen2/);
  await p.api.close();
});

test('unexpected HTTP metadata, redirect, body size and network errors fail with redacted structured errors', async () => {
  const cases = [
    () => response(200, 'SECRET'),
    () => response(201, sdp(), { 'Content-Type': 'text/plain', Location: '/whip/room/publisher/sessions/x', ETag: '"1"' }),
    () => response(201, sdp(), { 'Content-Type': 'application/sdp', Location: 'https://evil.test/SECRET', ETag: '"1"' }),
    () => response(201, sdp(), { 'Content-Type': 'application/sdp', Location: '/whip/room/publisher/sessions/x', ETag: 'W/"1"' }),
    () => response(201, 'x'.repeat(65537), { 'Content-Type': 'application/sdp', Location: '/whip/room/publisher/sessions/x', ETag: '"1"' }),
    () => { throw new Error(secret); },
  ];
  for (const make of cases) {
    const p = page({ fetch: (_, init) => init.method === 'DELETE' ? response(204) : make() });
    p.api.configure(config());
    await assert.rejects(p.api.startPublisher(), (e) => { assert.equal(e.name, 'BrowserAcceptanceError');
      assert.ok(e.code); assert.ok(e.operation); assert.doesNotMatch(String(e) + JSON.stringify(e), /SECRET|evil/); return true; });
    await p.api.close(); assert.ok(p.tracks.every((t) => t.readyState === 'ended'));
  }
});

test('close preempts hanging gather, is idempotent and leaves no owned timers or tracks', async () => {
  const p = page({ hangGather: true }); p.api.configure(config());
  const started = p.api.startPublisher();
  const rejection = assert.rejects(started, { code: 'OPERATION_CANCELLED' });
  await new Promise((resolve) => setTimeout(resolve, 10));
  await p.api.close(); await rejection;
  assert.equal((await p.api.snapshot()).phase, 'closed'); assert.equal(p.timers.size, 0);
  assert.ok(p.tracks.every((t) => t.readyState === 'ended'));
});

test('HTTP and stats deadlines settle even when external promises do not cooperate', async () => {
  const p = page({ fetch: () => new Promise(() => {}) }); p.api.configure(config());
  await assert.rejects(p.api.startViewer(), { code: 'OPERATION_TIMEOUT' }); await p.api.close();
  const options = {}; const stats = page(options); stats.api.configure(config()); await stats.api.startViewer();
  options.statsHang = true;
  await assert.rejects(stats.api.snapshot(), { code: 'OPERATION_TIMEOUT' }); await stats.api.close();
});

test('POST retains a valid Location for cleanup even when later response metadata is invalid', async () => {
  const p = page({ fetch: (_, init) => init.method === 'DELETE' ? response(204) :
    response(201, 'SECRET', { 'Content-Type': 'text/plain', Location: '/whip/room/publisher/sessions/x', ETag: '"1"' }) });
  p.api.configure(config()); await assert.rejects(p.api.startPublisher(), { code: 'HTTP_INVALID_CONTENT_TYPE' });
  await p.api.close();
  assert.equal(p.requests.at(-1).method, 'DELETE');
});

test('snapshot redacts reused secret values in approved stats fields and enforces record caps', async () => {
  const options = { stats: [{ id: secret, type: 'inbound-rtp', timestamp: 1, kind: 'video', packetsReceived: 1 }] };
  const p = page(options); p.api.configure({ ...config(), limits: { ...config().limits, stats_records: 1 } });
  await p.api.startViewer(); assert.doesNotMatch(JSON.stringify(await p.api.snapshot()), /SECRET/);
  options.stats.push({ id: 'another', type: 'transport' });
  await assert.rejects(p.api.snapshot(), { code: 'STATS_LIMIT' }); await p.api.close();
});

test('same remote track event is idempotent; an extra distinct track fails without retaining it', async () => {
  const p = page({ remoteTracks: true }); p.api.configure(config()); await p.api.startViewer();
  const existing = p.tracks.find((track) => track.kind === 'video');
  p.pcs[0].ontrack({ track: existing, streams: [] });
  assert.equal((await p.api.snapshot()).media.remote_tracks.length, 2);
  const extra = { kind: 'video', readyState: 'live', stop() { this.readyState = 'ended'; } };
  p.pcs[0].ontrack({ track: extra, streams: [] });
  await assert.rejects(p.api.snapshot(), { code: 'UNEXPECTED_REMOTE_TRACK' });
  assert.equal(extra.readyState, 'ended'); await p.api.close();
});

test('restart rejects unchanged ETag, invalid response fragment and a stale request accepted by server', async () => {
  for (const variant of ['etag', 'fragment', 'stale']) {
    let patches = 0;
    const p = page({ fetch: (url, init) => {
      if (init.method === 'POST') return response(201, sdp(9, 'recvonly'), {
        'Content-Type': 'application/sdp', Location: `${new URL(url).pathname}/sessions/x`, ETag: '"1"' });
      if (init.method === 'DELETE') return response(204);
      patches++;
      if (patches === 1) return response(204, null, { ETag: '"1"' });
      if (patches === 3) return response(204, null, { ETag: '"2"' });
      return response(200, variant === 'fragment' ? 'a=unknown:SECRET\r\n' : restartFragment,
        { 'Content-Type': 'application/trickle-ice-sdpfrag', ETag: variant === 'etag' ? '"1"' : '"2"' });
    } });
    p.api.configure(config()); await p.api.startPublisher();
    await assert.rejects(p.api.restartIce({ generation: 2 }),
      { code: { etag: 'HTTP_UNCHANGED_ETAG', fragment: 'INVALID_RESTART_FRAGMENT', stale: 'HTTP_UNEXPECTED_STATUS' }[variant] });
    assert.equal((await p.api.snapshot()).phase, 'failed'); await p.api.close();
  }
});

test('every restart uses the latest committed ETag and rejects concurrent mutations', async () => {
  let current = 1, patchCount = 0;
  const p = page({ fetch: (url, init) => {
    if (init.method === 'POST') return response(201, sdp(9, 'recvonly'), {
      'Content-Type': 'application/sdp', Location: `${new URL(url).pathname}/sessions/x`, ETag: '"1"' });
    if (init.method === 'DELETE') return response(204);
    if (++patchCount === 1) return response(204, null, { ETag: '"1"' });
    if (init.headers['If-Match'] !== `"${current}"`) return response(412, 'stale');
    current++;
    return response(200, restartFragment.replaceAll('remote2', `remote${current}`).replace('mnopqr2', `mnopqr${current}`),
      { 'Content-Type': 'application/trickle-ice-sdpfrag', ETag: `"${current}"` });
  } });
  p.api.configure(config());
  const first = p.api.startViewer(); await assert.rejects(p.api.startPublisher(), { code: 'INVALID_PHASE' }); await first;
  await p.api.restartIce({ generation: 2 }); await p.api.restartIce({ generation: 3 });
  const patches = p.requests.filter((r) => r.method === 'PATCH');
  assert.equal(patches[3].headers['If-Match'], '"2"');
  assert.equal(patches[4].headers['If-Match'], '"2"'); await p.api.close();
});

test('DELETE deadline preserves cleanup failure while tracks, PC, audio and timers are released', async () => {
  const options = {}; const p = page(options); p.api.configure(config()); await p.api.startPublisher();
  options.fetch = () => new Promise(() => {});
  const result = await p.api.close();
  assert.ok(result.cleanup_errors.some((error) => error.code === 'OPERATION_TIMEOUT'));
  assert.deepEqual(json(await p.api.close()), json(result)); assert.equal(p.timers.size, 0);
  assert.ok(p.tracks.every((track) => track.readyState === 'ended')); assert.equal(p.pcs[0].connectionState, 'closed');
});

test('generation budget bounds retained credential history before another restart is admitted', async () => {
  const p = page(); p.api.configure({ ...config(), limits: { ...config().limits, max_generations: 1 } });
  await p.api.startViewer();
  await assert.rejects(p.api.restartIce({ generation: 2 }), { code: 'GENERATION_LIMIT' });
  assert.equal(p.requests.filter((r) => r.method === 'PATCH').length, 1); await p.api.close();
});

test('restart rejects an unbundled answer before it can rewrite independent ICE transports', async () => {
  const p = page({ fetch: (url, init) => init.method === 'DELETE' ? response(204) :
    response(201, sdp(9, 'recvonly').replace('a=group:BUNDLE 0 1\r\n', ''), {
      'Content-Type': 'application/sdp', Location: `${new URL(url).pathname}/sessions/x`, ETag: '"1"' }) });
  p.api.configure(config()); await assert.rejects(p.api.startViewer(), { code: 'UNSUPPORTED_ICE_TRANSPORTS' });
  await p.api.close();
});

test('close stays terminal across microtasks between PATCH completion and mutation receipt', async () => {
  const MAX_MICROTASK_GAP = 12;
  for (let gap = 0; gap <= MAX_MICROTASK_GAP; gap++) {
    let closePromise, settled = false, closeBeforeSettlement = false;
    const calls = [];
    const closeScheduled = Promise.withResolvers();
    const p = page({
      getStats: () => { calls.push('stats'); return new Map(); },
      fetch: (url, init) => {
        calls.push(init.method);
        if (init.method === 'POST') return response(201, sdp(9, 'recvonly'), {
          'Content-Type': 'application/sdp', Location: `${new URL(url).pathname}/sessions/x`, ETag: '"1"' });
        if (init.method === 'PATCH') {
          init.signal.addEventListener('abort', () => {
            let scheduled = Promise.resolve();
            for (let i = 0; i < gap; i++) scheduled = scheduled.then(() => {});
            scheduled.then(() => {
              closeBeforeSettlement = !settled; calls.push('close'); closePromise = p.api.close(); closeScheduled.resolve();
            });
          }, { once: true });
          return response(204, null, { ETag: '"1"' });
        }
        return response(204);
      },
    });
    p.api.configure(config());
    const started = p.api.startPublisher().then((value) => { settled = true; return { value }; },
      (error) => { settled = true; return { error }; });
    await closeScheduled.promise;
    const outcome = await started; await closePromise;
    assert.equal((await p.api.snapshot()).phase, 'closed', `gap ${gap}: ${calls}`);
    // Once receipt sampling already completed, its promise may be resolved before
    // the caller's .then observes it. Close cannot retroactively reject that receipt.
    const sampledBeforeClose = calls.indexOf('stats') >= 0 && calls.indexOf('stats') < calls.indexOf('close');
    if (closeBeforeSettlement && !sampledBeforeClose) {
      assert.equal(outcome.error?.code, 'OPERATION_CANCELLED', `gap ${gap}: ${calls}`);
    }
    await assert.rejects(p.api.restartIce({ generation: 2 }), { code: 'INVALID_PHASE' });
    assert.equal(p.requests.filter((request) => request.method === 'DELETE').length, 1);
  }
});

test('operation-scope completion cannot restore active when close runs at its abort boundary', async () => {
  let firstSignal, closing;
  class ObservedAbortController extends AbortController {
    constructor() {
      super();
      if (!firstSignal) {
        firstSignal = this.signal;
        firstSignal.addEventListener('abort', () => { closing = p.api.close(); }, { once: true });
      }
    }
  }
  const p = page({ AbortController: ObservedAbortController }); p.api.configure(config());
  await assert.rejects(p.api.startPublisher(), { code: 'OPERATION_CANCELLED' });
  await closing;
  assert.equal((await p.api.snapshot()).phase, 'closed');
  await assert.rejects(p.api.restartIce({ generation: 2 }), { code: 'INVALID_PHASE' });
});

test('pending public sampling rejects restart before generation or HTTP side effects', async () => {
  const options = {}, p = page(options); p.api.configure(config()); await p.api.startPublisher();
  const sampled = Promise.withResolvers(); options.getStats = () => sampled.promise;
  const pending = p.api.snapshot();
  const before = p.requests.length;
  try {
    await assert.rejects(p.api.restartIce({ generation: 2 }), { code: 'SNAPSHOT_BUSY' });
    assert.equal(p.requests.length, before, 'no restart or stale-ETag PATCH after sampling conflict');
  } finally { sampled.resolve(new Map()); }
  const unchanged = await pending;
  assert.equal(unchanged.generation, 1); assert.equal(unchanged.phase, 'active');
  delete options.getStats;
  const restarted = await p.api.restartIce({ generation: 2 });
  assert.equal(restarted.phase, 'active'); assert.equal(restarted.api.stale_etag_status, 412);
  await p.api.close();
});

test('public sampling cannot take the receipt sampler while restart is in flight', async () => {
  const options = {}, p = page(options); p.api.configure(config()); await p.api.startPublisher();
  const entered = Promise.withResolvers(), reply = Promise.withResolvers();
  options.fetch = (_url, init) => {
    if (init.method === 'DELETE') return response(204);
    if (p.requests.filter((request) => request.method === 'PATCH').length === 3) return response(412, 'stale');
    entered.resolve(); return reply.promise;
  };
  const pending = p.api.restartIce({ generation: 2 }); await entered.promise;
  let result;
  try { await assert.rejects(p.api.snapshot(), { code: 'OPERATION_BUSY' }); }
  finally {
    reply.resolve(response(200, restartFragment, { 'Content-Type': 'application/trickle-ice-sdpfrag', ETag: '"2"' }));
    result = await pending;
  }
  assert.equal(result.phase, 'active'); await p.api.close();
});

test('ICE SHA-256 evidence binds applied credentials to role side and generation without exporting them', async () => {
  const p = page(); p.api.configure(config());
  assert.equal((await p.api.snapshot()).ice, null);
  const first = await p.api.startPublisher();
  const expected = createHash('sha256').update(
    '["turbo-acceptance-ice-v1",1,"publisher","local","gen1","abcdefghijklmnopqrstuv1"]').digest('hex');
  assert.equal(first.ice.generation, 1);
  assert.equal(first.ice.local_sha256, expected);
  assert.match(first.ice.remote_sha256, /^[a-f0-9]{64}$/);
  assert.deepEqual(json((await p.api.snapshot()).ice), json(first.ice));
  const second = await p.api.restartIce({ generation: 2 });
  assert.equal(second.ice.generation, 2);
  assert.notEqual(second.ice.local_sha256, first.ice.local_sha256);
  assert.notEqual(second.ice.remote_sha256, first.ice.remote_sha256);
  assert.doesNotMatch(JSON.stringify([first, second]), /gen1|gen2|gen9|remote2|abcdefghijkl|SECRET|ice-ufrag|ice-pwd/);
  await p.api.close(); assert.equal((await p.api.snapshot()).ice, null);
});

test('missing or hung Web Crypto fails explicitly without fingerprint fallback', async () => {
  const missing = page({ crypto: {} }); missing.api.configure(config());
  await assert.rejects(missing.api.startPublisher(), { code: 'CRYPTO_UNAVAILABLE' });
  assert.equal(missing.requests.length, 0); assert.equal(missing.pcs.length, 0); await missing.api.close();
  const stalled = page({ crypto: { subtle: { digest: () => new Promise(() => {}) } } });
  stalled.api.configure(config());
  await assert.rejects(stalled.api.startPublisher(), { code: 'OPERATION_TIMEOUT' });
  await stalled.api.close();
});
