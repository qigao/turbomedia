'use strict';

const { hashCanonical } = require('./canonical_json');
const { expandCases } = require('./manifest');
const { createCaseIdentity } = require('./ids');
const { evaluatePhaseMetrics } = require('./metrics');
const { LIMITS } = require('./constants');
const {
  CaseState,
  Outcome,
  createCaseMachine,
  stepCase,
  beginDrain,
  finishDrain,
} = require('./state_machine');

const CLEANUP_STEPS = Object.freeze([
  'stopSampling',
  'deleteMediaResources',
  'closeBrowsers',
  'waitCaseResourcesZero',
  'waitGlobalBaseline',
  'waitTurnBaseline',
  'teardownTopology',
]);
const OUTCOME_PRIORITY = Object.freeze({ PASS: 0, FAIL: 1, INCOMPLETE: 2, ERROR: 3 });
const SAFE_CODE = /^[A-Z0-9_:-]+$/;
const SHA256 = /^[0-9a-f]{64}$/;

class ControllerError extends Error {
  constructor(code, category, stage) {
    super(`${stage}: ${code}`);
    this.name = 'ControllerError';
    this.code = code;
    this.category = category;
    this.stage = stage;
  }
}

function fail(code, category, stage) {
  throw new ControllerError(code, category, stage);
}

function requireContext(context) {
  if (!context || typeof context !== 'object' || Array.isArray(context) ||
      !context.lab || typeof context.lab !== 'object' || Array.isArray(context.lab)) {
    fail('INVALID_CONTROLLER_CONTEXT', 'harness', 'context');
  }
  return context;
}

function requireMethod(object, name, stage = 'context') {
  if (typeof object[name] !== 'function') fail('MISSING_LAB_METHOD', 'harness', `${stage}:${name}`);
  return object[name].bind(object);
}

function validateDeadlines(manifest) {
  const names = ['preflight_ms', 'connect_ms', 'stable_ms', 'transition_ms', 'recovery_ms', 'drain_ms'];
  if (!manifest || typeof manifest !== 'object' || !manifest.phase_deadlines) {
    fail('INVALID_PHASE_DEADLINES', 'harness', 'manifest');
  }
  for (const name of names) {
    const value = manifest.phase_deadlines[name];
    if (!Number.isSafeInteger(value) || value <= 0) fail('INVALID_PHASE_DEADLINES', 'harness', `manifest:${name}`);
  }
}

function classifyError(error, fallbackCategory = 'harness') {
  const category = error && typeof error === 'object' && typeof error.category === 'string'
    ? error.category : fallbackCategory;
  const outcome = category === 'assertion' ? Outcome.FAIL : category === 'missing_evidence' ? Outcome.INCOMPLETE : Outcome.ERROR;
  const rawCode = error && typeof error === 'object' && typeof error.code === 'string' ? error.code : 'CONTROLLER_OPERATION_FAILED';
  const code = SAFE_CODE.test(rawCode) ? rawCode : 'CONTROLLER_OPERATION_FAILED';
  const rawStage = error && typeof error === 'object' && typeof error.stage === 'string' ? error.stage : 'controller';
  const stage = /^[A-Za-z0-9_.:-]+$/.test(rawStage) ? rawStage : 'controller';
  return Object.freeze({ outcome, reason: Object.freeze({ code, stage }) });
}

function mergeOutcome(current, next) {
  if (!Object.hasOwn(OUTCOME_PRIORITY, current) || !Object.hasOwn(OUTCOME_PRIORITY, next)) {
    fail('INVALID_OUTCOME', 'harness', 'aggregate');
  }
  return OUTCOME_PRIORITY[next] > OUTCOME_PRIORITY[current] ? next : current;
}

function eventFor(runtime, type, generation, extra = {}) {
  return {
    type,
    run_id: runtime.identity.run_id,
    case_id: runtime.identity.case_id,
    generation,
    sequence: runtime.machine.last_sequence + 1,
    ...extra,
  };
}

