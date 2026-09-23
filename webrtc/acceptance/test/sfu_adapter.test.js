'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { startFakeSfuServer } = require('./fixtures/fake_sfu_server');
const { createRunIdentity, createCaseIdentity } = require('../src/ids');
const { issueSfuToken } = require('../src/providers');
const { createHttpClient } = require('../src/http_client');

const context = Object.freeze({
  ...createCaseIdentity(createRunIdentity(() => 0, () => 'sfu', 'a'.repeat(64)), { case_key: 'chrome-relay' }, 0),
  room_id: 'room-a', publisher_id: 'publisher-a', viewer_id: 'viewer-a', publisher_session_id: 'publisher-session',
});
function token(scope, participant = context.publisher_id) {
  const audience = scope === 'sfu.media.delete' ? 'turbomedia-sfu-media' : 'turbomedia-sfu-control';
  return { schema_version: 1, provider_id: 'fake', token_id: scope, audience,
    scope: [scope], subject: 'acceptance', binding: { room_id: context.room_id, participant_id: participant },
    issued_at: '2026-09-08T00:00:00Z', expires_at: '2026-09-08T01:00:00Z', token: `secret-${scope}-${participant}` };
}
const writeToken = token('sfu.control.write');
const dangerToken = token('sfu.control.dangerous');
const deleteToken = token('sfu.media.delete');
const viewerDeleteToken = token('sfu.media.delete', context.viewer_id);
const publisher = { kind: 'whip', room_id: context.room_id, participant_id: context.publisher_id, session_id: context.publisher_session_id };
const viewer = { kind: 'whep', room_id: context.room_id, participant_id: context.viewer_id, session_id: 'viewer-session' };

async function setup(t, serverOptions = {}, adapterOptions = {}) {
  // Import inside the test so missing implementation is reported as a failed requirement.
  let exports;
  try { exports = require('../src/sfu_adapter'); } catch (error) {
    assert.fail(`SFU lifecycle adapter must exist: ${error.code}`);
  }
  const server = await startFakeSfuServer({ tokens: [writeToken, dangerToken, deleteToken, viewerDeleteToken], ...serverOptions });
  t.after(() => server.close());
  const adapter = exports.createSfuAdapter({ baseUrl: server.baseUrl, requestTimeoutMs: 250,
    maxBodyBytes: 4096, pollIntervalMs: 5, waitTimeoutMs: 100, ...adapterOptions });
  return { server, adapter };
}
async function attached(adapter) {
  await adapter.preflight();
  const baseline = await adapter.captureBaseline();
  await adapter.attachRoom(context, writeToken);
  return baseline;
}

