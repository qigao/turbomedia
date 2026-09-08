'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { fakeWebDriver, startFakeGrid, snapshot, PAGE_HASH } = require('./fixtures/fake_webdriver');
const browser = { name: 'chrome', version: '127.0.0', platform: 'Windows 11' };
const source = { test_page_url: 'https://acceptance.example.test/client.html', test_page_sha256: PAGE_HASH };
const relay_contract = { ip_family: 'ipv4', protocol: 'udp', relay_protocol: 'tcp', remote_candidate_types: ['host'] };
const context = { browser, relay_contract };
const secret = 'SECRET-token';
const configuration = () => ({ sfu_origin: 'https://sfu.example.test', whip_url: 'https://sfu.example.test/whip/room/publisher',
  whep_url: 'https://sfu.example.test/whep/room/viewer', token: secret,
  turn: { urls: ['turns:turn.example.test:5349?transport=tcp'], username: 'SECRET-user', credential: 'SECRET-turn' },
  run_id: 'run-test', case_id: 'case-test', participant_id: 'publisher' });
const hanging = () => new Promise(() => {});

function factory() {
  let exports;
  try { exports = require('../src/selenium_adapter'); }
  catch (error) { assert.fail(`Selenium adapter must implement the contract: ${error.code}`); }
  return exports.createSeleniumAdapter;
}
function setup(t, fakeOptions = {}, adapterOptions = {}) {
  const fake = fakeWebDriver(fakeOptions);
  const adapter = factory()({ source, gridUrl: 'https://grid.example.test/wd/hub',
    commandTimeoutMs: 100, ...fake, ...adapterOptions });
  t.after(() => adapter.closeAll());
  return { adapter, fake };
}

test('preflight validates exact capabilities and retains only two separate role sessions', async (t) => {
  const { adapter, fake } = setup(t);
  assert.deepEqual(Object.keys(adapter), ['preflightBrowser', 'openPublisher', 'openViewer', 'execute', 'collectCapabilities', 'closeRole', 'closeAll']);
  const preflight = await adapter.preflightBrowser(context);
  assert.deepEqual(preflight.capabilities, browser);
  assert.equal(preflight.page.sha256, PAGE_HASH);
  assert.equal(preflight.cors.verified, false);
  assert.equal(preflight.cors.requirement, 'same-origin-or-expose-location-etag');
  await adapter.openPublisher(context);
  await adapter.openViewer(context);
  assert.equal(fake.drivers.length, 2);
  assert.ok(fake.builders.every((b) => b.ignoreEnvironment && b.url === 'https://grid.example.test/wd/hub'));
  assert.deepEqual(await adapter.collectCapabilities('viewer'), browser);
  assert.deepEqual(fake.drivers[0].timeouts, { implicit: 0, pageLoad: 100, script: 100 });
  await assert.rejects(adapter.openPublisher(context), { code: 'BROWSER_ROLE_ALREADY_OPEN' });
  await adapter.closeAll();
  assert.deepEqual(fake.events.filter((e) => e[1] === 'quit'), [[1, 'quit'], [0, 'quit']]);
});

for (const [field, value, code] of [ ['browserVersion', '127.0.1', 'BROWSER_CAPABILITY_MISMATCH'],
  ['platformName', 'Linux', 'BROWSER_CAPABILITY_MISMATCH'], ['browserName', 'firefox', 'BROWSER_CAPABILITY_MISMATCH'],
  ['browserVersion', undefined, 'BROWSER_CAPABILITY_MISSING'], ['platformName', 42, 'BROWSER_CAPABILITY_INVALID'] ]) {
  test(`preflight rejects ${field} ${value}`, async (t) => {
    const capabilities = { browserName: 'chrome', browserVersion: '127.0.0', platformName: 'Windows 11', [field]: value };
    const { adapter, fake } = setup(t, { capabilities });
    await assert.rejects(adapter.preflightBrowser(context), { code });
    await adapter.closeAll();
    assert.equal(fake.events.filter((e) => e[1] === 'quit').length, 1);
  });
}

