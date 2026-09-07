'use strict';

const { LIMITS } = require('./constants');

const COUNTERS = Symbol('metrics counters');
const PERCENTILES = Object.freeze({ p50: 0.50, p95: 0.95, p99: 0.99 });
const METRIC_FIELDS = Object.freeze([
  'rtt_ms',
  'jitter_ms',
  'loss_ratio',
  'media_delta',
  'media_rate_per_second',
  'packets_lost_delta',
  'nack_delta',
  'nack_rate_per_second',
  'drain_duration_ms',
  'active_resources',
  'turn_allocations',
]);
const THRESHOLDS = Object.freeze([
  Object.freeze({ field: 'max_rtt_ms', metric: 'rtt_ms', statistic: 'p95', comparison: 'maximum' }),
  Object.freeze({ field: 'max_jitter_ms', metric: 'jitter_ms', statistic: 'p95', comparison: 'maximum' }),
  Object.freeze({ field: 'max_loss_ratio', metric: 'loss_ratio', statistic: 'p95', comparison: 'maximum' }),
  Object.freeze({ field: 'min_media_delta', metric: 'media_delta', statistic: 'min', comparison: 'minimum' }),
  Object.freeze({ field: 'max_drain_duration_ms', metric: 'drain_duration_ms', statistic: 'max', comparison: 'maximum' }),
]);

class MetricError extends Error {
  constructor(code, message) {
    super(`${code}: ${message}`);
    this.name = 'MetricError';
    this.code = code;
  }
}

function nearestRank(samples, percentile) {
  const values = normalizeNumericArray(samples, 'samples');
  if (values.length === 0) {
    throw new MetricError('INSUFFICIENT_SAMPLES', 'samples must not be empty');
  }
  if (typeof percentile !== 'number' || !Number.isFinite(percentile) || percentile <= 0 || percentile > 1) {
    throw new MetricError('INVALID_PERCENTILE', 'percentile must be finite and in (0, 1]');
  }
  const sorted = values.toSorted((left, right) => left - right);
  return sorted[Math.ceil(percentile * sorted.length) - 1];
}

function normalizeBrowserSnapshot(raw, previous, timestampMs) {
  const input = requirePlainObject(raw, 'raw');
  const timestamp = requireTimestamp(timestampMs);
  const rttMs = requireGauge(input, 'rtt_ms');
  const jitterMs = requireGauge(input, 'jitter_ms');
  const counters = {
    media_packets: requireCounter(input, 'media_packets'),
    packets_lost: requireCounter(input, 'packets_lost'),
    nack_count: requireCounter(input, 'nack_count'),
  };
  const previousCounters = normalizePrevious(previous, 'browser', timestamp, Object.keys(counters));
  const rates = previousCounters === null
    ? emptyBrowserRates()
    : calculateBrowserRates(counters, previousCounters.counters, timestamp - previousCounters.timestamp_ms);

  return freezeSnapshot({
    source: 'browser',
    timestamp_ms: timestamp,
    rtt_ms: rttMs,
    jitter_ms: jitterMs,
    ...rates,
  }, counters);
}

function normalizeSfuSnapshot(raw, previous, timestampMs) {
  const input = requirePlainObject(raw, 'raw');
  const timestamp = requireTimestamp(timestampMs);
  const activeResources = requireGauge(input, 'active_resources');
  const turnAllocations = requireGauge(input, 'turn_allocations');
  const counters = { media_packets: requireCounter(input, 'media_packets') };
  const previousCounters = normalizePrevious(previous, 'sfu', timestamp, Object.keys(counters));
  const rates = previousCounters === null
    ? emptySfuRates()
    : calculateSfuRates(counters, previousCounters.counters, timestamp - previousCounters.timestamp_ms);

  return freezeSnapshot({
    source: 'sfu',
    timestamp_ms: timestamp,
    active_resources: activeResources,
    turn_allocations: turnAllocations,
    ...rates,
  }, counters);
}

function appendBoundedSample(series, sample, maxSamples = LIMITS.MAX_SAMPLES_PER_CASE) {
  if (!Array.isArray(series)) {
    throw new MetricError('MALFORMED_SERIES', 'series must be an array');
  }
  const cap = requireSampleCap(maxSamples);
  if (series.length >= cap) {
    throw new MetricError('SAMPLE_LIMIT_EXCEEDED', `sample limit is ${cap}`);
  }
  const copiedSeries = series.map((entry) => copyPlainData(entry, 'series sample'));
  copiedSeries.push(copyPlainData(sample, 'sample'));
  return Object.freeze(copiedSeries);
}