function step(runtime, type, generation = runtime.machine.generation, extra = {}) {
  runtime.machine = stepCase(runtime.machine, eventFor(runtime, type, generation, extra));
  runtime.generation = runtime.machine.generation;
  return runtime.machine;
}

function advance(runtime, type, expectedPhase, generation = runtime.machine.generation, extra = {}) {
  const machine = step(runtime, type, generation, extra);
  if (machine.phase !== expectedPhase) fail('STATE_MACHINE_REJECTED_EVENT', 'harness', type);
  return machine;
}

function parentAbort(signal, controller) {
  if (!signal) return () => {};
  if (signal.aborted) controller.abort();
  const abort = () => controller.abort();
  signal.addEventListener('abort', abort, { once: true });
  return () => signal.removeEventListener('abort', abort);
}

async function withDeadline(stage, timeoutMs, category, signal, work) {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0) fail('INVALID_DEADLINE', 'harness', stage);
  if (signal?.aborted) fail('CANCELLED', 'harness', stage);
  const controller = new AbortController();
  const detach = parentAbort(signal, controller);
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, timeoutMs);
  try {
    const aborted = new Promise((_, reject) => controller.signal.addEventListener('abort', () => {
      reject(new ControllerError(timedOut ? 'PHASE_TIMEOUT' : 'CANCELLED', timedOut ? category : 'harness', stage));
    }, { once: true }));
    return await Promise.race([Promise.resolve().then(() => work(controller.signal)), aborted]);
  } finally {
    clearTimeout(timer);
    detach();
  }
}

function requireRelayEvidence(value, stage) {
  const evidence = value && typeof value === 'object' && value.relay ? value.relay : value;
  if (!evidence || typeof evidence !== 'object') fail('RELAY_EVIDENCE_MISSING', 'missing_evidence', stage);
  if (evidence.verified === true) {
    if (!Array.isArray(evidence.pairs) || evidence.pairs.length === 0 ||
        evidence.pairs.some((pair) => !pair || typeof pair.pair_id !== 'string' || pair.pair_id.length === 0)) {
      fail('RELAY_PAIR_MISSING', 'missing_evidence', stage);
    }
    return evidence;
  }
  const category = evidence.category === 'assertion' ? 'assertion' : 'missing_evidence';
  const code = typeof evidence.code === 'string' && SAFE_CODE.test(evidence.code) ? evidence.code : 'RELAY_EVIDENCE_MISSING';
  fail(code, category, stage);
}

function requireBrowserEvidence(value, role, stage, expectedGeneration, previous) {
  requireRelayEvidence(value, stage);
  const snapshot = value && typeof value === 'object' ? value.snapshot : null;
  const ice = snapshot && snapshot.ice;
  if (!snapshot || snapshot.role !== role || !ice ||
      !Number.isInteger(ice.generation) || !SHA256.test(ice.local_sha256) || !SHA256.test(ice.remote_sha256)) {
    fail('ICE_EVIDENCE_MISSING', 'missing_evidence', stage);
  }
  if (ice.generation !== expectedGeneration) fail('ICE_GENERATION_MISMATCH', 'assertion', stage);
  if (previous && (ice.local_sha256 === previous.local_sha256 || ice.remote_sha256 === previous.remote_sha256)) {
    fail('UNCHANGED_ICE_HASH', 'assertion', stage);
  }
  return Object.freeze({
    generation: ice.generation,
    local_sha256: ice.local_sha256,
    remote_sha256: ice.remote_sha256,
    pair_ids: Object.freeze(value.relay.pairs.map((pair) => pair.pair_id)),
  });
}

function requirePublisherMediaOrder(value) {
  const order = value && value.snapshot && value.snapshot.api && value.snapshot.api.local_media_order;
  if (!Array.isArray(order) || order.length !== 2 || order[0] !== 'audio' || order[1] !== 'video') {
    fail('INVALID_MEDIA_ORDER', 'assertion', 'openPublisher');
  }
}

