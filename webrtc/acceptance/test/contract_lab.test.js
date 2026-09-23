'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { createRunIdentity } = require('../src/ids');
const { loadManifest } = require('../src/manifest');
const { runManifest } = require('../src/controller');
const { createAcceptanceLab } = require('../src/runtime_lab');
const { writeContractLabReport } = require('../src/contract_lab_report');
const { startFakeSfuServer } = require('./fixtures/fake_sfu_server');
const { startFakeGrid, PAGE_HASH } = require('./fixtures/fake_webdriver');

const providerProcess = path.join(__dirname, '..', 'test_support', 'contract_lab_provider.js');
const hookProcess = path.join(__dirname, '..', 'test_support', 'contract_lab_hook.js');

async function workspace(t) {
  const root = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'webrtc-contract-lab-'));
  t.after(() => fs.promises.rm(root, { recursive: true, force: true }));
  const statePath = path.join(root, 'lab-state.json');
  await fs.promises.writeFile(statePath, JSON.stringify({ tokens: {}, turn: {}, inject: {} }), { mode: 0o600 });
  return { root, statePath };
}

function readState(statePath) {
  return JSON.parse(fs.readFileSync(statePath, 'utf8'));
}

function authorizeFromState(statePath) {
  return ({ token, audience, scope, room, participant }) => {
    if (typeof token !== 'string') return false;
    const state = readState(statePath);
    const claims = state.tokens && state.tokens[token];
    return Boolean(claims &&
      claims.audience === audience &&
      Array.isArray(claims.scope) && claims.scope.includes(scope) &&
      claims.binding && claims.binding.room_id === room &&
      (!participant || claims.binding.participant_id === participant));
  };
}

function sha(label) {
  return crypto.createHash('sha256').update(label).digest('hex');
}

function browserSnapshot(role, generation, state, inject = {}) {
  state.mediaPackets += 10;
  state.frames += 2;
  const pairId = `${role}-pair-${generation}`;
  const localId = `${role}-local-${generation}`;
  const remoteId = `${role}-remote-${generation}`;
  return {
    schema_version: 1,
    phase: 'active',
    role,
    generation,
    timestamp_ms: Date.now(),
    ice: {
      generation,
      local_sha256: sha(`${role}-local-${generation}`),
      remote_sha256: sha(`${role}-remote-${generation}`),
    },
    connection_state: 'connected',
    ice_connection_state: 'connected',
    signaling_state: 'stable',
    stats: [
      { id: `${role}-transport-${generation}`, type: 'transport', selectedCandidatePairId: pairId },
      { id: pairId, type: 'candidate-pair', localCandidateId: localId, remoteCandidateId: remoteId,
        state: 'succeeded', currentRoundTripTime: 0.02 },
      { id: localId, type: 'local-candidate',
        candidateType: inject.host_candidate ? 'host' : 'relay',
        protocol: 'udp', relayProtocol: 'tcp', address: '192.0.2.1' },
      { id: remoteId, type: 'remote-candidate', candidateType: 'host',
        protocol: 'udp', address: '192.0.2.2' },
      { id: `${role}-rtp-${generation}`, type: 'inbound-rtp', kind: 'video',
        packetsReceived: state.mediaPackets, packetsLost: 0, jitter: 0.002,
        nackCount: state.frames, framesDecoded: state.frames },
    ],
    api: {
      post_status: 201,
      trickle_status: 204,
      restart_status: state.restartStatus,
      stale_etag_status: state.staleStatus,
      delete_status: state.deleteStatus,
      local_media_order: role === 'publisher' ? ['audio', 'video'] : [],
      remote_answer_applied: true,
      location: state.resourceUrl,
      etag: state.etag,
      session_id: state.sessionId,
    },
    media: {
      remote_tracks: role === 'viewer'
        ? [{ kind: 'audio', ready_state: 'live', muted: false },
          { kind: 'video', ready_state: 'live', muted: false }]
        : [],
      presented_frames: role === 'viewer' ? state.frames : null,
    },
    cleanup_errors: [],
  };
}