test('SFU lifecycle orders health ready baseline attach tracks subscriptions stats deletes detach and baseline', async (t) => {
  const { server, adapter } = await setup(t, { trackDelayPolls: 2 });
  const issued = await issueSfuToken(async () => writeToken, { now_ms: Date.parse('2026-09-08T00:30:00Z') });
  await adapter.preflight();
  const baseline = await adapter.captureBaseline();
  assert.deepEqual(baseline, { node_id: 'fake-node', room_count: 0, session_count: 0, published_track_count: 0 });
  await adapter.attachRoom(context, issued);
  await assert.rejects(adapter.attachRoom({ ...context }, issued), { status: 400 });
  server.createMedia(publisher);
  const tracks = await adapter.waitForPublishedTracks(context, issued, ['audio', 'video']);
  assert.deepEqual(tracks, ['publisher-a-audio', 'publisher-a-video-1']);
  await adapter.setViewerSubscriptions(context, issued, tracks);
  await adapter.setViewerSubscriptions({ ...context }, issued, tracks);
  server.createMedia(viewer);
  const session = await adapter.getWebRtcSession(context, issued, viewer.session_id);
  assert.equal(session.relay_track_count, 2);
  assert.equal((await adapter.getNodeStats(issued)).session_count, 2);
  assert.deepEqual(await adapter.deleteMediaResource(viewer, viewerDeleteToken), { status: 204, already_deleted: false });
  assert.deepEqual(await adapter.deleteMediaResource({ ...viewer }, viewerDeleteToken), { status: 404, already_deleted: true });
  await adapter.deleteMediaResource(publisher, deleteToken);
  await adapter.detachRoom(context, dangerToken);
  await assert.rejects(adapter.detachRoom({ ...context }, dangerToken), { status: 400 });
  assert.deepEqual(await adapter.waitForBaseline(baseline, issued), baseline);
  assert.deepEqual(server.requests.slice(0, 4).map((r) => r.body?.type || r.url), ['/health', '/ready', 'get_node_stats', 'attach_room']);
  const commands = server.requests.filter((r) => ['attach_room', 'set_track_subscription', 'detach_room'].includes(r.body?.type)).map((r) => r.body);
  for (const command of commands) {
    assert.match(command.message_id, /^msg-[a-f0-9]{64}$/);
    assert.equal(command.correlation_id, context.case_id);
  }
  assert.equal(commands[0].message_id, commands[1].message_id);
  assert.equal(commands[2].message_id, commands[4].message_id);
  assert.notEqual(commands[2].message_id, commands[3].message_id);
  assert.equal(commands.at(-1).message_id, commands.at(-2).message_id);
  assert.equal(server.requests.filter((r) => r.url.includes('webrtc_sessions')).length, 4);
  assert.ok(!JSON.stringify(server.requests).includes('secret-'));
  assert.ok(server.requests.every((request) => request.token_outside_authorization === false));
  assert.ok(server.requests.filter((request) => request.body?.type === 'set_track_subscription')
    .every((request) => request.published_track_count_at_request === 2));
});

test('SFU rejects wrong mutation scope and never treats an unknown resource 404 as cleanup success', async (t) => {
  const { server, adapter } = await setup(t);
  await adapter.preflight();
  await adapter.captureBaseline();
  await assert.rejects(adapter.attachRoom(context, dangerToken), { code: 'HTTP_STATUS', status: 401 });
  await adapter.attachRoom(context, writeToken);
  await assert.rejects(adapter.detachRoom(context, writeToken), { code: 'HTTP_STATUS', status: 401 });
  await assert.rejects(adapter.deleteMediaResource(publisher, deleteToken), { code: 'HTTP_STATUS', status: 404 });
  server.createMedia(publisher);
  await adapter.deleteMediaResource(publisher, deleteToken);
  await assert.rejects(adapter.deleteMediaResource({ ...publisher, session_id: 'other' }, deleteToken), { code: 'HTTP_STATUS', status: 404 });
  server.fault({ status: 503, body: 'failure', type: 'text/plain' });
  await assert.rejects(adapter.deleteMediaResource(publisher, deleteToken), { code: 'HTTP_STATUS', status: 503 });
});

test('SFU DELETE rejects a scope-correct token with the control audience', async (t) => {
  const wrongAudience = { ...deleteToken, audience: 'turbomedia-sfu-control', token: 'secret-wrong-delete-audience' };
  const { server, adapter } = await setup(t, { tokens: [writeToken, deleteToken, wrongAudience] });
  await attached(adapter);
  server.createMedia(publisher);
  await assert.rejects(adapter.deleteMediaResource(publisher, wrongAudience), { code: 'HTTP_STATUS', status: 401 });
  assert.equal((await adapter.getWebRtcSession(context, writeToken, publisher.session_id)).session_id, publisher.session_id);
  assert.equal((await adapter.deleteMediaResource(publisher, deleteToken)).status, 204);
});

test('SFU control mutation rejects a scope-correct token with the media audience', async (t) => {
  const wrongAudience = { ...writeToken, audience: 'turbomedia-sfu-media', token: 'secret-wrong-control-audience' };
  const { adapter } = await setup(t, { tokens: [writeToken, wrongAudience] });
  await adapter.preflight();
  await adapter.captureBaseline();
  await assert.rejects(adapter.attachRoom(context, wrongAudience), { code: 'HTTP_STATUS', status: 401 });
  await adapter.attachRoom(context, writeToken);
});

