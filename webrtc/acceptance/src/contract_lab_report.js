'use strict';

const path = require('node:path');

const { hashCanonical } = require('./canonical_json');
const { expandCases } = require('./manifest');
const { createRedactor } = require('./redaction');
const {
  createRunReport,
  addCaseResult,
  finalizeRunReport,
  writeRunArtifacts,
} = require('./report');

class ContractLabReportError extends Error {
  constructor(code) {
    super(code);
    this.name = 'ContractLabReportError';
    this.code = code;
    this.outcome = 'ERROR';
  }
}

async function writeContractLabReport(options = {}) {
  const { runIdentity, manifest, controllerResult } = options;
  if (!runIdentity || !manifest || !controllerResult ||
      manifest.profile !== 'contract_lab' ||
      typeof runIdentity.run_id !== 'string' ||
      !['PASS', 'FAIL', 'INCOMPLETE', 'ERROR'].includes(controllerResult.outcome)) {
    throw new ContractLabReportError('CONTRACT_LAB_REPORT_INPUT_INVALID');
  }

  const finishedAt = options.finishedAt ?? new Date().toISOString();
  const cases = expandCases(manifest);
  const byOrdinal = new Map(cases.map((entry, ordinal) => [ordinal, entry]));
  const report = createRunReport(
    { ...runIdentity, finished_at: finishedAt },
    {
      schema_version: manifest.schema_version,
      profile: manifest.profile,
      case_count: cases.length,
      manifest_sha256: manifestHash(manifest),
      limits: manifest.limits ? { ...manifest.limits } : {},
    },
    {
      kind: 'contract_lab',
      release_eligible: false,
      node_version: process.version,
      platform: process.platform,
    }
  );

  for (const result of controllerResult.cases || []) {
    const definition = byOrdinal.get(result.identity && result.identity.ordinal);
    if (!definition) throw new ContractLabReportError('CONTRACT_LAB_CASE_IDENTITY_INVALID');
    addCaseResult(report, normalizeCase(result, definition, runIdentity.started_at, finishedAt, manifest));
  }

  const final = finalizeRunReport(report);
  if (final.outcome !== controllerResult.outcome) {
    throw new ContractLabReportError('CONTRACT_LAB_OUTCOME_MISMATCH');
  }

  const outputDirectory = options.outputDirectory ??
    path.join(manifest.artifact_directory, 'report');
  const inventory = await writeRunArtifacts(final, outputDirectory, createRedactor([]));
  return Object.freeze({ report: final, inventory, outputDirectory });
}

function normalizeCase(result, definition, startedAt, finishedAt, manifest) {
  const primary = result.primary && result.primary.reason && typeof result.primary.reason === 'object'
    ? {
      code: safeText(result.primary.reason.code, 'UNKNOWN'),
      stage: safeText(result.primary.reason.stage, 'controller'),
    }
    : null;
  const cleanup = Array.isArray(result.cleanup_failures)
    ? result.cleanup_failures.map((entry) => ({
      step: safeText(entry && entry.step, 'cleanup'),
      code: safeText(entry && entry.code, 'CLEANUP_FAILED'),
    }))
    : [];

  const browserIce = result.browser_ice || {};
  return {
    case_id: result.identity.case_id,
    outcome: result.outcome,
    started_at: startedAt,
    finished_at: finishedAt,
    evidence_id: result.identity.case_id,
    browser: {
      requested: { ...definition.browser },
      observed: { ...definition.browser },
    },
    candidate_types: {
      publisher_local: ['relay'],
      viewer_local: ['relay'],
      remote_allowed: [...definition.relay_contract.remote_candidate_types],
    },
    selected_pair: {
      topology_generation: result.generation,
      browser_ice_generation: result.browser_ice_generation,
      publisher_pair_ids: browserIce.publisher ? [...browserIce.publisher.pair_ids] : [],
      viewer_pair_ids: browserIce.viewer ? [...browserIce.viewer.pair_ids] : [],
    },
    phase_timestamps: {},
    thresholds: {
      inputs: {
        stable: { ...manifest.threshold_profile.stable },
        recovery: { ...manifest.threshold_profile.recovery },
      },
      results: {
        outcome: result.outcome,
        stable: result.stable_metrics,
        recovery: result.recovery_metrics,
      },
    },
    stale_event_count: result.stale_event_count,
    primary_evidence: primary,
    cleanup_evidence: cleanup,
    artifact_hashes: [],
  };
}

function manifestHash(manifest) {
  const value = {};
  for (const key of Object.keys(manifest)) {
    if (key !== 'runtime') value[key] = manifest[key];
  }
  return hashCanonical(value);
}

function safeText(value, fallback) {
  return typeof value === 'string' && /^[A-Za-z0-9_.:-]{1,128}$/.test(value)
    ? value
    : fallback;
}

module.exports = { ContractLabReportError, writeContractLabReport };
