'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { createContractValidator, validateContract } = require('../src/contracts');
const { LIMITS } = require('../src/constants');
const { createRedactor } = require('../src/redaction');
const {
  createRunReport,
  addCaseResult,
  finalizeRunReport,
  writeRunArtifacts,
} = require('../src/report');

const validator = createContractValidator(path.join(__dirname, '..', 'schemas'));
const GOLDEN_DIRECTORY = path.join(__dirname, 'golden');
const OUTPUT_FILES = Object.freeze(['run.json', 'summary.md', 'junit.xml']);
const PUBLICATION_LOCK = '.report-publish.lock';
const COMPLETION_MARKER = '.report-complete.json';
const SECRET = 'turn:"secret +/?';

function caseResult(overrides = {}) {
  return {
    case_id: 'case-001',
    outcome: 'FAIL',
    started_at: '2026-08-25T08:00:01Z',
    finished_at: '2026-08-25T08:00:09Z',
    evidence_id: 'evidence-001',
    browser: {
      requested: { browserName: 'firefox', platformName: 'linux' },
      observed: { browserName: 'firefox', browserVersion: '128.0' },
    },
    candidate_types: {
      publisher_local: ['relay'],
      viewer_local: ['relay'],
    },
    selected_pair: {
      local_candidate_type: 'relay',
      remote_candidate_type: 'relay',
      protocol: 'udp',
    },
    phase_timestamps: {
      connecting_at: '2026-08-25T08:00:02Z',
      draining_at: '2026-08-25T08:00:08Z',
      stable_at: '2026-08-25T08:00:04Z',
    },
    thresholds: {
      inputs: { max_rtt_ms: 40, minimum_samples: 2 },
      results: {
        outcome: 'FAIL',
        summaries: { rtt_ms: { sample_count: 2, missing_count: 0, min: 20, max: 41, p50: 20, p95: 41, p99: 41 } },
        failures: [{ metric: 'rtt_ms', statistic: 'p95', threshold: 40, value: 41 }],
        incomplete: [],
        errors: [],
      },
    },
    stale_event_count: 2,
    primary_evidence: {
      code: 'THRESHOLD_EXCEEDED',
      message: `raw=${SECRET}; json=${JSON.stringify(SECRET).slice(1, -1)}; url=${encodeURIComponent(SECRET)}`,
    },
    cleanup_evidence: [{ step: 'turn-allocation-check', outcome: 'PASS' }],
    artifact_hashes: [{ path: 'cases/case-001.json', size_bytes: 512, sha256: 'a'.repeat(64) }],
    ...overrides,
  };
}

function completedReport(runId = 'run-20260825-001') {
  const report = createRunReport(
    {
      run_id: runId,
      started_at: '2026-08-25T08:00:00Z',
      manifest_hash: 'b'.repeat(64),
    },
    { case_count: 2, manifest_sha256: 'b'.repeat(64), profile: 'diagnostic' },
    { coturn_version: '4.6.3', node_version: 'v22.18.0' }
  );
  addCaseResult(report, caseResult());
  addCaseResult(report, caseResult({
    case_id: 'case-002',
    evidence_id: 'evidence-002',
    outcome: 'PASS',
    primary_evidence: null,
    stale_event_count: 0,
    thresholds: {
      inputs: { max_rtt_ms: 40, minimum_samples: 2 },
      results: {
        outcome: 'PASS',
        summaries: { rtt_ms: { sample_count: 2, missing_count: 0, min: 18, max: 22, p50: 18, p95: 22, p99: 22 } },
        failures: [],
        incomplete: [],
        errors: [],
      },
    },
    artifact_hashes: [{ path: 'cases/case-002.json', size_bytes: 384, sha256: 'c'.repeat(64) }],
  }));
  return finalizeRunReport(report);
}

async function temporaryDirectory(t, prefix) {
  const directory = await fs.promises.mkdtemp(path.join(os.tmpdir(), prefix));
  t.after(() => fs.promises.rm(directory, { recursive: true, force: true }));
  return directory;
}

async function readOutputs(directory) {
  return Promise.all(OUTPUT_FILES.map((fileName) => fs.promises.readFile(path.join(directory, fileName))));
}

test('finalization freezes one complete model and preserves metric results without recomputing them', () => {
  const report = completedReport();

  assert.equal(Object.isFrozen(report), true);
  assert.equal(Object.isFrozen(report.cases[0].thresholds.results), true);
  assert.deepEqual(report.counts, { total: 2, PASS: 1, FAIL: 1, INCOMPLETE: 0, ERROR: 0 });
  assert.equal(report.outcome, 'FAIL');
  assert.equal(report.finished_at, '2026-08-25T08:00:09Z');
  assert.equal(report.cases[0].thresholds.results.summaries.rtt_ms.p95, 41);
  assert.throws(() => addCaseResult(report, caseResult({ case_id: 'case-003' })), /finalized/i);
});

