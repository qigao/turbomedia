'use strict';

(() => {
  const { MAX_SDPFRAG_BYTES, buildSdpfrag, parseSdpfrag } = turboSdpfrag;
  const SDP_TYPE = 'application/sdp';
  const FRAGMENT_TYPE = 'application/trickle-ice-sdpfrag';
  const DEFAULT_LIMITS = Object.freeze({ request_timeout_ms: 15_000, operation_timeout_ms: 30_000,
    close_timeout_ms: 5_000, response_bytes: MAX_SDPFRAG_BYTES, stats_records: 512, max_generations: 256 });
  const MAX_TEXT = 2_048;
  const MAX_TOKEN = 8_192;
  const MAX_TURN_URLS = 8;
  const MEDIA_COUNT = 2;
  const VIDEO_WIDTH = 640;
  const VIDEO_HEIGHT = 360;
  const VIDEO_FPS = 30;
  const SECOND_MS = 1_000;
  const AUDIO_HZ = 440;
  const CRLF = '\r\n';
  const STATS_FIELDS = new Set(['id', 'type', 'timestamp', 'kind', 'mediaType', 'transportId', 'codecId',
    'ssrc', 'mid', 'trackIdentifier', 'selectedCandidatePairId', 'localCandidateId', 'remoteCandidateId',
    'state', 'nominated', 'selected', 'candidateType', 'protocol', 'relayProtocol', 'address', 'ip', 'port',
    'networkType', 'currentRoundTripTime', 'totalRoundTripTime', 'roundTripTime', 'roundTripTimeMeasurements',
    'availableOutgoingBitrate', 'availableIncomingBitrate', 'bytesSent', 'bytesReceived', 'packetsSent',
    'packetsReceived', 'packetsLost', 'jitter', 'nackCount', 'pliCount', 'firCount', 'framesEncoded',
    'framesDecoded', 'framesReceived', 'framesSent', 'framesDropped', 'keyFramesDecoded', 'keyFramesEncoded',
    'frameWidth', 'frameHeight', 'framesPerSecond', 'totalDecodeTime', 'totalEncodeTime', 'totalSamplesReceived',
    'totalSamplesDuration', 'totalAudioEnergy', 'audioLevel', 'concealedSamples', 'silentConcealedSamples',
    'jitterBufferDelay', 'jitterBufferEmittedCount', 'mimeType', 'clockRate', 'channels', 'payloadType',
    'dtlsState', 'iceState', 'iceRole', 'dtlsRole', 'requestsSent', 'responsesReceived', 'consentRequestsSent']);

  class BrowserAcceptanceError extends Error {
    constructor(code, operation) {
      super(`${operation}: ${code}`);
      this.name = 'BrowserAcceptanceError'; this.code = code; this.operation = operation;
    }
  }
  const fail = (code, operation) => { throw new BrowserAcceptanceError(code, operation); };
  const object = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
  const text = (value, maximum = MAX_TEXT) => typeof value === 'string' && value.length > 0 &&
    value.length <= maximum && !/[\x00-\x20\x7f]/.test(value);
  function exact(value, allowed, operation = 'configure') {
    if (!object(value) || Object.keys(value).some((key) => !allowed.includes(key))) fail('INVALID_CONFIGURATION', operation);
  }
  function sanitized(error, operation) {
    return error instanceof BrowserAcceptanceError ? error : new BrowserAcceptanceError('EXTERNAL_OPERATION_FAILED', operation);
  }

  // One page event loop owns one role/PC. The controller owns case outcomes.
  // idle -> configured -> starting -> active <-> restarting; failure -> failed;
  // close preempts every phase -> closing -> closed. No restart/restore after close.
  let phase = 'idle', role = null, generation = 0, configuration = null;
  let pc = null, audio = null, oscillator = null, destination = null, canvas = null;
  let drawTimer = null, frameHandle = null, video = null, remoteStream = null;
  let resource = null, etag = null, localIce = null, remoteAnswer = null;
  let operationController = null, snapshotController = null, closePromise = null;
  let callbackFailure = null;
  const ownedTracks = new Set();
  const remoteTracks = new Map();
  const secrets = new Set();
  const evidence = { post_status: null, trickle_status: null, restart_status: null, stale_etag_status: null,
    delete_status: null, local_media_order: [], remote_answer_applied: false };
  let presentedFrames = null;
  let cleanupErrors = [];

  function remember(value) { if (typeof value === 'string' && value) secrets.add(value); }
  function redact(value) {
    let output = value;
    for (const secret of secrets) if (output.includes(secret)) output = output.split(secret).join('[redacted]');
    return output;
  }
  function validTurn(value) {
    exact(value, ['urls', 'username', 'credential']);
    if (!Array.isArray(value.urls) || !value.urls.length || value.urls.length > MAX_TURN_URLS ||
        !text(value.username) || !text(value.credential)) fail('INVALID_CONFIGURATION', 'configure');
    for (const url of value.urls) {
      if (!text(url) || !/^turns?:([a-zA-Z0-9.-]+|\[[a-fA-F0-9:]+\])(?::\d+)?(?:\?transport=(udp|tcp))?$/.test(url)) {
        fail('INVALID_CONFIGURATION', 'configure');
      }
      const parsed = new URL(url.replace(/^turns?:/, 'https://'));
      if (!parsed.hostname || parsed.port === '0') fail('INVALID_CONFIGURATION', 'configure');
    }
    return { urls: [...value.urls], username: value.username, credential: value.credential };
  }
  function endpoint(value, origin, kind) {
    if (!text(value)) fail('INVALID_CONFIGURATION', 'configure');
    const url = new URL(value);
    if (url.protocol !== 'https:' || url.origin !== origin || url.username || url.password || url.search || url.hash ||
        !new RegExp(`^/${kind}/[a-zA-Z0-9_-]+/[a-zA-Z0-9_-]+$`).test(url.pathname)) fail('INVALID_CONFIGURATION', 'configure');
    return url.href;
  }

  /** Memory-only input: HTTPS sfu_origin/whip_url/whep_url, token, turn {urls,
   * username, credential}, run_id/case_id/participant_id, optional tighter limits.
   * Both role endpoints must belong to sfu_origin. This page always uses relay.
   */
  function configure(value) {
    if (phase !== 'idle') fail('INVALID_PHASE', 'configure');
    try {
      exact(value, ['sfu_origin', 'whip_url', 'whep_url', 'token', 'turn', 'run_id', 'case_id', 'participant_id', 'limits']);
      if (!text(value.sfu_origin) || !text(value.token, MAX_TOKEN) ||
          !['run_id', 'case_id', 'participant_id'].every((key) => text(value[key]) && /^[a-zA-Z0-9_-]+$/.test(value[key]))) {
        fail('INVALID_CONFIGURATION', 'configure');
      }
      const origin = new URL(value.sfu_origin);
      if (origin.protocol !== 'https:' || origin.origin !== value.sfu_origin) fail('INVALID_CONFIGURATION', 'configure');
      const limits = { ...DEFAULT_LIMITS };
      if (value.limits !== undefined) {
        exact(value.limits, Object.keys(limits));
        for (const [key, limit] of Object.entries(value.limits)) {
          if (!Number.isSafeInteger(limit) || limit <= 0 || limit > limits[key]) fail('INVALID_CONFIGURATION', 'configure');
          limits[key] = limit;
        }
      }
      const normalized = { sfu_origin: origin.origin,
        whip_url: endpoint(value.whip_url, origin.origin, 'whip'), whep_url: endpoint(value.whep_url, origin.origin, 'whep'),
        token: value.token, turn: validTurn(value.turn), run_id: value.run_id, case_id: value.case_id,
        participant_id: value.participant_id, limits };
      configuration = normalized;
      remember(normalized.token); remember(normalized.turn.username); remember(normalized.turn.credential);
      phase = 'configured';
      return { phase };
    } catch { fail('INVALID_CONFIGURATION', 'configure'); }
  }

  // Each wait races a deadline and cancellation even if an external API ignores AbortSignal.
  // Async continuations must use ctx.wait before mutating owner state.
  async function scoped(timeout, parent, operation, work, controller = new AbortController()) {
    const abort = () => controller.abort(parent.reason instanceof BrowserAcceptanceError ? parent.reason :
      new BrowserAcceptanceError('OPERATION_CANCELLED', operation));
    if (parent?.aborted) abort();
    parent?.addEventListener('abort', abort, { once: true });
    const timer = setTimeout(() => controller.abort(new BrowserAcceptanceError('OPERATION_TIMEOUT', operation)), timeout);
    const check = () => { if (controller.signal.aborted) throw controller.signal.reason; };
    const wait = (promise) => new Promise((resolve, reject) => {
      const cancelled = () => { controller.signal.removeEventListener('abort', cancelled); reject(controller.signal.reason); };
      if (controller.signal.aborted) { Promise.resolve(promise).catch(() => {}); cancelled(); return; }
      controller.signal.addEventListener('abort', cancelled, { once: true });
      Promise.resolve(promise).then((value) => {
        controller.signal.removeEventListener('abort', cancelled);
        if (controller.signal.aborted) reject(controller.signal.reason); else resolve(value);
      }, (error) => { controller.signal.removeEventListener('abort', cancelled); reject(error); });
    });
    try { check(); return await work({ signal: controller.signal, check, wait }); }
    finally { clearTimeout(timer); parent?.removeEventListener('abort', abort); controller.abort(); }
  }

  async function readBody(response, context, limit, operation) {
    const declared = response.headers.get('Content-Length');
    if (declared !== null && (!/^\d+$/.test(declared) || Number(declared) > limit)) fail('HTTP_BODY_LIMIT', operation);
    if (!response.body) return '';
    const reader = response.body.getReader();
    const decoder = new TextDecoder('utf-8', { fatal: true });
    let bytes = 0, body = '';
    try {
      while (true) {
        const item = await context.wait(reader.read());
        if (item.done) break;
        bytes += item.value.byteLength;
        if (bytes > limit) fail('HTTP_BODY_LIMIT', operation);
        body += decoder.decode(item.value, { stream: true });
      }
      body += decoder.decode();
      return body;
    } finally { reader.cancel().catch(() => {}); }
  }
  function strongEtag(value, operation) {
    if (!text(value) || !/^"[1-9][0-9]*"$/.test(value)) fail('HTTP_INVALID_ETAG', operation);
    return value;
  }
  function resourceUrl(value, endpointUrl, operation) {
    if (!text(value)) fail('HTTP_INVALID_LOCATION', operation);
    const url = new URL(value, endpointUrl), base = new URL(endpointUrl);
    if (url.origin !== base.origin || url.username || url.password || url.search || url.hash ||
        !url.pathname.startsWith(`${base.pathname}/sessions/`) ||
        !/^[a-zA-Z0-9_-]+$/.test(url.pathname.slice(`${base.pathname}/sessions/`.length))) fail('HTTP_INVALID_LOCATION', operation);
    return url.href;
  }
  async function request(spec, context, settings = configuration) {
    return scoped(settings.limits.request_timeout_ms, context.signal, spec.operation, async (http) => {
      const headers = { Authorization: `Bearer ${settings.token}` };
      if (spec.body !== undefined) headers['Content-Type'] = spec.type;
      if (spec.etag) headers['If-Match'] = spec.etag;
      const response = await http.wait(fetch(spec.url, { method: spec.method, headers, body: spec.body,
        signal: http.signal, redirect: 'error', credentials: 'omit', cache: 'no-store', referrerPolicy: 'no-referrer' }));
      try {
        if (response.redirected || response.status !== spec.status) fail('HTTP_UNEXPECTED_STATUS', spec.operation);
        if (spec.method === 'POST') resource = resourceUrl(response.headers.get('Location'), spec.url, spec.operation);
        if (spec.responseType && response.headers.get('Content-Type')?.toLowerCase() !== spec.responseType) {
          fail('HTTP_INVALID_CONTENT_TYPE', spec.operation);
        }
        let nextEtag = null;
        // Location is retained immediately after validation: malformed later headers/body
        // still leave a concrete resource for close. Invalid Location is never followed.
        if (spec.requireEtag) nextEtag = strongEtag(response.headers.get('ETag'), spec.operation);
        const body = await readBody(response, http, settings.limits.response_bytes, spec.operation);
        if ((spec.responseType && !body) || (spec.status === 204 && body)) fail('HTTP_INVALID_BODY', spec.operation);
        return { status: response.status, body, etag: nextEtag };
      } finally { if (response.body && !response.body.locked) response.body.cancel().catch(() => {}); }
    });
  }

  function description(value, operation) {
    if (typeof value !== 'string' || value.length > MAX_SDPFRAG_BYTES || !value.startsWith(`v=0${CRLF}`) ||
        !value.endsWith(CRLF) || /[^\x20-\x7e\r\n]/.test(value)) fail('INVALID_SDP', operation);
    const lines = value.slice(0, -CRLF.length).split(CRLF);
    if (lines.some((line) => !line || /[\r\n]/.test(line))) fail('INVALID_SDP', operation);
    const session = [], media = [];
    for (const line of lines) {
      if (line.startsWith('m=')) media.push([]);
      (media.length ? media[media.length - 1] : session).push(line);
    }
    if (media.length !== MEDIA_COUNT) fail('INVALID_MEDIA_ORDER', operation);
    const singleton = (list, prefix) => {
      const found = list.filter((line) => line.startsWith(prefix));
      if (found.length > 1) fail('INVALID_SDP', operation);
      return found.length ? found[0].slice(prefix.length) : null;
    };
    const details = media.map((lines) => ({ lines, mLine: lines[0], mid: singleton(lines, 'a=mid:'),
      ufrag: singleton(lines, 'a=ice-ufrag:') ?? singleton(session, 'a=ice-ufrag:'),
      pwd: singleton(lines, 'a=ice-pwd:') ?? singleton(session, 'a=ice-pwd:') }));
    const bundle = singleton(session, 'a=group:BUNDLE ');
    if (bundle !== details.map((item) => item.mid).join(' ') || details[0].mid === details[1].mid ||
        details.some((item) => item.ufrag !== details[0].ufrag || item.pwd !== details[0].pwd)) {
      fail('UNSUPPORTED_ICE_TRANSPORTS', operation);
    }
    const fragment = { ufrag: details[0].ufrag, pwd: details[0].pwd,
      media: details.map((item) => ({ mLine: item.mLine.replace(/^(m=\w+) \d+ /, '$1 9 '), mid: item.mid,
        candidates: item.lines.filter((line) => line.startsWith('a=candidate:')).map((line) => line.slice(2)),
        endOfCandidates: item.lines.includes('a=end-of-candidates') })) };
    try { buildSdpfrag(fragment); } catch { fail('INVALID_SDP', operation); }
    remember(fragment.ufrag); remember(fragment.pwd);
    return { session, media: details, fragment };
  }
  function verifyOffer(value, operation) {
    const parsed = description(value, operation);
    const expectedDirection = role === 'publisher' ? 'a=sendonly' : 'a=recvonly';
    if (!parsed.media[0].mLine.startsWith('m=audio ') || !parsed.media[1].mLine.startsWith('m=video ') ||
        parsed.media.some((item) => !item.lines.includes(expectedDirection) || /^m=\w+ 0 /.test(item.mLine))) {
      fail('INVALID_MEDIA_ORDER', operation);
    }
    evidence.local_media_order = ['audio', 'video'];
    return parsed;
  }
  function rtcConfiguration(turn) { return { iceServers: [turn], iceTransportPolicy: 'relay', bundlePolicy: 'max-bundle' }; }
  async function gather(context) {
    if (pc.iceGatheringState === 'complete') return;
    const connection = pc;
    let listener;
    const completed = new Promise((resolve) => {
      listener = () => { if (connection.iceGatheringState === 'complete') resolve(); };
      connection.addEventListener('icegatheringstatechange', listener); listener();
    });
    try { await context.wait(completed); }
    finally { connection.removeEventListener('icegatheringstatechange', listener); }
  }
  async function synthetic(context) {
    audio = new AudioContext();
    destination = audio.createMediaStreamDestination();
    for (const track of destination.stream.getTracks()) ownedTracks.add(track);
    oscillator = audio.createOscillator(); oscillator.frequency.value = AUDIO_HZ;
    oscillator.connect(destination); oscillator.start();
    await context.wait(audio.resume());
    if (audio.state !== 'running') fail('AUDIO_NOT_RUNNING', 'startPublisher');
    canvas = document.createElement('canvas'); canvas.width = VIDEO_WIDTH; canvas.height = VIDEO_HEIGHT;
    const drawing = canvas.getContext('2d');
    if (!drawing) fail('CANVAS_UNAVAILABLE', 'startPublisher');
    let frame = 0;
    const draw = () => {
      drawing.fillStyle = frame++ % 2 ? '#124178' : '#207830';
      drawing.fillRect(0, 0, VIDEO_WIDTH, VIDEO_HEIGHT);
    };
    draw(); drawTimer = setInterval(draw, SECOND_MS / VIDEO_FPS);
    const stream = canvas.captureStream(VIDEO_FPS);
    for (const track of stream.getTracks()) ownedTracks.add(track);
    const audioTracks = destination.stream.getAudioTracks(), videoTracks = stream.getVideoTracks();
    if (audioTracks.length !== 1 || videoTracks.length !== 1) fail('INVALID_SYNTHETIC_MEDIA', 'startPublisher');
    pc.addTransceiver(audioTracks[0], { direction: 'sendonly', streams: [destination.stream] });
    pc.addTransceiver(videoTracks[0], { direction: 'sendonly', streams: [stream] });
  }
  function observeViewer() {
    video = document.getElementById('remote-video');
    if (!video || typeof video.requestVideoFrameCallback !== 'function') fail('FRAME_EVIDENCE_UNAVAILABLE', 'startViewer');
    remoteStream = new MediaStream(); video.srcObject = remoteStream;
    const frame = (_now, metadata) => {
      if (phase === 'closing' || phase === 'closed') return;
      presentedFrames = metadata.presentedFrames;
      frameHandle = video.requestVideoFrameCallback(frame);
    };
    frameHandle = video.requestVideoFrameCallback(frame);
    pc.ontrack = (event) => {
      if (phase === 'closing' || phase === 'closed') { event.track.stop(); return; }
      if (remoteTracks.get(event.track.kind) === event.track) return;
      if (!['audio', 'video'].includes(event.track.kind) || remoteTracks.has(event.track.kind)) {
        event.track.stop();
        callbackFailure = new BrowserAcceptanceError('UNEXPECTED_REMOTE_TRACK', 'track');
        operationController?.abort(callbackFailure); return;
      }
      ownedTracks.add(event.track);
      remoteTracks.set(event.track.kind, event.track); remoteStream.addTrack(event.track);
    };
    pc.addTransceiver('audio', { direction: 'recvonly' });
    pc.addTransceiver('video', { direction: 'recvonly' });
  }
  async function mutate(name, initial, next, work) {
    if (phase !== initial || operationController) fail('INVALID_PHASE', name);
    phase = next;
    const controller = new AbortController(); operationController = controller;
    try {
      await scoped(configuration.limits.operation_timeout_ms, null, name, work, controller);
      if (callbackFailure) throw callbackFailure;
      phase = 'active'; return await snapshot();
    } catch (error) {
      if (phase !== 'closing' && phase !== 'closed') phase = 'failed';
      throw sanitized(error, name);
    } finally { if (operationController === controller) operationController = null; }
  }
  function start(requestedRole) {
    const name = requestedRole === 'publisher' ? 'startPublisher' : 'startViewer';
    return mutate(name, 'configured', 'starting', async (context) => {
      role = requestedRole; generation = 1; pc = new RTCPeerConnection(rtcConfiguration(configuration.turn));
      if (role === 'publisher') await synthetic(context); else observeViewer();
      const offer = await context.wait(pc.createOffer());
      await context.wait(pc.setLocalDescription(offer));
      verifyOffer(pc.localDescription.sdp, name);
      // Send the checked actual local description; candidates travel in the subsequent bounded PATCH.
      const offerBody = pc.localDescription.sdp.split(CRLF).filter((line) =>
        !line.startsWith('a=candidate:') && line !== 'a=end-of-candidates').join(CRLF);
      const post = await request({ operation: name, url: role === 'publisher' ? configuration.whip_url : configuration.whep_url,
        method: 'POST', body: offerBody, type: SDP_TYPE, status: 201, responseType: SDP_TYPE, requireEtag: true }, context);
      etag = post.etag; evidence.post_status = post.status;
      description(post.body, name);
      await context.wait(pc.setRemoteDescription({ type: 'answer', sdp: post.body }));
      remoteAnswer = post.body; evidence.remote_answer_applied = true;
      if (video) await context.wait(video.play());
      await gather(context);
      const local = verifyOffer(pc.localDescription.sdp, name).fragment;
      localIce = { ufrag: local.ufrag, pwd: local.pwd };
      local.media.forEach((media) => { media.endOfCandidates = true; });
      const patch = await request({ operation: 'trickle', method: 'PATCH', url: resource,
        body: buildSdpfrag(local), type: FRAGMENT_TYPE, etag, status: 204, requireEtag: true }, context);
      if (patch.etag !== etag) fail('HTTP_UNEXPECTED_ETAG', 'trickle');
      evidence.trickle_status = patch.status;
    });
  }
  const startPublisher = () => start('publisher');
  const startViewer = () => start('viewer');

  function restartedAnswer(fragment) {
    const parsed = description(remoteAnswer, 'restartIce');
    if (!fragment.ufrag || !fragment.pwd || fragment.ufrag === parsed.fragment.ufrag || fragment.pwd === parsed.fragment.pwd) {
      fail('UNCHANGED_REMOTE_ICE', 'restartIce');
    }
    for (const item of fragment.media) {
      const media = parsed.media.find((entry) => entry.mid === item.mid);
      if (!media || media.mLine.split(' ')[0] !== item.mLine.split(' ')[0]) fail('INVALID_RESTART_MEDIA', 'restartIce');
    }
    const keep = (line) => !/^a=(ice-ufrag:|ice-pwd:|candidate:|end-of-candidates$)/.test(line);
    const lines = parsed.session.filter(keep);
    for (const media of parsed.media) {
      lines.push(...media.lines.filter(keep), `a=ice-ufrag:${fragment.ufrag}`, `a=ice-pwd:${fragment.pwd}`);
      const incoming = fragment.media.find((item) => item.mid === media.mid);
      if (incoming) {
        for (const value of incoming.candidates) lines.push(`a=${value}`);
        if (incoming.endOfCandidates) lines.push('a=end-of-candidates');
      }
    }
    remember(fragment.ufrag); remember(fragment.pwd);
    return lines.join(CRLF) + CRLF;
  }

  /** Requires the exact next generation. Optional turn replaces credentials for
   * subsequent allocations; it never changes the fixed relay policy or endpoint.
   */
  async function restartIce(value) {
    if (phase !== 'active') fail('INVALID_PHASE', 'restartIce');
    if (!object(value) || Object.keys(value).some((key) => !['generation', 'turn'].includes(key)) ||
        !Number.isSafeInteger(value.generation) || value.generation !== generation + 1) fail('INVALID_GENERATION', 'restartIce');
    if (value.generation > configuration.limits.max_generations) fail('GENERATION_LIMIT', 'restartIce');
    let turn;
    try { turn = value.turn === undefined ? configuration.turn : validTurn(value.turn); }
    catch { fail('INVALID_CONFIGURATION', 'restartIce'); }
    return mutate('restartIce', 'active', 'restarting', async (context) => {
      generation = value.generation;
      remember(turn.username); remember(turn.credential);
      pc.setConfiguration(rtcConfiguration(turn)); configuration.turn = turn;
      const offer = await context.wait(pc.createOffer({ iceRestart: true }));
      await context.wait(pc.setLocalDescription(offer)); await gather(context);
      const fragment = verifyOffer(pc.localDescription.sdp, 'restartIce').fragment;
      if (fragment.ufrag === localIce.ufrag || fragment.pwd === localIce.pwd) fail('UNCHANGED_LOCAL_ICE', 'restartIce');
      fragment.media.forEach((media) => { media.endOfCandidates = true; });
      const body = buildSdpfrag(fragment), previousEtag = etag;
      const patch = await request({ operation: 'restartIce', method: 'PATCH', url: resource, body,
        type: FRAGMENT_TYPE, etag: previousEtag, status: 200, responseType: FRAGMENT_TYPE, requireEtag: true }, context);
      if (patch.etag === previousEtag) fail('HTTP_UNCHANGED_ETAG', 'restartIce');
      // SFU committed its generation. Retain its ETag even if applying the answer fails.
      etag = patch.etag; evidence.restart_status = patch.status;
      let answer;
      try { answer = restartedAnswer(parseSdpfrag(patch.body)); }
      catch (error) { if (error instanceof BrowserAcceptanceError) throw error; fail('INVALID_RESTART_FRAGMENT', 'restartIce'); }
      await context.wait(pc.setRemoteDescription({ type: 'answer', sdp: answer }));
      remoteAnswer = answer; localIce = { ufrag: fragment.ufrag, pwd: fragment.pwd };
      const stale = await request({ operation: 'staleEtag', method: 'PATCH', url: resource, body,
        type: FRAGMENT_TYPE, etag: previousEtag, status: 412 }, context);
      evidence.stale_etag_status = stale.status;
    });
  }

  async function snapshot() {
    if (snapshotController) fail('SNAPSHOT_BUSY', 'snapshot');
    if (callbackFailure) throw callbackFailure;
    const controller = new AbortController(); snapshotController = controller;
    const limits = configuration?.limits ?? DEFAULT_LIMITS;
    try {
      return await scoped(limits.request_timeout_ms, null, 'snapshot', async (context) => {
        const raw = pc ? await context.wait(pc.getStats()) : new Map();
        if (raw.size > limits.stats_records) fail('STATS_LIMIT', 'snapshot');
        const stats = [];
        for (const entry of raw.values()) {
          const normalized = {};
          for (const [key, value] of Object.entries(entry)) {
            if (!STATS_FIELDS.has(key)) continue;
            if (typeof value === 'string') {
              if (value.length > MAX_TEXT) fail('STATS_LIMIT', 'snapshot');
              normalized[key] = redact(value);
            } else if (typeof value === 'boolean' || (typeof value === 'number' && Number.isFinite(value))) normalized[key] = value;
          }
          stats.push(normalized);
        }
        return { schema_version: 1, phase, role, generation, timestamp_ms: performance.now(),
          connection_state: pc?.connectionState ?? null, ice_connection_state: pc?.iceConnectionState ?? null,
          signaling_state: pc?.signalingState ?? null, stats,
          api: { ...evidence, local_media_order: [...evidence.local_media_order],
            location: resource === null ? null : redact(resource), etag,
            session_id: resource === null ? null : redact(new URL(resource).pathname.split('/').at(-1)) },
          media: { remote_tracks: [...remoteTracks.values()].map((track) => ({ kind: track.kind,
            ready_state: track.readyState, muted: track.muted })), presented_frames: presentedFrames },
          cleanup_errors: cleanupErrors.map((error) => ({ ...error })) };
      }, controller);
    } catch (error) { throw sanitized(error, 'snapshot'); }
    finally { if (snapshotController === controller) snapshotController = null; }
  }

  function close() {
    if (closePromise) return closePromise;
    phase = 'closing';
    operationController?.abort(new BrowserAcceptanceError('OPERATION_CANCELLED', 'close'));
    snapshotController?.abort(new BrowserAcceptanceError('OPERATION_CANCELLED', 'close'));
    const settings = configuration;
    const cap = settings?.limits.close_timeout_ms ?? DEFAULT_LIMITS.close_timeout_ms;
    closePromise = (async () => {
      const attempt = async (operation, work) => {
        try { await work(); }
        catch (error) { const safe = sanitized(error, operation); cleanupErrors.push({ code: safe.code, operation: safe.operation }); }
      };
      if (drawTimer !== null) { clearInterval(drawTimer); drawTimer = null; }
      if (frameHandle !== null) { video.cancelVideoFrameCallback(frameHandle); frameHandle = null; }
      if (pc) pc.ontrack = null;
      const deadline = performance.now() + cap;
      try {
        if (resource && settings) await attempt('delete', () => scoped(cap, null, 'delete', async (context) => {
          const result = await request({ operation: 'delete', method: 'DELETE', url: resource, status: 204 }, context, settings);
          evidence.delete_status = result.status;
        }));
        await attempt('closePeer', () => { pc?.close(); });
        for (const track of ownedTracks) await attempt('stopTrack', () => track.stop());
        await attempt('stopAudio', () => { oscillator?.stop(); oscillator?.disconnect(); destination?.disconnect(); });
        await attempt('detachVideo', () => { if (video) { video.pause(); video.srcObject = null; } });
        if (audio) await attempt('closeAudio', () => scoped(Math.max(1, deadline - performance.now()), null,
          'closeAudio', (context) => context.wait(audio.close())));
      } finally {
        pc = null; audio = null; oscillator = null; destination = null; canvas = null; video = null; remoteStream = null;
        ownedTracks.clear(); remoteTracks.clear(); configuration = null; resource = null; etag = null;
        localIce = null; remoteAnswer = null; secrets.clear(); callbackFailure = null; phase = 'closed';
      }
      return { phase, cleanup_errors: cleanupErrors.map((error) => ({ ...error })) };
    })();
    return closePromise;
  }

  window.turboAcceptance = Object.freeze({ configure, startPublisher, startViewer, restartIce, snapshot, close });
})();
