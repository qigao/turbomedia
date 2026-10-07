'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { loadManifest } = require('../src/manifest');
const { createFakeLab } = require('./fixtures/fake_lab');
const { executeAcceptanceRun, RESULT_FILE } = require('../src/runner');

const manifestPath = path.join(__dirname, 'fixtures', 'minimal-manifest.json');

async function outputDirectory(t) {
  const directory = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'webrtc-runner-'));
  t.after(() => fs.promises.rm(directory, { recursive: true, force: true }));
  return directory;
}

test('runner executes the real controller with an injected lab and writes one bounded safe result artifact', async (t) => {
  const output = await outputDirectory(t);
  const manifest = loadManifest(manifestPath, { outputDirectory: output });
  const fake = createFakeLab();

  const result = await executeAcceptanceRun({
    manifest,
    clock: () => Date.parse('2026-09-23T00:00:00Z'),
    randomUUID: () => 'runner-test',
    labFactory: async () => fake.lab,
  });

  assert.deepEqual(result, {
    outcome: 'PASS',
    run_id: 'run-runner-test',
    artifact: RESULT_FILE,
  });
  const artifact = JSON.parse(await fs.promises.readFile(path.join(output, RESULT_FILE), 'utf8'));
  assert.equal(artifact.run_id, 'run-runner-test');
  assert.equal(artifact.outcome, 'PASS');
  assert.equal(artifact.cases.length, 4);
  assert.ok(artifact.cases.every((entry) => entry.outcome === 'PASS'));
  assert.ok(artifact.cases.every((entry) =>
    Object.keys(entry).toSorted().join(',') === 'case_id,cleanup_failures,outcome,primary'));
});

for (const outcome of ['FAIL', 'INCOMPLETE', 'ERROR']) {
  test(`runner preserves ${outcome} without serializing arbitrary external evidence`, async (t) => {
    const output = await outputDirectory(t);
    const manifest = loadManifest(manifestPath, { outputDirectory: output });
    const secret = 'SECRET_RUNNER_PAYLOAD';
    const fakeResult = {
      outcome,
      primary: {
        outcome,
        reason: { code: 'SAFE_CODE', stage: 'runner', message: secret, token: secret },
      },
      cases: [{
        identity: { case_id: 'case-safe' },
        outcome,
        primary: {
          outcome,
          reason: { code: 'SAFE_CODE', stage: 'case', message: secret },
        },
        cleanup_failures: [{ step: 'close', code: 'CLEANUP_FAILED', detail: secret }],
      }],
    };

    const result = await executeAcceptanceRun({
      manifest,
      clock: () => 0,
      randomUUID: () => 'safe',
      labFactory: async () => ({}),
      runController: async () => fakeResult,
    });

    assert.equal(result.outcome, outcome);
    const text = await fs.promises.readFile(path.join(output, RESULT_FILE), 'utf8');
    assert.equal(text.includes(secret), false);
    const artifact = JSON.parse(text);
    assert.equal(artifact.outcome, outcome);
    assert.deepEqual(artifact.primary, { outcome, code: 'SAFE_CODE', stage: 'runner' });
  });
}

test('runner refuses to overwrite an existing result artifact', async (t) => {
  const output = await outputDirectory(t);
  const manifest = loadManifest(manifestPath, { outputDirectory: output });
  await fs.promises.writeFile(path.join(output, RESULT_FILE), 'owned', 'utf8');

  await assert.rejects(
    executeAcceptanceRun({
      manifest,
      clock: () => 0,
      randomUUID: () => 'collision',
      labFactory: async () => ({}),
      runController: async () => ({ outcome: 'PASS', cases: [], primary: null }),
    }),
    { code: 'RUN_ARTIFACT_EXISTS', outcome: 'ERROR' }
  );
  assert.equal(await fs.promises.readFile(path.join(output, RESULT_FILE), 'utf8'), 'owned');
});