test('report aggregation uses the controller outcome priority ERROR > INCOMPLETE > FAIL > PASS', () => {
  const report = createRunReport(
    { run_id: 'run-priority', started_at: '2026-08-25T08:00:00Z' },
    { case_count: 2 },
    { kind: 'contract_lab', release_eligible: false }
  );
  addCaseResult(report, caseResult({ case_id: 'case-fail', evidence_id: 'evidence-fail', outcome: 'FAIL' }));
  addCaseResult(report, caseResult({
    case_id: 'case-incomplete',
    evidence_id: 'evidence-incomplete',
    outcome: 'INCOMPLETE',
    thresholds: {
      inputs: {},
      results: { outcome: 'INCOMPLETE' },
    },
  }));
  assert.equal(finalizeRunReport(report).outcome, 'INCOMPLETE');
});

test('report schema forbids release eligibility for contract lab evidence', () => {
  const report = JSON.parse(JSON.stringify(completedReport('run-contract-lab-schema')));
  report.environment = { kind: 'contract_lab', release_eligible: true };
  const validation = validateContract(validator, 'report', report);
  assert.equal(validation.valid, false);
  assert.ok(validation.errors.some((entry) =>
    entry.instancePath === '/environment/release_eligible' || entry.keyword === 'const'));
});

test('three formats match golden bytes, remain schema-valid, and agree on outcome and case counts', async (t) => {
  const firstDirectory = await temporaryDirectory(t, 'webrtc-report-first-');
  const secondDirectory = await temporaryDirectory(t, 'webrtc-report-second-');
  const report = completedReport();
  const redactor = createRedactor([SECRET]);

  const firstInventory = await writeRunArtifacts(report, firstDirectory, redactor);
  const secondInventory = await writeRunArtifacts(report, secondDirectory, redactor);
  const [first, second] = await Promise.all([readOutputs(firstDirectory), readOutputs(secondDirectory)]);

  for (let index = 0; index < OUTPUT_FILES.length; index += 1) {
    const goldenName = `report-v1.${['json', 'md', 'xml'][index]}`;
    const golden = await fs.promises.readFile(path.join(GOLDEN_DIRECTORY, goldenName));
    assert.deepEqual(first[index], second[index], `${OUTPUT_FILES[index]} must be byte-stable`);
    assert.deepEqual(first[index], golden, `${OUTPUT_FILES[index]} must match its golden fixture`);
    assert.equal(first[index].includes(Buffer.from('\r')), false, `${OUTPUT_FILES[index]} must use LF`);
  }

  const canonical = JSON.parse(first[0].toString('utf8'));
  assert.deepEqual(validateContract(validator, 'report', canonical), { valid: true, errors: [] });
  assert.equal(canonical.outcome, 'FAIL');
  assert.equal(canonical.counts.total, 2);
  assert.equal(canonical.cases.length, 2);
  assert.match(first[1].toString('utf8'), /\| Total \| PASS \| FAIL \| INCOMPLETE \| ERROR \|/);
  assert.match(first[1].toString('utf8'), /\| 2 \| 1 \| 1 \| 0 \| 0 \|/);
  assert.match(first[2].toString('utf8'), /tests="2" failures="1" errors="0" skipped="0"/);
  assert.deepEqual(firstInventory, secondInventory);
  assert.equal(firstInventory.artifacts.length, 3);
  assert.ok(firstInventory.artifacts.every((entry) => /^[0-9a-f]{64}$/.test(entry.sha256) && entry.size_bytes > 0));
  for (let index = 0; index < firstInventory.artifacts.length; index += 1) {
    assert.equal(firstInventory.artifacts[index].size_bytes, first[index].length);
    assert.equal(firstInventory.artifacts[index].sha256, crypto.createHash('sha256').update(first[index]).digest('hex'));
  }
  assert.deepEqual(
    JSON.parse(await fs.promises.readFile(path.join(firstDirectory, COMPLETION_MARKER), 'utf8')),
    { schema_version: 1, outcome: 'FAIL', artifacts: firstInventory.artifacts }
  );
});