for (const [name, trackIds, expectedRelayCount] of [
  ['unknown track IDs', ['unknown-audio', 'unknown-video'], 0],
  ['actual registered track IDs', ['publisher-a-audio', 'publisher-a-video-1'], 2],
]) {
  test(`SFU accepts pending ${name} subscriptions but only matching registered tracks relay`, async (t) => {
    const { server, adapter } = await setup(t, { trackDelayPolls: 2 });
    await attached(adapter);
    const client = createHttpClient({ baseUrl: server.baseUrl });
    // Characterize the existing wire boundary: unlike the acceptance adapter,
    // the SFU accepts desired subscriptions before sender/viewer provisioning.
    for (const trackId of trackIds) {
      const response = await client.request({ segments: ['api', 'v1', 'commands'], method: 'POST',
        token: writeToken.token, body: { type: 'set_track_subscription', room_id: context.room_id,
          receiver_participant_id: context.viewer_id, track_id: trackId, enabled: true } });
      assert.equal(response.status, 200);
      assert.equal(response.body.ok, true);
    }
    server.createMedia(publisher);
    await adapter.waitForPublishedTracks(context, writeToken, ['audio', 'video']);
    server.createMedia(viewer);
    const session = await adapter.getWebRtcSession(context, writeToken, viewer.session_id);
    assert.equal(session.relay_track_count, expectedRelayCount);
    assert.equal(session.remote_frame_count, 0);
  });
}

for (const [name, fault, code] of [
  ['wrong content type', { body: '{}', type: 'text/html' }, 'HTTP_CONTENT_TYPE'],
  ['JSON-like content type', { body: '{}', type: 'application/jsonp' }, 'HTTP_CONTENT_TYPE'],
  ['non-UTF8 content type', { body: '{}', type: 'application/json;charset=latin1' }, 'HTTP_CONTENT_TYPE'],
  ['invalid UTF8', { chunks: [Buffer.from([0xff])] }, 'HTTP_INVALID_UTF8'],
  ['invalid JSON', { body: '{secret-token' }, 'HTTP_INVALID_JSON'],
  ['oversized body', { body: 'x'.repeat(4097) }, 'HTTP_BODY_LIMIT'],
  ['chunked oversized body', { chunks: ['x'.repeat(3000), 'x'.repeat(3000)] }, 'HTTP_BODY_LIMIT'],
  ['server error', { status: 503, body: { ok: false } }, 'HTTP_STATUS'],
  ['redirect', { status: 302, body: { ok: true } }, 'HTTP_STATUS'],
  ['header timeout', { hang: true }, 'HTTP_TIMEOUT'],
  ['body timeout', { streamHang: true }, 'HTTP_TIMEOUT'],
  ['false success envelope', { body: { ok: false } }, 'SFU_INVALID_RESPONSE'],
]) {
  test(`SFU fails safely on ${name}`, async (t) => {
    const { server, adapter } = await setup(t, {}, { requestTimeoutMs: 50 });
    server.fault(fault);
    await assert.rejects(adapter.preflight(), (error) => {
      assert.equal(error.code, code);
      assert.ok(!JSON.stringify(error).includes('secret-'));
      assert.ok(!error.message.includes('secret-'));
      return true;
    });
    assert.equal(server.requests.length, 1);
  });
}

test('SFU missing tracks and residual baseline time out without fabricating success', async (t) => {
  const { server, adapter } = await setup(t, { missingTracks: true });
  const baseline = await attached(adapter);
  server.createMedia(publisher);
  await assert.rejects(adapter.waitForPublishedTracks(context, writeToken, ['audio', 'video']), { code: 'SFU_TRACKS_TIMEOUT' });
  await assert.rejects(adapter.waitForBaseline(baseline, writeToken), { code: 'SFU_BASELINE_TIMEOUT' });
  assert.equal(server.requests.filter((r) => r.body?.type === 'set_track_subscription').length, 0);
});

