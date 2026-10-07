'use strict';

const http = require('node:http');
const https = require('node:https');
const { isIP } = require('node:net');
const { Builder, logging } = require('selenium-webdriver');
const { Command, Name } = require('selenium-webdriver/lib/command');
const { createRedactor } = require('./redaction');

const MAX_COMMAND_MS = 45_000;
const MAX_INPUT_BYTES = 65_536;
const MAX_RESULT_BYTES = 1_048_576;
// Two pages may each report 512 fixed-enum cleanup failures plus local failures.
// This separate budget is not reduced by the caller's sampling result cap.
const MAX_CLEANUP_BYTES = 262_144;
const MAX_STATS = 512;
const MAX_DEPTH = 12;
const MAX_NODES = 50_000;
const MAX_TEXT = 8_192;
const MAX_SECRETS = 1_536;
const MAX_SECRET_BYTES = 1_048_576;
const HTTP_REDIRECT_MIN = 300;
const HTTP_REDIRECT_MAX = 399;
const METHODS = Object.freeze(['configure', 'startPublisher', 'startViewer', 'restartIce', 'snapshot', 'close']);
const ROLES = ['publisher', 'viewer'];
const HASH = /^[a-f0-9]{64}$/;
const CANDIDATE_TYPES = ['host', 'srflx', 'prflx', 'relay'];
const STATS_FIELDS = new Set(('id type timestamp kind mediaType transportId codecId ssrc mid trackIdentifier ' +
  'selectedCandidatePairId localCandidateId remoteCandidateId state nominated selected candidateType protocol relayProtocol address ip port ' +
  'networkType currentRoundTripTime totalRoundTripTime roundTripTime roundTripTimeMeasurements availableOutgoingBitrate availableIncomingBitrate ' +
  'bytesSent bytesReceived packetsSent packetsReceived packetsLost jitter nackCount pliCount firCount framesEncoded framesDecoded framesReceived ' +
  'framesSent framesDropped keyFramesDecoded keyFramesEncoded frameWidth frameHeight framesPerSecond totalDecodeTime totalEncodeTime ' +
  'totalSamplesReceived totalSamplesDuration totalAudioEnergy audioLevel concealedSamples silentConcealedSamples jitterBufferDelay ' +
  'jitterBufferEmittedCount mimeType clockRate channels payloadType dtlsState iceState iceRole dtlsRole requestsSent responsesReceived consentRequestsSent').split(' '));
const STATS_TEXT_FIELDS = new Set(('id type kind mediaType transportId codecId mid trackIdentifier selectedCandidatePairId ' +
  'localCandidateId remoteCandidateId state candidateType protocol relayProtocol address ip networkType mimeType dtlsState iceState iceRole dtlsRole').split(' '));
const NULLABLE_API_TEXT = ['location', 'etag', 'session_id'];
const API_STATUSES = ['post_status', 'trickle_status', 'restart_status', 'stale_etag_status', 'delete_status'];
const PAGE_CLEANUP_CODES = new Set(['OPERATION_TIMEOUT', 'OPERATION_CANCELLED', 'EXTERNAL_OPERATION_FAILED',
  'HTTP_BODY_LIMIT', 'HTTP_INVALID_ETAG', 'HTTP_INVALID_LOCATION', 'HTTP_UNEXPECTED_STATUS', 'HTTP_INVALID_CONTENT_TYPE', 'HTTP_INVALID_BODY']);
const PAGE_CLEANUP_OPERATIONS = new Set(['delete', 'closePeer', 'stopTrack', 'stopAudio', 'detachVideo', 'closeAudio']);
// These are Task 8's fixed error codes, not arbitrary browser diagnostics.
// Categories describe evidence for the controller; they do not decide outcome.
const PAGE_FAILURE_CATEGORIES = Object.freeze({
  HTTP_BODY_LIMIT: 'assertion', HTTP_INVALID_ETAG: 'assertion', HTTP_INVALID_LOCATION: 'assertion',
  HTTP_UNEXPECTED_STATUS: 'assertion', HTTP_INVALID_CONTENT_TYPE: 'assertion', HTTP_INVALID_BODY: 'assertion',
  HTTP_UNEXPECTED_ETAG: 'assertion', HTTP_UNCHANGED_ETAG: 'assertion', INVALID_RESTART_FRAGMENT: 'assertion',
  UNCHANGED_REMOTE_ICE: 'assertion', UNCHANGED_LOCAL_ICE: 'assertion', INVALID_RESTART_MEDIA: 'assertion',
  INVALID_SDP: 'assertion', INVALID_MEDIA_ORDER: 'assertion', UNEXPECTED_REMOTE_TRACK: 'assertion',
  CRYPTO_UNAVAILABLE: 'missing_evidence', FRAME_EVIDENCE_UNAVAILABLE: 'missing_evidence',
  CANVAS_UNAVAILABLE: 'missing_evidence', AUDIO_NOT_RUNNING: 'missing_evidence',
  UNSUPPORTED_ICE_TRANSPORTS: 'missing_evidence',
  INVALID_CONFIGURATION: 'harness', INVALID_PHASE: 'harness', SNAPSHOT_BUSY: 'harness', OPERATION_BUSY: 'harness',
  OPERATION_CANCELLED: 'harness', OPERATION_TIMEOUT: 'harness', EXTERNAL_OPERATION_FAILED: 'harness',
  INVALID_GENERATION: 'harness', GENERATION_LIMIT: 'harness', STATS_LIMIT: 'harness', INVALID_SYNTHETIC_MEDIA: 'harness',
});
const PAGE_FAILURE_OPERATIONS = new Set([...METHODS, ...PAGE_CLEANUP_OPERATIONS, 'trickle', 'staleEtag', 'track']);
const trustedErrors = new WeakSet();

