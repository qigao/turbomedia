'use strict';

const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const { performance } = require('node:perf_hooks');
const { createHttpClient, httpError, positiveLimit } = require('./http_client');
const { hashCanonical } = require('./canonical_json');
const { createContractValidator, validateContract, LIMITS } = require('./contracts');

const validator = createContractValidator(path.join(__dirname, '..', 'schemas'));
const DEFAULT_WAIT_TIMEOUT_MS = 30_000;
const DEFAULT_POLL_INTERVAL_MS = 100;
const MAX_PARTICIPANTS = 2;
const MAX_MEDIA_RESOURCES = LIMITS.MAX_CASES * MAX_PARTICIPANTS;
const GAUGES = Object.freeze(['room_count', 'session_count', 'published_track_count']);

function fail(code, operation) { throw httpError(code, operation, 'sfu'); }
function object(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }
function counter(value) { return Number.isSafeInteger(value) && value >= 0; }
function identifier(value) { return typeof value === 'string' && value.length > 0 && value.length <= 128; }
function entity(response, name, operation) {
  if (!object(response.body) || response.body.ok !== true || (name && !object(response.body[name]))) {
    fail('SFU_INVALID_RESPONSE', operation);
  }
  return name ? response.body[name] : response.body;
}
function nodeSnapshot(stats) {
  if (!identifier(stats.node_id) || typeof stats.draining !== 'boolean' || !GAUGES.every((key) => counter(stats[key]))) {
    fail('SFU_INVALID_RESPONSE', 'node_stats');
  }
  return Object.freeze({ node_id: stats.node_id, ...Object.fromEntries(GAUGES.map((key) => [key, stats[key]])) });
}
function credential(token, required = false) {
  if (token === undefined && !required) return undefined;
  if (!validateContract(validator, 'sfu-token', token).valid) fail('SFU_INVALID_TOKEN', 'token');
  return token.token;
}
function caseKey(context) {
  if (!object(context) || !['run_id', 'case_id', 'room_id', 'publisher_id', 'viewer_id'].every((key) => identifier(context[key]))) {
    fail('SFU_INVALID_CONTEXT', 'context');
  }
  return hashCanonical({ run_id: context.run_id, case_id: context.case_id, room_id: context.room_id,
    publisher_id: context.publisher_id, viewer_id: context.viewer_id });
}

/**
 * Single runner/owner adapter. The case machine owns workflow state; these bounded
 * caches retain only confirmed setup/track/delete receipts. HTTP failures never
 * advance receipts. Callers explicitly retry cleanup and preserve its failures.
 * Context uses Task 2 run_id/case_id plus room_id, publisher_id, viewer_id and
 * publisher_session_id. Tokens are validated Task 3 provider envelopes, in memory.
 */
