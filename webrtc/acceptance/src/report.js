'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const { canonicalStringify } = require('./canonical_json');
const { LIMITS } = require('./constants');
const { createContractValidator, validateContract } = require('./contracts');

const SCHEMA_DIRECTORY = path.join(__dirname, '..', 'schemas');
const OUTCOMES = Object.freeze(['PASS', 'FAIL', 'INCOMPLETE', 'ERROR']);
const OUTCOME_PRIORITY = Object.freeze({ PASS: 0, INCOMPLETE: 1, FAIL: 2, ERROR: 3 });
const OUTPUTS = Object.freeze([
  Object.freeze({ name: 'run.json', render: renderCanonicalJson }),
  Object.freeze({ name: 'summary.md', render: renderMarkdown }),
  Object.freeze({ name: 'junit.xml', render: renderJunit }),
]);
const PUBLICATION_LOCK = '.report-publish.lock';
const COMPLETION_MARKER = '.report-complete.json';
const MAX_CLEANUP_EVIDENCE = 8;
const FINAL_REPORTS = new WeakSet();
let contractValidator = null;

class ReportError extends Error {
  constructor(code, message) {
    super(`${code}: ${message}`);
    this.name = 'ReportError';
    this.code = code;
    this.outcome = 'ERROR';
  }
}

function createRunReport(runIdentity, manifestSummary, environment) {
  const identity = copyPlainData(runIdentity, 'runIdentity');
  requireNonEmptyString(identity.run_id, 'runIdentity.run_id');
  requireNonEmptyString(identity.started_at, 'runIdentity.started_at');
  if (identity.finished_at !== undefined) {
    requireNonEmptyString(identity.finished_at, 'runIdentity.finished_at');
  }

  return {
    schema_version: 1,
    run_id: identity.run_id,
    outcome: null,
    started_at: identity.started_at,
    finished_at: identity.finished_at ?? null,
    manifest_summary: requirePlainObjectCopy(manifestSummary, 'manifestSummary'),
    environment: requirePlainObjectCopy(environment, 'environment'),
    counts: emptyCounts(),
    cases: [],
  };
}

function addCaseResult(report, caseResult) {
  assertMutableReport(report);
  if (report.cases.length >= LIMITS.MAX_CASES) {
    throw new ReportError('CASE_LIMIT_EXCEEDED', `report case limit is ${LIMITS.MAX_CASES}`);
  }

  const copied = normalizeCaseResult(caseResult);
  if (report.cases.some((entry) => entry.case_id === copied.case_id)) {
    throw new ReportError('DUPLICATE_CASE_ID', 'case_id must be unique within a report');
  }
  report.cases.push(copied);
  return report;
}

function finalizeRunReport(report) {
  assertMutableReport(report);
  if (report.finished_at === null) {
    if (report.cases.length === 0) {
      throw new ReportError('REPORT_FINISHED_AT_MISSING', 'an empty report requires an explicit finished_at');
    }
    report.finished_at = report.cases.reduce(
      (latest, caseResult) => caseResult.finished_at > latest ? caseResult.finished_at : latest,
      report.cases[0].finished_at
    );
  }
  const counts = emptyCounts();
  let outcome = 'PASS';
  for (const caseResult of report.cases) {
    counts.total += 1;
    counts[caseResult.outcome] += 1;
    if (OUTCOME_PRIORITY[caseResult.outcome] > OUTCOME_PRIORITY[outcome]) {
      outcome = caseResult.outcome;
    }
  }
  report.counts = counts;
  report.outcome = outcome;
  deepFreeze(report);
  FINAL_REPORTS.add(report);
  return report;
}