function failure(code, operation, category = 'harness') {
  const error = Object.assign(new Error(`${operation}: ${code}`), { code, operation, category });
  trustedErrors.add(error);
  return error;
}
function fail(code, operation, category) { throw failure(code, operation, category); }
function object(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }
function counter(value) { return Number.isSafeInteger(value) && value >= 0; }
function label(value) { return typeof value === 'string' && value.length > 0 && value.length <= MAX_TEXT; }
function exact(value, fields) {
  return object(value) && Object.keys(value).every((key) => fields.includes(key));
}

// Shared source is evaluated in the browser before Selenium serializes the
// callback value, and again at the Node boundary. No accessor/toJSON is invoked.
// O(nodes + text bytes) time/space with fixed depth, node and byte budgets.
function copyBounded(value, limits) {
  let nodes = 0, bytes = 0;
  const ancestors = new Set();
  const encoder = new TextEncoder();
  const invalid = () => { throw new Error('INVALID_JSON'); };
  const charge = (value) => {
    bytes += encoder.encode(JSON.stringify(value)).byteLength;
    if (bytes > limits.bytes) invalid();
  };
  function copy(value, depth) {
    if (++nodes > limits.nodes || depth > limits.depth) invalid();
    if (value === null || typeof value === 'boolean' || (typeof value === 'number' && Number.isFinite(value))) {
      charge(value); return value;
    }
    if (typeof value === 'string') { if (value.length > limits.text) invalid(); charge(value); return value; }
    if (!value || typeof value !== 'object' || ancestors.has(value)) invalid();
    const keys = Reflect.ownKeys(value);
    if (keys.length > limits.nodes - nodes) invalid();
    ancestors.add(value);
    const array = Array.isArray(value);
    if (!array) {
      const prototype = Object.getPrototypeOf(value);
      // Cross-realm plain objects have a null parent above Object.prototype.
      if (prototype !== null && (Object.getPrototypeOf(prototype) !== null ||
          !Object.hasOwn(prototype, 'hasOwnProperty'))) invalid();
    } else if (value.length > limits.nodes || keys.length !== value.length + 1) invalid();
    const output = array ? [] : {};
    bytes += keys.length + 2;
    for (const key of keys) {
      if (array && key === 'length') continue;
      if (typeof key !== 'string' || key.length > limits.text || ['__proto__', 'constructor', 'prototype'].includes(key)) invalid();
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (!descriptor || !descriptor.enumerable || !Object.hasOwn(descriptor, 'value')) invalid();
      if (array && (!/^(0|[1-9][0-9]*)$/.test(key) || Number(key) >= value.length)) invalid();
      charge(key);
      output[key] = copy(descriptor.value, depth + 1);
    }
    ancestors.delete(value);
    if (bytes > limits.bytes) invalid();
    return Object.freeze(output);
  }
  return copy(value, 0);
}

