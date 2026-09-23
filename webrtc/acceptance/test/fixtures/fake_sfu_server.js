'use strict';

const http = require('node:http');
const { createRedactor } = require('../../src/redaction');

// Only the remote SFU boundary is replaced; tests use the real fetch transport.
async function startFakeSfuServer(options = {}) {
  const requests = [];
  const rooms = new Map();
  const sessions = new Map();
  const subscriptions = new Map();
  const tokens = options.tokens || [];
  const redact = createRedactor(tokens.map((entry) => entry.token));
  let fault = null;
  let polls = 0;
  let mediaSequence = 0;
  let residualSessions = options.residualSessions || 0;
  let cleanupResidueInjected = false;
  const stats = () => ({
    node_id: 'fake-node', draining: false, room_count: rooms.size,
    session_count: sessions.size + residualSessions,
    published_track_count: [...sessions.values()].reduce((sum, session) => sum + session.registered_track_ids.length, 0),
    total_packets_routed: 42, total_bytes_routed: 4200, total_layer_switches: 0,
  });
  function send(res, status, body, type = 'application/json') {
    res.writeHead(status, { 'Content-Type': type });
    res.end(typeof body === 'string' ? body : JSON.stringify(body));
  }
  function authorized(req, { audience, scope, room, participant }) {
    const prefix = 'Bearer ';
    const header = req.headers.authorization;
    const token = typeof header === 'string' && header.startsWith(prefix) ? header.slice(prefix.length) : null;
    if (typeof options.authorize === 'function') {
      try {
        return options.authorize({ token, audience, scope, room, participant }) === true;
      } catch {
        return false;
      }
    }
    return tokens.some((entry) => token === entry.token &&
      entry.audience === audience && entry.scope.includes(scope) && entry.binding.room_id === room &&
      (!participant || entry.binding.participant_id === participant));
  }
  function relayTrackCount(session) {
    if (session.kind !== 'whep') return 0;
    const published = new Set([...sessions.values()]
      .filter((entry) => entry.room_id === session.room_id && entry.kind === 'whip')
      .flatMap((entry) => entry.registered_track_ids));
    return [...subscriptions.values()].filter((desired) => desired.room_id === session.room_id &&
      desired.receiver_participant_id === session.participant_id && desired.enabled &&
      published.has(desired.track_id)).length;
  }
  const server = http.createServer(async (req, res) => {
    let text = '';
    for await (const chunk of req) text += chunk;
    let body = null;
    if (text) {
      try { body = JSON.parse(text); }
      catch { body = text; }
    }
    const nonAuthInput = JSON.stringify({ url: req.url, body,
      headers: Object.fromEntries(Object.entries(req.headers).filter(([key]) => key !== 'authorization')) });
    requests.push(JSON.parse(redact(JSON.stringify({ method: req.method, url: req.url, body,
      authorization_present: Boolean(req.headers.authorization),
      published_track_count_at_request: stats().published_track_count,
      token_outside_authorization: tokens.some((entry) => nonAuthInput.includes(entry.token) ||
        nonAuthInput.includes(encodeURIComponent(entry.token))) }))));
    if (fault) {
      const current = fault;
      fault = null;
      if (current.hang) return;
      if (current.streamHang) {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.write('{');
        return;
      }
      if (current.chunks) {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        for (const chunk of current.chunks) res.write(chunk);
        res.end();
        return;
      }
      return send(res, current.status || 200, current.body, current.type);
    }
    if (req.url === '/health') return send(res, 200, { ok: true, node_stats: stats() });
    if (req.url === '/ready') return send(res, 200, { ok: true, draining: false });
    const parts = req.url.split('/').slice(1).map(decodeURIComponent);
    if (req.method === 'POST' && ['whip', 'whep'].includes(parts[0]) && parts.length === 3) {
      const [kind, room, participant] = parts;
      const scope = kind === 'whip' ? 'sfu.media.publish' : 'sfu.media.subscribe';
      if (!authorized(req, { audience: 'turbomedia-sfu-media', scope, room, participant })) {
        return send(res, 401, 'Unauthorized', 'text/plain');
      }
      if (!rooms.has(room)) return send(res, 404, 'Room not found', 'text/plain');
      const sessionId = `session-${++mediaSequence}`;
      const key = JSON.stringify([kind, room, participant, sessionId]);
      sessions.set(key, {
        kind, room_id: room, participant_id: participant, session_id: sessionId,
        state: 'connected', remote_description_set: true, registered_track_ids: [],
        remote_frame_count: 0, local_candidate_count: 1, local_answer: null,
        local_ice_candidates: [], version: 1,
      });
      res.setHeader('Location', `/${kind}/${encodeURIComponent(room)}/${encodeURIComponent(participant)}/sessions/${sessionId}`);
      res.setHeader('ETag', '"1"');
      return send(res, 201, 'contract-answer', 'application/sdp');
    }
    if (req.method === 'PATCH' && ['whip', 'whep'].includes(parts[0]) &&
        parts.length === 5 && parts[3] === 'sessions') {
      const [kind, room, participant, , id] = parts;
      if (!authorized(req, { audience: 'turbomedia-sfu-media', scope: 'sfu.media.trickle', room, participant })) {
        return send(res, 401, 'Unauthorized', 'text/plain');
      }
      const key = JSON.stringify([kind, room, participant, id]);
      const session = sessions.get(key);
      if (!session) return send(res, 404, 'Media session not found', 'text/plain');
      const expected = `"${session.version}"`;
      const presented = req.headers['if-match'];
      if (presented !== expected) {
        if (options.acceptStaleEtag === true && /^"[1-9][0-9]*"$/.test(presented || '')) {
          res.setHeader('ETag', expected);
          return send(res, 204, '', 'text/plain');
        }
        return send(res, 412, 'Media resource ETag is stale', 'text/plain');
      }
      if (typeof body === 'string' && body.includes('restart')) {
        session.version += 1;
        res.setHeader('ETag', `"${session.version}"`);
        return send(res, 200,
          'a=ice-ufrag:remote2\r\na=ice-pwd:remoteabcdefghijklmnopqr2\r\n',
          'application/trickle-ice-sdpfrag');
      }
      res.setHeader('ETag', expected);
      return send(res, 204, '', 'text/plain');
    }
    if (req.method === 'DELETE') {
      const [kind, room, participant, , id] = parts;
      if (!authorized(req, { audience: 'turbomedia-sfu-media', scope: 'sfu.media.delete', room, participant })) {
        return send(res, 401, 'Unauthorized', 'text/plain');
      }
      const key = JSON.stringify([kind, room, participant, id]);
      if (!sessions.has(key)) return send(res, 404, 'Media session not found', 'text/plain');
      sessions.delete(key);
      if (options.cleanupResidueOnDelete === true && !cleanupResidueInjected) {
        cleanupResidueInjected = true;
        residualSessions += 1;
      }
      return send(res, 204, '', 'text/plain');
    }
    if (req.method === 'GET' && parts[0] === 'api') {
      const room = parts[3];
      const id = parts[5];
      const session = [...sessions.values()].find((entry) => entry.room_id === room && entry.session_id === id);
      if (!session) return send(res, 404, { ok: false });
      polls += 1;
      if (session.kind === 'whip' && polls > (options.trackDelayPolls || 0)) {
        session.registered_track_ids = [`${session.participant_id}-audio`];
        if (!options.missingTracks) session.registered_track_ids.push(`${session.participant_id}-video-1`);
      }
      const { kind, registered_track_ids, ...snapshot } = session;
      return send(res, 200, { ok: true, webrtc_session: { ...snapshot,
        remote_track_count: registered_track_ids.length, relay_track_count: relayTrackCount(session) } });
    }
    if (req.url !== '/api/v1/commands' || !body) return send(res, 404, { ok: false });
    if (body.type === 'get_node_stats') return send(res, 200, { ok: true, node_stats: stats() });
    if (body.type === 'get_room_stats') {
      if (!rooms.has(body.room_id)) return send(res, 404, { ok: false });
      const ownSessions = [...sessions.values()].filter((entry) => entry.room_id === body.room_id);
      return send(res, 200, { ok: true, room_stats: {
        room_id: body.room_id, session_count: ownSessions.length, participant_count: ownSessions.length,
        published_track_count: options.roomTrackCount ?? ownSessions.reduce((sum, entry) => sum + entry.registered_track_ids.length, 0),
        total_packets_routed: 42, total_bytes_routed: 4200, total_layer_switches: 0,
      } });
    }
    const scope = body.type === 'detach_room' ? 'sfu.control.dangerous' : 'sfu.control.write';
    if (!authorized(req, { audience: 'turbomedia-sfu-control', scope, room: body.room_id,
      participant: body.participant_id })) {
      return send(res, 401, { ok: false });
    }
    if (body.type === 'attach_room') {
      if (body.max_participants !== 2) return send(res, 400, { ok: false });
      if (rooms.has(body.room_id)) return send(res, 400, { ok: false });
      rooms.set(body.room_id, body.max_participants);
    } else if (body.type === 'set_track_subscription') {
      if (!body.receiver_participant_id || !body.track_id) {
        return send(res, 400, { ok: false });
      }
      // The runtime accepts desired state even without a sender track or viewer.
      // Only a later matching published track can contribute to relay evidence.
      const key = JSON.stringify([body.room_id, body.receiver_participant_id, body.track_id]);
      subscriptions.set(key, { room_id: body.room_id, receiver_participant_id: body.receiver_participant_id,
        track_id: body.track_id, enabled: body.enabled !== false });
    } else if (body.type === 'detach_room') {
      if (!rooms.has(body.room_id) || [...sessions.values()].some((entry) => entry.room_id === body.room_id)) {
        return send(res, 400, { ok: false });
      }
      rooms.delete(body.room_id);
      for (const [key, desired] of subscriptions) if (desired.room_id === body.room_id) subscriptions.delete(key);
    } else return send(res, 400, { ok: false });
    return send(res, 200, { ok: true });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    baseUrl: `http://127.0.0.1:${server.address().port}`,
    requests,
    fault(value) { fault = value; },
    createMedia(resource) {
      if (!rooms.has(resource.room_id)) throw new Error('room must be attached');
      const key = JSON.stringify([resource.kind, resource.room_id, resource.participant_id, resource.session_id]);
      sessions.set(key, { ...resource, state: 'connected', remote_description_set: true,
        registered_track_ids: [], remote_frame_count: 0,
        local_candidate_count: 0, local_answer: null, local_ice_candidates: [] });
    },
    async close() {
      server.closeAllConnections();
      await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
    },
  };
}

module.exports = { startFakeSfuServer };