test('SFU blocks subscriptions before track confirmation and validates operation order', async (t) => {
  const { adapter, server } = await setup(t);
  await assert.rejects(adapter.attachRoom(context, writeToken), { code: 'SFU_SETUP_ORDER' });
  await attached(adapter);
  await assert.rejects(adapter.setViewerSubscriptions(context, writeToken, ['publisher-a-audio']), { code: 'SFU_TRACKS_UNCONFIRMED' });
  assert.equal(server.requests.length, 4);
});

test('SFU individually encodes URL segments and rejects unsafe origins and dot segments', async (t) => {
  const { adapter, server } = await setup(t);
  await assert.rejects(adapter.getWebRtcSession({ ...context, room_id: 'room /?#%' }, writeToken, 'session /?#%'), { status: 404 });
  assert.equal(server.requests[0].url, '/api/v1/rooms/room%20%2F%3F%23%25/webrtc_sessions/session%20%2F%3F%23%25');
  await assert.rejects(adapter.deleteMediaResource({ ...publisher, session_id: '..' }, deleteToken), { code: 'HTTP_INVALID_PATH' });
  const { createSfuAdapter } = require('../src/sfu_adapter');
  for (const baseUrl of ['http://secret@example.test', 'http://example.test?token=secret', 'http://example.test/#secret']) {
    assert.throws(() => createSfuAdapter({ baseUrl }), { code: 'HTTP_INVALID_URL' });
  }
});

test('SFU propagates cancellation during HTTP and poll waits with no extra request', async (t) => {
  const { adapter, server } = await setup(t, { missingTracks: true });
  await attached(adapter);
  server.createMedia(publisher);
  const controller = new AbortController();
  const pending = adapter.waitForPublishedTracks(context, writeToken, ['audio', 'video'], controller.signal);
  setTimeout(() => controller.abort(new Error('secret-abort-reason')), 15);
  await assert.rejects(pending, { code: 'HTTP_ABORTED' });
  const count = server.requests.length;
  await assert.rejects(adapter.getNodeStats(writeToken, controller.signal), { code: 'HTTP_ABORTED' });
  assert.equal(server.requests.length, count);
});

test('SFU invalidates confirmed tracks when the publisher resource is deleted', async (t) => {
  const { adapter, server } = await setup(t);
  await attached(adapter);
  server.createMedia(publisher);
  const tracks = await adapter.waitForPublishedTracks(context, writeToken, ['audio', 'video']);
  await adapter.deleteMediaResource(publisher, deleteToken);
  await assert.rejects(adapter.setViewerSubscriptions(context, writeToken, tracks), { code: 'SFU_TRACKS_UNCONFIRMED' });
});

test('SFU requires a fresh successful preflight after a failed repeat preflight', async (t) => {
  const { adapter, server } = await setup(t);
  await adapter.preflight();
  await adapter.captureBaseline();
  server.fault({ status: 503, body: { ok: false } });
  await assert.rejects(adapter.preflight(), { code: 'HTTP_STATUS' });
  await assert.rejects(adapter.attachRoom(context, writeToken), { code: 'SFU_SETUP_ORDER' });
});

test('SFU only supports the audio-first track contract', async (t) => {
  const { adapter, server } = await setup(t);
  await attached(adapter);
  server.createMedia(publisher);
  await assert.rejects(adapter.waitForPublishedTracks(context, writeToken, ['video', 'audio']), { code: 'SFU_INVALID_TRACK_CONTRACT' });
  await assert.rejects(adapter.setViewerSubscriptions(context, writeToken, ['publisher-a-video-1']), { code: 'SFU_TRACKS_UNCONFIRMED' });
});