test('one redactor removes raw JSON-escaped and URL-encoded secret variants before every format is written', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-redaction-');
  await writeRunArtifacts(completedReport(), directory, createRedactor([SECRET]));

  const combined = Buffer.concat(await readOutputs(directory)).toString('utf8');
  const canonical = JSON.parse((await fs.promises.readFile(path.join(directory, 'run.json'))).toString('utf8'));
  assert.equal(
    canonical.cases[0].primary_evidence.message,
    'raw=[REDACTED]; json=[REDACTED]; url=[REDACTED]'
  );
  assert.equal(combined.includes(SECRET), false);
  assert.equal(combined.includes(JSON.stringify(SECRET).slice(1, -1)), false);
  assert.equal(combined.toLowerCase().includes(encodeURIComponent(SECRET).toLowerCase()), false);
  assert.match(combined, /\[REDACTED\]/);
});

test('artifact bytes reaching the per-case cap fail as ERROR without truncation or final files', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-cap-');
  const builder = createRunReport(
    { run_id: 'run-cap', started_at: '2026-08-25T08:00:00Z', finished_at: '2026-08-25T08:00:01Z' },
    { case_count: 1 },
    { node_version: 'v22.18.0' }
  );
  addCaseResult(builder, caseResult({
    artifact_hashes: [{ path: 'cases/case-001.json', size_bytes: LIMITS.MAX_ARTIFACT_BYTES_PER_CASE, sha256: 'd'.repeat(64) }],
  }));

  await assert.rejects(
    writeRunArtifacts(finalizeRunReport(builder), directory, createRedactor([])),
    (error) => error && error.outcome === 'ERROR' && error.code === 'ARTIFACT_LIMIT_REACHED'
  );
  assert.deepEqual(await fs.promises.readdir(directory), []);
});

test('schema failure is ERROR and occurs before official report files are created', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-schema-');

  await assert.rejects(
    writeRunArtifacts(completedReport(), directory, createRedactor(['run_id'])),
    (error) => error && error.outcome === 'ERROR' && error.code === 'REPORT_SCHEMA_INVALID'
  );
  assert.deepEqual(await fs.promises.readdir(directory), []);
});

test('hash verification failure is ERROR and removes staged files before publication', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-hash-');
  const realReadFile = fs.promises.readFile.bind(fs.promises);
  let corrupted = false;
  t.mock.method(fs.promises, 'readFile', async (...arguments_) => {
    const bytes = await realReadFile(...arguments_);
    if (!corrupted && String(arguments_[0]).includes('.tmp-')) {
      corrupted = true;
      const changed = Buffer.from(bytes);
      changed[0] ^= 1;
      return changed;
    }
    return bytes;
  });

  await assert.rejects(
    writeRunArtifacts(completedReport(), directory, createRedactor([SECRET])),
    (error) => error && error.outcome === 'ERROR' && error.code === 'ARTIFACT_HASH_MISMATCH'
  );
  assert.equal(corrupted, true);
  assert.deepEqual(await fs.promises.readdir(directory), []);
});

test('concurrent writers use one exclusive publication owner and never clobber the winner', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-concurrent-');
  const realMkdir = fs.promises.mkdir.bind(fs.promises);
  let arrivals = 0;
  let releaseBoth;
  const bothArrived = new Promise((resolve) => { releaseBoth = resolve; });
  t.mock.method(fs.promises, 'mkdir', async (...arguments_) => {
    const result = await realMkdir(...arguments_);
    if (path.resolve(arguments_[0]) === path.resolve(directory)) {
      arrivals += 1;
      if (arrivals === 2) releaseBoth();
      await bothArrived;
    }
    return result;
  });

  const results = await Promise.allSettled([
    writeRunArtifacts(completedReport('run-concurrent-winner'), directory, createRedactor([SECRET])),
    writeRunArtifacts(completedReport('run-concurrent-loser'), directory, createRedactor([SECRET])),
  ]);

  assert.equal(results.filter((entry) => entry.status === 'fulfilled').length, 1);
  const rejected = results.find((entry) => entry.status === 'rejected');
  assert.equal(rejected.reason.outcome, 'ERROR');
  assert.equal(rejected.reason.code, 'REPORT_PUBLICATION_BUSY');
  const canonical = JSON.parse(await fs.promises.readFile(path.join(directory, 'run.json'), 'utf8'));
  assert.match(canonical.run_id, /^run-concurrent-(winner|loser)$/);
  assert.equal(await fs.promises.readFile(path.join(directory, COMPLETION_MARKER), 'utf8').then(JSON.parse).then((value) => value.schema_version), 1);
  assert.equal((await fs.promises.readdir(directory)).includes(PUBLICATION_LOCK), false);
});