function browserMethods(role, inject = {}) {
  const state = {
    configuration: null,
    resourceUrl: null,
    sessionId: null,
    etag: null,
    generation: 0,
    restartStatus: null,
    staleStatus: null,
    deleteStatus: null,
    mediaPackets: 0,
    frames: 0,
  };

  const request = async (url, init) => {
    const response = await fetch(url, { ...init, redirect: 'manual' });
    return response;
  };

  const methods = {
    async configure(configuration) {
      state.configuration = configuration;
      return { phase: 'configured' };
    },
    async startPublisher() {
      return start('whip');
    },
    async startViewer() {
      return start('whep');
    },
    async snapshot() {
      return browserSnapshot(role, state.generation, state, inject);
    },
    async restartIce(argument) {
      if (argument && argument.turn) state.configuration.turn = argument.turn;
      const previousEtag = state.etag;
      const restart = await request(state.resourceUrl, {
        method: 'PATCH',
        headers: {
          Authorization: `Bearer ${state.configuration.token}`,
          'Content-Type': 'application/trickle-ice-sdpfrag',
          'If-Match': previousEtag,
        },
        body: 'restart',
      });
      state.restartStatus = restart.status;
      state.etag = restart.headers.get('etag') || state.etag;
      const stale = await request(state.resourceUrl, {
        method: 'PATCH',
        headers: {
          Authorization: `Bearer ${state.configuration.token}`,
          'Content-Type': 'application/trickle-ice-sdpfrag',
          'If-Match': previousEtag,
        },
        body: 'stale',
      });
      state.staleStatus = stale.status;
      state.generation = argument.generation;
      return browserSnapshot(role, state.generation, state, inject);
    },
    async close() {
      if (state.resourceUrl) {
        const response = await request(state.resourceUrl, {
          method: 'DELETE',
          headers: { Authorization: `Bearer ${state.configuration.token}` },
        });
        state.deleteStatus = response.status;
      }
      return { phase: 'closed', cleanup_errors: [] };
    },
  };

  async function start(kind) {
    const endpoint = kind === 'whip'
      ? state.configuration.whip_url
      : state.configuration.whep_url;
    const created = await request(endpoint, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${state.configuration.token}`,
        'Content-Type': 'application/sdp',
      },
      body: 'contract-offer',
    });
    assert.equal(created.status, 201);
    state.etag = created.headers.get('etag');
    state.resourceUrl = new URL(created.headers.get('location'), endpoint).href;
    state.sessionId = state.resourceUrl.split('/').at(-1);
    const trickle = await request(state.resourceUrl, {
      method: 'PATCH',
      headers: {
        Authorization: `Bearer ${state.configuration.token}`,
        'Content-Type': 'application/trickle-ice-sdpfrag',
        'If-Match': state.etag,
      },
      body: 'initial',
    });
    assert.equal(trickle.status, 204);
    state.generation = 1;
    return browserSnapshot(role, 1, state, inject);
  }

  return methods;
}

async function writeManifest(root, statePath, sfuBaseUrl) {
  const manifestPath = path.join(root, 'contract lab manifest.json');
  const command = (script) => ({ command: [process.execPath, script, statePath] });
  const value = {
    schema_version: 2,
    profile: 'contract_lab',
    source: {
      commit: '1'.repeat(40),
      require_clean_tree: true,
      test_page_url: 'https://acceptance.example.test/client.html',
      test_page_sha256: PAGE_HASH,
    },
    grid: {
      endpoint_env: 'TURBO_MEDIA_ACCEPTANCE_GRID_URL',
      browsers: [{ name: 'chrome', version: '127.0.0', platform: 'Windows 11' }],
    },
    sfu: {
      base_url: sfuBaseUrl,
      whip_endpoint: '/whip',
      whep_endpoint: '/whep',
      token_provider: command(providerProcess),
    },
    turn: { credential_provider: command(providerProcess) },
    topologies: [{
      topology_id: 'contract-ipv4',
      relay_contract: {
        schema_version: 1,
        ip_family: 'ipv4',
        protocol: 'udp',
        relay_protocol: 'tcp',
        remote_candidate_types: ['host'],
      },
      hooks: {
        setup: command(hookProcess),
        transition: command(hookProcess),
        teardown: command(hookProcess),
        probe: command(hookProcess),
      },
    }],
    scenarios: [{
      scenario_id: 'contract-expiry-migration',
      topology_id: 'contract-ipv4',
      workflow: { credential_expiry: true, topology_transition: true },
      sample_interval_ms: 250,
      duration_ms: 250,
    }],
    phase_deadlines: {
      preflight_ms: 2_000,
      connect_ms: 2_000,
      stable_ms: 2_000,
      transition_ms: 2_000,
      recovery_ms: 2_000,
      drain_ms: 2_000,
    },
    threshold_profile: {
      profile_id: 'contract-lab',
      stable: { minimum_samples: 1, max_rtt_ms: 100, min_media_delta: 1 },
      recovery: { minimum_samples: 1, max_rtt_ms: 100, min_media_delta: 1 },
      drain: { maximum_duration_ms: 2_000 },
    },
    artifact_directory: path.join(root, 'artifacts'),
  };
  await fs.promises.writeFile(manifestPath, JSON.stringify(value), 'utf8');
  return manifestPath;
}

async function runLab(t, inject = {}) {
  const { root, statePath } = await workspace(t);
  const state = readState(statePath);
  state.inject = { ...inject };
  fs.writeFileSync(statePath, JSON.stringify(state), { mode: 0o600 });

  const sfu = await startFakeSfuServer({
    authorize: authorizeFromState(statePath),
    acceptStaleEtag: inject.stale_etag_accepted === true,
    cleanupResidueOnDelete: inject.cleanup_residue === true,
  });
  t.after(() => sfu.close());

  const preflight = { methods: browserMethods('publisher', inject) };
  const publisher = { methods: browserMethods('publisher', inject) };
  const viewer = { methods: browserMethods('viewer', inject) };
  const grid = await startFakeGrid({ webdriver: { sessions: [preflight, publisher, viewer] } });
  t.after(() => grid.close());

  const manifestPath = await writeManifest(root, statePath, sfu.baseUrl);
  const manifest = loadManifest(manifestPath, { outputDirectory: path.join(root, 'artifacts') });
  const runIdentity = createRunIdentity(Date.now, () => 'contract-lab', 'a'.repeat(64));
  const lab = createAcceptanceLab({
    manifest,
    runIdentity,
    dependencies: {
      env: { TURBO_MEDIA_ACCEPTANCE_GRID_URL: grid.gridUrl },
      allowLoopbackHttp: true,
    },
  });

  const result = await runManifest({ lab, run_identity: runIdentity }, manifest);
  return { result, manifest, runIdentity, state: readState(statePath), sfu, grid, root };
}

test('contract lab PASS crosses real process, WebDriver HTTP, SFU HTTP, expiry, restart and drain boundaries', async (t) => {
  const lab = await runLab(t);
  assert.equal(lab.result.outcome, 'PASS');
  assert.equal(lab.result.cases.length, 1);
  assert.equal(lab.result.cases[0].outcome, 'PASS');
  assert.equal(lab.result.cases[0].browser_ice_generation, 2);
  const freshCredentialId = lab.result.cases[0].fresh_turn_credential_id;
  assert.match(freshCredentialId, /^turn-[1-9][0-9]*$/);
  assert.ok(Object.hasOwn(lab.state.turn, freshCredentialId));
  assert.equal(freshCredentialId, `turn-${lab.state.turn_counter}`);

  assert.ok(lab.state.turn_counter >= 3);
  assert.ok(lab.state.hooks.some((entry) => entry.probe_kind === 'credential_expiry' && entry.effective));
  assert.ok(lab.state.hooks.some((entry) => entry.action === 'transition' && entry.effective));
  assert.ok(lab.state.hooks.some((entry) => entry.probe_kind === 'turn_baseline' && entry.effective));

  const methods = lab.sfu.requests.map((entry) => [entry.method, entry.url]);
  assert.ok(methods.some(([method, url]) => method === 'POST' && url.startsWith('/whip/')));
  assert.ok(methods.some(([method, url]) => method === 'POST' && url.startsWith('/whep/')));
  assert.ok(methods.filter(([method]) => method === 'PATCH').length >= 6);
  assert.ok(methods.filter(([method]) => method === 'DELETE').length >= 2);
  assert.ok(lab.grid.requests.some((entry) => entry.method === 'POST' && entry.url === '/session'));
  assert.ok(lab.grid.requests.some((entry) => entry.url.includes('/execute/async')));
  assert.equal(JSON.stringify(lab.sfu.requests).includes('contract-sfu-secret'), false);

  const reportDirectory = path.join(lab.root, 'report');
  const published = await writeContractLabReport({
    runIdentity: lab.runIdentity,
    manifest: lab.manifest,
    controllerResult: lab.result,
    outputDirectory: reportDirectory,
    finishedAt: new Date().toISOString(),
  });
  assert.equal(published.report.environment.kind, 'contract_lab');
  assert.equal(published.report.environment.release_eligible, false);
  assert.equal(published.report.outcome, 'PASS');
  assert.equal(published.report.counts.total, 1);
  assert.equal(published.report.counts.PASS, 1);

  const runJson = await fs.promises.readFile(path.join(reportDirectory, 'run.json'), 'utf8');
  const markdown = await fs.promises.readFile(path.join(reportDirectory, 'summary.md'), 'utf8');
  const junit = await fs.promises.readFile(path.join(reportDirectory, 'junit.xml'), 'utf8');
  const canonical = JSON.parse(runJson);
  assert.equal(canonical.environment.kind, 'contract_lab');
  assert.equal(canonical.environment.release_eligible, false);
  assert.equal(canonical.outcome, 'PASS');
  assert.match(markdown, /- Environment: `contract_lab`/);
  assert.match(markdown, /- Release eligible: false/);
  assert.match(markdown, /\| 1 \| 1 \| 0 \| 0 \| 0 \|/);
  assert.match(junit, /tests="1" failures="0" errors="0" skipped="0"/);
  assert.match(junit, /environment_kind="contract_lab"/);
  assert.match(junit, /release_eligible="false"/);
  const combined = runJson + markdown + junit;
  for (const forbidden of ['contract-sfu-secret', 'contract-turn-secret']) {
    assert.equal(combined.includes(forbidden), false);
  }
});

test('contract lab classifies host candidate as FAIL', async (t) => {
  const lab = await runLab(t, { host_candidate: true });
  assert.equal(lab.result.outcome, 'FAIL');
  assert.equal(lab.result.cases[0].primary.reason.code, 'RELAY_LOCAL_NOT_RELAY');
});

test('contract lab classifies an expired credential that still succeeds as FAIL', async (t) => {
  const lab = await runLab(t, { expired_credential_accepted: true });
  assert.equal(lab.result.outcome, 'FAIL');
  assert.equal(lab.result.cases[0].primary.reason.code, 'EXPIRED_CREDENTIAL_ACCEPTED');
});

test('contract lab classifies accepted stale ETag as FAIL', async (t) => {
  const lab = await runLab(t, { stale_etag_accepted: true });
  assert.equal(lab.result.outcome, 'FAIL');
  assert.equal(lab.result.cases[0].primary.reason.code, 'STALE_ETAG_NOT_REJECTED');
});

test('contract lab classifies post-delete cleanup residue as ERROR', async (t) => {
  const lab = await runLab(t, { cleanup_residue: true });
  assert.equal(lab.result.outcome, 'ERROR');
  assert.ok(lab.result.cases[0].cleanup_failures.some((entry) =>
    entry.step === 'waitCaseResourcesZero' || entry.step === 'waitGlobalBaseline' ||
    entry.step === 'waitTurnBaseline'));
});