test('unsafe Grid URLs fail without echo or session allocation; absent Grid is missing evidence', async () => {
  for (const gridUrl of ['http://grid.example.test', 'https://user:SECRET@grid.test', 'https://grid.test/?SECRET',
    'https://grid.test/#SECRET', 'file:///grid', 'http://localhost:4444']) {
    assert.throws(() => factory()({ source, gridUrl }), { code: 'BROWSER_UNSAFE_GRID_URL' });
  }
  const adapter = factory()({ source, profile: 'diagnostic' });
  await assert.rejects(adapter.preflightBrowser(context), { code: 'BROWSER_GRID_UNCONFIGURED', category: 'missing_evidence' });
  assert.throws(() => factory()({ source, gridUrl: 'http://localhost:4444', profile: 'release', allowLoopbackHttp: true }), { code: 'BROWSER_UNSAFE_GRID_URL' });
  assert.throws(() => factory()({ source, commandTimeoutMs: 45_001 }), { code: 'BROWSER_INVALID_OPTIONS' });
});

for (const [name, settings, code] of [
  ['missing API', { page: (w) => { delete w.turboAcceptance; } }, 'BROWSER_PAGE_API_INVALID'],
  ['extra API', { page: (w) => { w.turboAcceptance = Object.freeze({ ...w.turboAcceptance, other() {} }); } }, 'BROWSER_PAGE_API_INVALID'],
  ['hash mismatch', { pageBody: 'wrong page' }, 'BROWSER_PAGE_HASH_MISMATCH'],
  ['navigation redirect', { redirect: 'https://other.example.test/client.html' }, 'BROWSER_PAGE_URL_MISMATCH'],
]) test(`preflight rejects ${name}`, async (t) => {
  const { adapter } = setup(t, settings);
  await assert.rejects(adapter.preflightBrowser(context), { code });
});

test('page operations preserve ICE hashes and observed zero versus missing media, redact addresses', async (t) => {
  const { adapter } = setup(t);
  await adapter.openPublisher(context);
  assert.deepEqual(await adapter.execute('publisher', 'configure', configuration()), { phase: 'configured' });
  const evidence = await adapter.execute('publisher', 'startPublisher');
  assert.equal(evidence.relay.verified, true);
  assert.equal(evidence.snapshot.stats[4].packetsReceived, 0);
  assert.equal(Object.hasOwn(evidence.snapshot.stats[4], 'bytesReceived'), false);
  assert.deepEqual(evidence.snapshot.media, { remote_tracks: [], presented_frames: null });
  assert.deepEqual(evidence.snapshot.ice, { generation: 1, local_sha256: 'a'.repeat(64), remote_sha256: 'b'.repeat(64) });
  assert.ok(!JSON.stringify(evidence).includes('192.0.2.'));
  assert.equal(Object.hasOwn(evidence, 'outcome'), false);
});

for (const [name, change, code, category] of [
  ['host local', (s) => { s.stats[2].candidateType = 'host'; }, 'RELAY_LOCAL_NOT_RELAY', 'assertion'],
  ['srflx local', (s) => { s.stats[2].candidateType = 'srflx'; }, 'RELAY_LOCAL_NOT_RELAY', 'assertion'],
  ['absent pair', (s) => { s.stats = []; }, 'RELAY_PAIR_MISSING', 'missing_evidence'],
  ['dangling pair', (s) => { s.stats.splice(1, 1); }, 'RELAY_PAIR_MISSING', 'missing_evidence'],
  ['dangling candidate', (s) => { s.stats.splice(2, 1); }, 'RELAY_CANDIDATE_MISSING', 'missing_evidence'],
  ['remote type', (s) => { s.stats[3].candidateType = 'srflx'; }, 'RELAY_REMOTE_TYPE_MISMATCH', 'assertion'],
  ['IP family', (s) => { s.stats[2].address = '2001:db8::1'; }, 'RELAY_ADDRESS_FAMILY_MISMATCH', 'assertion'],
  ['protocol', (s) => { s.stats[2].protocol = 'tcp'; }, 'RELAY_PROTOCOL_MISMATCH', 'assertion'],
  ['TURN transport', (s) => { s.stats[2].relayProtocol = 'udp'; }, 'RELAY_PROTOCOL_MISMATCH', 'assertion'],
  ['unobserved TURN transport', (s) => { delete s.stats[2].relayProtocol; }, 'RELAY_PROTOCOL_MISSING', 'missing_evidence'],
  ['config and counters only', (s) => { s.stats = [{ id: 'rtp', type: 'inbound-rtp', framesDecoded: 999 }]; s.api.post_status = 200; }, 'RELAY_PAIR_MISSING', 'missing_evidence'],
]) test(`relay evidence refuses ${name}`, async (t) => {
  const { adapter } = setup(t, { snapshot: () => { const s = snapshot(); change(s); return s; } });
  await adapter.openPublisher(context);
  const result = await adapter.execute('publisher', 'snapshot');
  assert.equal(result.relay.verified, false);
  assert.equal(result.relay.code, code);
  assert.equal(result.relay.category, category);
});