function evaluatePhaseMetrics(series, thresholdProfile) {
  const inspected = inspectSeries(series);
  if (!inspected.valid) {
    return freezeResult({ outcome: 'ERROR', summaries: {}, failures: [], incomplete: [], errors: [inspected.error] });
  }

  const profile = inspectThresholdProfile(thresholdProfile);
  if (!profile.valid) {
    if (profile.error.code === 'MISSING_THRESHOLD_PROFILE') {
      return freezeResult({
        outcome: 'INCOMPLETE', summaries: summarizeSeries(inspected.samples, []), failures: [],
        incomplete: [{ metric: 'threshold_profile', reason: 'missing threshold profile' }], errors: [],
      });
    }
    return freezeResult({ outcome: 'ERROR', summaries: {}, failures: [], incomplete: [], errors: [profile.error] });
  }

  const requiredMetrics = profile.thresholds.map((threshold) => threshold.metric);
  const summaries = summarizeSeries(inspected.samples, requiredMetrics);
  const incomplete = [];
  const failures = [];
  const minimumSamples = profile.minimum_samples;
  if (inspected.samples.length < minimumSamples) {
    incomplete.push({ metric: 'series', reason: 'insufficient samples', required: minimumSamples, actual: inspected.samples.length });
  }
  if (profile.max_sample_gap_ms !== null) {
    const gap = firstSamplingGap(inspected.samples, profile.max_sample_gap_ms);
    if (gap !== null) {
      incomplete.push({ metric: 'series', reason: 'sampling gap', maximum_ms: profile.max_sample_gap_ms, actual_ms: gap });
    }
  }
  for (const threshold of profile.thresholds) {
    const summary = summaries[threshold.metric];
    if (!summary || summary.missing_count > 0) {
      incomplete.push({ metric: threshold.metric, reason: 'missing evidence' });
      continue;
    }
    if (inspected.samples.length >= minimumSamples && summary.sample_count < minimumSamples) {
      incomplete.push({ metric: threshold.metric, reason: 'insufficient samples', required: minimumSamples, actual: summary.sample_count });
      continue;
    }
    const value = summary[threshold.statistic];
    const exceeds = threshold.comparison === 'maximum' ? value > threshold.value : value < threshold.value;
    if (exceeds) {
      failures.push({ metric: threshold.metric, statistic: threshold.statistic, threshold: threshold.value, value });
    }
  }
  const outcome = incomplete.length > 0 ? 'INCOMPLETE' : failures.length > 0 ? 'FAIL' : 'PASS';
  return freezeResult({ outcome, summaries, failures, incomplete, errors: [] });
}

function emptyBrowserRates() {
  return {
    media_delta: null,
    media_rate_per_second: null,
    packets_lost_delta: null,
    nack_delta: null,
    nack_rate_per_second: null,
    loss_ratio: null,
  };
}

function calculateBrowserRates(current, previous, elapsedMs) {
  const mediaDelta = counterDelta(current.media_packets, previous.media_packets, 'media_packets');
  const lostDelta = counterDelta(current.packets_lost, previous.packets_lost, 'packets_lost');
  const nackDelta = counterDelta(current.nack_count, previous.nack_count, 'nack_count');
  const seconds = elapsedSeconds(elapsedMs);
  const deliveredAndLost = mediaDelta + lostDelta;
  return {
    media_delta: mediaDelta,
    media_rate_per_second: mediaDelta / seconds,
    packets_lost_delta: lostDelta,
    nack_delta: nackDelta,
    nack_rate_per_second: nackDelta / seconds,
    loss_ratio: deliveredAndLost === 0 ? 0 : lostDelta / deliveredAndLost,
  };
}

function emptySfuRates() {
  return { media_delta: null, media_rate_per_second: null };
}

function calculateSfuRates(current, previous, elapsedMs) {
  const mediaDelta = counterDelta(current.media_packets, previous.media_packets, 'media_packets');
  return { media_delta: mediaDelta, media_rate_per_second: mediaDelta / elapsedSeconds(elapsedMs) };
}

function normalizePrevious(previous, source, timestamp, counterNames) {
  if (previous === null || previous === undefined) return null;
  if (!previous || typeof previous !== 'object' || previous.source !== source || previous[COUNTERS] === undefined) {
    throw new MetricError('MALFORMED_PREVIOUS', `previous ${source} snapshot is invalid`);
  }
  const previousTimestamp = requireTimestamp(previous.timestamp_ms);
  if (timestamp <= previousTimestamp) {
    throw new MetricError(timestamp < previousTimestamp ? 'TIMESTAMP_REGRESSION' : 'TIMESTAMP_NON_MONOTONIC', 'timestamp must advance');
  }
  const counters = previous[COUNTERS];
  for (const name of counterNames) {
    if (!Object.hasOwn(counters, name) || !Number.isSafeInteger(counters[name]) || counters[name] < 0) {
      throw new MetricError('MALFORMED_PREVIOUS', `previous counter ${name} is invalid`);
    }
  }
  return { timestamp_ms: previousTimestamp, counters };
}