function requireTrackIds(value) {
  if (!Array.isArray(value) || value.length !== 2 ||
      value.some((id) => typeof id !== 'string' || id.length === 0) || value[0] === value[1]) {
    fail('PUBLISHED_TRACK_EVIDENCE_INVALID', 'harness', 'confirmPublishedTracks');
  }
  return Object.freeze([...value]);
}

function requireChangedRelayPair(previous, current, stage) {
  const oldPairs = new Set(previous.pair_ids);
  if (current.pair_ids.some((pairId) => oldPairs.has(pairId))) {
    fail('UNCHANGED_RELAY_PAIR', 'assertion', stage);
  }
}

function verifyReceipt(runtime, receipt, stage, expectedAction, expectedGeneration, expectedSequence) {
  if (!receipt || typeof receipt !== 'object' || Array.isArray(receipt) ||
      receipt.schema_version !== 2 ||
      receipt.topology_id !== runtime.definition.topology_id ||
      receipt.action !== expectedAction ||
      receipt.generation !== expectedGeneration ||
      receipt.sequence !== expectedSequence ||
      receipt.effective !== true ||
      typeof receipt.evidence_id !== 'string' || receipt.evidence_id.length === 0 ||
      receipt.relay_contract_hash !== runtime.relay_contract_hash) {
    fail('TOPOLOGY_RECEIPT_MISMATCH', 'harness', stage);
  }
  return Object.freeze({
    evidence_id: receipt.evidence_id,
    content_hash: hashCanonical(receipt),
  });
}

function evaluateSamples(samples, thresholdProfile, stage, manifest) {
  const configuredCap = manifest.limits && manifest.limits.max_samples_per_case !== undefined
    ? manifest.limits.max_samples_per_case : LIMITS.MAX_SAMPLES_PER_CASE;
  if (!Number.isSafeInteger(configuredCap) || configuredCap < 1 || configuredCap > LIMITS.MAX_SAMPLES_PER_CASE) {
    fail('INVALID_SAMPLE_LIMIT', 'harness', stage);
  }
  if (!Array.isArray(samples)) fail('METRICS_RESULT_INVALID', 'harness', stage);
  if (samples.length > configuredCap) fail('SAMPLE_LIMIT_EXCEEDED', 'harness', stage);
  const result = evaluatePhaseMetrics(samples, thresholdProfile);
  if (!result || !Object.hasOwn(OUTCOME_PRIORITY, result.outcome)) fail('METRICS_RESULT_INVALID', 'harness', stage);
  if (result.outcome === Outcome.PASS) return result;
  const category = result.outcome === Outcome.FAIL ? 'assertion' : result.outcome === Outcome.INCOMPLETE ? 'missing_evidence' : 'harness';
  const code = result.outcome === Outcome.FAIL ? 'THRESHOLD_FAILED' : result.outcome === Outcome.INCOMPLETE ? 'METRICS_INCOMPLETE' : 'METRICS_ERROR';
  const error = new ControllerError(code, category, stage);
  error.metrics = result;
  throw error;
}

function createRuntime(context, caseDefinition) {
  if (!context.case_identity) fail('CASE_IDENTITY_REQUIRED', 'harness', 'run_case');
  const identity = context.case_identity;
  const relayContract = caseDefinition && caseDefinition.relay_contract;
  if (!relayContract || relayContract.schema_version !== 1) fail('RELAY_CONTRACT_MISSING', 'harness', 'run_case');
  return {
    identity,
    definition: caseDefinition,
    relay_contract: relayContract,
    relay_contract_hash: hashCanonical(relayContract),
    generation: 0,
    machine: createCaseMachine(identity),
    samples: { stable: [], recovery: [] },
    browser_ice_generation: null,
    browser_ice: Object.create(null),
    confirmed_track_ids: null,
    fresh_turn_credential_id: null,
    primary: null,
  };
}