test('missing relay contract and malformed generation hash never manufacture evidence', async (t) => {
  const { adapter } = setup(t);
  await adapter.openPublisher({ browser });
  assert.equal((await adapter.execute('publisher', 'snapshot')).relay.code, 'RELAY_CONTRACT_MISSING');
  const bad = setup(t, { snapshot: () => { const s = snapshot(); s.ice.generation = 2; return s; } });
  await bad.adapter.openPublisher(context);
  await assert.rejects(bad.adapter.execute('publisher', 'snapshot'), { code: 'BROWSER_PAGE_RESULT_INVALID' });
});

test('unknown operations, roles, huge or accessor arguments fail before page effects', async (t) => {
  const { adapter, fake } = setup(t);
  await adapter.openPublisher(context);
  let reads = 0;
  const accessor = Object.defineProperty({}, 'token', { enumerable: true, get() { reads++; return secret; } });
  await assert.rejects(adapter.execute('publisher', 'configure', accessor), { code: 'BROWSER_INPUT_INVALID' });
  await assert.rejects(adapter.execute('publisher', 'configure', { token: 'x'.repeat(70_000) }), { code: 'BROWSER_INPUT_INVALID' });
  await assert.rejects(adapter.execute('publisher', 'eval', secret), { code: 'BROWSER_OPERATION_INVALID' });
  await assert.rejects(adapter.execute('unknown', 'snapshot'), { code: 'BROWSER_ROLE_INVALID' });
  await assert.rejects(adapter.execute('publisher', 'startViewer'), { code: 'BROWSER_OPERATION_INVALID' });
  assert.equal(reads, 0);
  assert.equal(fake.events.filter((e) => e[1] === 'configure').length, 0);
});

test('browser result accessors are never executed and unknown secret fields are rejected', async (t) => {
  let reads = 0;
  for (const mutate of [
    (s) => Object.defineProperty(s, 'secret', { enumerable: true, get() { reads++; return secret; } }),
    (s) => { s.token = secret; return s; },
    (s) => { s.stats[0].credential = secret; return s; },
    (s) => { s.stats = Array.from({ length: 513 }, () => ({ id: 'x' })); return s; },
  ]) {
    const { adapter } = setup(t, { snapshot: () => mutate(snapshot()) });
    await adapter.openPublisher(context);
    await assert.rejects(adapter.execute('publisher', 'snapshot'), { code: 'BROWSER_PAGE_RESULT_INVALID' });
  }
  assert.equal(reads, 0);
});

test('external errors and reused secrets in permitted string fields are never exported', async (t) => {
  const { adapter } = setup(t, { snapshot: () => { const s = snapshot(); s.api.session_id = encodeURIComponent(secret); return s; },
    methods: { restartIce: () => { throw Object.assign(new Error(secret), { code: secret, cause: configuration() }); } } });
  await adapter.openPublisher(context);
  await adapter.execute('publisher', 'configure', configuration());
  const result = await adapter.execute('publisher', 'snapshot');
  assert.equal(result.snapshot.api.session_id, '[REDACTED]');
  await assert.rejects(adapter.execute('publisher', 'restartIce', { generation: 2 }), (error) => {
    assert.equal(error.code, 'BROWSER_PAGE_OPERATION_FAILED');
    assert.ok(!JSON.stringify(error).includes(secret));
    assert.ok(!error.message.includes(secret)); return true;
  });
});