async function writeRunArtifacts(report, outputDirectory, redactor) {
  assertFinalReport(report);
  if (typeof outputDirectory !== 'string' || outputDirectory.length === 0) {
    throw new ReportError('OUTPUT_DIRECTORY_INVALID', 'outputDirectory must be a non-empty string');
  }
  if (typeof redactor !== 'function') {
    throw new ReportError('REDACTOR_INVALID', 'redactor must be a function');
  }

  let redactedReport;
  try {
    redactedReport = redactValue(report, redactor);
    canonicalStringify(redactedReport);
    deepFreeze(redactedReport);
  } catch {
    throw new ReportError('REDACTION_FAILED', 'report redaction did not produce valid canonical data');
  }

  validateReport(redactedReport);
  enforceArtifactCaps(redactedReport);

  let rendered;
  try {
    rendered = OUTPUTS.map((output) => Object.freeze({
      name: output.name,
      bytes: Buffer.from(output.render(redactedReport), 'utf8'),
    }));
  } catch (error) {
    if (error instanceof ReportError) throw error;
    throw new ReportError('REPORT_RENDER_FAILED', 'a report format could not be rendered');
  }
  if (rendered.some((entry) => entry.bytes.includes(13))) {
    throw new ReportError('REPORT_RENDER_FAILED', 'report formats must use LF line endings');
  }

  return writeArtifactGroup(rendered, path.resolve(outputDirectory), redactedReport.outcome);
}

function redactValue(value, redactor) {
  if (typeof value === 'string') {
    const redacted = redactor(value);
    if (typeof redacted !== 'string') throw new TypeError('redactor result must be a string');
    return redacted;
  }
  if (value === null || typeof value === 'boolean' || typeof value === 'number') return value;
  if (Array.isArray(value)) return value.map((entry) => redactValue(entry, redactor));

  const redacted = Object.create(null);
  for (const [key, entry] of Object.entries(value)) {
    const redactedKey = redactor(key);
    if (typeof redactedKey !== 'string') throw new TypeError('redactor result must be a string');
    if (Object.hasOwn(redacted, redactedKey)) throw new TypeError('redaction produced duplicate keys');
    redacted[redactedKey] = redactValue(entry, redactor);
  }
  return redacted;
}

function normalizeCaseResult(caseResult) {
  const copied = requirePlainObjectCopy(caseResult, 'caseResult');
  requireNonEmptyString(copied.case_id, 'caseResult.case_id');
  requireOutcome(copied.outcome, 'caseResult.outcome');
  requireNonEmptyString(copied.started_at, 'caseResult.started_at');
  requireNonEmptyString(copied.finished_at, 'caseResult.finished_at');
  requireNonEmptyString(copied.evidence_id, 'caseResult.evidence_id');
  requireBrowser(copied.browser);
  requirePlainObject(copied.candidate_types, 'caseResult.candidate_types');
  requirePlainObject(copied.selected_pair, 'caseResult.selected_pair');
  requireTimestampMap(copied.phase_timestamps);
  requireThresholds(copied.thresholds);
  if (!Number.isSafeInteger(copied.stale_event_count) || copied.stale_event_count < 0) {
    throw new TypeError('caseResult.stale_event_count must be a non-negative safe integer');
  }
  if (copied.primary_evidence !== null &&
      (typeof copied.primary_evidence !== 'string' && !isPlainObject(copied.primary_evidence))) {
    throw new TypeError('caseResult.primary_evidence must be null, a string, or an object');
  }
  if (!Array.isArray(copied.cleanup_evidence)) {
    throw new TypeError('caseResult.cleanup_evidence must be an array');
  }
  requireArtifactHashes(copied.artifact_hashes);
  return copied;
}

function requireBrowser(browser) {
  requirePlainObject(browser, 'caseResult.browser');
  requirePlainObject(browser.requested, 'caseResult.browser.requested');
  requirePlainObject(browser.observed, 'caseResult.browser.observed');
}

function requireTimestampMap(timestamps) {
  requirePlainObject(timestamps, 'caseResult.phase_timestamps');
  for (const [name, value] of Object.entries(timestamps)) {
    requireNonEmptyString(name, 'caseResult.phase_timestamps key');
    requireNonEmptyString(value, `caseResult.phase_timestamps.${name}`);
  }
}

