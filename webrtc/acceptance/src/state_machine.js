'use strict';

const CaseState = Object.freeze({
  CREATED: 'CREATED',
  PREPARING: 'PREPARING',
  CONNECTING: 'CONNECTING',
  STABLE: 'STABLE',
  TRANSITIONING: 'TRANSITIONING',
  RECOVERING: 'RECOVERING',
  DRAINING: 'DRAINING',
  PASSED: 'PASSED',
  FAILED: 'FAILED',
  INCOMPLETE: 'INCOMPLETE',
  ERROR: 'ERROR',
});

const Outcome = Object.freeze({
  PASS: 'PASS',
  FAIL: 'FAIL',
  INCOMPLETE: 'INCOMPLETE',
  ERROR: 'ERROR',
});

const TERMINAL_STATES = Object.freeze({
  [CaseState.PASSED]: true,
  [CaseState.FAILED]: true,
  [CaseState.INCOMPLETE]: true,
  [CaseState.ERROR]: true,
});

const FAILURE_OUTCOMES = Object.freeze({
  [Outcome.FAIL]: true,
  [Outcome.INCOMPLETE]: true,
  [Outcome.ERROR]: true,
});

const EVENT_TYPES = Object.freeze({
  start: true,
  preflight_ok: true,
  media_stable: true,
  transition_due: true,
  hook_effective: true,
  recovery_verified: true,
  complete: true,
  timeout: true,
  cancel: true,
  failure: true,
  cleanup_complete: true,
});

const TRANSITIONS = deepFreeze({
  [CaseState.CREATED]: { start: CaseState.PREPARING },
  [CaseState.PREPARING]: { preflight_ok: CaseState.CONNECTING },
  [CaseState.CONNECTING]: { media_stable: CaseState.STABLE },
  [CaseState.STABLE]: {
    transition_due: CaseState.TRANSITIONING,
    complete: CaseState.DRAINING,
  },
  [CaseState.TRANSITIONING]: { hook_effective: CaseState.RECOVERING },
  [CaseState.RECOVERING]: { recovery_verified: CaseState.STABLE },
  [CaseState.DRAINING]: { cleanup_complete: 'FINISH_DRAIN' },
});

function createCaseMachine(identity) {
  const copiedIdentity = normalizeIdentity(identity);
  return freezeState({
    identity: copiedIdentity,
    phase: CaseState.CREATED,
    generation: 0,
    last_sequence: -1,
    stale_event_count: 0,
    primary: null,
    cleanup_failures: [],
    last_receipt: null,
    outcome: null,
  });
}

