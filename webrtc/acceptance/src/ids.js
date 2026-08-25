'use strict';

const { hashCanonical } = require('./canonical_json');

function createRunIdentity(clock, randomUUID, manifestHash) {
  if (typeof clock !== 'function' || typeof randomUUID !== 'function') {
    throw new TypeError('clock and randomUUID must be functions');
  }
  if (typeof manifestHash !== 'string' || !/^[0-9a-f]{64}$/.test(manifestHash)) {
    throw new TypeError('manifestHash must be a lowercase SHA-256 hash');
  }
  const startedAt = new Date(clock());
  if (Number.isNaN(startedAt.getTime())) {
    throw new TypeError('clock must return a valid timestamp');
  }
  const uuid = randomUUID();
  if (typeof uuid !== 'string' || uuid.length === 0) {
    throw new TypeError('randomUUID must return a non-empty string');
  }
  return Object.freeze({
    run_id: `run-${uuid}`,
    manifest_hash: manifestHash,
    started_at: startedAt.toISOString(),
  });
}

function createCaseIdentity(runIdentity, caseDefinition, ordinal) {
  if (!runIdentity || typeof runIdentity.run_id !== 'string' || runIdentity.run_id.length === 0) {
    throw new TypeError('runIdentity must contain run_id');
  }
  if (!caseDefinition || typeof caseDefinition.case_key !== 'string' || caseDefinition.case_key.length === 0) {
    throw new TypeError('caseDefinition must contain case_key');
  }
  if (!Number.isInteger(ordinal) || ordinal < 0) {
    throw new RangeError('ordinal must be a non-negative integer');
  }
  const caseIdHash = hashCanonical({
    case_key: caseDefinition.case_key,
    ordinal,
    run_id: runIdentity.run_id,
  });
  return Object.freeze({
    run_id: runIdentity.run_id,
    case_id: `case-${caseIdHash.slice(0, 16)}`,
    case_key: caseDefinition.case_key,
    ordinal,
  });
}

module.exports = { createRunIdentity, createCaseIdentity };