function browserCommand(settings, operation, argument, done, copy) {
  const finish = (code) => done({ ok: false, code });
  (async () => {
    if (location.href !== settings.url) return finish('BROWSER_PAGE_URL_MISMATCH');
    const descriptor = Object.getOwnPropertyDescriptor(window, 'turboAcceptance');
    const api = descriptor && Object.hasOwn(descriptor, 'value') ? descriptor.value : null;
    if (!api || !Object.isFrozen(api) || Reflect.ownKeys(api).length !== settings.methods.length ||
        !settings.methods.every((key) => {
          const field = Object.getOwnPropertyDescriptor(api, key);
          return field && field.enumerable && Object.hasOwn(field, 'value') && typeof field.value === 'function';
        })) return finish('BROWSER_PAGE_API_INVALID');
    if (operation === 'inspect') {
      const abort = new AbortController();
      const timer = setTimeout(() => abort.abort(), settings.timeout);
      try {
        const response = await fetch(settings.url, { cache: 'no-store', credentials: 'omit', redirect: 'error', signal: abort.signal });
        if (!response.ok || !/^text\/html(?:;|$)/i.test(response.headers.get('Content-Type') || '') || !response.body) {
          return finish('BROWSER_PAGE_ASSET_INVALID');
        }
        const reader = response.body.getReader(), chunks = [];
        let bytes = 0;
        try {
          while (true) {
            const part = await reader.read();
            if (part.done) break;
            bytes += part.value.byteLength;
            if (bytes > settings.limits.bytes) { await reader.cancel(); return finish('BROWSER_PAGE_ASSET_INVALID'); }
            chunks.push(part.value);
          }
        } finally { reader.releaseLock(); }
        const body = new Uint8Array(bytes);
        let offset = 0;
        for (const chunk of chunks) { body.set(chunk, offset); offset += chunk.byteLength; }
        const digest = await crypto.subtle.digest('SHA-256', body);
        const radix = 16, width = 2;
        const sha256 = Array.from(new Uint8Array(digest), (byte) => byte.toString(radix).padStart(width, '0')).join('');
        return done({ ok: true, value: { sha256, scope: 'html-response', bytes } });
      } catch { return finish('BROWSER_PAGE_ASSET_INVALID'); }
      finally { clearTimeout(timer); abort.abort(); }
    }
    let value;
    try { value = await api[operation](argument); }
    catch (error) {
      const ownText = (key) => {
        if (!error || (typeof error !== 'object' && typeof error !== 'function')) return undefined;
        const field = Object.getOwnPropertyDescriptor(error, key);
        return field && Object.hasOwn(field, 'value') && typeof field.value === 'string' ? field.value : undefined;
      };
      const code = ownText('code'), stage = ownText('operation');
      if (settings.failureCodes.includes(code) && settings.failureOperations.includes(stage)) {
        return done({ ok: false, code, operation: stage });
      }
      return finish('BROWSER_PAGE_OPERATION_FAILED');
    }
    try { done({ ok: true, value: copy(value, settings.limits) }); }
    catch { finish('BROWSER_PAGE_RESULT_INVALID'); }
  })().catch(() => finish('BROWSER_PAGE_RESULT_INVALID'));
}
const SCRIPT = `return (${browserCommand}).call(null, arguments[0], arguments[1], arguments[2], arguments[3], ${copyBounded});`;
const PAGE_ERROR_CODES = new Set(['BROWSER_PAGE_URL_MISMATCH', 'BROWSER_PAGE_API_INVALID', 'BROWSER_PAGE_ASSET_INVALID',
  'BROWSER_PAGE_OPERATION_FAILED', 'BROWSER_PAGE_RESULT_INVALID']);

function safeUrl(value, loopback, operation) {
  try {
    if (!label(value) || /[\s\\]/.test(value)) throw Error();
    const url = new URL(value);
    if (url.username || url.password || url.search || url.hash || /[?#]/.test(value) ||
        (url.protocol !== 'https:' && !(loopback && url.protocol === 'http:' &&
          ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)))) throw Error();
    return url;
  } catch { fail(operation === 'grid' ? 'BROWSER_UNSAFE_GRID_URL' : 'BROWSER_INVALID_SOURCE', operation); }
}

function commandAgent(grid, entry) {
  // Certificate verification is an adapter invariant, including diagnostics;
  // an unrelated NODE_TLS_REJECT_UNAUTHORIZED setting cannot weaken it.
  const agent = grid.protocol === 'https:' ? new https.Agent({ keepAlive: false, rejectUnauthorized: true }) : new http.Agent({ keepAlive: false });
  const addRequest = agent.addRequest.bind(agent);
  agent.addRequest = (request, options) => {
    const controller = entry.commandController;
    const stop = () => request.destroy(new Error('BROWSER_TRANSPORT_STOPPED'));
    const emit = request.emit;
    // Gate response delivery itself: a response listener alone runs alongside
    // Selenium's redirect listener, which would still issue an unguarded request.
    // This only rejects transport redirects; Selenium still owns HTTP parsing.
    request.emit = function (event, ...args) {
      if (event === 'response' && args[0].statusCode >= HTTP_REDIRECT_MIN && args[0].statusCode <= HTTP_REDIRECT_MAX) {
        controller?.abort(failure('BROWSER_GRID_COMMAND_FAILED', 'redirect'));
        args[0].destroy(); stop(); return true;
      }
      return emit.call(this, event, ...args);
    };
    // The pinned HttpClient retries ECONNRESET, including cancellation-induced
    // resets. A command is never retryable here (POST may already have committed).
    // Normalize at the public Node agent boundary, before Selenium's listener.
    request.prependListener('error', (error) => { error.code = 'BROWSER_TRANSPORT_FAILED'; });
    if (!controller || controller.signal.aborted) { queueMicrotask(stop); return; }
    controller.signal.addEventListener('abort', stop, { once: true });
    request.once('close', () => controller.signal.removeEventListener('abort', stop));
    addRequest(request, options);
  };
  return agent;
}

function normalizedBrowser(value, operation) {
  if (!exact(value, ['name', 'version', 'platform'])) fail('BROWSER_CAPABILITY_INVALID', operation, 'missing_evidence');
  for (const field of ['name', 'version', 'platform']) {
    if (value[field] === undefined || value[field] === '') fail('BROWSER_CAPABILITY_MISSING', operation, 'missing_evidence');
    if (!label(value[field]) || value[field].trim() !== value[field] || /[\x00-\x1f\x7f]/.test(value[field])) {
      fail('BROWSER_CAPABILITY_INVALID', operation, 'missing_evidence');
    }
  }
  if (!['chrome', 'MicrosoftEdge', 'firefox', 'safari'].includes(value.name) || !/^\d+(?:\.\d+)*$/.test(value.version)) {
    fail('BROWSER_CAPABILITY_INVALID', operation, 'missing_evidence');
  }
  return Object.freeze({ name: value.name, version: value.version, platform: value.platform });
}

