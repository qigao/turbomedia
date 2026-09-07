'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const {
  nearestRank,
  normalizeBrowserSnapshot,
  normalizeSfuSnapshot,
  appendBoundedSample,
  evaluatePhaseMetrics,
} = require('../src/metrics');

function browserRaw(overrides = {}) {
  return {
    rtt_ms: 20,
    jitter_ms: 4,
    media_packets: 100,
    packets_lost: 5,
    nack_count: 2,
    ...overrides,
  };
}

function sfuRaw(overrides = {}) {
  return {
    active_resources: 2,
    turn_allocations: 1,
    media_packets: 100,
    ...overrides,
  };
}

test('nearest rank uses the hand-checked [1,2,3,4,100] vector for p50 p95 and p99', () => {
  const samples = [1, 2, 3, 4, 100];

  assert.equal(nearestRank(samples, 0.50), 3);
  assert.equal(nearestRank(samples, 0.95), 100);
  assert.equal(nearestRank(samples, 0.99), 100);
});

test('nearest rank handles single odd even and repeated samples without averaging', () => {
  assert.equal(nearestRank([7], 0.95), 7);
  assert.equal(nearestRank([8, 1, 4, 2], 0.50), 2);
  assert.equal(nearestRank([3, 3, 3, 9], 0.95), 9);
  assert.equal(nearestRank([3, 3, 3, 9], 0.50), 3);
});

test('browser normalization produces counter deltas rates and loss ratio from monotonic counters', () => {
  const first = normalizeBrowserSnapshot(browserRaw(), null, 1_000);
  const next = normalizeBrowserSnapshot(browserRaw({
    rtt_ms: 25,
    jitter_ms: 5,
    media_packets: 160,
    packets_lost: 8,
    nack_count: 5,
  }), first, 3_000);

  assert.deepEqual(next, {
    source: 'browser',
    timestamp_ms: 3_000,
    rtt_ms: 25,
    jitter_ms: 5,
    media_delta: 60,
    media_rate_per_second: 30,
    packets_lost_delta: 3,
    nack_delta: 3,
    nack_rate_per_second: 1.5,
    loss_ratio: 3 / 63,
  });
});

test('SFU normalization preserves gauges and converts only monotonic media counter to delta and rate', () => {
  const first = normalizeSfuSnapshot(sfuRaw(), null, 1_000);
  const next = normalizeSfuSnapshot(sfuRaw({ active_resources: 1, turn_allocations: 0, media_packets: 130 }), first, 2_000);

  assert.deepEqual(next, {
    source: 'sfu',
    timestamp_ms: 2_000,
    active_resources: 1,
    turn_allocations: 0,
    media_delta: 30,
    media_rate_per_second: 30,
  });
});

test('snapshot normalization rejects counter reset timestamp regression malformed and non-finite input', () => {
  const firstBrowser = normalizeBrowserSnapshot(browserRaw(), null, 1_000);
  const firstSfu = normalizeSfuSnapshot(sfuRaw(), null, 1_000);

  assert.throws(() => normalizeBrowserSnapshot(browserRaw({ media_packets: 99 }), firstBrowser, 2_000), /COUNTER_RESET/);
  assert.throws(() => normalizeSfuSnapshot(sfuRaw({ media_packets: 99 }), firstSfu, 2_000), /COUNTER_RESET/);
  assert.throws(() => normalizeBrowserSnapshot(browserRaw(), firstBrowser, 999), /TIMESTAMP_REGRESSION/);
  assert.throws(() => normalizeBrowserSnapshot(browserRaw({ rtt_ms: NaN }), null, 1_000), /NON_FINITE/);
  assert.throws(() => normalizeSfuSnapshot(sfuRaw({ active_resources: Infinity }), null, 1_000), /NON_FINITE/);
  assert.throws(() => normalizeBrowserSnapshot({ rtt_ms: 1 }, null, 1_000), /MISSING_FIELD/);
});

test('append bounded sample returns a new series and rejects the configured hard cap without dropping samples', () => {
  const series = [{ timestamp_ms: 1 }];
  const appended = appendBoundedSample(series, { timestamp_ms: 2 }, 2);

  assert.deepEqual(series, [{ timestamp_ms: 1 }]);
  assert.deepEqual(appended, [{ timestamp_ms: 1 }, { timestamp_ms: 2 }]);
  assert.throws(() => appendBoundedSample(appended, { timestamp_ms: 3 }, 2), /SAMPLE_LIMIT_EXCEEDED/);
});

