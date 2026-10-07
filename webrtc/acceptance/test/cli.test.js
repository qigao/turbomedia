'use strict';

const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');

const cliPath = path.join(__dirname, '..', 'src', 'cli.js');
const fixtureProcess = path.join(__dirname, '..', 'test_support', 'cli_process.js');
const sourceManifest = path.join(__dirname, 'fixtures', 'minimal-manifest.json');

function lines(value) {
  return value.trimEnd().split('\n').filter((line) => line.length > 0);
}

function runNode(script, args, env = {}) {
  return spawnSync(process.execPath, [script, ...args], {
    encoding: 'utf8',
    windowsHide: true,
    shell: false,
    env: { ...process.env, ...env },
  });
}

async function tempWorkspace(t) {
  const root = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'webrtc cli space '));
  t.after(() => fs.promises.rm(root, { recursive: true, force: true }));
  const manifestPath = path.join(root, 'manifest file.json');
  await fs.promises.copyFile(sourceManifest, manifestPath);
  return { root, manifestPath };
}

function parseSingleSummary(result) {
  const stdoutLines = lines(result.stdout);
  assert.equal(stdoutLines.length, 1, `stdout must contain one JSON line: ${result.stdout}`);
  return JSON.parse(stdoutLines[0]);
}

function spawnFixture(args, env = {}) {
  const child = spawn(process.execPath, [fixtureProcess, ...args], {
    windowsHide: true,
    shell: false,
    env: { ...process.env, ...env },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  const completed = new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => resolve({ code, signal, stdout, stderr }));
  });
  return { child, completed };
}

async function waitForTrace(filePath, event, timeoutMs = 2_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    let values = [];
    try {
      const text = await fs.promises.readFile(filePath, 'utf8');
      values = lines(text).map((line) => JSON.parse(line));
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
    }
    if (values.some((entry) => entry.event === event)) return values;
    await delay(10);
  }
  throw new Error(`trace event did not arrive: ${event}`);
}

test('validate accepts a manifest path containing spaces and emits exactly one PASS JSON line', async (t) => {
  const { manifestPath } = await tempWorkspace(t);
  const result = runNode(cliPath, ['validate', '--manifest', manifestPath]);

  assert.equal(result.status, 0);
  assert.deepEqual(parseSingleSummary(result), {
    schema_version: 1,
    command: 'validate',
    outcome: 'PASS',
    exit_code: 0,
  });
  assert.equal(result.stderr, '');
});

test('validate rejects invalid manifests without echoing the path or parser payload', async (t) => {
  const { root } = await tempWorkspace(t);
  const manifestPath = path.join(root, 'SECRET manifest.json');
  await fs.promises.writeFile(manifestPath, '{"secret":"PAYLOAD_MARKER"', 'utf8');

  const result = runNode(cliPath, ['validate', '--manifest', manifestPath]);
  assert.equal(result.status, 4);
  assert.deepEqual(parseSingleSummary(result), {
    schema_version: 1,
    command: 'validate',
    outcome: 'ERROR',
    exit_code: 4,
    code: 'MANIFEST_INVALID',
  });
  assert.match(result.stderr, /MANIFEST_INVALID/);
  assert.equal((result.stdout + result.stderr).includes('SECRET manifest.json'), false);
  assert.equal((result.stdout + result.stderr).includes('PAYLOAD_MARKER'), false);
});

test('production run fails closed as INCOMPLETE before provider side effects when Grid evidence is unavailable', async (t) => {
  const { root } = await tempWorkspace(t);
  const outputDirectory = path.join(root, 'production output');
  const manifestPath = path.join(root, 'production manifest.json');
  const manifest = JSON.parse(await fs.promises.readFile(sourceManifest, 'utf8'));
  manifest.artifact_directory = outputDirectory;
  await fs.promises.writeFile(manifestPath, JSON.stringify(manifest), 'utf8');

  const env = { ...process.env };
  delete env.TURBO_MEDIA_ACCEPTANCE_GRID_URL;
  const result = spawnSync(process.execPath, [cliPath, 'run', '--manifest', manifestPath], {
    encoding: 'utf8',
    windowsHide: true,
    shell: false,
    env,
  });

  assert.equal(result.status, 3);
  const summary = parseSingleSummary(result);
  assert.equal(summary.command, 'run');
  assert.equal(summary.outcome, 'INCOMPLETE');
  assert.equal(summary.exit_code, 3);
  assert.equal(result.stderr, '');
  const artifact = JSON.parse(await fs.promises.readFile(path.join(outputDirectory, 'result.json'), 'utf8'));
  assert.equal(artifact.outcome, 'INCOMPLETE');
  assert.equal(artifact.cases.length, 0);
});

for (const [outcome, exitCode] of [['PASS', 0], ['FAIL', 2], ['INCOMPLETE', 3], ['ERROR', 4]]) {
  test(`run maps ${outcome} to stable exit code ${exitCode}`, async (t) => {
    const { manifestPath } = await tempWorkspace(t);
    const result = runNode(fixtureProcess, ['run', '--manifest', manifestPath], {
      TURBO_CLI_FIXTURE_MODE: 'outcome',
      TURBO_CLI_FIXTURE_OUTCOME: outcome,
    });

    assert.equal(result.status, exitCode);
    assert.deepEqual(parseSingleSummary(result), {
      schema_version: 1,
      command: 'run',
      outcome,
      exit_code: exitCode,
      run_id: 'run-fixture',
    });
    assert.equal(result.stderr, '');
  });
}