function validateRelayContract(value) {
  if (value === undefined) return null;
  if (!exact(value, ['schema_version', 'ip_family', 'protocol', 'relay_protocol', 'remote_candidate_types']) ||
      value.schema_version !== 1 || !['ipv4', 'ipv6'].includes(value.ip_family) || !['udp', 'tcp'].includes(value.protocol) ||
      !['udp', 'tcp', 'tls'].includes(value.relay_protocol) || !Array.isArray(value.remote_candidate_types) ||
      !value.remote_candidate_types.length || value.remote_candidate_types.length > CANDIDATE_TYPES.length ||
      new Set(value.remote_candidate_types).size !== value.remote_candidate_types.length ||
      !value.remote_candidate_types.every((type) => CANDIDATE_TYPES.includes(type))) fail('BROWSER_INPUT_INVALID', 'relay_contract');
  return value;
}

function relayEvidence(stats, contract) {
  const missing = (code) => ({ verified: false, code, category: 'missing_evidence' });
  const mismatch = (code) => ({ verified: false, code, category: 'assertion' });
  if (!contract) return missing('RELAY_CONTRACT_MISSING');
  const records = new Map();
  for (const record of stats) {
    if (!label(record.id) || records.has(record.id)) return missing('RELAY_STATS_INVALID');
    records.set(record.id, record);
  }
  const transports = stats.filter((record) => record.type === 'transport');
  const selected = transports.length ? transports.map((record) => record.selectedCandidatePairId) :
    stats.filter((record) => record.type === 'candidate-pair' && record.selected === true).map((record) => record.id);
  if (!selected.length) return missing('RELAY_PAIR_MISSING');
  const pairs = [];
  for (const id of new Set(selected)) {
    const pair = records.get(id);
    if (!pair || pair.type !== 'candidate-pair') return missing('RELAY_PAIR_MISSING');
    if (pair.state !== 'succeeded') return missing('RELAY_PAIR_NOT_SUCCEEDED');
    const local = records.get(pair.localCandidateId), remote = records.get(pair.remoteCandidateId);
    if (!local || !remote || local.type !== 'local-candidate' || remote.type !== 'remote-candidate') return missing('RELAY_CANDIDATE_MISSING');
    if (!CANDIDATE_TYPES.includes(local.candidateType) || !CANDIDATE_TYPES.includes(remote.candidateType)) return missing('RELAY_CANDIDATE_TYPE_MISSING');
    if (local.candidateType !== 'relay') return mismatch('RELAY_LOCAL_NOT_RELAY');
    if (!contract.remote_candidate_types.includes(remote.candidateType)) return mismatch('RELAY_REMOTE_TYPE_MISMATCH');
    if (!local.protocol || !remote.protocol || !local.relayProtocol) return missing('RELAY_PROTOCOL_MISSING');
    if (local.protocol !== contract.protocol || remote.protocol !== contract.protocol || local.relayProtocol !== contract.relay_protocol) {
      return mismatch('RELAY_PROTOCOL_MISMATCH');
    }
    const family = contract.ip_family === 'ipv4' ? 4 : 6;
    for (const candidate of [local, remote]) {
      const address = candidate.address ?? candidate.ip;
      if (typeof address !== 'string' || !isIP(address)) return missing('RELAY_ADDRESS_MISSING');
      if (isIP(address) !== family) return mismatch('RELAY_ADDRESS_FAMILY_MISMATCH');
      if (candidate.address && candidate.ip && candidate.address !== candidate.ip) return missing('RELAY_ADDRESS_CONFLICT');
    }
    pairs.push({ pair_id: pair.id, local_candidate_id: local.id, remote_candidate_id: remote.id,
      local_candidate_type: local.candidateType, remote_candidate_type: remote.candidateType,
      protocol: local.protocol, relay_protocol: local.relayProtocol, ip_family: contract.ip_family });
  }
  return { verified: true, pairs };
}