function requireThresholds(thresholds) {
  requirePlainObject(thresholds, 'caseResult.thresholds');
  requirePlainObject(thresholds.inputs, 'caseResult.thresholds.inputs');
  requirePlainObject(thresholds.results, 'caseResult.thresholds.results');
  requireOutcome(thresholds.results.outcome, 'caseResult.thresholds.results.outcome');
}

function requireArtifactHashes(artifactHashes) {
  if (!Array.isArray(artifactHashes)) {
    throw new TypeError('caseResult.artifact_hashes must be an array');
  }
  const paths = new Set();
  for (const entry of artifactHashes) {
    requirePlainObject(entry, 'caseResult.artifact_hashes entry');
    requireNonEmptyString(entry.path, 'caseResult.artifact_hashes path');
    if (paths.has(entry.path)) {
      throw new ReportError('DUPLICATE_ARTIFACT_PATH', 'artifact paths must be unique within a case');
    }
    paths.add(entry.path);
    if (!Number.isSafeInteger(entry.size_bytes) || entry.size_bytes < 0) {
      throw new TypeError('caseResult.artifact_hashes size_bytes must be a non-negative safe integer');
    }
    if (typeof entry.sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(entry.sha256)) {
      throw new ReportError('ARTIFACT_HASH_INVALID', 'artifact sha256 must be lowercase hexadecimal');
    }
  }
}

function validateReport(report) {
  contractValidator ??= createContractValidator(SCHEMA_DIRECTORY);
  const validation = validateContract(contractValidator, 'report', report);
  if (!validation.valid) {
    throw new ReportError('REPORT_SCHEMA_INVALID', 'redacted report does not satisfy report schema v1');
  }
}

function enforceArtifactCaps(report) {
  const cap = artifactCap(report.manifest_summary);
  for (const caseResult of report.cases) {
    let declaredBytes = 0;
    for (const artifact of caseResult.artifact_hashes) {
      if (artifact.size_bytes >= cap || declaredBytes >= cap - artifact.size_bytes) {
        throw new ReportError('ARTIFACT_LIMIT_REACHED', `case artifact bytes must remain below ${cap}`);
      }
      declaredBytes += artifact.size_bytes;
    }
    if (Buffer.byteLength(canonicalStringify(caseResult), 'utf8') >= cap) {
      throw new ReportError('ARTIFACT_LIMIT_REACHED', `serialized case bytes must remain below ${cap}`);
    }
  }
}

function artifactCap(manifestSummary) {
  const nested = isPlainObject(manifestSummary.limits)
    ? manifestSummary.limits.max_artifact_bytes_per_case
    : undefined;
  const value = manifestSummary.max_artifact_bytes_per_case ?? nested ?? LIMITS.MAX_ARTIFACT_BYTES_PER_CASE;
  if (!Number.isSafeInteger(value) || value < 1 || value > LIMITS.MAX_ARTIFACT_BYTES_PER_CASE) {
    throw new ReportError('ARTIFACT_LIMIT_INVALID', 'manifest artifact byte cap is outside the contract');
  }
  return value;
}

function renderCanonicalJson(report) {
  return `${canonicalStringify(report)}\n`;
}

function renderMarkdown(report) {
  const lines = [
    '# WebRTC Acceptance Report',
    '',
    `- Run ID: ${markdownInline(report.run_id)}`,
    `- Outcome: ${markdownInline(report.outcome)}`,
    `- Started: ${markdownInline(report.started_at)}`,
    `- Finished: ${markdownInline(report.finished_at)}`,
    '',
    '## Counts',
    '',
    '| Total | PASS | FAIL | INCOMPLETE | ERROR |',
    '| ---: | ---: | ---: | ---: | ---: |',
    `| ${report.counts.total} | ${report.counts.PASS} | ${report.counts.FAIL} | ${report.counts.INCOMPLETE} | ${report.counts.ERROR} |`,
    '',
    '## Cases',
    '',
    '| Case | Outcome | Requested browser | Observed browser | Candidate types | Selected pair | Phase timestamps | Threshold inputs/results | Stale events | Primary evidence | Cleanup evidence | Artifact hashes |',
    '| --- | --- | --- | --- | --- | --- | --- | --- | ---: | --- | --- | --- |',
  ];
  for (const caseResult of report.cases) {
    lines.push(`| ${[
      caseResult.case_id,
      caseResult.outcome,
      canonicalStringify(caseResult.browser.requested),
      canonicalStringify(caseResult.browser.observed),
      canonicalStringify(caseResult.candidate_types),
      canonicalStringify(caseResult.selected_pair),
      canonicalStringify(caseResult.phase_timestamps),
      canonicalStringify(caseResult.thresholds),
      String(caseResult.stale_event_count),
      canonicalStringify(caseResult.primary_evidence),
      canonicalStringify(caseResult.cleanup_evidence),
      canonicalStringify(caseResult.artifact_hashes),
    ].map(markdownCell).join(' | ')} |`);
  }
  return `${lines.join('\n')}\n`;
}