async function preflightRun(context, manifest, signal) {
  requireContext(context);
  validateDeadlines(manifest);
  const lab = context.lab;
  const cases = context.cases || expandCases(manifest);
  const timeout = manifest.phase_deadlines.preflight_ms;
  const preflightArtifacts = requireMethod(lab, 'preflightArtifacts', 'preflight');
  await withDeadline('preflightArtifacts', timeout, 'harness', signal,
    (phaseSignal) => preflightArtifacts(manifest, phaseSignal));

  const preflightCase = requireMethod(lab, 'preflightCase', 'preflight');
  for (const caseDefinition of cases) {
    const observed = await withDeadline('preflightCase', timeout, 'missing_evidence', signal,
      (phaseSignal) => preflightCase(caseDefinition, phaseSignal));
    if (!observed || observed.available !== true) fail('BROWSER_CAPABILITY_MISSING', 'missing_evidence', 'preflightCase');
  }

  const checks = [
    ['preflightProviders', [manifest]],
    ['preflightTopologies', [manifest]],
    ['preflightSfu', [manifest]],
    ['preflightTurn', [manifest]],
  ];
  for (const [name, args] of checks) {
    const method = requireMethod(lab, name, 'preflight');
    await withDeadline(name, timeout, 'harness', signal, (phaseSignal) => method(...args, phaseSignal));
  }
  return Object.freeze({ case_count: cases.length, cases });
}