function stepCase(machine, event) {
  assertMachine(machine);
  if (isTerminal(machine)) {
    throw new Error('INVALID_TRANSITION: terminal case machine cannot accept events');
  }

  const envelope = safelyNormalize(() => inspectEnvelope(machine, event));
  if (!envelope.valid) {
    return enterErrorDrain(machine, envelope.code, envelope.message);
  }

  if (envelope.value.generation < machine.generation) {
    return nextState(machine, { stale_event_count: machine.stale_event_count + 1 });
  }
  if (envelope.value.generation > machine.generation) {
    return enterErrorDrain(machine, 'FUTURE_GENERATION', 'future generation event received');
  }

  let receipt = null;
  if (envelope.value.type === 'hook_effective') {
    receipt = safelyNormalize(() => normalizeReceipt(event, envelope.value));
    if (!receipt.valid) {
      return enterErrorDrain(machine, receipt.code, receipt.message);
    }
    if (isConflictingReceipt(machine, envelope.value.generation, receipt.value)) {
      return enterErrorDrain(machine, 'CONFLICTING_RECEIPT', 'conflicting receipt evidence for the same generation');
    }
    if (isMatchingReceipt(machine, envelope.value.generation, receipt.value)) {
      return nextState(machine, { stale_event_count: machine.stale_event_count + 1 });
    }
  }

  if (envelope.value.sequence <= machine.last_sequence) {
    return nextState(machine, { stale_event_count: machine.stale_event_count + 1 });
  }

  if (envelope.value.type === 'failure' || envelope.value.type === 'timeout' || envelope.value.type === 'cancel') {
    const failure = safelyNormalize(() => normalizeFailure(event, envelope.value));
    if (!failure.valid) {
      return enterErrorDrain(machine, failure.code, failure.message);
    }
    return enterDrain(machine, failure.value.outcome, failure.value.reason, envelope.value.sequence);
  }

  const transitions = Object.hasOwn(TRANSITIONS, machine.phase) ? TRANSITIONS[machine.phase] : null;
  const nextPhase = transitions && Object.hasOwn(transitions, envelope.value.type)
    ? transitions[envelope.value.type]
    : null;
  if (!nextPhase) {
    return enterErrorDrain(machine, 'INVALID_TRANSITION', `event ${envelope.value.type} is invalid in ${machine.phase}`);
  }
  if (envelope.value.type === 'cleanup_complete') {
    const cleanupFailures = safelyNormalize(() => normalizeEventCleanupFailures(event));
    if (!cleanupFailures.valid) {
      return enterErrorDrain(machine, cleanupFailures.code, cleanupFailures.message);
    }
    return nextState(finishDrain(machine, cleanupFailures.value), { last_sequence: envelope.value.sequence });
  }

  const updates = {
    phase: nextPhase,
    last_sequence: envelope.value.sequence,
  };
  if (envelope.value.type === 'transition_due') {
    updates.generation = machine.generation + 1;
  }
  if (envelope.value.type === 'hook_effective') {
    updates.last_receipt = receipt.value;
  }
  if (envelope.value.type === 'complete') {
    updates.primary = null;
  }
  return nextState(machine, updates);
}

function beginDrain(machine, primaryOutcome, reason) {
  assertMachine(machine);
  if (isTerminal(machine)) {
    throw new Error('INVALID_TRANSITION: terminal case machine cannot begin drain');
  }
  if (machine.phase === CaseState.DRAINING) {
    return nextState(machine, {});
  }
  if (primaryOutcome === undefined || primaryOutcome === null || primaryOutcome === Outcome.PASS) {
    return nextState(machine, { phase: CaseState.DRAINING, primary: null });
  }
  if (!Object.hasOwn(FAILURE_OUTCOMES, primaryOutcome)) {
    throw new TypeError('primaryOutcome must be FAIL, INCOMPLETE, ERROR, or PASS');
  }
  return nextState(machine, {
    phase: CaseState.DRAINING,
    primary: freezePrimary(primaryOutcome, reason),
  });
}

function finishDrain(machine, cleanupFailures) {
  assertMachine(machine);
  if (machine.phase !== CaseState.DRAINING) {
    throw new Error(`INVALID_TRANSITION: finishDrain is invalid in ${machine.phase}`);
  }
  const copiedFailures = normalizeCleanupFailures(cleanupFailures);
  const outcome = machine.primary
    ? machine.primary.outcome
    : copiedFailures.length > 0
      ? Outcome.ERROR
      : Outcome.PASS;
  return nextState(machine, {
    phase: terminalStateFor(outcome),
    outcome,
    cleanup_failures: copiedFailures,
  });
}

function inspectEnvelope(machine, event) {
  if (!event || typeof event !== 'object' || Array.isArray(event)) {
    return invalidEnvelope('INVALID_EVENT', 'event must be an object');
  }
  const runId = ownData(event, 'run_id');
  const caseId = ownData(event, 'case_id');
  const type = ownData(event, 'type');
  const generation = ownData(event, 'generation');
  const sequence = ownData(event, 'sequence');
  if (!runId.valid || !caseId.valid || !type.valid || !generation.valid || !sequence.valid) {
    return invalidEnvelope('INVALID_EVENT', 'event fields must be own data properties');
  }
  if (typeof runId.value !== 'string' || runId.value.length === 0 ||
      typeof caseId.value !== 'string' || caseId.value.length === 0) {
    return invalidEnvelope('INVALID_EVENT', 'event must contain non-empty run_id and case_id');
  }
  if (runId.value !== machine.identity.run_id || caseId.value !== machine.identity.case_id) {
    return invalidEnvelope('IDENTITY_MISMATCH', 'event identity mismatch');
  }
  if (typeof type.value !== 'string' || type.value.length === 0) {
    return invalidEnvelope('INVALID_EVENT', 'event type must be non-empty');
  }
  if (!Object.hasOwn(EVENT_TYPES, type.value)) {
    return invalidEnvelope('UNKNOWN_EVENT', `unknown event type: ${type.value}`);
  }
  if (!Number.isInteger(generation.value) || generation.value < 0 ||
      !Number.isInteger(sequence.value) || sequence.value < 0) {
    return invalidEnvelope('INVALID_EVENT', 'event generation and sequence must be non-negative integers');
  }
  return Object.freeze({
    valid: true,
    value: Object.freeze({
      type: type.value,
      generation: generation.value,
      sequence: sequence.value,
    }),
  });
}

