'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const contracts = require('../src/contracts');
const { LIMITS: CONSTANT_LIMITS } = require('../src/constants');
const { createContractValidator, validateContract } = contracts;

const validator = createContractValidator(path.join(__dirname, '..', 'schemas'));

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function validManifest() {
  return {
    schema_version: 2,
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
      relay_contract: {
        schema_version: 1,
        ip_family: 'ipv4',
        protocol: 'udp',
        relay_protocol: 'tcp',
        remote_candidate_types: ['host'],
      },
      hooks: {
        setup: { command: ['topology-hook', 'setup'] },
        transition: { command: ['topology-hook', 'transition'] },
        teardown: { command: ['topology-hook', 'teardown'] },
      },
    }],
    scenarios: [{
      scenario_id: 'relay-baseline',
      topology_id: 'restricted-nat-ipv4',
      workflow: { credential_expiry: false, topology_transition: false },
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
    schema_version: 2,
    hook_id: 'restricted-nat-ipv4-transition',
    topology_id: 'restricted-nat-ipv4',
    action: 'transition',
    generation: 2,
    sequence: 4,
    relay_contract_hash: 'a'.repeat(64),
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

function loadContractsWithLimits(t, limits) {
  const isolatedDirectory = fs.mkdtempSync(path.join(__dirname, '.contracts-limits-'));
  const contractsPath = path.join(isolatedDirectory, 'contracts.js');
  const constantsPath = path.join(isolatedDirectory, 'constants.js');

  fs.copyFileSync(path.join(__dirname, '..', 'src', 'contracts.js'), contractsPath);
  fs.writeFileSync(
    constantsPath,
    `'use strict';\nconst LIMITS = Object.freeze(${JSON.stringify(limits)});\nmodule.exports = { LIMITS };\n`,
    'utf8'
  );
  t.after(() => {
    delete require.cache[require.resolve(contractsPath)];
    fs.rmSync(isolatedDirectory, { recursive: true, force: true });
  });
  return require(contractsPath);
}

test('contracts expose the shared LIMITS object and compile resource caps from it', (t) => {
  assert.strictEqual(contracts.LIMITS, CONSTANT_LIMITS);

  const limits = {
    MAX_CASES: 1,
    MAX_PROVIDER_OUTPUT_BYTES: 7,
    MAX_HOOK_OUTPUT_BYTES: 8,
    MAX_CASE_LOG_BYTES: 9,
    MAX_SAMPLES_PER_CASE: 10,
    MAX_ARTIFACT_BYTES_PER_CASE: 11,
    MIN_SAMPLE_INTERVAL_MS: 500,
    MAX_CASE_DURATION_MS: 2_000,
  };
  const isolatedContracts = loadContractsWithLimits(t, limits);
  const isolatedValidator = isolatedContracts.createContractValidator(path.join(__dirname, '..', 'schemas'));
  const manifest = validManifest();
  manifest.sfu.token_provider.command = ['sfu-token-provider'];
  manifest.turn.credential_provider.command = ['turn-credential-provider'];
  manifest.topologies[0].hooks.setup.command = ['topology-hook'];
  manifest.topologies[0].hooks.transition.command = ['topology-hook'];
  manifest.topologies[0].hooks.teardown.command = ['topology-hook'];
  manifest.scenarios[0].sample_interval_ms = limits.MIN_SAMPLE_INTERVAL_MS;
  manifest.scenarios[0].duration_ms = limits.MAX_CASE_DURATION_MS;
  manifest.limits = {
    max_cases: limits.MAX_CASES,
    max_provider_output_bytes: limits.MAX_PROVIDER_OUTPUT_BYTES,
    max_hook_output_bytes: limits.MAX_HOOK_OUTPUT_BYTES,
    max_case_log_bytes: limits.MAX_CASE_LOG_BYTES,
    max_samples_per_case: limits.MAX_SAMPLES_PER_CASE,
    max_artifact_bytes_per_case: limits.MAX_ARTIFACT_BYTES_PER_CASE,
  };
  assert.deepEqual(isolatedContracts.validateContract(isolatedValidator, 'manifest', manifest), {
    valid: true,
    errors: [],
  });

  const tooManyCases = clone(manifest);
  tooManyCases.scenarios.push(clone(tooManyCases.scenarios[0]));
  assertRejectedWith(isolatedContracts, isolatedValidator, 'manifest', tooManyCases);

  const tooFastSampling = clone(manifest);
  tooFastSampling.scenarios[0].sample_interval_ms = limits.MIN_SAMPLE_INTERVAL_MS - 1;
  assertRejectedWith(isolatedContracts, isolatedValidator, 'manifest', tooFastSampling);

  const tooLongCase = clone(manifest);
  tooLongCase.scenarios[0].duration_ms = limits.MAX_CASE_DURATION_MS + 1;
  assertRejectedWith(isolatedContracts, isolatedValidator, 'manifest', tooLongCase);

  const tooMuchProviderOutput = clone(manifest);
  tooMuchProviderOutput.limits.max_provider_output_bytes = limits.MAX_PROVIDER_OUTPUT_BYTES + 1;
  assertRejectedWith(isolatedContracts, isolatedValidator, 'manifest', tooMuchProviderOutput);
});

function assertRejectedWith(contractApi, contractValidator, schemaName, value) {
  const result = contractApi.validateContract(contractValidator, schemaName, value);
  assert.equal(result.valid, false, `${schemaName} should be rejected`);
  assert.ok(result.errors.length > 0, `${schemaName} should expose schema errors`);
}

test('contracts reject legacy manifest and hook receipt v1 without fallback', () => {
  const manifest = validManifest();
  manifest.schema_version = 1;
  assertRejected('manifest', manifest);

  const receipt = validHookReceipt();
  receipt.schema_version = 1;
  assertRejected('hook-receipt', receipt);
});

test('contracts accepts a legal minimum manifest', () => {
  const result = validateContract(validator, 'manifest', validManifest());
  assert.deepEqual(result, { valid: true, errors: [] });
});

test('contracts require an explicit versioned relay contract for every topology', () => {
  const missing = validManifest();
  delete missing.topologies[0].relay_contract;
  assertRejected('manifest', missing);

  const duplicateRemoteType = validManifest();
  duplicateRemoteType.topologies[0].relay_contract.remote_candidate_types = ['host', 'host'];
  assertRejected('manifest', duplicateRemoteType);
});

test('contracts reject manifest fields outside the declared contract', () => {
  const manifest = validManifest();
  manifest.grid.untrusted_fallback = true;
  assertRejected('manifest', manifest);
});

test('contracts require explicit scenario workflow semantics', () => {
  const manifest = validManifest();
  delete manifest.scenarios[0].workflow;
  assertRejected('manifest', manifest);
});

test('contracts reject credential-expiry workflows without transition recovery', () => {
  const manifest = validManifest();
  manifest.scenarios[0].workflow = { credential_expiry: true, topology_transition: false };
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

test('contracts accept a manifest sampling-gap threshold and reject an invalid duration', () => {
  const accepted = validManifest();
  accepted.threshold_profile.stable.max_sample_gap_ms = 1_000;
  assert.deepEqual(validateContract(validator, 'manifest', accepted), { valid: true, errors: [] });

  const rejected = validManifest();
  rejected.threshold_profile.stable.max_sample_gap_ms = 0;
  assertRejected('manifest', rejected);
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