async function runCase(context, caseDefinition, signal) {
  requireContext(context);
  validateDeadlines(context.manifest);
  const lab = context.lab;
  const runtime = createRuntime(context, caseDefinition);
  const deadlines = context.manifest.phase_deadlines;
  advance(runtime, 'start', CaseState.PREPARING);

  try {
    const prepare = requireMethod(lab, 'prepareCase');
    const setupSequence = runtime.machine.last_sequence + 1;
    const prepared = await withDeadline('prepareCase', deadlines.connect_ms, 'harness', signal,
      (phaseSignal) => prepare(runtime, setupSequence, phaseSignal));
    if (!prepared || !prepared.topology_receipt) fail('TOPOLOGY_RECEIPT_MISSING', 'harness', 'prepareCase');
    verifyReceipt(runtime, prepared.topology_receipt, 'prepareCase', 'setup', 0, setupSequence);
    advance(runtime, 'preflight_ok', CaseState.CONNECTING);

    await withDeadline('attachRoom', deadlines.connect_ms, 'harness', signal,
      (phaseSignal) => requireMethod(lab, 'attachRoom')(runtime, phaseSignal));

    const publisher = await withDeadline('openPublisher', deadlines.connect_ms, 'assertion', signal,
      (phaseSignal) => requireMethod(lab, 'openPublisher')(runtime, phaseSignal));
    requirePublisherMediaOrder(publisher);
    runtime.browser_ice.publisher = requireBrowserEvidence(publisher, 'publisher', 'openPublisher', 1, null);

    const publishedTracks = await withDeadline('confirmPublishedTracks', deadlines.connect_ms, 'harness', signal,
      (phaseSignal) => requireMethod(lab, 'confirmPublishedTracks')(runtime, phaseSignal));
    runtime.confirmed_track_ids = requireTrackIds(publishedTracks);

    await withDeadline('setViewerSubscriptions', deadlines.connect_ms, 'harness', signal,
      (phaseSignal) => requireMethod(lab, 'setViewerSubscriptions')(runtime, runtime.confirmed_track_ids, phaseSignal));

    const viewer = await withDeadline('openViewer', deadlines.connect_ms, 'assertion', signal,
      (phaseSignal) => requireMethod(lab, 'openViewer')(runtime, phaseSignal));
    runtime.browser_ice.viewer = requireBrowserEvidence(viewer, 'viewer', 'openViewer', 1, null);
    runtime.browser_ice_generation = 1;
    advance(runtime, 'media_stable', CaseState.STABLE);

    const samplePhase = requireMethod(lab, 'samplePhase');
    runtime.samples.stable = await withDeadline('stable', deadlines.stable_ms, 'assertion', signal,
      (phaseSignal) => samplePhase(runtime, 'stable', phaseSignal));
    runtime.stable_metrics = evaluateSamples(runtime.samples.stable, context.manifest.threshold_profile.stable, 'stable', context.manifest);

    const workflow = caseDefinition.scenario && caseDefinition.scenario.workflow;
    if (!workflow || typeof workflow.credential_expiry !== 'boolean' || typeof workflow.topology_transition !== 'boolean') {
      fail('SCENARIO_WORKFLOW_MISSING', 'harness', 'workflow');
    }
    if (workflow.credential_expiry && !workflow.topology_transition) {
      fail('SCENARIO_WORKFLOW_INVALID', 'harness', 'workflow');
    }

    if (workflow.credential_expiry) {
      const assertion = await withDeadline('credential_expiry', deadlines.transition_ms, 'assertion', signal,
        (phaseSignal) => requireMethod(lab, 'assertCredentialExpiry')(runtime, phaseSignal));
      if (!assertion || assertion.old_credential_rejected !== true) fail('EXPIRED_CREDENTIAL_ACCEPTED', 'assertion', 'credential_expiry');
      if (typeof assertion.fresh_credential_id !== 'string' ||
          !/^[A-Za-z0-9_.:-]{1,128}$/.test(assertion.fresh_credential_id)) {
        fail('FRESH_CREDENTIAL_EVIDENCE_MISSING', 'missing_evidence', 'credential_expiry');
      }
      runtime.fresh_turn_credential_id = assertion.fresh_credential_id;
    }

    if (workflow.topology_transition) {
      advance(runtime, 'transition_due', CaseState.TRANSITIONING);
      const transitionSequence = runtime.machine.last_sequence + 1;
      const receipt = await withDeadline('transition', deadlines.transition_ms, 'harness', signal,
        (phaseSignal) => requireMethod(lab, 'transitionTopology')(runtime, transitionSequence, phaseSignal));
      const receiptEvidence = verifyReceipt(runtime, receipt, 'transition', 'transition',
        runtime.machine.generation, transitionSequence);
      advance(runtime, 'hook_effective', CaseState.RECOVERING, runtime.machine.generation, { evidence: receiptEvidence });

      const expectedIceGeneration = runtime.browser_ice_generation + 1;
      const restarted = await withDeadline('recovery', deadlines.recovery_ms, 'assertion', signal,
        (phaseSignal) => requireMethod(lab, 'restartIce')(
          runtime, expectedIceGeneration, runtime.fresh_turn_credential_id, phaseSignal));
      if (runtime.fresh_turn_credential_id !== null &&
          (!restarted || restarted.credential_id !== runtime.fresh_turn_credential_id)) {
        fail('RESTART_CREDENTIAL_MISMATCH', 'assertion', 'recovery');
      }
      const publisherIce = requireBrowserEvidence(restarted && restarted.publisher, 'publisher',
        'recovery:publisher', expectedIceGeneration, runtime.browser_ice.publisher);
      const viewerIce = requireBrowserEvidence(restarted && restarted.viewer, 'viewer',
        'recovery:viewer', expectedIceGeneration, runtime.browser_ice.viewer);
      requireChangedRelayPair(runtime.browser_ice.publisher, publisherIce, 'recovery:publisher');
      requireChangedRelayPair(runtime.browser_ice.viewer, viewerIce, 'recovery:viewer');
      if (restarted.publisher.snapshot.api.stale_etag_status !== 412 ||
          restarted.viewer.snapshot.api.stale_etag_status !== 412) {
        fail('STALE_ETAG_NOT_REJECTED', 'assertion', 'recovery');
      }
      runtime.browser_ice.publisher = publisherIce;
      runtime.browser_ice.viewer = viewerIce;
      runtime.browser_ice_generation = expectedIceGeneration;

      runtime.samples.recovery = await withDeadline('recovery_sample', deadlines.recovery_ms, 'assertion', signal,
        (phaseSignal) => samplePhase(runtime, 'recovery', phaseSignal));
      runtime.recovery_metrics = evaluateSamples(runtime.samples.recovery, context.manifest.threshold_profile.recovery, 'recovery', context.manifest);
      advance(runtime, 'recovery_verified', CaseState.STABLE);
    }

    advance(runtime, 'complete', CaseState.DRAINING);
  } catch (error) {
    const primary = classifyError(error);
    runtime.primary = primary;
    if (runtime.machine.phase !== CaseState.DRAINING) {
      step(runtime, 'failure', runtime.machine.generation, { outcome: primary.outcome, reason: primary.reason });
    }
  }

  return drainCase(context, runtime, runtime.primary, signal);
}