function normalizeFailure(event, envelope) {
  const outcome = ownData(event, 'outcome');
  const reason = ownData(event, 'reason');
  if (!outcome.valid || !reason.valid) {
    return invalidEnvelope('INVALID_FAILURE', 'failure fields must be own data properties');
  }
  if (envelope.type === 'failure') {
    if (!outcome.present || !Object.hasOwn(FAILURE_OUTCOMES, outcome.value)) {
      return invalidEnvelope('INVALID_FAILURE', 'failure requires FAIL, INCOMPLETE, or ERROR outcome');
    }
    if (!reason.present) {
      return invalidEnvelope('INVALID_FAILURE', 'failure requires a non-empty reason');
    }
    return Object.freeze({ valid: true, value: freezeFailure(outcome.value, reason.value) });
  }

  const effectiveOutcome = outcome.present ? outcome.value : Outcome.ERROR;
  if (!Object.hasOwn(FAILURE_OUTCOMES, effectiveOutcome)) {
    return invalidEnvelope('INVALID_FAILURE', `${envelope.type} outcome must be FAIL, INCOMPLETE, or ERROR`);
  }
  const effectiveReason = reason.present ? reason.value : `${envelope.type} received`;
  if (!isReason(effectiveReason)) {
    return invalidEnvelope('INVALID_FAILURE', `${envelope.type} reason must be non-empty when supplied`);
  }
  return Object.freeze({ valid: true, value: freezeFailure(effectiveOutcome, effectiveReason) });
}

function normalizeReceipt(event, envelope) {
  const evidenceField = ownData(event, 'evidence');
  if (!evidenceField.valid) {
    return invalidEnvelope('INVALID_RECEIPT', 'receipt evidence must be an own data property');
  }
  const evidence = evidenceField.present ? evidenceField.value : event;
  if (!evidence || typeof evidence !== 'object' || Array.isArray(evidence)) {
    return invalidEnvelope('INVALID_RECEIPT', 'hook_effective requires receipt evidence');
  }
  const evidenceId = ownData(evidence, 'evidence_id');
  const receiptId = ownData(evidence, 'receipt_id');
  const contentHash = ownData(evidence, 'content_hash');
  const evidenceHash = ownData(evidence, 'evidence_hash');
  if (!evidenceId.valid || !receiptId.valid || !contentHash.valid || !evidenceHash.valid) {
    return invalidEnvelope('INVALID_RECEIPT', 'receipt fields must be own data properties');
  }
  const effectiveEvidenceId = evidenceId.present ? evidenceId.value : receiptId.value;
  const effectiveContentHash = contentHash.present ? contentHash.value : evidenceHash.value;
  if (typeof effectiveEvidenceId !== 'string' || effectiveEvidenceId.length === 0 ||
      typeof effectiveContentHash !== 'string' || !/^[0-9a-f]{64}$/.test(effectiveContentHash)) {
    return invalidEnvelope('INVALID_RECEIPT', 'hook_effective receipt requires evidence_id and lowercase SHA-256 content_hash');
  }
  return Object.freeze({
    valid: true,
    value: freezeState({
      generation: envelope.generation,
      evidence_id: effectiveEvidenceId,
      content_hash: effectiveContentHash,
    }),
  });
}