function counterDelta(current, previous, name) {
  const delta = current - previous;
  if (delta < 0) throw new MetricError('COUNTER_RESET', `counter ${name} decreased`);
  return delta;
}

function elapsedSeconds(elapsedMs) {
  if (!Number.isFinite(elapsedMs) || elapsedMs <= 0) {
    throw new MetricError('TIMESTAMP_NON_MONOTONIC', 'timestamp must advance');
  }
  return elapsedMs / 1_000;
}

function inspectSeries(series) {
  if (!Array.isArray(series)) return invalidSeries('MALFORMED_SERIES', undefined);
  const samples = [];
  let previousTimestamp = null;
  for (let index = 0; index < series.length; index += 1) {
    let sample;
    try {
      sample = copyPlainData(series[index], 'series sample');
    } catch (error) {
      return invalidSeries(error.code || 'MALFORMED_SAMPLE', index);
    }
    const timestamp = sample.timestamp_ms;
    if (typeof timestamp !== 'number' || !Number.isFinite(timestamp)) return invalidSeries('NON_FINITE', index);
    if (previousTimestamp !== null && timestamp < previousTimestamp) return invalidSeries('TIMESTAMP_REGRESSION', index);
    previousTimestamp = timestamp;
    for (const field of METRIC_FIELDS) {
      if (sample[field] !== undefined && sample[field] !== null &&
          (typeof sample[field] !== 'number' || !Number.isFinite(sample[field]))) {
        return invalidSeries('NON_FINITE', index);
      }
    }
    samples.push(sample);
  }
  return { valid: true, samples };
}

function invalidSeries(code, index) {
  return { valid: false, error: index === undefined ? { code } : { code, index } };
}

function inspectThresholdProfile(value) {
  if (value === null || value === undefined) return { valid: false, error: { code: 'MISSING_THRESHOLD_PROFILE' } };
  let profile;
  try {
    profile = copyPlainData(value, 'threshold profile');
  } catch (error) {
    return { valid: false, error: { code: error.code || 'MALFORMED_THRESHOLD_PROFILE' } };
  }
  if (!Number.isSafeInteger(profile.minimum_samples) || profile.minimum_samples < 1) {
    return { valid: false, error: { code: 'MALFORMED_THRESHOLD_PROFILE' } };
  }
  let maxGap = null;
  if (profile.max_sample_gap_ms !== undefined) {
    if (typeof profile.max_sample_gap_ms !== 'number' || !Number.isFinite(profile.max_sample_gap_ms) || profile.max_sample_gap_ms < 0) {
      return { valid: false, error: { code: 'MALFORMED_THRESHOLD_PROFILE' } };
    }
    maxGap = profile.max_sample_gap_ms;
  }
  const thresholds = [];
  for (const definition of THRESHOLDS) {
    if (profile[definition.field] === undefined) continue;
    if (typeof profile[definition.field] !== 'number' || !Number.isFinite(profile[definition.field]) || profile[definition.field] < 0) {
      return { valid: false, error: { code: 'MALFORMED_THRESHOLD_PROFILE' } };
    }
    thresholds.push({ ...definition, value: profile[definition.field] });
  }
  return { valid: true, minimum_samples: profile.minimum_samples, max_sample_gap_ms: maxGap, thresholds };
}

function summarizeSeries(samples, requiredMetrics) {
  const fields = new Set(requiredMetrics);
  for (const sample of samples) {
    for (const field of METRIC_FIELDS) {
      if (sample[field] !== undefined && sample[field] !== null) fields.add(field);
    }
  }
  const summaries = {};
  for (const field of fields) {
    const values = [];
    let missingCount = 0;
    for (const sample of samples) {
      if (sample[field] === undefined || sample[field] === null) missingCount += 1;
      else values.push(sample[field]);
    }
    summaries[field] = values.length === 0
      ? { sample_count: 0, missing_count: missingCount, min: null, max: null, p50: null, p95: null, p99: null }
      : {
        sample_count: values.length,
        missing_count: missingCount,
        min: Math.min(...values),
        max: Math.max(...values),
        p50: nearestRank(values, PERCENTILES.p50),
        p95: nearestRank(values, PERCENTILES.p95),
        p99: nearestRank(values, PERCENTILES.p99),
      };
  }
  return summaries;
}

function firstSamplingGap(samples, maxGapMs) {
  for (let index = 1; index < samples.length; index += 1) {
    const gap = samples[index].timestamp_ms - samples[index - 1].timestamp_ms;
    if (gap > maxGapMs) return gap;
  }
  return null;
}