for (const [name, serverOptions] of [
  ['room', { roomTrackCount: 1 }],
  ['publisher session', { missingTracks: true, roomTrackCount: 2 }],
]) {
  test(`SFU waits for the ${name} registration count even when the other count is complete`, async (t) => {
    const { adapter, server } = await setup(t, serverOptions);
    await attached(adapter);
    server.createMedia(publisher);
    await assert.rejects(adapter.waitForPublishedTracks(context, writeToken, ['audio', 'video']), { code: 'SFU_TRACKS_TIMEOUT' });
    assert.equal(server.requests.filter((request) => request.body?.type === 'set_track_subscription').length, 0);
  });
}

test('SFU rejects a second participant before viewer subscriptions are established', async (t) => {
  const { adapter, server } = await setup(t, { roomTrackCount: 2 });
  await attached(adapter);
  server.createMedia(publisher);
  server.createMedia(viewer);
  await assert.rejects(adapter.waitForPublishedTracks(context, writeToken, ['audio', 'video']),
    { code: 'SFU_INVALID_TRACK_CONTRACT' });
  assert.equal(server.requests.filter((request) => request.body?.type === 'set_track_subscription').length, 0);
});

test('SFU rejects changed node identity and malformed counters instead of accepting a zero baseline', async (t) => {
  const { adapter, server } = await setup(t);
  const baseline = await attached(adapter);
  server.fault({ body: { ok: true, node_stats: { node_id: 'other-node', draining: false,
    room_count: 0, session_count: 0, published_track_count: 0 } } });
  await assert.rejects(adapter.waitForBaseline(baseline, writeToken), { code: 'SFU_NODE_CHANGED' });
  server.fault({ body: { ok: true, node_stats: { node_id: 'fake-node', draining: false,
    room_count: 0, session_count: '0', published_track_count: 0 } } });
  await assert.rejects(adapter.getNodeStats(writeToken), { code: 'SFU_INVALID_RESPONSE' });
});

test('SFU detach refuses a room with live media and unexpected DELETE success stays a failure', async (t) => {
  const { adapter, server } = await setup(t);
  await attached(adapter);
  server.createMedia(publisher);
  await assert.rejects(adapter.detachRoom(context, dangerToken), { status: 400 });
  server.fault({ status: 200, body: '', type: 'text/plain' });
  await assert.rejects(adapter.deleteMediaResource(publisher, deleteToken), { code: 'HTTP_STATUS', status: 200 });
  await adapter.deleteMediaResource(publisher, deleteToken);
  server.fault({ status: 404, body: '<html>missing</html>', type: 'text/html' });
  await assert.rejects(adapter.deleteMediaResource(publisher, deleteToken), { code: 'HTTP_CONTENT_TYPE' });
});

test('SFU validates token envelopes before sending a mutation', async (t) => {
  const { adapter, server } = await setup(t);
  await adapter.preflight();
  await adapter.captureBaseline();
  const count = server.requests.length;
  await assert.rejects(adapter.attachRoom(context, { token: 'secret-incomplete' }), { code: 'SFU_INVALID_TOKEN' });
  assert.equal(server.requests.length, count);
});

test('SFU baseline matches pre-existing gauges while ignoring cumulative routing counters', async (t) => {
  const { adapter } = await setup(t, { residualSessions: 3 });
  const baseline = await attached(adapter);
  assert.equal(baseline.session_count, 3);
  await adapter.detachRoom(context, dangerToken);
  assert.deepEqual(await adapter.waitForBaseline(baseline, writeToken), baseline);
});

test('SFU poll deadline bounds a stalled request even when its request deadline is longer', async (t) => {
  const { adapter, server } = await setup(t, {}, { requestTimeoutMs: 1000, waitTimeoutMs: 40 });
  await attached(adapter);
  server.createMedia(publisher);
  server.fault({ streamHang: true });
  await assert.rejects(adapter.waitForPublishedTracks(context, writeToken, ['audio', 'video']), { code: 'SFU_TRACKS_TIMEOUT' });
});
