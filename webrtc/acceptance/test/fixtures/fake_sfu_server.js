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
  const stats = () => ({
    node_id: 'fake-node', draining: false, room_count: rooms.size,
    session_count: sessions.size + (options.residualSessions || 0),
    published_track_count: [...sessions.values()].reduce((sum, session) => sum + session.remote_track_count, 0),
    total_packets_routed: 42, total_bytes_routed: 4200, total_layer_switches: 0,
  });
  function send(res, status, body, type = 'application/json') {
    res.writeHead(status, { 'Content-Type': type });
    res.end(typeof body === 'string' ? body : JSON.stringify(body));
  }
  function authorized(req, scope, room, participant) {
    return tokens.some((entry) => req.headers.authorization === `Bearer ${entry.token}` &&
      entry.scope.includes(scope) && entry.binding.room_id === room &&
      (!participant || entry.binding.participant_id === participant));
  }
  const server = http.createServer(async (req, res) => {
    let text = '';
    for await (const chunk of req) text += chunk;
    const body = text ? JSON.parse(text) : null;
    const nonAuthInput = JSON.stringify({ url: req.url, body,
      headers: Object.fromEntries(Object.entries(req.headers).filter(([key]) => key !== 'authorization')) });
    requests.push(JSON.parse(redact(JSON.stringify({ method: req.method, url: req.url, body,
      authorization_present: Boolean(req.headers.authorization),
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
    if (req.method === 'DELETE') {
      const [kind, room, participant, , id] = parts;
      if (!authorized(req, 'sfu.media.delete', room, participant)) return send(res, 401, 'Unauthorized', 'text/plain');
      const key = JSON.stringify([kind, room, participant, id]);
      if (!sessions.has(key)) return send(res, 404, 'Media session not found', 'text/plain');
      sessions.delete(key);
      return send(res, 204, '', 'text/plain');
    }
    if (req.method === 'GET' && parts[0] === 'api') {
      const room = parts[3];
      const id = parts[5];
      const session = [...sessions.values()].find((entry) => entry.room_id === room && entry.session_id === id);
      if (!session) return send(res, 404, { ok: false });
      polls += 1;
      if (session.kind === 'whip' && polls > (options.trackDelayPolls || 0)) {
        session.remote_track_count = options.missingTracks ? 1 : 2;
      }
      const { kind, ...snapshot } = session;
      return send(res, 200, { ok: true, webrtc_session: snapshot });
    }
    if (req.url !== '/api/v1/commands' || !body) return send(res, 404, { ok: false });
    if (body.type === 'get_node_stats') return send(res, 200, { ok: true, node_stats: stats() });
    if (body.type === 'get_room_stats') {
      if (!rooms.has(body.room_id)) return send(res, 404, { ok: false });
      const ownSessions = [...sessions.values()].filter((entry) => entry.room_id === body.room_id);
      return send(res, 200, { ok: true, room_stats: {
        room_id: body.room_id, session_count: ownSessions.length, participant_count: ownSessions.length,
        published_track_count: options.roomTrackCount ?? ownSessions.reduce((sum, entry) => sum + entry.remote_track_count, 0),
        total_packets_routed: 42, total_bytes_routed: 4200, total_layer_switches: 0,
      } });
    }
    const scope = body.type === 'detach_room' ? 'sfu.control.dangerous' : 'sfu.control.write';
    if (!authorized(req, scope, body.room_id)) return send(res, 401, { ok: false });
    if (body.type === 'attach_room') {
      if (body.max_participants !== 2) return send(res, 400, { ok: false });
      if (rooms.has(body.room_id)) return send(res, 400, { ok: false });
      rooms.set(body.room_id, body.max_participants);
    } else if (body.type === 'set_track_subscription') {
      const publisher = [...sessions.values()].find((entry) => entry.room_id === body.room_id && entry.kind === 'whip');
      if (!publisher || publisher.remote_track_count !== 2 ||
          ![`${publisher.participant_id}-audio`, `${publisher.participant_id}-video-1`].includes(body.track_id)) {
        return send(res, 400, { ok: false });
      }
      subscriptions.set(body.track_id, body.receiver_participant_id);
    } else if (body.type === 'detach_room') {
      if (!rooms.has(body.room_id) || [...sessions.values()].some((entry) => entry.room_id === body.room_id)) {
        return send(res, 400, { ok: false });
      }
      rooms.delete(body.room_id);
      subscriptions.clear();
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
      if (resource.kind === 'whep' && subscriptions.size !== 2) throw new Error('subscriptions must precede WHEP');
      const key = JSON.stringify([resource.kind, resource.room_id, resource.participant_id, resource.session_id]);
      sessions.set(key, { ...resource, state: 'connected', remote_description_set: true,
        remote_track_count: 0, remote_frame_count: 5, relay_track_count: resource.kind === 'whep' ? 2 : 0,
        local_candidate_count: 0, local_answer: null, local_ice_candidates: [] });
    },
    async close() {
      server.closeAllConnections();
      await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
    },
  };
}

module.exports = { startFakeSfuServer };