function renderJunit(report) {
  const suiteAttributes = xmlAttributes({
    name: 'webrtc-acceptance',
    tests: report.counts.total,
    failures: report.counts.FAIL,
    errors: report.counts.ERROR,
    skipped: report.counts.INCOMPLETE,
  });
  const lines = ['<?xml version="1.0" encoding="UTF-8"?>', `<testsuite ${suiteAttributes}>`];
  for (const caseResult of report.cases) {
    lines.push(`  <testcase ${xmlAttributes({ classname: 'webrtc.acceptance', name: caseResult.case_id })}>`);
    const evidence = canonicalStringify(caseResult.primary_evidence);
    if (caseResult.outcome === 'FAIL') {
      lines.push(`    <failure message="acceptance assertion failed">${xmlText(evidence)}</failure>`);
    } else if (caseResult.outcome === 'ERROR') {
      lines.push(`    <error message="acceptance harness error">${xmlText(evidence)}</error>`);
    } else if (caseResult.outcome === 'INCOMPLETE') {
      lines.push(`    <skipped message="acceptance evidence incomplete">${xmlText(evidence)}</skipped>`);
    }
    lines.push(`    <system-out>${xmlText(canonicalStringify(caseResult))}</system-out>`);
    lines.push('  </testcase>');
  }
  lines.push('</testsuite>');
  const xml = `${lines.join('\n')}\n`;
  assertXml10Characters(xml);
  return xml;
}

async function writeArtifactGroup(rendered, outputDirectory, outcome) {
  const staged = [];
  const published = [];
  const lockPath = path.join(outputDirectory, PUBLICATION_LOCK);
  const completionPath = path.join(outputDirectory, COMPLETION_MARKER);
  let lockHandle = null;
  let ownsLock = false;
  try {
    await fs.promises.mkdir(outputDirectory, { recursive: true });
    lockHandle = await acquirePublicationLock(lockPath);
    ownsLock = true;
    await lockHandle.sync();
    await assertTargetsAbsent([...rendered, { name: COMPLETION_MARKER }], outputDirectory);
    for (const artifact of rendered) {
      const temporaryPath = path.join(
        outputDirectory,
        `.${artifact.name}.tmp-${process.pid}-${crypto.randomUUID()}`
      );
      staged.push({ ...artifact, temporaryPath, finalPath: path.join(outputDirectory, artifact.name) });
      await writeSyncedFile(temporaryPath, artifact.bytes);
    }

    const stagedInventory = [];
    for (const artifact of staged) {
      const actual = await fs.promises.readFile(artifact.temporaryPath);
      if (!actual.equals(artifact.bytes)) {
        throw new ReportError('ARTIFACT_HASH_MISMATCH', 'staged report bytes differ from rendered bytes');
      }
      stagedInventory.push(inventoryEntry(artifact.name, actual));
    }

    for (const artifact of staged) {
      await fs.promises.rename(artifact.temporaryPath, artifact.finalPath);
      published.push(artifact.finalPath);
    }

    const finalInventory = [];
    for (let index = 0; index < staged.length; index += 1) {
      const actual = await fs.promises.readFile(staged[index].finalPath);
      const entry = inventoryEntry(staged[index].name, actual);
      if (entry.sha256 !== stagedInventory[index].sha256 ||
          entry.size_bytes !== stagedInventory[index].size_bytes) {
        throw new ReportError('ARTIFACT_HASH_MISMATCH', 'published report bytes differ from staged bytes');
      }
      finalInventory.push(entry);
    }

    const result = deepFreeze({ outcome, artifacts: finalInventory });
    const completionBytes = Buffer.from(canonicalStringify({
      schema_version: 1,
      outcome,
      artifacts: finalInventory,
    }) + '\n', 'utf8');
    await lockHandle.writeFile(completionBytes);
    await lockHandle.sync();
    await lockHandle.close();
    lockHandle = null;
    const actualCompletionBytes = await fs.promises.readFile(lockPath);
    if (!actualCompletionBytes.equals(completionBytes)) {
      throw new ReportError('ARTIFACT_HASH_MISMATCH', 'completion marker bytes differ from its artifact inventory');
    }
    await fs.promises.rename(lockPath, completionPath);
    return result;
  } catch (error) {
    const primary = error instanceof ReportError
      ? error
      : new ReportError('ARTIFACT_WRITE_FAILED', 'report artifact group was not committed');
    const cleanupEvidence = await cleanupIncompletePublication({
      lockHandle,
      lockPath,
      ownsLock,
      staged,
      published,
    });
    primary.cleanup_evidence = deepFreeze(cleanupEvidence);
    throw primary;
  }
}