test('deadlines and cancellation bound Grid creation and page commands with no overlapping mutation', async (t) => {
  const build = setup(t, { build: hanging }, { commandTimeoutMs: 20 });
  await assert.rejects(build.adapter.preflightBrowser(context), { code: 'BROWSER_COMMAND_TIMEOUT' });
  const { adapter, fake } = setup(t, { methods: { snapshot: hanging } }, { commandTimeoutMs: 30 });
  await adapter.openPublisher(context);
  const pending = adapter.execute('publisher', 'snapshot');
  await assert.rejects(adapter.execute('publisher', 'restartIce', { generation: 2 }), { code: 'BROWSER_ROLE_BUSY' });
  await assert.rejects(pending, { code: 'BROWSER_COMMAND_TIMEOUT' });
  await assert.rejects(adapter.execute('publisher', 'snapshot'), { code: 'BROWSER_ROLE_UNAVAILABLE' });
  const cancel = setup(t, { methods: { snapshot: hanging } });
  await cancel.adapter.openPublisher(context);
  const controller = new AbortController();
  const work = cancel.adapter.execute('publisher', 'snapshot', undefined, controller.signal);
  controller.abort(secret);
  await assert.rejects(work, { code: 'BROWSER_COMMAND_ABORTED' });
  await adapter.closeAll();
  assert.equal(fake.events.filter((e) => e[1] === 'quit').length, 1);
});

test('partial viewer open keeps primary failure and reverse cleanup attempts all failures once', async (t) => {
  const { adapter, fake } = setup(t, { sessions: [ { quit: () => { throw Error(secret); } },
    { get: () => { throw Error(secret); }, quit: hanging } ] }, { commandTimeoutMs: 20 });
  await adapter.openPublisher(context);
  await assert.rejects(adapter.openViewer(context), { code: 'BROWSER_GRID_COMMAND_FAILED' });
  const result = await adapter.closeAll();
  assert.equal(result.cleanup_failures.length, 2);
  assert.deepEqual(result.cleanup_failures.map((f) => [f.role, f.code]),
    [['viewer', 'BROWSER_COMMAND_TIMEOUT'], ['publisher', 'BROWSER_GRID_COMMAND_FAILED']]);
  assert.deepEqual(await adapter.closeAll(), result);
  assert.deepEqual(fake.events.filter((e) => e[1] === 'quit'), [[1, 'quit'], [0, 'quit']]);
  await assert.rejects(adapter.openViewer(context), { code: 'BROWSER_ADAPTER_CLOSED' });
  assert.ok(!JSON.stringify(result).includes(secret));
});

test('page cleanup errors survive session quit and closeRole is terminal and idempotent', async (t) => {
  const { adapter, fake } = setup(t, { methods: { close: () => ({ phase: 'closed', cleanup_errors: [{ code: 'HTTP_OPERATION_FAILED', operation: 'delete' }] }) } });
  await adapter.openPublisher(context);
  const result = await adapter.closeRole('publisher');
  assert.equal(result.cleanup_failures.length, 1);
  assert.deepEqual(await adapter.closeRole('publisher'), result);
  await assert.rejects(adapter.execute('publisher', 'snapshot'), { code: 'BROWSER_ROLE_UNAVAILABLE' });
  assert.equal(fake.events.filter((e) => e[1] === 'quit').length, 1);
});

test('actual pinned Selenium Builder honors explicit Grid and browser despite environment overrides', async (t) => {
  const grid = await startFakeGrid();
  t.after(() => grid.close());
  const original = { url: process.env.SELENIUM_REMOTE_URL, browser: process.env.SELENIUM_BROWSER };
  process.env.SELENIUM_REMOTE_URL = 'http://127.0.0.1:1';
  process.env.SELENIUM_BROWSER = 'firefox:999:Linux';
  t.after(() => {
    for (const [name, value] of [['SELENIUM_REMOTE_URL', original.url], ['SELENIUM_BROWSER', original.browser]]) {
      if (value === undefined) delete process.env[name]; else process.env[name] = value;
    }
  });
  const adapter = factory()({ source, gridUrl: grid.gridUrl, profile: 'diagnostic', allowLoopbackHttp: true, commandTimeoutMs: 500 });
  t.after(() => adapter.closeAll());
  await adapter.preflightBrowser(context);
  await adapter.openPublisher(context);
  await adapter.execute('publisher', 'configure', configuration());
  const result = await adapter.execute('publisher', 'snapshot');
  assert.equal(result.relay.verified, true);
  assert.equal(grid.requests[0].body.capabilities.alwaysMatch.browserName, 'chrome');
  assert.equal(grid.requests[0].body.capabilities.alwaysMatch.browserVersion, '127.0.0');
  assert.ok(grid.requests.some((request) => request.url === '/status'));
  await adapter.closeAll();
  assert.equal(grid.drivers.size, 0);
});

