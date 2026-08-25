'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const {
  CaseState,
  Outcome,
  createCaseMachine,
  stepCase,
  beginDrain,
  finishDrain,
} = require('../src/state_machine');

const IDENTITY = Object.freeze({ run_id: 'run-acceptance', case_id: 'case-relay' });

function event(type, sequence, generation, extra = {}) {
  return {
    type,
    run_id: IDENTITY.run_id,
    case_id: IDENTITY.case_id,
    generation,
    sequence,
    ...extra,
  };
}

function reachStable() {
  let machine = createCaseMachine(IDENTITY);
  machine = stepCase(machine, event('start', 1, 0));
  machine = stepCase(machine, event('preflight_ok', 2, 0));
  return stepCase(machine, event('media_stable', 3, 0));
}

test('case machine follows every normal transition through recovery and PASS drain', () => {
  let machine = createCaseMachine(IDENTITY);
  assert.equal(machine.phase, CaseState.CREATED);
  assert.equal(machine.generation, 0);
  assert.equal(machine.last_sequence, -1);

  const cases = [
    ['start', CaseState.PREPARING, 0],
    ['preflight_ok', CaseState.CONNECTING, 0],
    ['media_stable', CaseState.STABLE, 0],
    ['transition_due', CaseState.TRANSITIONING, 1],
    ['hook_effective', CaseState.RECOVERING, 1],
    ['recovery_verified', CaseState.STABLE, 1],
    ['complete', CaseState.DRAINING, 1],
  ];
  for (const [type, phase, generation] of cases) {
    const extra = type === 'hook_effective'
      ? { evidence: { evidence_id: 'hook-1', content_hash: 'a'.repeat(64) } }
      : {};
    machine = stepCase(machine, event(type, machine.last_sequence + 1, machine.generation, extra));
    assert.equal(machine.phase, phase, type);
    assert.equal(machine.generation, generation, type);
  }

  const terminal = finishDrain(machine, []);
  assert.equal(terminal.phase, CaseState.PASSED);
  assert.equal(terminal.outcome, Outcome.PASS);
  assert.equal(terminal.primary, null);
});

test('complete drains directly from STABLE without a transition', () => {
  const machine = stepCase(reachStable(), event('complete', 4, 0));

  assert.equal(machine.phase, CaseState.DRAINING);
  assert.equal(finishDrain(machine, []).phase, CaseState.PASSED);
});

test('failure outcomes, timeout, and cancel preempt every pre-drain active state', () => {
  const activeMachines = [
    createCaseMachine(IDENTITY),
    stepCase(createCaseMachine(IDENTITY), event('start', 1, 0)),
    stepCase(stepCase(createCaseMachine(IDENTITY), event('start', 1, 0)), event('preflight_ok', 2, 0)),
    reachStable(),
    stepCase(reachStable(), event('transition_due', 4, 0)),
    stepCase(
      stepCase(reachStable(), event('transition_due', 4, 0)),
      event('hook_effective', 5, 1, { evidence: { evidence_id: 'hook-1', content_hash: 'a'.repeat(64) } })
    ),
  ];
  const cases = [
    ['failure', Outcome.FAIL, 'threshold exceeded', CaseState.FAILED],
    ['failure', Outcome.INCOMPLETE, 'lab capability missing', CaseState.INCOMPLETE],
    ['failure', Outcome.ERROR, 'provider crashed', CaseState.ERROR],
    ['timeout', undefined, undefined, CaseState.ERROR],
    ['cancel', undefined, undefined, CaseState.ERROR],
  ];

  for (const [type, outcome, reason, terminalState] of cases) {
    for (const active of activeMachines) {
      const next = stepCase(active, event(type, active.last_sequence + 1, active.generation, {
        ...(outcome === undefined ? {} : { outcome, reason }),
      }));
      assert.equal(next.phase, CaseState.DRAINING, `${type} from ${active.phase}`);
      assert.equal(next.primary.outcome, outcome || Outcome.ERROR, `${type} primary from ${active.phase}`);
      assert.equal(finishDrain(next, []).phase, terminalState, `${type} terminal from ${active.phase}`);
    }
  }
});

test('stale old generation and stale sequence count without changing state', () => {
  const stable = reachStable();
  const advanced = stepCase(stable, event('transition_due', 4, 0));
  const oldGeneration = stepCase(advanced, event('media_stable', 5, 0));
  const duplicateSequence = stepCase(oldGeneration, event('hook_effective', 4, 1, {
    evidence: { evidence_id: 'hook-1', content_hash: 'a'.repeat(64) },
  }));

  assert.notStrictEqual(oldGeneration, advanced);
  assert.equal(oldGeneration.phase, CaseState.TRANSITIONING);
  assert.equal(oldGeneration.generation, 1);
  assert.equal(oldGeneration.last_sequence, 4);
  assert.equal(oldGeneration.stale_event_count, 1);
  assert.equal(duplicateSequence.phase, CaseState.TRANSITIONING);
  assert.equal(duplicateSequence.last_sequence, 4);
  assert.equal(duplicateSequence.stale_event_count, 2);
});