async function acquirePublicationLock(lockPath) {
  try {
    return await fs.promises.open(lockPath, 'wx+', 0o600);
  } catch (error) {
    if (error && error.code === 'EEXIST') {
      throw new ReportError('REPORT_PUBLICATION_BUSY', 'an existing publication marker owns or blocks this output directory');
    }
    throw new ReportError('ARTIFACT_WRITE_FAILED', 'the report publication marker could not be created');
  }
}

async function cleanupIncompletePublication({ lockHandle, lockPath, ownsLock, staged, published }) {
  const evidence = [];
  if (lockHandle) {
    try {
      await lockHandle.close();
    } catch (error) {
      addCleanupEvidence(evidence, 'close', 'publication-lock', error);
    }
  }
  for (const fileName of published) {
    await cleanupPath(fileName, path.basename(fileName), evidence);
  }
  for (const artifact of staged) {
    await cleanupPath(artifact.temporaryPath, `temporary:${artifact.name}`, evidence);
  }
  if (ownsLock) {
    await cleanupPath(lockPath, 'publication-lock', evidence);
  }
  return evidence;
}

async function cleanupPath(fileName, artifact, evidence) {
  try {
    await fs.promises.unlink(fileName);
  } catch (error) {
    if (error && error.code === 'ENOENT') return;
    addCleanupEvidence(evidence, 'unlink', artifact, error);
  }
}

function addCleanupEvidence(evidence, operation, artifact, error) {
  if (evidence.length >= MAX_CLEANUP_EVIDENCE) return;
  const rawCode = error && typeof error.code === 'string' ? error.code : 'UNKNOWN';
  const code = /^[A-Z0-9_]+$/.test(rawCode) ? rawCode.slice(0, 64) : 'UNKNOWN';
  evidence.push({ operation, artifact, code });
}

async function assertTargetsAbsent(rendered, outputDirectory) {
  for (const artifact of rendered) {
    try {
      await fs.promises.lstat(path.join(outputDirectory, artifact.name));
    } catch (error) {
      if (error && error.code === 'ENOENT') continue;
      throw error;
    }
    throw new ReportError('ARTIFACT_TARGET_EXISTS', 'report output directory already contains an official artifact');
  }
}

async function writeSyncedFile(fileName, bytes) {
  let handle;
  let primaryError = null;
  try {
    handle = await fs.promises.open(fileName, 'wx', 0o600);
    await handle.writeFile(bytes);
    await handle.sync();
  } catch (error) {
    primaryError = error;
    throw error;
  } finally {
    if (handle) {
      try {
        await handle.close();
      } catch (closeError) {
        if (!primaryError) throw closeError;
      }
    }
  }
}