async function drainCase(context, runtime, primaryResult, _signal) {
  requireContext(context);
  const lab = context.lab;
  const timeout = context.manifest.phase_deadlines.drain_ms;
  if (runtime.machine.phase !== CaseState.DRAINING) {
    runtime.machine = beginDrain(runtime.machine, primaryResult ? primaryResult.outcome : Outcome.PASS,
      primaryResult ? primaryResult.reason : undefined);
  }
  const cleanupFailures = [];
  const teardownSequence = runtime.machine.last_sequence + 1;
  for (const stepName of CLEANUP_STEPS) {
    try {
      const result = await withDeadline(`drain:${stepName}`, timeout, 'harness', undefined,
        (phaseSignal) => stepName === 'teardownTopology'
          ? requireMethod(lab, stepName, 'drain')(runtime, teardownSequence, phaseSignal)
          : requireMethod(lab, stepName, 'drain')(runtime, phaseSignal));
      if (result && Array.isArray(result.cleanup_failures)) {
        for (const failure of result.cleanup_failures) {
          const code = failure && typeof failure.code === 'string' && SAFE_CODE.test(failure.code) ? failure.code : 'CLEANUP_FAILED';
          cleanupFailures.push(Object.freeze({ step: stepName, code }));
        }
      }
    } catch (error) {
      const safe = classifyError(error);
      cleanupFailures.push(Object.freeze({ step: stepName, code: safe.reason.code }));
    }
  }
  runtime.machine = finishDrain(runtime.machine, cleanupFailures);
  return Object.freeze({
    identity: runtime.identity,
    outcome: runtime.machine.outcome,
    primary: runtime.machine.primary,
    cleanup_failures: runtime.machine.cleanup_failures,
    stale_event_count: runtime.machine.stale_event_count,
    generation: runtime.machine.generation,
    browser_ice_generation: runtime.browser_ice_generation,
    confirmed_track_ids: runtime.confirmed_track_ids,
    fresh_turn_credential_id: runtime.fresh_turn_credential_id,
    stable_metrics: runtime.stable_metrics || null,
    recovery_metrics: runtime.recovery_metrics || null,
  });
}

async function runManifest(context, manifest, signal) {
  requireContext(context);
  validateDeadlines(manifest);
  if (!context.run_identity) fail('RUN_IDENTITY_REQUIRED', 'harness', 'run_manifest');
  const cases = expandCases(manifest);
  const runContext = { ...context, manifest, cases };
  try {
    await preflightRun(runContext, manifest, signal);
  } catch (error) {
    const primary = classifyError(error);
    return Object.freeze({ outcome: primary.outcome, primary, cases: Object.freeze([]) });
  }

  const results = [];
  let outcome = Outcome.PASS;
  for (let ordinal = 0; ordinal < cases.length; ordinal += 1) {
    if (signal?.aborted) {
      outcome = mergeOutcome(outcome, Outcome.ERROR);
      break;
    }
    const identity = createCaseIdentity(context.run_identity, cases[ordinal], ordinal);
    const result = await runCase({ ...runContext, case_identity: identity }, cases[ordinal], signal);
    results.push(result);
    outcome = mergeOutcome(outcome, result.outcome);
  }
  return Object.freeze({ outcome, primary: null, cases: Object.freeze(results) });
}

module.exports = {
  CLEANUP_STEPS,
  ControllerError,
  preflightRun,
  runCase,
  drainCase,
  runManifest,
};