function normalizeEventCleanupFailures(event) {
  const cleanupFailures = ownData(event, 'cleanup_failures');
  if (!cleanupFailures.valid) {
    return invalidEnvelope('INVALID_CLEANUP', 'cleanup_failures must be an own data property');
  }
  return Object.freeze({
    valid: true,
    value: normalizeCleanupFailures(cleanupFailures.present ? cleanupFailures.value : []),
  });
}

function isConflictingReceipt(machine, generation, receipt) {
  if (!machine.last_receipt || machine.last_receipt.generation !== generation) {
    return false;
  }
  return machine.last_receipt.evidence_id !== receipt.evidence_id ||
    machine.last_receipt.content_hash !== receipt.content_hash;
}

function isMatchingReceipt(machine, generation, receipt) {
  if (!machine.last_receipt || machine.last_receipt.generation !== generation) {
    return false;
  }
  return machine.last_receipt.evidence_id === receipt.evidence_id &&
    machine.last_receipt.content_hash === receipt.content_hash;
}

function enterDrain(machine, outcome, reason, sequence) {
  const primary = machine.primary || freezePrimary(outcome, reason);
  return nextState(machine, {
    phase: CaseState.DRAINING,
    primary,
    last_sequence: sequence,
  });
}

function enterErrorDrain(machine, code, message) {
  const primary = machine.primary || freezePrimary(Outcome.ERROR, { code, message });
  return nextState(machine, {
    phase: CaseState.DRAINING,
    primary,
  });
}

function freezeFailure(outcome, reason) {
  return freezeState({ outcome, reason: copyValue(reason) });
}

function freezePrimary(outcome, reason) {
  if (!isReason(reason)) {
    throw new TypeError('primary reason must be a non-empty string or structured value');
  }
  return freezeFailure(outcome, reason);
}

function normalizeCleanupFailures(cleanupFailures) {
  if (!Array.isArray(cleanupFailures)) {
    throw new TypeError('cleanupFailures must be an array');
  }
  const copied = [];
  for (let index = 0; index < cleanupFailures.length; index += 1) {
    const descriptor = Object.getOwnPropertyDescriptor(cleanupFailures, index);
    if (!descriptor || !Object.hasOwn(descriptor, 'value')) {
      throw new TypeError('cleanupFailures cannot contain sparse arrays or accessors');
    }
    copied.push(copyValue(descriptor.value));
  }
  return freezeState(copied);
}

function terminalStateFor(outcome) {
  switch (outcome) {
    case Outcome.PASS: return CaseState.PASSED;
    case Outcome.FAIL: return CaseState.FAILED;
    case Outcome.INCOMPLETE: return CaseState.INCOMPLETE;
    case Outcome.ERROR: return CaseState.ERROR;
    default: throw new TypeError(`unsupported outcome: ${outcome}`);
  }
}

function nextState(machine, updates) {
  return freezeState({
    identity: machine.identity,
    phase: machine.phase,
    generation: machine.generation,
    last_sequence: machine.last_sequence,
    stale_event_count: machine.stale_event_count,
    primary: machine.primary,
    cleanup_failures: machine.cleanup_failures,
    last_receipt: machine.last_receipt,
    outcome: machine.outcome,
    ...updates,
  });
}

function normalizeIdentity(identity) {
  if (!identity || typeof identity !== 'object' || Array.isArray(identity)) {
    throw new TypeError('identity must be an object');
  }
  const runId = ownData(identity, 'run_id');
  const caseId = ownData(identity, 'case_id');
  const caseKey = ownData(identity, 'case_key');
  const ordinal = ownData(identity, 'ordinal');
  if (!runId.valid || !caseId.valid || !caseKey.valid || !ordinal.valid ||
      typeof runId.value !== 'string' || runId.value.length === 0 ||
      typeof caseId.value !== 'string' || caseId.value.length === 0 ||
      typeof caseKey.value !== 'string' || caseKey.value.length === 0 ||
      !Number.isInteger(ordinal.value) || ordinal.value < 0) {
    throw new TypeError('identity must contain non-empty run_id, case_id, case_key, and non-negative ordinal');
  }
  return freezeState({
    run_id: runId.value,
    case_id: caseId.value,
    case_key: caseKey.value,
    ordinal: ordinal.value,
  });
}

