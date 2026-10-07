'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const { canonicalStringify, hashCanonical } = require('./canonical_json');
const { createRunIdentity } = require('./ids');
const { runManifest } = require('./controller');

const RESULT_FILE = 'result.json';
const SAFE_TEXT = /^[A-Za-z0-9_.:-]+$/;

class RunnerError extends Error {
  constructor(code) {
    super(code);
    this.name = 'RunnerError';
    this.code = code;
    this.outcome = 'ERROR';
  }
}

async function executeAcceptanceRun(options = {}) {
  if (!options || typeof options !== 'object' || Array.isArray(options) ||
      !options.manifest || typeof options.manifest !== 'object' || Array.isArray(options.manifest)) {
    throw new RunnerError('RUN_OPTIONS_INVALID');
  }

  const manifest = options.manifest;
  const clock = options.clock ?? Date.now;
  const randomUUID = options.randomUUID ?? crypto.randomUUID;
  const runController = options.runController ?? runManifest;
  const labFactory = options.labFactory ?? defaultLabFactory;
  if (typeof clock !== 'function' || typeof randomUUID !== 'function' ||
      typeof runController !== 'function' || typeof labFactory !== 'function') {
    throw new RunnerError('RUN_OPTIONS_INVALID');
  }

  const runIdentity = createRunIdentity(clock, randomUUID, hashManifest(manifest));
  let lab;
  try {
    lab = await labFactory({ manifest, runIdentity });
  } catch {
    throw new RunnerError('RUN_LAB_INIT_FAILED');
  }

  let result;
  try {
    result = await runController({
      lab,
      run_identity: runIdentity,
    }, manifest, options.signal);
  } catch {
    throw new RunnerError('RUN_CONTROLLER_FAILED');
  }

  const outcome = safeOutcome(result && result.outcome);
  const safeResult = Object.freeze({
    schema_version: 1,
    run_id: runIdentity.run_id,
    outcome,
    cases: Object.freeze(Array.isArray(result && result.cases)
      ? result.cases.map(safeCaseResult)
      : []),
    primary: safePrimary(result && result.primary),
  });

  await writeResultArtifact(options.outputDirectory ?? manifest.artifact_directory, safeResult);
  return Object.freeze({
    outcome,
    run_id: runIdentity.run_id,
    artifact: RESULT_FILE,
  });
}

function hashManifest(manifest) {
  const copy = {};
  for (const key of Object.keys(manifest)) {
    if (key === 'runtime') continue;
    copy[key] = manifest[key];
  }
  return hashCanonical(copy);
}

function safeOutcome(value) {
  return ['PASS', 'FAIL', 'INCOMPLETE', 'ERROR'].includes(value) ? value : 'ERROR';
}

function safeCaseResult(value) {
  const identity = value && value.identity;
  const caseId = identity && typeof identity.case_id === 'string' && SAFE_TEXT.test(identity.case_id)
    ? identity.case_id : 'case-unknown';
  return Object.freeze({
    case_id: caseId,
    outcome: safeOutcome(value && value.outcome),
    primary: safePrimary(value && value.primary),
    cleanup_failures: Object.freeze(Array.isArray(value && value.cleanup_failures)
      ? value.cleanup_failures.map(safeCleanupFailure)
      : []),
  });
}

function safePrimary(value) {
  if (!value || typeof value !== 'object') return null;
  const reason = value.reason && typeof value.reason === 'object' ? value.reason : {};
  return Object.freeze({
    outcome: safeOutcome(value.outcome),
    code: safeText(reason.code, 'UNKNOWN'),
    stage: safeText(reason.stage, 'controller'),
  });
}

function safeCleanupFailure(value) {
  return Object.freeze({
    step: safeText(value && value.step, 'cleanup'),
    code: safeText(value && value.code, 'CLEANUP_FAILED'),
  });
}

function safeText(value, fallback) {
  return typeof value === 'string' && value.length <= 128 && SAFE_TEXT.test(value) ? value : fallback;
}

async function writeResultArtifact(outputDirectory, value) {
  if (typeof outputDirectory !== 'string' || outputDirectory.length === 0) {
    throw new RunnerError('RUN_OUTPUT_INVALID');
  }
  const directory = path.resolve(outputDirectory);
  const target = path.join(directory, RESULT_FILE);
  const bytes = Buffer.from(canonicalStringify(value) + '\n', 'utf8');
  let handle;
  let created = false;
  try {
    await fs.promises.mkdir(directory, { recursive: true });
    handle = await fs.promises.open(target, 'wx', 0o600);
    created = true;
    await handle.writeFile(bytes);
    await handle.sync();
    await handle.close();
    handle = null;
  } catch (error) {
    if (handle) {
      try { await handle.close(); } catch {}
    }
    if (created) {
      try { await fs.promises.unlink(target); } catch {}
    }
    if (error && error.code === 'EEXIST') throw new RunnerError('RUN_ARTIFACT_EXISTS');
    throw new RunnerError('RUN_ARTIFACT_WRITE_FAILED');
  }
}

async function defaultLabFactory(options) {
  const { createAcceptanceLab } = require('./runtime_lab');
  return createAcceptanceLab(options);
}

module.exports = {
  RESULT_FILE,
  RunnerError,
  executeAcceptanceRun,
  writeResultArtifact,
};