for (const args of [
  ['unknown', '--manifest', sourceManifest],
  ['run'],
  ['run', '--manifest'],
  ['run', '--manifest', sourceManifest, '--manifest', sourceManifest],
  ['run', '--manifest', sourceManifest, '--unknown', 'x'],
  ['validate', '--manifest', sourceManifest, '--output', 'x'],
]) {
  test(`invalid CLI shape fails ERROR without guessing: ${args.join(' ')}`, () => {
    const result = runNode(cliPath, args);
    assert.equal(result.status, 4);
    const summary = parseSingleSummary(result);
    assert.equal(summary.outcome, 'ERROR');
    assert.equal(summary.exit_code, 4);
    assert.match(summary.code, /^CLI_/);
    assert.equal(lines(result.stderr).length, 1);
  });
}

test('run only permits output and label runtime overrides and passes them without shell parsing', async (t) => {
  const { root, manifestPath } = await tempWorkspace(t);
  const tracePath = path.join(root, 'trace file.jsonl');
  const outputDirectory = path.join(root, 'output directory');

  const result = runNode(fixtureProcess, [
    'run', '--manifest', manifestPath,
    '--output', outputDirectory,
    '--label', 'nightly turn',
  ], {
    TURBO_CLI_FIXTURE_MODE: 'outcome',
    TURBO_CLI_FIXTURE_OUTCOME: 'PASS',
    TURBO_CLI_FIXTURE_TRACE: tracePath,
  });

  assert.equal(result.status, 0);
  const trace = lines(await fs.promises.readFile(tracePath, 'utf8')).map(JSON.parse);
  assert.equal(trace.length, 1);
  assert.deepEqual(trace[0], {
    event: 'started',
    outputDirectory,
    runLabel: 'nightly turn',
  });
});

test('arbitrary external error metadata is collapsed and never reaches stdout or stderr', async (t) => {
  const { manifestPath } = await tempWorkspace(t);
  const secret = 'SECRET_EXTERNAL_ERROR_TOKEN';
  const result = runNode(fixtureProcess, ['run', '--manifest', manifestPath], {
    TURBO_CLI_FIXTURE_MODE: 'secret_error',
    TURBO_CLI_FIXTURE_SECRET: secret,
  });

  assert.equal(result.status, 4);
  assert.deepEqual(parseSingleSummary(result), {
    schema_version: 1,
    command: 'run',
    outcome: 'ERROR',
    exit_code: 4,
    code: 'RUN_ERROR',
  });
  assert.equal((result.stdout + result.stderr).includes(secret), false);
});

test('first signal aborts once, allows one drain, and exits through the normal ERROR result', async (t) => {
  const { root, manifestPath } = await tempWorkspace(t);
  const tracePath = path.join(root, 'signal trace.jsonl');
  const { completed } = spawnFixture(['run', '--manifest', manifestPath], {
    TURBO_CLI_FIXTURE_MODE: 'signal',
    TURBO_CLI_FIXTURE_TRACE: tracePath,
  });

  const result = await completed;

  assert.equal(result.code, 4);
  assert.equal(result.signal, null);
  const trace = lines(await fs.promises.readFile(tracePath, 'utf8')).map(JSON.parse);
  assert.equal(trace.filter((entry) => entry.event === 'drain').length, 1);
  assert.deepEqual(trace.filter((entry) => entry.event === 'signal_dispatch').map((entry) => entry.signal), ['SIGTERM']);
  const summary = JSON.parse(lines(result.stdout)[0]);
  assert.equal(summary.outcome, 'ERROR');
  assert.equal(lines(result.stdout).length, 1);
  assert.equal(result.stderr, '');
});

test('second signal exits with ERROR, warns that artifacts may be incomplete, and never starts a second drain', async (t) => {
  const { root, manifestPath } = await tempWorkspace(t);
  const tracePath = path.join(root, 'double signal trace.jsonl');
  const { completed } = spawnFixture(['run', '--manifest', manifestPath], {
    TURBO_CLI_FIXTURE_MODE: 'double_signal',
    TURBO_CLI_FIXTURE_TRACE: tracePath,
  });

  const result = await completed;

  assert.equal(result.code, 4);
  assert.equal(result.signal, null);
  const trace = lines(await fs.promises.readFile(tracePath, 'utf8')).map(JSON.parse);
  assert.equal(trace.filter((entry) => entry.event === 'drain').length, 1);
  assert.deepEqual(trace.filter((entry) => entry.event === 'signal_dispatch').map((entry) => entry.signal), ['SIGINT', 'SIGTERM']);
  assert.deepEqual(JSON.parse(lines(result.stdout)[0]), {
    schema_version: 1,
    command: 'run',
    outcome: 'ERROR',
    exit_code: 4,
    code: 'SECOND_SIGNAL',
  });
  assert.equal(lines(result.stdout).length, 1);
  assert.match(result.stderr, /ARTIFACTS_MAY_BE_INCOMPLETE/);
});
