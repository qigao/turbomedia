'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');

const {
  createContractValidator,
  validateContract,
} = require('../src/contracts');

const validator = createContractValidator(path.join(__dirname, '..', 'schemas'));

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function validManifest() {
  return {
    schema_version: 1,
    profile: 'diagnostic',
    source: {
      commit: '0123456789012345678901234567890123456789',
      require_clean_tree: true,
      test_page_url: 'https://acceptance.example.test/client.html',
      test_page_sha256: 'a'.repeat(64),
    },
    grid: {
      endpoint_env: 'TURBO_MEDIA_ACCEPTANCE_GRID_URL',
      browsers: [{ name: 'chrome', version: '127.0.0', platform: 'Windows 11' }],
    },
    sfu: {
      base_url: 'https://sfu.example.test',
      whip_endpoint: '/whip',
      whep_endpoint: '/whep',
      token_provider: { command: ['sfu-token-provider', '--json'] },
    },
    turn: {
      credential_provider: { command: ['turn-credential-provider', '--json'] },
    },
    topologies: [{
      topology_id: 'restricted-nat-ipv4',
      hooks: {
        setup: { command: ['topology-hook', 'setup'] },
        transition: { command: ['topology-hook', 'transition'] },
        teardown: { command: ['topology-hook', 'teardown'] },
      },
    }],
    scenarios: [{
      scenario_id: 'relay-baseline',
      topology_id: 'restricted-nat-ipv4',
      sample_interval_ms: 250,
      duration_ms: 1_000,
    }],
    phase_deadlines: {
      preflight_ms: 1_000,
      connect_ms: 1_000,
      stable_ms: 1_000,
      transition_ms: 1_000,
      recovery_ms: 1_000,
      drain_ms: 1_000,
    },
    threshold_profile: {
      profile_id: 'diagnostic-default',
      stable: { minimum_samples: 1 },
      recovery: { minimum_samples: 1 },
      drain: { maximum_duration_ms: 1_000 },
    },
    artifact_directory: 'artifacts/webrtc-acceptance',
  };
}

function validTurnCredential() {
  return {
    schema_version: 1,
    provider_id: 'turn-lab-v1',
    credential_id: 'turn-credential-1',
    urls: ['turns:turn.example.test:5349?transport=tcp'],
    username: 'expiry:alice',
    credential: 'ephemeral-turn-secret',
    issued_at: '2026-08-25T08:00:00Z',
    expires_at: '2026-08-25T08:02:00Z',
    coturn_version: '4.6.3',
  };
}

function validHookReceipt() {
  return {
    schema_version: 1,
    hook_id: 'restricted-nat-ipv4-transition',
    topology_id: 'restricted-nat-ipv4',
    action: 'transition',
    generation: 2,
    sequence: 4,
    started_at: '2026-08-25T08:01:00Z',
    finished_at: '2026-08-25T08:01:01Z',
    observed_at: '2026-08-25T08:01:01Z',
    status: 'ok',
    effective: true,
    evidence_id: 'lab-receipt-123',
  };
}

function validReport() {
  return {
    schema_version: 1,
    run_id: 'run-123',
    outcome: 'PASS',
    started_at: '2026-08-25T08:00:00Z',
    finished_at: '2026-08-25T08:02:00Z',
    cases: [],
  };
}

function assertRejected(schemaName, value) {
  const result = validateContract(validator, schemaName, value);
  assert.equal(result.valid, false, `${schemaName} should be rejected`);
  assert.ok(result.errors.length > 0, `${schemaName} should expose schema errors`);
}

test('contracts accepts a legal minimum manifest', () => {
  const result = validateContract(validator, 'manifest', validManifest());
  assert.deepEqual(result, { valid: true, errors: [] });
});

test('contracts reject manifest fields outside the declared contract', () => {
  const manifest = validManifest();
  manifest.grid.untrusted_fallback = true;
  assertRejected('manifest', manifest);
});

test('contracts reject more than 256 declared scenarios', () => {
  const manifest = validManifest();
  manifest.scenarios = Array.from({ length: 257 }, (_, index) => ({
    scenario_id: `scenario-${index}`,
    topology_id: 'restricted-nat-ipv4',
    sample_interval_ms: 250,
    duration_ms: 1_000,
  }));
  assertRejected('manifest', manifest);
});

test('contracts reject sampling intervals below 250 milliseconds', () => {
  const manifest = validManifest();
  manifest.scenarios[0].sample_interval_ms = 249;
  assertRejected('manifest', manifest);
});

test('contracts reject case durations above one hour', () => {
  const manifest = validManifest();
  manifest.scenarios[0].duration_ms = 3_600_001;
  assertRejected('manifest', manifest);
});

test('contracts reject TURN credentials without an expiry', () => {
  const credential = clone(validTurnCredential());
  delete credential.expires_at;
  assertRejected('turn-credential', credential);
});

test('contracts reject hook receipts that contain secrets', () => {
  const receipt = validHookReceipt();
  receipt.secret = 'must-not-appear';
  assertRejected('hook-receipt', receipt);
});

test('contracts reject reports with unknown outcomes', () => {
  const report = validReport();
  report.outcome = 'RETRYING';
  assertRejected('report', report);
});