test('Grid readiness is evidence-bearing and never inferred from fake session creation', async (t) => {
  const { adapter } = setup(t, { status: { ready: false, message: secret } });
  await assert.rejects(adapter.preflightBrowser(context), { code: 'BROWSER_GRID_NOT_READY', category: 'missing_evidence' });
});

test('nested API metadata cannot smuggle browser-returned secret objects into results', async (t) => {
  for (const mutate of [
    (s) => { s.api.etag = { token: 'UNKNOWN-browser-secret' }; },
    (s) => { s.connection_state = { credential: 'UNKNOWN-browser-secret' }; },
    (s) => { s.api.local_media_order = ['audio', { password: 'UNKNOWN-browser-secret' }]; },
    (s) => { s.stats[4].packetsReceived = 'UNKNOWN-browser-secret'; },
  ]) {
    const { adapter } = setup(t, { snapshot: () => { const s = snapshot(); mutate(s); return s; } });
    await adapter.openPublisher(context);
    await assert.rejects(adapter.execute('publisher', 'snapshot'), { code: 'BROWSER_PAGE_RESULT_INVALID' });
  }
});

test('exact duplicate session identity fails before configuring the viewer', async (t) => {
  const { adapter } = setup(t, { sessionId: 'same' });
  await adapter.openPublisher(context);
  await assert.rejects(adapter.openViewer(context), { code: 'BROWSER_SESSION_NOT_DISTINCT' });
});

test('IPv6 selected pairs preserve real track and presented-frame evidence', async (t) => {
  const { adapter } = setup(t, { snapshot: () => {
    const s = snapshot('viewer'); s.stats[2].address = '2001:db8::1'; s.stats[3].address = '2001:db8::2';
    s.media = { remote_tracks: [{ kind: 'audio', ready_state: 'live', muted: false }, { kind: 'video', ready_state: 'live', muted: false }], presented_frames: 15 };
    return s;
  } });
  await adapter.openViewer({ browser, relay_contract: { ...relay_contract, ip_family: 'ipv6' } });
  const result = await adapter.execute('viewer', 'snapshot');
  assert.equal(result.relay.verified, true);
  assert.equal(result.relay.pairs[0].ip_family, 'ipv6');
  assert.equal(result.snapshot.media.remote_tracks.length, 2);
  assert.equal(result.snapshot.media.presented_frames, 15);
});

test('actual Grid creation timeout cancels the socket without retrying the mutation', async (t) => {
  const grid = await startFakeGrid({ hang: '/session' });
  t.after(() => grid.close());
  const adapter = factory()({ source, gridUrl: grid.gridUrl, profile: 'diagnostic', allowLoopbackHttp: true, commandTimeoutMs: 30 });
  t.after(() => adapter.closeAll());
  await assert.rejects(adapter.preflightBrowser(context), { code: 'BROWSER_COMMAND_TIMEOUT' });
  await new Promise((resolve) => setTimeout(resolve, 75));
  assert.equal(grid.requests.length, 1);
  const cleanup = await adapter.closeAll();
  assert.equal(cleanup.cleanup_failures[0].code, 'BROWSER_SESSION_CREATION_UNCONFIRMED');
});

test('aborted creation never allocates a session and late creation is boundedly quit', async (t) => {
  const controller = new AbortController(); controller.abort();
  const cancelled = setup(t);
  await assert.rejects(cancelled.adapter.openPublisher(context, controller.signal), { code: 'BROWSER_COMMAND_ABORTED' });
  assert.equal(cancelled.fake.drivers.length, 0);
  let complete;
  const external = fakeWebDriver();
  const { adapter } = setup(t, { build: () => new Promise((resolve) => { complete = resolve; }) }, { commandTimeoutMs: 20 });
  await assert.rejects(adapter.openPublisher(context), { code: 'BROWSER_COMMAND_TIMEOUT' });
  const result = await adapter.closeAll();
  assert.equal(result.cleanup_failures[0].code, 'BROWSER_SESSION_CREATION_UNCONFIRMED');
  complete(external.builderFactory().build());
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(external.events.filter((e) => e[1] === 'quit'), [[0, 'quit']]);
});