test('append bounded sample accepts normalized snapshots without leaking their private counter baseline', () => {
  const snapshot = normalizeBrowserSnapshot(browserRaw(), null, 1_000);
  const series = appendBoundedSample([], snapshot, 1);

  assert.equal(series.length, 1);
  assert.deepEqual(series[0], snapshot);
  assert.deepEqual(Object.getOwnPropertySymbols(series[0]), []);
});

test('phase evaluation reports all required summary percentiles and passes values equal to their threshold', () => {
  const result = evaluatePhaseMetrics([
    { timestamp_ms: 1_000, rtt_ms: 20, jitter_ms: 2, loss_ratio: 0.01, media_delta: 10, drain_duration_ms: 100 },
    { timestamp_ms: 1_250, rtt_ms: 30, jitter_ms: 3, loss_ratio: 0.02, media_delta: 12, drain_duration_ms: 100 },
    { timestamp_ms: 1_500, rtt_ms: 40, jitter_ms: 4, loss_ratio: 0.03, media_delta: 15, drain_duration_ms: 100 },
  ], {
    minimum_samples: 3,
    max_rtt_ms: 40,
    max_jitter_ms: 4,
    max_loss_ratio: 0.03,
    min_media_delta: 10,
    max_drain_duration_ms: 100,
    max_sample_gap_ms: 250,
  });

  assert.equal(result.outcome, 'PASS');
  assert.deepEqual(result.summaries.rtt_ms, {
    sample_count: 3,
    missing_count: 0,
    min: 20,
    max: 40,
    p50: 30,
    p95: 40,
    p99: 40,
  });
  assert.deepEqual(result.failures, []);
});

test('phase evaluation fails only when a threshold is exceeded', () => {
  const result = evaluatePhaseMetrics([
    { timestamp_ms: 1_000, rtt_ms: 20, media_delta: 10 },
    { timestamp_ms: 1_250, rtt_ms: 41, media_delta: 10 },
  ], { minimum_samples: 2, max_rtt_ms: 40, min_media_delta: 10 });

  assert.equal(result.outcome, 'FAIL');
  assert.deepEqual(result.failures, [{ metric: 'rtt_ms', statistic: 'p95', threshold: 40, value: 41 }]);
});

test('phase evaluation treats missing evidence insufficient samples and sampling gaps as incomplete', () => {
  const missing = evaluatePhaseMetrics([
    { timestamp_ms: 1_000, rtt_ms: 20 },
    { timestamp_ms: 1_250, rtt_ms: 21 },
  ], { minimum_samples: 2, max_rtt_ms: 30, min_media_delta: 1 });
  const insufficient = evaluatePhaseMetrics([
    { timestamp_ms: 1_000, rtt_ms: 20, media_delta: 1 },
  ], { minimum_samples: 2, max_rtt_ms: 30, min_media_delta: 1 });
  const gap = evaluatePhaseMetrics([
    { timestamp_ms: 1_000, rtt_ms: 20, media_delta: 1 },
    { timestamp_ms: 1_501, rtt_ms: 20, media_delta: 1 },
  ], { minimum_samples: 2, max_rtt_ms: 30, min_media_delta: 1, max_sample_gap_ms: 500 });

  assert.equal(missing.outcome, 'INCOMPLETE');
  assert.deepEqual(missing.incomplete, [{ metric: 'media_delta', reason: 'missing evidence' }]);
  assert.equal(insufficient.outcome, 'INCOMPLETE');
  assert.deepEqual(insufficient.incomplete, [{ metric: 'series', reason: 'insufficient samples', required: 2, actual: 1 }]);
  assert.equal(gap.outcome, 'INCOMPLETE');
  assert.deepEqual(gap.incomplete, [{ metric: 'series', reason: 'sampling gap', maximum_ms: 500, actual_ms: 501 }]);
});

test('phase evaluation classifies invalid normalized input as error instead of manufacturing a value', () => {
  const result = evaluatePhaseMetrics([
    { timestamp_ms: 1_000, rtt_ms: 20, media_delta: 1 },
    { timestamp_ms: 999, rtt_ms: 21, media_delta: 1 },
  ], { minimum_samples: 2, max_rtt_ms: 30, min_media_delta: 1 });

  assert.equal(result.outcome, 'ERROR');
  assert.deepEqual(result.errors, [{ code: 'TIMESTAMP_REGRESSION', index: 1 }]);
});