test('a stale publication marker fails safe and is never stolen', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-stale-lock-');
  const lockPath = path.join(directory, PUBLICATION_LOCK);
  await fs.promises.writeFile(lockPath, 'stale-owner', 'utf8');

  await assert.rejects(
    writeRunArtifacts(completedReport(), directory, createRedactor([SECRET])),
    (error) => error && error.outcome === 'ERROR' && error.code === 'REPORT_PUBLICATION_BUSY'
  );
  assert.equal(await fs.promises.readFile(lockPath, 'utf8'), 'stale-owner');
  assert.deepEqual(await fs.promises.readdir(directory), [PUBLICATION_LOCK]);
});

test('rollback cleanup failure preserves primary error and leaves no completeness marker', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-cleanup-failure-');
  const realRename = fs.promises.rename.bind(fs.promises);
  const realUnlink = fs.promises.unlink.bind(fs.promises);
  let renameCount = 0;
  t.mock.method(fs.promises, 'rename', async (...arguments_) => {
    renameCount += 1;
    if (renameCount === 2) {
      const error = new Error('injected rename failure');
      error.code = 'EIO';
      throw error;
    }
    return realRename(...arguments_);
  });
  t.mock.method(fs.promises, 'unlink', async (...arguments_) => {
    if (path.basename(arguments_[0]) === 'run.json') {
      const error = new Error('injected rollback failure');
      error.code = 'EACCES';
      throw error;
    }
    return realUnlink(...arguments_);
  });

  let failure;
  try {
    await writeRunArtifacts(completedReport(), directory, createRedactor([SECRET]));
  } catch (error) {
    failure = error;
  }
  assert.equal(failure.code, 'ARTIFACT_WRITE_FAILED');
  assert.equal(failure.outcome, 'ERROR');
  assert.deepEqual(failure.cleanup_evidence, [{
    operation: 'unlink',
    artifact: 'run.json',
    code: 'EACCES',
  }]);
  const entries = await fs.promises.readdir(directory);
  assert.deepEqual(entries, ['run.json']);
  assert.equal(entries.includes(COMPLETION_MARKER), false);
  assert.equal(entries.includes(PUBLICATION_LOCK), false);
});

test('JUnit rejects XML 1.0 forbidden control characters before publication', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-invalid-xml-');
  const report = createRunReport(
    { run_id: 'run-invalid-xml', started_at: '2026-08-25T08:00:00Z' },
    { case_count: 1 },
    { node_version: 'v22.18.0' }
  );
  addCaseResult(report, caseResult({ case_id: 'case-\u0000invalid' }));

  await assert.rejects(
    writeRunArtifacts(finalizeRunReport(report), directory, createRedactor([])),
    (error) => error && error.outcome === 'ERROR' && error.code === 'XML_CHARACTER_INVALID'
  );
  assert.deepEqual(await fs.promises.readdir(directory), []);
});

test('JUnit preserves attribute TAB LF and CR with XML character references', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-xml-whitespace-');
  const report = createRunReport(
    { run_id: 'run-xml-whitespace', started_at: '2026-08-25T08:00:00Z' },
    { case_count: 1 },
    { node_version: 'v22.18.0' }
  );
  addCaseResult(report, caseResult({ case_id: 'case-\tline\nreturn\rfinish' }));

  await writeRunArtifacts(finalizeRunReport(report), directory, createRedactor([]));
  const xml = await fs.promises.readFile(path.join(directory, 'junit.xml'), 'utf8');
  assert.match(xml, /name="case-&#x9;line&#xA;return&#xD;finish"/);
  assert.equal(xml.includes('\r'), false);
});

test('a partial publish failure removes already-renamed reports and all unique temporary files', async (t) => {
  const directory = await temporaryDirectory(t, 'webrtc-report-atomic-');
  const realRename = fs.promises.rename.bind(fs.promises);
  let renameCount = 0;
  t.mock.method(fs.promises, 'rename', async (...arguments_) => {
    renameCount += 1;
    if (renameCount === 2) {
      const error = new Error('injected rename failure');
      error.code = 'EIO';
      throw error;
    }
    return realRename(...arguments_);
  });

  await assert.rejects(
    writeRunArtifacts(completedReport(), directory, createRedactor([SECRET])),
    (error) => error && error.outcome === 'ERROR' && error.code === 'ARTIFACT_WRITE_FAILED'
  );

  const entries = await fs.promises.readdir(directory);
  assert.deepEqual(entries, []);
  assert.equal(renameCount, 2);
  assert.equal(entries.some((entry) => entry.includes('.tmp-')), false);
  await assert.rejects(fs.promises.access(path.join(directory, 'run.json')));
  await assert.rejects(fs.promises.access(path.join(directory, 'junit.xml')));
});