function validCleanup(value) {
  return Array.isArray(value) && value.length <= MAX_STATS && value.every((entry) =>
    exact(entry, ['code', 'operation']) && label(entry.code) && label(entry.operation));
}
function cleanupEvidence(value) {
  return { code: PAGE_CLEANUP_CODES.has(value.code) ? value.code : 'BROWSER_PAGE_CLEANUP_FAILED',
    operation: PAGE_CLEANUP_OPERATIONS.has(value.operation) ? value.operation : 'page_cleanup' };
}
function validStat(record) {
  return object(record) && Object.entries(record).every(([key, value]) => {
    if (!STATS_FIELDS.has(key)) return false;
    if (STATS_TEXT_FIELDS.has(key)) return typeof value === 'string';
    if (key === 'nominated' || key === 'selected') return typeof value === 'boolean';
    return typeof value === 'number' && Number.isFinite(value);
  });
}
function validateSnapshot(value, role) {
  if (!exact(value, ['schema_version', 'phase', 'role', 'generation', 'timestamp_ms', 'ice', 'connection_state',
    'ice_connection_state', 'signaling_state', 'stats', 'api', 'media', 'cleanup_errors']) ||
      value.schema_version !== 1 || !['idle', 'configured', 'starting', 'active', 'restarting', 'failed', 'closing', 'closed'].includes(value.phase) ||
      (value.role !== null && value.role !== role) || !counter(value.generation) ||
      !Number.isFinite(value.timestamp_ms) || value.timestamp_ms < 0 || !Array.isArray(value.stats) || value.stats.length > MAX_STATS ||
      !value.stats.every(validStat) ||
      ![null, 'new', 'connecting', 'connected', 'disconnected', 'failed', 'closed'].includes(value.connection_state) ||
      ![null, 'new', 'checking', 'connected', 'completed', 'disconnected', 'failed', 'closed'].includes(value.ice_connection_state) ||
      ![null, 'stable', 'have-local-offer', 'have-remote-offer', 'have-local-pranswer', 'have-remote-pranswer', 'closed'].includes(value.signaling_state) ||
      !exact(value.api, ['post_status', 'trickle_status', 'restart_status', 'stale_etag_status', 'delete_status',
        'local_media_order', 'remote_answer_applied', 'location', 'etag', 'session_id']) ||
      !API_STATUSES.every((key) => value.api[key] === null || counter(value.api[key])) ||
      !NULLABLE_API_TEXT.every((key) => value.api[key] === null || typeof value.api[key] === 'string') ||
      typeof value.api.remote_answer_applied !== 'boolean' || !Array.isArray(value.api.local_media_order) ||
      value.api.local_media_order.length > ROLES.length || !value.api.local_media_order.every((kind) => ['audio', 'video'].includes(kind)) ||
      !exact(value.media, ['remote_tracks', 'presented_frames']) || !Array.isArray(value.media.remote_tracks) ||
      value.media.remote_tracks.length > ROLES.length || !value.media.remote_tracks.every((track) =>
        exact(track, ['kind', 'ready_state', 'muted']) && ['audio', 'video'].includes(track.kind) &&
        ['live', 'ended'].includes(track.ready_state) && typeof track.muted === 'boolean') ||
      !(value.media.presented_frames === null || counter(value.media.presented_frames)) || !validCleanup(value.cleanup_errors)) {
    fail('BROWSER_PAGE_RESULT_INVALID', 'snapshot');
  }
  if (value.ice !== null && (!exact(value.ice, ['generation', 'local_sha256', 'remote_sha256']) ||
      value.ice.generation !== value.generation || !HASH.test(value.ice.local_sha256) || !HASH.test(value.ice.remote_sha256))) {
    fail('BROWSER_PAGE_RESULT_INVALID', 'snapshot');
  }
}

/**
 * One adapter per case, one Node event-loop owner. options: source (manifest
 * source), gridUrl (resolved explicit environment value), profile, optional
 * diagnostic allowLoopbackHttp, commandTimeoutMs <= 45000, tighter input/result
 * byte limits, and injectable builderFactory at the external Grid boundary.
 * caseContext: {browser, relay_contract?}; configuration is passed only through
 * execute('publisher'|'viewer', 'configure', Task8Configuration, signal).
 * preflight reserves the publisher session; openPublisher adopts it once.
 * Role lifecycle: absent -> preparing -> prepared -> open -> failed/closed.
 * No retry/reopen/restore; close is a terminal latch, including during a command.
 * Snapshot results are {snapshot, relay}; relay is evidence, never an outcome.
 * Errors carry only fixed code/operation/category; close returns all cleanup
 * failures. A failed/unknown remote creation requires lab-side session reaping.
 */