function inventoryEntry(fileName, bytes) {
  return {
    path: fileName,
    size_bytes: bytes.length,
    sha256: crypto.createHash('sha256').update(bytes).digest('hex'),
  };
}

function assertMutableReport(report) {
  if (!isPlainObject(report) || !Array.isArray(report.cases)) {
    throw new TypeError('report must be created by createRunReport');
  }
  if (Object.isFrozen(report) || FINAL_REPORTS.has(report)) {
    throw new ReportError('REPORT_FINALIZED', 'report is already finalized');
  }
}

function assertFinalReport(report) {
  if (!isPlainObject(report) || !FINAL_REPORTS.has(report) || !Object.isFrozen(report)) {
    throw new ReportError('REPORT_NOT_FINALIZED', 'report must be finalized before writing');
  }
}

function emptyCounts() {
  return { total: 0, PASS: 0, FAIL: 0, INCOMPLETE: 0, ERROR: 0 };
}

function requireOutcome(value, name) {
  if (!OUTCOMES.includes(value)) throw new TypeError(`${name} must be a supported outcome`);
}

function requireNonEmptyString(value, name) {
  if (typeof value !== 'string' || value.length === 0) throw new TypeError(`${name} must be a non-empty string`);
}

function requirePlainObject(value, name) {
  if (!isPlainObject(value)) throw new TypeError(`${name} must be a plain object`);
  return value;
}

function requirePlainObjectCopy(value, name) {
  const copied = copyPlainData(value, name);
  return requirePlainObject(copied, name);
}

function copyPlainData(value, name) {
  try {
    return JSON.parse(canonicalStringify(value));
  } catch {
    throw new TypeError(`${name} must contain only finite, acyclic plain JSON data`);
  }
}

function isPlainObject(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function markdownInline(value) {
  return `\`${String(value).replaceAll('`', '\\`')}\``;
}

function markdownCell(value) {
  return String(value)
    .replaceAll('\\', '\\\\')
    .replaceAll('|', '\\|')
    .replaceAll('\t', '&#x9;')
    .replaceAll('\n', '&#xA;')
    .replaceAll('\r', '&#xD;');
}

function xmlAttributes(attributes) {
  return Object.entries(attributes)
    .map(([name, value]) => `${name}="${xmlAttribute(String(value))}"`)
    .join(' ');
}

function xmlAttribute(value) {
  const text = String(value);
  assertXml10Characters(text);
  return text.replace(/[&<>"'\t\n\r]/g, (character) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&apos;',
    '\t': '&#x9;',
    '\n': '&#xA;',
    '\r': '&#xD;',
  })[character]);
}

function xmlText(value) {
  const text = String(value);
  assertXml10Characters(text);
  return text
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&apos;');
}

function assertXml10Characters(value) {
  for (let index = 0; index < value.length;) {
    const codePoint = value.codePointAt(index);
    const valid = codePoint === 0x9 || codePoint === 0xA || codePoint === 0xD ||
      codePoint >= 0x20 && codePoint <= 0xD7FF ||
      codePoint >= 0xE000 && codePoint <= 0xFFFD ||
      codePoint >= 0x10000 && codePoint <= 0x10FFFF;
    if (!valid) {
      throw new ReportError('XML_CHARACTER_INVALID', 'JUnit data contains a character forbidden by XML 1.0');
    }
    index += codePoint > 0xFFFF ? 2 : 1;
  }
}

function deepFreeze(value, seen = new Set()) {
  if (!value || typeof value !== 'object' || seen.has(value)) return value;
  seen.add(value);
  for (const key of Object.keys(value)) deepFreeze(value[key], seen);
  return Object.freeze(value);
}

module.exports = {
  createRunReport,
  addCaseResult,
  finalizeRunReport,
  writeRunArtifacts,
};