test('conflicting duplicate hook receipt becomes the first ERROR instead of stale', () => {
  const transitioning = stepCase(reachStable(), event('transition_due', 4, 0));
  const recovering = stepCase(transitioning, event('hook_effective', 5, 1, {
    evidence: { evidence_id: 'hook-1', content_hash: 'a'.repeat(64) },
  }));
  const conflict = stepCase(recovering, event('hook_effective', 5, 1, {
    evidence: { evidence_id: 'hook-1', content_hash: 'b'.repeat(64) },
  }));

  assert.equal(conflict.phase, CaseState.DRAINING);
  assert.equal(conflict.primary.outcome, Outcome.ERROR);
  assert.match(conflict.primary.reason.message, /conflicting receipt/i);
});

test('matching duplicate hook receipt is stale even when its sequence is newer', () => {
  const transitioning = stepCase(reachStable(), event('transition_due', 4, 0));
  const receipt = { evidence: { evidence_id: 'hook-1', content_hash: 'a'.repeat(64) } };
  const recovering = stepCase(transitioning, event('hook_effective', 5, 1, receipt));
  const duplicate = stepCase(recovering, event('hook_effective', 6, 1, receipt));

  assert.equal(duplicate.phase, CaseState.RECOVERING);
  assert.equal(duplicate.last_sequence, 5);
  assert.equal(duplicate.stale_event_count, 1);
  assert.equal(duplicate.primary, null);
});

test('identity mismatch and future generation become ERROR before stale handling', () => {
  const stable = reachStable();
  const identityMismatch = stepCase(stable, event('media_stable', 0, 0, { case_id: 'case-other' }));
  const futureGeneration = stepCase(stable, event('complete', 4, 1));

  assert.equal(identityMismatch.phase, CaseState.DRAINING);
  assert.equal(identityMismatch.primary.outcome, Outcome.ERROR);
  assert.match(identityMismatch.primary.reason.message, /identity mismatch/i);
  assert.equal(futureGeneration.phase, CaseState.DRAINING);
  assert.equal(futureGeneration.primary.outcome, Outcome.ERROR);
  assert.match(futureGeneration.primary.reason.message, /future generation/i);
});

test('unknown type and known type in the wrong state become ERROR while terminal delivery fails fast', () => {
  const created = createCaseMachine(IDENTITY);
  const unknown = stepCase(created, event('unrecognized', 1, 0));
  const invalid = stepCase(created, event('complete', 1, 0));
  const terminal = finishDrain(stepCase(reachStable(), event('complete', 4, 0)), []);

  assert.equal(unknown.phase, CaseState.DRAINING);
  assert.equal(unknown.primary.outcome, Outcome.ERROR);
  assert.equal(invalid.phase, CaseState.DRAINING);
  assert.equal(invalid.primary.outcome, Outcome.ERROR);
  assert.throws(() => stepCase(terminal, event('start', 5, 0)), /INVALID_TRANSITION/);
});

test('first primary outcome wins and cleanup evidence never overwrites it', () => {
  const failed = beginDrain(reachStable(), Outcome.FAIL, { code: 'threshold', value: 12 });
  const preserved = beginDrain(failed, Outcome.ERROR, 'provider crashed later');
  const terminal = finishDrain(preserved, [{ step: 'delete', message: 'resource remained' }]);

  assert.equal(preserved.primary.outcome, Outcome.FAIL);
  assert.deepEqual(preserved.primary.reason, { code: 'threshold', value: 12 });
  assert.equal(terminal.phase, CaseState.FAILED);
  assert.equal(terminal.cleanup_failures.length, 1);
});

test('cleanup-only evidence resolves to ERROR and all machine data is recursively immutable', () => {
  const identity = { run_id: 'run-immutable', case_id: 'case-immutable', nested: { source: 'caller' } };
  const machine = createCaseMachine(identity);
  const draining = beginDrain(machine, Outcome.PASS, { ignored: true });
  const cleanupFailures = [{ step: 'close', details: { resource: 'viewer' } }];
  const terminal = finishDrain(draining, cleanupFailures);

  identity.nested.source = 'changed';
  cleanupFailures[0].details.resource = 'changed';
  assert.equal(terminal.phase, CaseState.ERROR);
  assert.equal(terminal.outcome, Outcome.ERROR);
  assert.equal(terminal.cleanup_failures[0].details.resource, 'viewer');
  assert.ok(Object.isFrozen(terminal));
  assert.ok(Object.isFrozen(terminal.cleanup_failures));
  assert.ok(Object.isFrozen(terminal.cleanup_failures[0].details));
  assert.throws(() => { terminal.cleanup_failures.push({}); }, TypeError);
  assert.throws(() => { terminal.identity.run_id = 'mutated'; }, TypeError);
  assert.equal(machine.phase, CaseState.CREATED);
});

test('invalid envelopes and invalid failure evidence fail fast into ERROR drain', () => {
  const machine = createCaseMachine(IDENTITY);
  const noType = stepCase(machine, { ...event('', 1, 0) });
  const missingReason = stepCase(machine, event('failure', 1, 0, { outcome: Outcome.FAIL }));
  const passFailure = stepCase(machine, event('failure', 1, 0, { outcome: Outcome.PASS, reason: 'not a failure' }));

  for (const result of [noType, missingReason, passFailure]) {
    assert.equal(result.phase, CaseState.DRAINING);
    assert.equal(result.primary.outcome, Outcome.ERROR);
  }
});