function createSfuAdapter(options = {}) {
  const client = createHttpClient(options);
  const waitTimeoutMs = positiveLimit(options.waitTimeoutMs ?? DEFAULT_WAIT_TIMEOUT_MS, 'wait_timeout');
  const pollIntervalMs = positiveLimit(options.pollIntervalMs ?? DEFAULT_POLL_INTERVAL_MS, 'poll_interval');
  const attached = new Set();
  const confirmedTracks = new Map();
  const deleted = new Set();
  let setupReceipt = 'none';

  async function command(type, fields, token, signal, context) {
    const body = { type, ...fields };
    if (context) {
      caseKey(context);
      body.correlation_id = context.case_id;
      body.message_id = `msg-${hashCanonical({ run_id: context.run_id, case_id: context.case_id, command: body })}`;
    }
    const response = await client.request({ segments: ['api', 'v1', 'commands'], method: 'POST', body,
      token: credential(token, Boolean(context)), signal, operation: type });
    entity(response, null, type);
    return response;
  }

  async function poll(operation, timeoutCode, signal, observe) {
    const controller = new AbortController();
    const abort = () => controller.abort();
    if (signal?.aborted) fail('HTTP_ABORTED', operation);
    signal?.addEventListener('abort', abort, { once: true });
    const deadline = performance.now() + waitTimeoutMs;
    let timedOut = false;
    const timer = setTimeout(() => { timedOut = true; controller.abort(); }, waitTimeoutMs);
    try {
      while (true) {
        const result = await observe(controller.signal);
        if (performance.now() >= deadline) fail(timeoutCode, operation);
        if (result !== null) return result;
        await delay(Math.min(pollIntervalMs, deadline - performance.now()), undefined, { signal: controller.signal });
      }
    } catch (error) {
      if (signal?.aborted) fail('HTTP_ABORTED', operation);
      if (timedOut) fail(timeoutCode, operation);
      throw error;
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
      controller.abort();
    }
  }

  const adapter = {
    async preflight(signal) {
      setupReceipt = 'none';
      const health = entity(await client.request({ segments: ['health'], signal, operation: 'health' }), 'node_stats', 'health');
      nodeSnapshot(health);
      const ready = entity(await client.request({ segments: ['ready'], signal, operation: 'ready' }), null, 'ready');
      if (ready.draining !== false || health.draining !== false) fail('SFU_NOT_READY', 'preflight');
      setupReceipt = 'ready';
      return Object.freeze({ ready: true, node_id: health.node_id });
    },
    async captureBaseline(signal) {
      if (setupReceipt !== 'ready' || attached.size) fail('SFU_SETUP_ORDER', 'capture_baseline');
      const baseline = nodeSnapshot(await adapter.getNodeStats(undefined, signal));
      setupReceipt = 'baseline';
      return baseline;
    },
    async attachRoom(context, token, signal) {
      if (setupReceipt !== 'baseline') fail('SFU_SETUP_ORDER', 'attach_room');
      const key = caseKey(context);
      if (!attached.has(key) && attached.size >= LIMITS.MAX_CASES) fail('SFU_RESOURCE_LIMIT', 'attach_room');
      const response = await command('attach_room', { room_id: context.room_id,
        participant_id: context.publisher_id, max_participants: MAX_PARTICIPANTS }, token, signal, context);
      attached.add(key);
      return response.body;
    },
    async waitForPublishedTracks(context, token, expectedTracks, signal) {
      const key = caseKey(context);
      if (!attached.has(key)) fail('SFU_SETUP_ORDER', 'published_tracks');
      // Existing server.c numbers all registrations, not video-only registrations.
      // The acceptance publisher contract is exactly audio then video; no other
      // order is inferred from counts, which are the existing public observations.
      if (!Array.isArray(expectedTracks) || expectedTracks.length !== MAX_PARTICIPANTS ||
          expectedTracks[0] !== 'audio' || expectedTracks[1] !== 'video' || !identifier(context.publisher_session_id)) {
        fail('SFU_INVALID_TRACK_CONTRACT', 'published_tracks');
      }
      return poll('published_tracks', 'SFU_TRACKS_TIMEOUT', signal, async (pollSignal) => {
        const session = await adapter.getWebRtcSession(context, token, context.publisher_session_id, pollSignal);
        if (session.participant_id !== context.publisher_id) fail('SFU_INVALID_RESPONSE', 'published_tracks');
        const room = entity(await command('get_room_stats', { room_id: context.room_id,
          participant_id: context.publisher_id }, token, pollSignal), 'room_stats', 'published_tracks');
        if (room.room_id !== context.room_id || !counter(room.participant_count) || !counter(room.published_track_count)) {
          fail('SFU_INVALID_RESPONSE', 'published_tracks');
        }
        if (room.participant_count !== 1 ||
            session.remote_track_count > expectedTracks.length || room.published_track_count > expectedTracks.length) {
          fail('SFU_INVALID_TRACK_CONTRACT', 'published_tracks');
        }
        if (session.remote_track_count !== expectedTracks.length || room.published_track_count !== expectedTracks.length) return null;
        const ids = Object.freeze([`${context.publisher_id}-audio`, `${context.publisher_id}-video-1`]);
        confirmedTracks.set(key, { room_id: context.room_id, participant_id: context.publisher_id,
          session_id: context.publisher_session_id, ids });
        return ids;
      });
    },
    async setViewerSubscriptions(context, token, trackIds, signal) {
      const receipt = confirmedTracks.get(caseKey(context));
      if (!receipt || receipt.session_id !== context.publisher_session_id || !Array.isArray(trackIds) ||
          trackIds.length !== receipt.ids.length || !trackIds.every((id, index) => id === receipt.ids[index])) {
        fail('SFU_TRACKS_UNCONFIRMED', 'set_track_subscription');
      }
      const results = [];
      for (const trackId of trackIds) {
        results.push((await command('set_track_subscription', { room_id: context.room_id,
          participant_id: context.viewer_id, receiver_participant_id: context.viewer_id,
          track_id: trackId, enabled: true }, token, signal, context)).body);
      }
      return results;
    },
    async getWebRtcSession(context, token, sessionId, signal) {
      const response = await client.request({ segments: ['api', 'v1', 'rooms', context.room_id, 'webrtc_sessions', sessionId],
        token: credential(token), signal, operation: 'get_webrtc_session' });
      const session = entity(response, 'webrtc_session', 'get_webrtc_session');
      if (session.room_id !== context.room_id || session.session_id !== sessionId || !identifier(session.participant_id) ||
          !counter(session.remote_track_count) || !counter(session.remote_frame_count) || !counter(session.relay_track_count)) {
        fail('SFU_INVALID_RESPONSE', 'get_webrtc_session');
      }
      return session;
    },
    async getNodeStats(token, signal) {
      const stats = entity(await command('get_node_stats', {}, token, signal), 'node_stats', 'get_node_stats');
      nodeSnapshot(stats);
      return stats;
    },
    async deleteMediaResource(resource, token, signal) {
      if (!object(resource) || !['whip', 'whep'].includes(resource.kind)) fail('SFU_INVALID_RESOURCE', 'delete_media_resource');
      const segments = [resource.kind, resource.room_id, resource.participant_id, 'sessions', resource.session_id];
      const key = client.resourceKey(segments);
      if (!deleted.has(key) && deleted.size >= MAX_MEDIA_RESOURCES) fail('SFU_RESOURCE_LIMIT', 'delete_media_resource');
      const response = await client.request({ segments, method: 'DELETE', token: credential(token, true), signal,
        statuses: deleted.has(key) ? [204, 404] : [204], responseType: 'text', operation: 'delete_media_resource' });
      deleted.add(key);
      for (const [caseId, receipt] of confirmedTracks) {
        if (receipt.room_id === resource.room_id && receipt.participant_id === resource.participant_id &&
            receipt.session_id === resource.session_id) confirmedTracks.delete(caseId);
      }
      return Object.freeze({ status: response.status, already_deleted: response.status === 404 });
    },
    async detachRoom(context, token, signal) {
      const key = caseKey(context);
      const response = await command('detach_room', { room_id: context.room_id,
        participant_id: context.publisher_id }, token, signal, context);
      attached.delete(key);
      confirmedTracks.delete(key);
      return response.body;
    },
    async waitForBaseline(baseline, token, signal) {
      if (!object(baseline) || !identifier(baseline.node_id) || !GAUGES.every((key) => counter(baseline[key]))) {
        fail('SFU_INVALID_BASELINE', 'wait_for_baseline');
      }
      const expected = { ...baseline };
      return poll('wait_for_baseline', 'SFU_BASELINE_TIMEOUT', signal, async (pollSignal) => {
        const snapshot = nodeSnapshot(await adapter.getNodeStats(token, pollSignal));
        if (snapshot.node_id !== expected.node_id) fail('SFU_NODE_CHANGED', 'wait_for_baseline');
        return GAUGES.every((key) => snapshot[key] === expected[key]) ? snapshot : null;
      });
    },
  };
  return Object.freeze(adapter);
}

module.exports = { createSfuAdapter };
