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
  [CaseState.DRAINING]: {},
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

  const envelope = inspectEnvelope(machine, event);
  if (!envelope.valid) {
    return enterErrorDrain(machine, envelope.code, envelope.message);
  }

  if (event.type === 'hook_effective') {
    const receipt = normalizeReceipt(event);
    if (!receipt.valid) {
      return enterErrorDrain(machine, receipt.code, receipt.message);
    }
    if (isConflictingReceipt(machine, event.generation, receipt.value)) {
      return enterErrorDrain(machine, 'CONFLICTING_RECEIPT', 'conflicting receipt evidence for the same generation');
    }
    if (isMatchingReceipt(machine, event.generation, receipt.value)) {
      return nextState(machine, { stale_event_count: machine.stale_event_count + 1 });
    }
  }

  if (event.generation < machine.generation || event.sequence <= machine.last_sequence) {
    return nextState(machine, { stale_event_count: machine.stale_event_count + 1 });
  }
  if (event.generation > machine.generation) {
    return enterErrorDrain(machine, 'FUTURE_GENERATION', 'future generation event received');
  }

  if (event.type === 'failure' || event.type === 'timeout' || event.type === 'cancel') {
    const failure = normalizeFailure(event);
    if (!failure.valid) {
      return enterErrorDrain(machine, failure.code, failure.message);
    }
    return enterDrain(machine, failure.value.outcome, failure.value.reason, event.sequence);
  }

  const nextPhase = TRANSITIONS[machine.phase][event.type];
  if (!nextPhase) {
    return enterErrorDrain(machine, 'INVALID_TRANSITION', `event ${event.type} is invalid in ${machine.phase}`);
  }

  const updates = {
    phase: nextPhase,
    last_sequence: event.sequence,
  };
  if (event.type === 'transition_due') {
    updates.generation = machine.generation + 1;
  }
  if (event.type === 'hook_effective') {
    updates.last_receipt = normalizeReceipt(event).value;
  }
  if (event.type === 'complete') {
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
  if (!FAILURE_OUTCOMES[primaryOutcome]) {
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
  if (typeof event.run_id !== 'string' || event.run_id.length === 0 ||
      typeof event.case_id !== 'string' || event.case_id.length === 0) {
    return invalidEnvelope('INVALID_EVENT', 'event must contain non-empty run_id and case_id');
  }
  if (event.run_id !== machine.identity.run_id || event.case_id !== machine.identity.case_id) {
    return invalidEnvelope('IDENTITY_MISMATCH', 'event identity mismatch');
  }
  if (typeof event.type !== 'string' || event.type.length === 0) {
    return invalidEnvelope('INVALID_EVENT', 'event type must be non-empty');
  }
  if (!EVENT_TYPES[event.type]) {
    return invalidEnvelope('UNKNOWN_EVENT', `unknown event type: ${event.type}`);
  }
  if (!Number.isInteger(event.generation) || event.generation < 0 ||
      !Number.isInteger(event.sequence) || event.sequence < 0) {
    return invalidEnvelope('INVALID_EVENT', 'event generation and sequence must be non-negative integers');
  }
  return Object.freeze({ valid: true });
}

function normalizeFailure(event) {
  if (event.type === 'failure') {
    if (!FAILURE_OUTCOMES[event.outcome]) {
      return invalidEnvelope('INVALID_FAILURE', 'failure requires FAIL, INCOMPLETE, or ERROR outcome');
    }
    if (!isReason(event.reason)) {
      return invalidEnvelope('INVALID_FAILURE', 'failure requires a non-empty reason');
    }
    return Object.freeze({ valid: true, value: freezeFailure(event.outcome, event.reason) });
  }

  const outcome = event.outcome === undefined ? Outcome.ERROR : event.outcome;
  if (!FAILURE_OUTCOMES[outcome]) {
    return invalidEnvelope('INVALID_FAILURE', `${event.type} outcome must be FAIL, INCOMPLETE, or ERROR`);
  }
  const reason = event.reason === undefined ? `${event.type} received` : event.reason;
  if (!isReason(reason)) {
    return invalidEnvelope('INVALID_FAILURE', `${event.type} reason must be non-empty when supplied`);
  }
  return Object.freeze({ valid: true, value: freezeFailure(outcome, reason) });
}

function normalizeReceipt(event) {
  const evidence = event.evidence === undefined ? event : event.evidence;
  if (!evidence || typeof evidence !== 'object' || Array.isArray(evidence)) {
    return invalidEnvelope('INVALID_RECEIPT', 'hook_effective requires receipt evidence');
  }
  const evidenceId = evidence.evidence_id === undefined ? evidence.receipt_id : evidence.evidence_id;
  const contentHash = evidence.content_hash === undefined ? evidence.evidence_hash : evidence.content_hash;
  if (typeof evidenceId !== 'string' || evidenceId.length === 0 ||
      typeof contentHash !== 'string' || !/^[0-9a-f]{64}$/.test(contentHash)) {
    return invalidEnvelope('INVALID_RECEIPT', 'hook_effective receipt requires evidence_id and lowercase SHA-256 content_hash');
  }
  return Object.freeze({
    valid: true,
    value: freezeState({ generation: event.generation, evidence_id: evidenceId, content_hash: contentHash }),
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
  return freezeState(cleanupFailures.map((failure) => copyValue(failure)));
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
  if (!identity || typeof identity !== 'object' || Array.isArray(identity) ||
      typeof identity.run_id !== 'string' || identity.run_id.length === 0 ||
      typeof identity.case_id !== 'string' || identity.case_id.length === 0) {
    throw new TypeError('identity must contain non-empty run_id and case_id');
  }
  return freezeState({ run_id: identity.run_id, case_id: identity.case_id });
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
  return Boolean(TERMINAL_STATES[machine.phase]);
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
      copied[key] = copyValue(descriptor.value, ancestors);
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