test('close preempts a page command and still attempts viewer then publisher with a cancelled signal', async (t) => {
  const { adapter, fake } = setup(t, { sessions: [{ methods: { snapshot: hanging } }, {}] });
  await adapter.openPublisher(context); await adapter.openViewer(context);
  const pending = adapter.execute('publisher', 'snapshot');
  const rejected = assert.rejects(pending, { code: 'BROWSER_COMMAND_ABORTED' });
  const controller = new AbortController(); controller.abort(secret);
  const cleanup = await adapter.closeAll(controller.signal);
  await rejected;
  assert.deepEqual(fake.events.filter((e) => e[1] === 'quit'), [[1, 'quit'], [0, 'quit']]);
  assert.equal(cleanup.cleanup_failures.filter((entry) => entry.code === 'BROWSER_COMMAND_ABORTED').length, 2);
});

test('malformed relay contract is rejected before creating any session', async (t) => {
  const { adapter, fake } = setup(t);
  await assert.rejects(adapter.openPublisher({ browser, relay_contract: { ...relay_contract, ip_family: 'IPv4' } }), { code: 'BROWSER_INPUT_INVALID' });
  assert.equal(fake.drivers.length, 0);
});

test('a nominated pair alone cannot replace selected pair evidence; every selected transport is verified', async (t) => {
  const nominated = setup(t, { snapshot: () => {
    const s = snapshot(); s.stats.shift(); s.stats[0].nominated = true; return s;
  } });
  await nominated.adapter.openPublisher(context);
  assert.equal((await nominated.adapter.execute('publisher', 'snapshot')).relay.verified, false);
  const multiple = setup(t, { snapshot: () => {
    const s = snapshot(); s.stats.push({ id: 'transport2', type: 'transport', selectedCandidatePairId: 'missing' }); return s;
  } });
  await multiple.adapter.openPublisher(context);
  assert.equal((await multiple.adapter.execute('publisher', 'snapshot')).relay.code, 'RELAY_PAIR_MISSING');
});

test('browser callback result is bounded again at the external Grid boundary', async (t) => {
  const { adapter, fake } = setup(t, {}, { maxResultBytes: 4096 });
  await adapter.openPublisher(context);
  let reads = 0;
  fake.drivers[0].executeAsyncScript = async () => Object.defineProperty({}, 'value', { enumerable: true, get() { reads++; return secret; } });
  await assert.rejects(adapter.execute('publisher', 'snapshot'), { code: 'BROWSER_PAGE_RESULT_INVALID' });
  assert.equal(reads, 0);
});

test('actual Grid redirects cannot leave the approved endpoint or create a second session', async (t) => {
  const destination = await startFakeGrid();
  t.after(() => destination.close());
  const grid = await startFakeGrid({ redirect: `${destination.gridUrl}/session` });
  t.after(() => grid.close());
  const adapter = factory()({ source, gridUrl: grid.gridUrl, profile: 'diagnostic', allowLoopbackHttp: true, commandTimeoutMs: 100 });
  t.after(() => adapter.closeAll());
  await assert.rejects(adapter.preflightBrowser(context), { code: 'BROWSER_GRID_COMMAND_FAILED' });
  assert.equal(destination.requests.length, 0);
  assert.equal(grid.requests.length, 1);
});

test('cleanup preserves known page failure codes and stages while replacing untrusted diagnostics', async (t) => {
  const { adapter } = setup(t, { methods: { close: () => ({ phase: 'closed', cleanup_errors: [
    { code: 'OPERATION_TIMEOUT', operation: 'delete' }, { code: 'UNKNOWN-secret', operation: 'UNKNOWN-secret' } ] }) } });
  await adapter.openPublisher(context);
  const result = await adapter.closeAll();
  assert.deepEqual(result.cleanup_failures, [
    { role: 'publisher', code: 'OPERATION_TIMEOUT', operation: 'delete' },
    { role: 'publisher', code: 'BROWSER_PAGE_CLEANUP_FAILED', operation: 'page_cleanup' } ]);
});