function assertMachine(machine) {
  if (!machine || typeof machine !== 'object' || !machine.identity ||
      typeof machine.phase !== 'string' || !Number.isInteger(machine.generation) || machine.generation < 0 ||
      !Number.isInteger(machine.last_sequence) || machine.last_sequence < -1 ||
      !Number.isInteger(machine.stale_event_count) || machine.stale_event_count < 0) {
    throw new TypeError('machine is not a valid case machine');
  }
}

function isTerminal(machine) {
  return Object.hasOwn(TERMINAL_STATES, machine.phase);
}

function invalidEnvelope(code, message) {
  return Object.freeze({ valid: false, code, message });
}

function isReason(value) {
  if (typeof value === 'string') {
    return value.trim().length > 0;
  }
  if (!value || typeof value !== 'object') {
    return false;
  }
  return Array.isArray(value) ? value.length > 0 : Object.keys(value).length > 0;
}

function ownData(object, key) {
  if (!object || typeof object !== 'object') {
    return Object.freeze({ valid: false, present: false, value: undefined });
  }
  const descriptor = Object.getOwnPropertyDescriptor(object, key);
  if (!descriptor) {
    return Object.freeze({ valid: true, present: false, value: undefined });
  }
  if (!Object.hasOwn(descriptor, 'value')) {
    return Object.freeze({ valid: false, present: false, value: undefined });
  }
  return Object.freeze({ valid: true, present: true, value: descriptor.value });
}

function safelyNormalize(normalize) {
  try {
    return normalize();
  } catch {
    return invalidEnvelope('INVALID_EVENT_PAYLOAD', 'event payload must contain finite, acyclic own data values');
  }
}

function copyValue(value, ancestors = new Set()) {
  if (value === null || typeof value === 'boolean' || typeof value === 'string') {
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) {
      throw new TypeError('state evidence cannot contain a non-finite number');
    }
    return value;
  }
  if (!value || typeof value !== 'object') {
    throw new TypeError(`state evidence cannot contain ${typeof value}`);
  }
  if (ancestors.has(value)) {
    throw new TypeError('state evidence cannot be cyclic');
  }
  ancestors.add(value);
  try {
    if (Array.isArray(value)) {
      const copied = [];
      for (let index = 0; index < value.length; index += 1) {
        const descriptor = Object.getOwnPropertyDescriptor(value, index);
        if (!descriptor || !Object.hasOwn(descriptor, 'value')) {
          throw new TypeError('state evidence cannot contain sparse arrays or accessors');
        }
        copied.push(copyValue(descriptor.value, ancestors));
      }
      return freezeState(copied);
    }
    if (Object.getPrototypeOf(value) !== Object.prototype && Object.getPrototypeOf(value) !== null ||
        Object.getOwnPropertySymbols(value).length !== 0) {
      throw new TypeError('state evidence must use plain objects');
    }
    const copied = {};
    for (const key of Object.keys(value)) {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (!descriptor || !Object.hasOwn(descriptor, 'value')) {
        throw new TypeError('state evidence cannot contain accessors');
      }
      Object.defineProperty(copied, key, {
        value: copyValue(descriptor.value, ancestors),
        enumerable: true,
        writable: false,
        configurable: false,
      });
    }
    return freezeState(copied);
  } finally {
    ancestors.delete(value);
  }
}

function deepFreeze(value, seen = new Set()) {
  if (!value || typeof value !== 'object' || seen.has(value)) {
    return value;
  }
  seen.add(value);
  for (const key of Object.keys(value)) {
    deepFreeze(value[key], seen);
  }
  return Object.freeze(value);
}

function freezeState(value) {
  return deepFreeze(value);
}

module.exports = {
  CaseState,
  Outcome,
  createCaseMachine,
  stepCase,
  beginDrain,
  finishDrain,
};