function requireSampleCap(value) {
  if (!Number.isSafeInteger(value) || value < 1 || value > LIMITS.MAX_SAMPLES_PER_CASE) {
    throw new MetricError('INVALID_SAMPLE_CAP', `sample cap must be between 1 and ${LIMITS.MAX_SAMPLES_PER_CASE}`);
  }
  return value;
}

function requireTimestamp(value) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new MetricError('NON_FINITE', 'timestamp_ms must be finite');
  }
  return value;
}

function requireGauge(object, name) {
  const value = readOwn(object, name);
  if (value === undefined) throw new MetricError('MISSING_FIELD', `missing ${name}`);
  if (typeof value !== 'number' || !Number.isFinite(value)) throw new MetricError('NON_FINITE', `${name} must be finite`);
  if (value < 0) throw new MetricError('NEGATIVE_VALUE', `${name} must not be negative`);
  return value;
}

function requireCounter(object, name) {
  const value = requireGauge(object, name);
  if (!Number.isSafeInteger(value)) throw new MetricError('MALFORMED_COUNTER', `${name} must be a safe integer`);
  return value;
}

function requirePlainObject(value, name) {
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
      (Object.getPrototypeOf(value) !== Object.prototype && Object.getPrototypeOf(value) !== null)) {
    throw new MetricError('MALFORMED_INPUT', `${name} must be a plain object`);
  }
  return value;
}

function readOwn(object, name) {
  const descriptor = Object.getOwnPropertyDescriptor(object, name);
  if (!descriptor) return undefined;
  if (!Object.hasOwn(descriptor, 'value')) throw new MetricError('MALFORMED_INPUT', `${name} must not be an accessor`);
  return descriptor.value;
}

function copyPlainData(value, name, ancestors = new Set()) {
  if (value === null || typeof value !== 'object') {
    if (typeof value === 'number' && !Number.isFinite(value)) throw new MetricError('NON_FINITE', `${name} contains non-finite number`);
    return value;
  }
  if (ancestors.has(value)) throw new MetricError('MALFORMED_INPUT', `${name} must not be cyclic`);
  ancestors.add(value);
  try {
    if (Array.isArray(value)) {
      const copied = [];
      for (let index = 0; index < value.length; index += 1) {
        const descriptor = Object.getOwnPropertyDescriptor(value, index);
        if (!descriptor || !Object.hasOwn(descriptor, 'value')) throw new MetricError('MALFORMED_INPUT', `${name} must not contain accessors or holes`);
        copied.push(copyPlainData(descriptor.value, name, ancestors));
      }
      return Object.freeze(copied);
    }
    requirePlainObject(value, name);
    if (Object.getOwnPropertySymbols(value).some((symbol) => symbol !== COUNTERS)) {
      throw new MetricError('MALFORMED_INPUT', `${name} must not contain symbols`);
    }
    const copied = {};
    for (const key of Object.keys(value)) {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (!descriptor || !Object.hasOwn(descriptor, 'value')) throw new MetricError('MALFORMED_INPUT', `${name} must not contain accessors`);
      copied[key] = copyPlainData(descriptor.value, name, ancestors);
    }
    return Object.freeze(copied);
  } finally {
    ancestors.delete(value);
  }
}

function normalizeNumericArray(value, name) {
  if (!Array.isArray(value)) throw new MetricError('MALFORMED_INPUT', `${name} must be an array`);
  return value.map((entry, index) => {
    const descriptor = Object.getOwnPropertyDescriptor(value, index);
    if (!descriptor || !Object.hasOwn(descriptor, 'value')) throw new MetricError('MALFORMED_INPUT', `${name} must not contain accessors or holes`);
    if (typeof descriptor.value !== 'number' || !Number.isFinite(descriptor.value)) {
      throw new MetricError('NON_FINITE', `${name}[${index}] must be finite`);
    }
    return descriptor.value;
  });
}

function freezeSnapshot(snapshot, counters) {
  Object.defineProperty(snapshot, COUNTERS, {
    value: Object.freeze({ ...counters }),
    enumerable: false,
    writable: false,
    configurable: false,
  });
  return Object.freeze(snapshot);
}

function freezeResult(result) {
  return deepFreeze(result);
}

function deepFreeze(value, seen = new Set()) {
  if (!value || typeof value !== 'object' || seen.has(value)) return value;
  seen.add(value);
  for (const key of Object.keys(value)) deepFreeze(value[key], seen);
  return Object.freeze(value);
}

module.exports = {
  nearestRank,
  normalizeBrowserSnapshot,
  normalizeSfuSnapshot,
  appendBoundedSample,
  evaluatePhaseMetrics,
};
