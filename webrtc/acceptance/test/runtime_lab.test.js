'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { loadManifest } = require('../src/manifest');
const { runManifest } = require('../src/controller');
const { createAcceptanceLab } = require('../src/runtime_lab');

const fixture = path.join(__dirname, 'fixtures', 'minimal-manifest.json');

async function workspace(t) {
  const directory = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'webrtc-runtime-lab-'));
  t.after(() => fs.promises.rm(directory, { recursive: true, force: true }));
  return directory;
}

function runIdentity() {
  return {
    run_id: 'run-runtime-lab',
    manifest_hash: 'a'.repeat(64),
    started_at: '2026-09-23T00:00:00.000Z',
  };
}

function caseRuntime(manifest) {
  return {
    identity: {
      run_id: 'run-runtime-lab',
      case_id: 'case-runtime-lab',
    },
    definition: {
      topology_id: manifest.topologies[0].topology_id,
      scenario: manifest.scenarios[0],
    },
    machine: {
      generation: 0,
      last_sequence: 0,
    },
    relay_contract_hash: 'b'.repeat(64),
  };
}

function dependencies(calls, corsVerified = true) {
  return {
    env: { TURBO_MEDIA_ACCEPTANCE_GRID_URL: 'https://grid.example.test/wd/hub' },
    now: () => Date.parse('2026-09-23T00:00:00Z'),
    runBoundedCommand: async (options) => {
      calls.push(options);
      if (options.operation === 'sfu_token') {
        const request = options.request;
        return {
          schema_version: 1,
          provider_id: 'runtime-provider',
          token_id: 'runtime-token',
          audience: request.audience,
          scope: request.scope,
          subject: request.subject,
          binding: request.binding,
          issued_at: '2026-09-23T00:00:00.000Z',
          expires_at: '2026-09-23T01:00:00.000Z',
          token: 'SECRET_SFU_TOKEN',
        };
      }
      if (options.operation === 'turn_preflight' || options.operation === 'turn_credential') {
        return {
          schema_version: 1,
          provider_id: 'runtime-turn',
          credential_id: 'turn-1',
          urls: ['turns:turn.example.test:5349?transport=tcp'],
          username: 'SECRET_TURN_USER',
          credential: 'SECRET_TURN_CREDENTIAL',
          issued_at: '2026-09-23T00:00:00.000Z',
          expires_at: '2026-09-23T01:00:00.000Z',
          coturn_version: '4.6.3',
        };
      }
      throw new Error('unexpected command');
    },
    createSfuAdapter: () => ({
      async preflight() { return { ready: true }; },
      async captureBaseline() {
        return { node_id: 'node', room_count: 0, session_count: 0, published_track_count: 0 };
      },
    }),
    createSeleniumAdapter: (options) => ({
      async preflightBrowser() {
        calls.push({ browser_grid_url: options.gridUrl });
        return { cors: { verified: corsVerified } };
      },
      async closeAll() { return { cleanup_failures: [] }; },
    }),
  };
}

test('runtime lab preflight uses the declared Grid env and bounded stdin provider requests without secret argv', async (t) => {
  const output = await workspace(t);
  const manifest = loadManifest(fixture, { outputDirectory: output });
  const calls = [];
  const lab = createAcceptanceLab({
    manifest,
    runIdentity: runIdentity(),
    dependencies: dependencies(calls),
  });

  await lab.preflightArtifacts(manifest);
  await lab.preflightCase({
    browser: manifest.grid.browsers[0],
    relay_contract: manifest.topologies[0].relay_contract,
  });
  await lab.preflightProviders(manifest);
  await lab.preflightSfu(manifest);
  await lab.preflightTurn(manifest);
  await lab.preflightTopologies(manifest);

  assert.equal(calls.find((entry) => entry.browser_grid_url).browser_grid_url,
    'https://grid.example.test/wd/hub');
  const providerCalls = calls.filter((entry) => entry.executable);
  assert.ok(providerCalls.length >= 2);
  assert.ok(providerCalls.every((entry) => entry.args.every((arg) => !arg.includes('SECRET_'))));
  assert.ok(providerCalls.every((entry) => entry.request && typeof entry.request === 'object'));
  assert.equal(JSON.stringify(providerCalls).includes('SECRET_SFU_TOKEN'), false);
  assert.equal(JSON.stringify(providerCalls).includes('SECRET_TURN_CREDENTIAL'), false);
});

test('release run without Grid or providers is INCOMPLETE before provider SFU or media effects', async (t) => {
  const output = await workspace(t);
  const diagnostic = loadManifest(fixture, { outputDirectory: output });
  const manifest = { ...diagnostic, profile: 'release' };
  const providerCalls = [];
  const sfuCalls = [];
  const identity = runIdentity();
  const lab = createAcceptanceLab({
    manifest,
    runIdentity: identity,
    dependencies: {
      env: {},
      runBoundedCommand: async (options) => {
        providerCalls.push(options);
        throw new Error('provider must not run before missing Grid is classified');
      },
      createSfuAdapter: () => ({
        async preflight() { sfuCalls.push('preflight'); return { ready: true }; },
        async captureBaseline() {
          sfuCalls.push('captureBaseline');
          return { node_id: 'node', room_count: 0, session_count: 0, published_track_count: 0 };
        },
      }),
    },
  });

  const result = await runManifest({ lab, run_identity: identity }, manifest);
  assert.equal(result.outcome, 'INCOMPLETE');
  assert.equal(result.cases.length, 0);
  assert.equal(result.primary.reason.code, 'BROWSER_GRID_UNCONFIGURED');
  assert.deepEqual(providerCalls, []);
  assert.deepEqual(sfuCalls, []);
});

test('release runtime fails closed when topology probe, CORS, or TURN baseline evidence is unavailable', async (t) => {
  const output = await workspace(t);
  const diagnostic = loadManifest(fixture, { outputDirectory: output });
  const manifest = { ...diagnostic, profile: 'release' };
  const calls = [];
  const lab = createAcceptanceLab({
    manifest,
    runIdentity: runIdentity(),
    dependencies: dependencies(calls, false),
  });

  await assert.rejects(
    lab.preflightCase({
      browser: manifest.grid.browsers[0],
      relay_contract: manifest.topologies[0].relay_contract,
    }),
    { code: 'CORS_EVIDENCE_INCOMPLETE', category: 'missing_evidence' }
  );
  await assert.rejects(
    lab.preflightTopologies(manifest),
    { code: 'TOPOLOGY_PROBE_UNAVAILABLE', category: 'missing_evidence' }
  );
  await assert.rejects(
    lab.preflightTurn(manifest),
    { code: 'TURN_REACHABILITY_UNVERIFIED', category: 'missing_evidence' }
  );
  assert.deepEqual(await lab.waitTurnBaseline(caseRuntime(manifest)), {
    cleanup_failures: [{ code: 'TURN_BASELINE_UNVERIFIED' }],
  });
});

test('credential-expiry runtime reports missing evidence instead of manufacturing a rejection', async (t) => {
  const output = await workspace(t);
  const manifest = loadManifest(fixture, { outputDirectory: output });
  const lab = createAcceptanceLab({
    manifest,
    runIdentity: runIdentity(),
    dependencies: dependencies([]),
  });

  await assert.rejects(
    lab.assertCredentialExpiry(caseRuntime(manifest)),
    { code: 'CREDENTIAL_EXPIRY_PROBE_UNAVAILABLE', category: 'missing_evidence' }
  );
});