function createSeleniumAdapter(options = {}) {
  const timeout = options.commandTimeoutMs ?? MAX_COMMAND_MS;
  const inputBytes = options.maxInputBytes ?? MAX_INPUT_BYTES;
  const resultBytes = options.maxResultBytes ?? MAX_RESULT_BYTES;
  if (!Number.isSafeInteger(timeout) || timeout <= 0 || timeout > MAX_COMMAND_MS ||
      !Number.isSafeInteger(inputBytes) || inputBytes <= 0 || inputBytes > MAX_INPUT_BYTES ||
      !Number.isSafeInteger(resultBytes) || resultBytes <= 0 || resultBytes > MAX_RESULT_BYTES) fail('BROWSER_INVALID_OPTIONS', 'options');
  const limits = { bytes: resultBytes, depth: MAX_DEPTH, nodes: MAX_NODES, text: MAX_TEXT };
  const copy = (value, operation, bytes = resultBytes) => {
    try { return copyBounded(value, { ...limits, bytes }); }
    catch { fail(operation === 'input' ? 'BROWSER_INPUT_INVALID' : 'BROWSER_PAGE_RESULT_INVALID', operation); }
  };
  const source = copy(options.source, 'input', inputBytes);
  if (!object(source) || !HASH.test(source.test_page_sha256)) fail('BROWSER_INVALID_SOURCE', 'source');
  const pageUrl = safeUrl(source.test_page_url, false, 'source').href;
  const grid = options.gridUrl === undefined ? null : safeUrl(options.gridUrl,
    ['diagnostic', 'contract_lab'].includes(options.profile) &&
      options.allowLoopbackHttp === true, 'grid');
  const builderFactory = options.builderFactory ?? (() => new Builder());
  if (typeof builderFactory !== 'function') fail('BROWSER_INVALID_OPTIONS', 'options');
  const roles = new Map(), secrets = new Set();
  let secretBytes = 0, closed = false, closePromise = null, expectedBrowser = null;
  let redact = createRedactor([]);
  const settings = { url: pageUrl, methods: METHODS, timeout, limits,
    failureCodes: Object.keys(PAGE_FAILURE_CATEGORIES), failureOperations: [...PAGE_FAILURE_OPERATIONS] };

  function roleEntry(role, available = true) {
    if (!ROLES.includes(role)) fail('BROWSER_ROLE_INVALID', 'role');
    const entry = roles.get(role);
    if (available && (!entry || entry.phase !== 'open')) fail('BROWSER_ROLE_UNAVAILABLE', 'role');
    return entry;
  }
  function checkOpen() { if (closed) fail('BROWSER_ADAPTER_CLOSED', 'open'); }
  function remember(argument) {
    const added = [argument?.token, argument?.turn?.username, argument?.turn?.credential].filter((v) => typeof v === 'string' && v);
    for (const value of added) {
      if (secrets.has(value)) continue;
      const bytes = Buffer.byteLength(value);
      if (secrets.size >= MAX_SECRETS || secretBytes + bytes > MAX_SECRET_BYTES) fail('BROWSER_INPUT_INVALID', 'secrets');
      secrets.add(value); secretBytes += bytes;
    }
    redact = createRedactor([...secrets]);
  }
  function safeResult(value) {
    const visit = (value) => {
      if (typeof value === 'string') return redact(value);
      if (!value || typeof value !== 'object') return value;
      if (Array.isArray(value)) return value.map(visit);
      return Object.fromEntries(Object.entries(value).map(([key, item]) => {
        if (['address', 'ip'].includes(key)) return [key, '[REDACTED]'];
        if (key === 'cleanup_errors') return [key, item.map(cleanupEvidence)];
        return [key, visit(item)];
      }));
    };
    return copy(visit(value), 'result');
  }

  async function command(entry, operation, work, signal) {
    if (entry.phase === 'closed' && !['close', 'quit'].includes(operation)) fail('BROWSER_ROLE_UNAVAILABLE', operation);
    if (signal?.aborted) fail('BROWSER_COMMAND_ABORTED', operation);
    const controller = new AbortController();
    entry.commandController = controller;
    const onAbort = () => controller.abort(failure('BROWSER_COMMAND_ABORTED', operation));
    signal?.addEventListener('abort', onAbort, { once: true });
    let timer;
    const stopped = new Promise((_, reject) => {
      controller.signal.addEventListener('abort', () => {
        entry.agent?.destroy(); reject(controller.signal.reason);
      }, { once: true });
      timer = setTimeout(() => controller.abort(failure('BROWSER_COMMAND_TIMEOUT', operation)), timeout);
    });
    try {
      // Work runs after the cancellation listener is installed. External failures
      // are never inspected, interpolated or attached as a cause.
      const result = await Promise.race([Promise.resolve().then(() => {
        if (controller.signal.aborted) throw controller.signal.reason;
        return work();
      }), stopped]);
      if (controller.signal.aborted) throw controller.signal.reason;
      return result;
    } catch (error) {
      throw trustedErrors.has(error) ? error : failure('BROWSER_GRID_COMMAND_FAILED', operation);
    } finally {
      clearTimeout(timer); signal?.removeEventListener('abort', onAbort);
      if (entry.commandController === controller) entry.commandController = null;
    }
  }
  async function page(entry, operation, argument, signal) {
    const pageSettings = operation === 'close' ? { ...settings, limits: { ...limits, bytes: MAX_CLEANUP_BYTES } } : settings;
    const raw = await command(entry, operation, () => entry.driver.executeAsyncScript(SCRIPT, pageSettings, operation, argument ?? null), signal);
    const envelope = copy(raw, 'result', pageSettings.limits.bytes);
    if (!exact(envelope, ['ok', 'value', 'code', 'operation']) || typeof envelope.ok !== 'boolean') fail('BROWSER_PAGE_RESULT_INVALID', operation);
    if (!envelope.ok) {
      if (typeof envelope.code === 'string' && Object.hasOwn(PAGE_FAILURE_CATEGORIES, envelope.code) && PAGE_FAILURE_OPERATIONS.has(envelope.operation)) {
        fail(envelope.code, envelope.operation, PAGE_FAILURE_CATEGORIES[envelope.code]);
      }
      const code = PAGE_ERROR_CODES.has(envelope.code) ? envelope.code : 'BROWSER_PAGE_RESULT_INVALID';
      fail(code, operation);
    }
    return envelope.value;
  }
  async function capabilities(entry, signal) {
    const actual = await command(entry, 'capabilities', async () => {
      const caps = await entry.driver.getCapabilities();
      return normalizedBrowser({ name: caps.get('browserName'), version: caps.get('browserVersion'), platform: caps.get('platformName') }, 'capabilities');
    }, signal);
    if (Object.keys(actual).some((key) => actual[key] !== expectedBrowser[key])) fail('BROWSER_CAPABILITY_MISMATCH', 'capabilities', 'missing_evidence');
    return actual;
  }
  function context(value) {
    const result = copy(value, 'input', inputBytes);
    const browser = normalizedBrowser(result.browser, 'context');
    if (expectedBrowser && Object.keys(browser).some((key) => browser[key] !== expectedBrowser[key])) fail('BROWSER_INPUT_INVALID', 'context');
    const contract = validateRelayContract(result.relay_contract);
    expectedBrowser = browser;
    return contract;
  }
  async function prepare(role, value, signal) {
    checkOpen();
    if (signal?.aborted) fail('BROWSER_COMMAND_ABORTED', 'prepare');
    const contract = context(value);
    if (!grid) fail('BROWSER_GRID_UNCONFIGURED', 'preflight', 'missing_evidence');
    if (roles.has(role)) fail('BROWSER_ROLE_ALREADY_OPEN', 'prepare');
    const entry = { role, phase: 'preparing', contract, driver: null, busy: true, pageReady: false, failures: [] };
    roles.set(role, entry);
    // Protocol serialization and response buffering remain Selenium-owned.
    entry.agent = commandAgent(grid, entry);
    try {
      // Explicit child logger levels prevent previously enabled payload logging.
      for (const name of ['driver.Builder', 'driver.http', 'driver.http.Executor']) logging.getLogger(name).setLevel(logging.Level.OFF);
      await command(entry, 'create_session', async () => {
        const driver = await builderFactory().disableEnvironmentOverrides().usingServer(grid.href).usingHttpAgent(entry.agent)
          .withCapabilities({ browserName: expectedBrowser.name, browserVersion: expectedBrowser.version,
            platformName: expectedBrowser.platform }).build();
        entry.driver = driver;
        if (entry.phase === 'closed') {
          await attempt(entry, 'late_quit', () => command(entry, 'quit', () => driver.quit()));
          entry.agent.destroy();
        }
      }, signal);
      if (entry.phase !== 'preparing') fail('BROWSER_ROLE_UNAVAILABLE', 'prepare');
      const status = copy(await command(entry, 'grid_status', () => entry.driver.getExecutor().execute(new Command(Name.GET_SERVER_STATUS)), signal), 'result');
      if (!object(status) || status.ready !== true) fail('BROWSER_GRID_NOT_READY', 'grid_status', 'missing_evidence');
      entry.capabilities = await capabilities(entry, signal);
      const session = await command(entry, 'session_identity', () => entry.driver.getSession(), signal);
      entry.sessionId = session.getId();
      if (!label(entry.sessionId) || [...roles.values()].some((other) => other !== entry && other.sessionId === entry.sessionId)) {
        fail('BROWSER_SESSION_NOT_DISTINCT', 'prepare');
      }
      await command(entry, 'timeouts', () => entry.driver.manage().setTimeouts({ implicit: 0, pageLoad: timeout, script: timeout }), signal);
      await command(entry, 'navigate', () => entry.driver.get(pageUrl), signal);
      const asset = await page(entry, 'inspect', undefined, signal);
      if (!exact(asset, ['sha256', 'scope', 'bytes']) || asset.scope !== 'html-response' || !counter(asset.bytes)) fail('BROWSER_PAGE_RESULT_INVALID', 'inspect');
      if (asset.sha256 !== source.test_page_sha256) fail('BROWSER_PAGE_HASH_MISMATCH', 'inspect', 'missing_evidence');
      if (entry.phase !== 'preparing') fail('BROWSER_ROLE_UNAVAILABLE', 'prepare');
      entry.pageReady = true; entry.phase = 'prepared';
      entry.preflight = safeResult({ capabilities: entry.capabilities, page: asset,
        cors: { verified: false, requirement: 'same-origin-or-expose-location-etag' } });
      return entry;
    } catch (error) {
      if (entry.phase !== 'closed') entry.phase = 'failed';
      throw trustedErrors.has(error) ? error : failure('BROWSER_GRID_COMMAND_FAILED', 'prepare');
    } finally { entry.busy = false; }
  }
  async function open(role, value, signal) {
    checkOpen();
    let entry = roles.get(role);
    if (entry) {
      if (role !== 'publisher' || entry.phase !== 'prepared' || entry.busy) fail('BROWSER_ROLE_ALREADY_OPEN', 'open');
      entry.contract = context(value);
      if (signal?.aborted) fail('BROWSER_COMMAND_ABORTED', 'open');
    } else entry = await prepare(role, value, signal);
    if (closed || entry.phase !== 'prepared') fail('BROWSER_ROLE_UNAVAILABLE', 'open');
    entry.phase = 'open';
    return entry.preflight;
  }
  async function exclusive(entry, operation, work) {
    if (entry.busy) fail('BROWSER_ROLE_BUSY', operation);
    entry.busy = true;
    try {
      const result = await work();
      if (entry.phase !== 'open') fail('BROWSER_ROLE_UNAVAILABLE', operation);
      return result;
    } catch (error) {
      if (entry.phase === 'open') entry.phase = 'failed';
      throw error;
    } finally { entry.busy = false; }
  }
  async function attempt(entry, operation, work) {
    try { await work(); }
    catch (error) {
      entry.failures.push({ role: entry.role, operation,
        code: trustedErrors.has(error) ? error.code : 'BROWSER_GRID_COMMAND_FAILED' });
    }
  }
  // Only fixed role/operation/code enums enter the accumulator. Unlike page
  // evidence it contains no external text and needs no credential redactor.
  function receipt(entries) {
    return copy({ cleanup_failures: entries.flatMap((entry) => entry.failures) }, 'cleanup_result', MAX_CLEANUP_BYTES);
  }
  async function closeEntry(entry, signal) {
    if (entry.closePromise) return entry.closePromise;
    entry.phase = 'closed';
    entry.commandController?.abort(failure('BROWSER_COMMAND_ABORTED', 'close'));
    entry.closePromise = (async () => {
      if (entry.driver) {
        if (entry.pageReady) await attempt(entry, 'close', async () => {
          const result = await page(entry, 'close', undefined, signal);
          if (!exact(result, ['phase', 'cleanup_errors']) || result.phase !== 'closed' || !validCleanup(result.cleanup_errors)) {
            fail('BROWSER_PAGE_RESULT_INVALID', 'close');
          }
          for (const error of result.cleanup_errors) entry.failures.push({ role: entry.role, ...cleanupEvidence(error) });
        });
        // A cancelled caller must not prevent the independent bounded quit attempt.
        await attempt(entry, 'quit', () => command(entry, 'quit', () => entry.driver.quit()));
      } else entry.failures.push({ role: entry.role, operation: 'create_session', code: 'BROWSER_SESSION_CREATION_UNCONFIRMED' });
      entry.agent?.destroy();
    })();
    await entry.closePromise;
  }

  return Object.freeze({
    async preflightBrowser(value, signal) { return (await prepare('publisher', value, signal)).preflight; },
    openPublisher(value, signal) { return open('publisher', value, signal); },
    openViewer(value, signal) { return open('viewer', value, signal); },
    async execute(role, operation, argument, signal) {
      if (!ROLES.includes(role)) fail('BROWSER_ROLE_INVALID', 'execute');
      if (!METHODS.includes(operation) || (operation === 'startPublisher' && role !== 'publisher') ||
          (operation === 'startViewer' && role !== 'viewer')) fail('BROWSER_OPERATION_INVALID', 'execute');
      const entry = roleEntry(role);
      if (entry.busy) fail('BROWSER_ROLE_BUSY', operation);
      const input = argument === undefined ? undefined : copy(argument, 'input', inputBytes);
      if (!['configure', 'restartIce'].includes(operation) && input !== undefined) fail('BROWSER_INPUT_INVALID', operation);
      if (operation === 'close') { await closeEntry(entry, signal); return receipt([entry]); }
      remember(input);
      return exclusive(entry, operation, async () => {
        const value = await page(entry, operation, input, signal);
        if (operation === 'configure') {
          if (!exact(value, ['phase']) || value.phase !== 'configured') fail('BROWSER_PAGE_RESULT_INVALID', operation);
          return safeResult(value);
        }
        validateSnapshot(value, role);
        return safeResult({ snapshot: value, relay: relayEvidence(value.stats, entry.contract) });
      });
    },
    async collectCapabilities(role, signal) {
      const entry = roleEntry(role);
      return exclusive(entry, 'capabilities', async () => safeResult(await capabilities(entry, signal)));
    },
    async closeRole(role, signal) {
      const entry = roleEntry(role, false);
      if (!entry) {
        const empty = { role, phase: 'closed', failures: [] };
        roles.set(role, empty);
        empty.closePromise = Promise.resolve();
        return receipt([empty]);
      }
      await closeEntry(entry, signal);
      return receipt([entry]);
    },
    async closeAll(signal) {
      if (!closePromise) {
        closed = true;
        closePromise = (async () => {
          // Complete every resource release before any receipt construction.
          for (const entry of [...roles.values()].reverse()) await attempt(entry, 'close_role', () => closeEntry(entry, signal));
        })();
      }
      await closePromise;
      return receipt([...roles.values()].reverse());
    },
  });
}

module.exports = { createSeleniumAdapter };
