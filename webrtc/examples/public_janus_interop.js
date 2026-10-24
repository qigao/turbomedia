#!/usr/bin/env node
'use strict';

// Native TurboMedia ICE/DTLS/SCTP against the public Janus EchoTest service.
// The local WebSocket adapts signaling only; media packets travel to Janus.
const crypto = require('node:crypto');
const http = require('node:http');
const path = require('node:path');
const { spawn } = require('node:child_process');
const JANUS_WS = process.env.PUBLIC_JANUS_WS || 'wss://janus.conf.meetecho.com/ws';
const NATIVE_BIN = process.env.WEBRTC_SIGNALLED_PEER_BIN || process.argv[2] ||
  path.join(process.cwd(), 'build', 'platform-Linux', 'bin', 'rtc_signaled_peer');
const TIMEOUT = Number(process.env.PUBLIC_WEBRTC_TIMEOUT_MS || 90000);
const PAYLOAD = 'hello over signaled ICE';
const ECHO_PREFIX = 'Janus EchoTest here! You wrote: ';
const txid = () => crypto.randomBytes(12).toString('hex');

function timeout(promise, ms, name) {
  let timer;
  return Promise.race([
    promise,
    new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(name + ' timeout')), ms); }),
  ]).finally(() => clearTimeout(timer));
}
function frame(op, value) {
  const p = Buffer.isBuffer(value) ? value : Buffer.from(value);
  let h;
  if (p.length < 126) h = Buffer.from([0x80 | op, p.length]);
  else if (p.length < 65536) {
    h = Buffer.alloc(4); h[0] = 0x80 | op; h[1] = 126; h.writeUInt16BE(p.length, 2);
  } else {
    h = Buffer.alloc(10); h[0] = 0x80 | op; h[1] = 127; h.writeBigUInt64BE(BigInt(p.length), 2);
  }
  return Buffer.concat([h, p]);
}

class LocalPeer {
  constructor(socket, head, onMessage) {
    this.socket = socket; this.buf = Buffer.alloc(0); this.closed = false; this.onMessage = onMessage;
    socket.setNoDelay(true);
    socket.on('data', b => this.consume(b));
    socket.on('close', () => {
      this.closed = true;
      console.log('[local-signaling] native WebSocket TCP connection closed');
    });
    socket.on('error', e => console.error('[local-ws] ' + e.message));
    if (head.length) this.consume(head);
  }
  send(obj) {
    if (this.closed) return;
    const summary = { type: obj && obj.type };
    if (obj && typeof obj.sdp === 'string') summary.sdp_bytes = Buffer.byteLength(obj.sdp);
    if (obj && typeof obj.candidate === 'string') summary.candidate_type = obj.candidate.match(/\btyp\s+(\w+)/)?.[1] || 'unknown';
    console.log('[local-signaling] -> native ' + JSON.stringify(summary));
    this.socket.write(frame(1, JSON.stringify(obj)));
  }
  consume(chunk) {
    this.buf = Buffer.concat([this.buf, chunk]);
    while (this.buf.length >= 2) {
      const b0 = this.buf[0], b1 = this.buf[1], op = b0 & 15, masked = !!(b1 & 128);
      let n = b1 & 127, off = 2;
      if (n === 126) { if (this.buf.length < 4) return; n = this.buf.readUInt16BE(2); off = 4; }
      else if (n === 127) {
        if (this.buf.length < 10) return;
        const wide = this.buf.readBigUInt64BE(2); if (wide > 65536n) return this.socket.destroy();
        n = Number(wide); off = 10;
      }
      let mask;
      if (masked) { if (this.buf.length < off + 4) return; mask = this.buf.subarray(off, off + 4); off += 4; }
      if (n > 65536) return this.socket.destroy();
      if (this.buf.length < off + n) return;
      const p = Buffer.from(this.buf.subarray(off, off + n)); this.buf = this.buf.subarray(off + n);
      if (masked) for (let i = 0; i < p.length; i++) p[i] ^= mask[i & 3];
      if (op === 8) {
        console.log('[local-signaling] native peer sent WebSocket CLOSE');
        this.socket.end(frame(8, p));
        this.closed = true;
        return;
      }
      if (op === 9) { this.socket.write(frame(10, p)); continue; }
      if (op === 1) this.onMessage(p.toString('utf8'));
    }
  }
  close() { if (!this.closed) { this.closed = true; this.socket.end(frame(8, Buffer.alloc(0))); } }
}

function localSignaling(onMessage) {
  let peer = null, chain = Promise.resolve();
  const server = http.createServer((_req, res) => { res.writeHead(404); res.end(); });
  server.on('upgrade', (req, socket, head) => {
    const protocols = String(req.headers['sec-websocket-protocol'] || '').split(',').map(s => s.trim());
    const key = req.headers['sec-websocket-key'];
    if (req.url !== '/' || !protocols.includes('webrtc-signaling') || !key) {
      socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n'); return;
    }
    const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' +
      accept + '\r\nSec-WebSocket-Protocol: webrtc-signaling\r\n\r\n');
    if (peer && !peer.closed) { socket.destroy(); return; }
    peer = new LocalPeer(socket, head, raw => {
      chain = chain.then(async () => {
        const message = JSON.parse(raw);
        const summary = { type: message && message.type };
        if (message && typeof message.sdp === 'string') summary.sdp_bytes = Buffer.byteLength(message.sdp);
        if (message && typeof message.candidate === 'string') summary.candidate_type = message.candidate.match(/\btyp\s+(\w+)/)?.[1] || 'unknown';
        console.log('[local-signaling] <- native ' + JSON.stringify(summary));
        await onMessage(message, peer);
      }).catch(e => {
        console.error('[signaling] ' + (e.stack || e));
        if (peer && !peer.closed) peer.send({ type: 'error', message: String(e.message || e) });
      });
    });
  });
  return {
    server,
    getPeer: () => peer,
    listen: () => new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', () => { server.removeListener('error', reject); resolve(server.address().port); });
    }),
    close: () => new Promise(resolve => { if (peer) peer.close(); server.close(() => resolve()); }),
  };
}

async function connectJanus() {
  if (typeof WebSocket !== 'function') throw new Error('Node 22+ built-in WebSocket is required');
  const ws = new WebSocket(JANUS_WS, ['janus-protocol']);
  const pending = new Map(), handlers = new Set();
  let closed = false;
  ws.addEventListener('message', ev => {
    let m; try { m = JSON.parse(String(ev.data)); } catch (error) {
      console.error('[janus] invalid JSON response: ' + error.message);
      return;
    }
    if (process.env.PUBLIC_JANUS_DEBUG === '1') {
      console.log('[janus-wire] ' + JSON.stringify({
        janus: m.janus,
        transaction: m.transaction,
        session_id: m.session_id,
        sender: m.sender,
        error: m.error,
        has_jsep: Boolean(m.jsep),
        jsep_type: m.jsep && m.jsep.type,
        candidate_completed: m.candidate && m.candidate.completed,
      }));
    }
    if (m.transaction && pending.has(m.transaction)) {
      const p = pending.get(m.transaction);
      if ((m.janus === 'ack' && p.ack) || m.janus === 'success') {
        pending.delete(m.transaction); clearTimeout(p.timer); p.resolve(m); return;
      }
      if (m.janus === 'error') {
        pending.delete(m.transaction); clearTimeout(p.timer);
        p.reject(new Error('Janus API error during ' + p.operation + ': ' + JSON.stringify(m.error || m))); return;
      }
    }
    for (const fn of handlers) { try { fn(m); } catch (e) { console.error('[janus-event] ' + e.message); } }
  });
  ws.addEventListener('close', () => {
    closed = true;
    for (const p of pending.values()) { clearTimeout(p.timer); p.reject(new Error('Janus WebSocket closed')); }
    pending.clear();
  });
  await timeout(new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, { once: true });
    ws.addEventListener('error', () => reject(new Error('public Janus WebSocket connection failed')), { once: true });
  }), TIMEOUT, 'Janus WebSocket connect');
  return {
    request(payload, ack) {
      if (closed || ws.readyState !== WebSocket.OPEN) return Promise.reject(new Error('Janus socket is closed'));
      const id = txid();
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => { pending.delete(id); reject(new Error('Janus request timeout: ' + payload.janus)); }, TIMEOUT);
        pending.set(id, { resolve, reject, timer, ack: !!ack, operation: payload.janus });
        ws.send(JSON.stringify(Object.assign({}, payload, { transaction: id })));
      });
    },
    onEvent: fn => handlers.add(fn),
    close: () => { try { ws.close(); } catch {} },
  };
}

async function main() {
  if (!NATIVE_BIN) throw new Error('WEBRTC_SIGNALLED_PEER_BIN is required');
  const janus = await connectJanus();
  let sessionId = null, handleId = null, local = null, child = null, nativeTimedOut = false;
  let answerSent = false, offerSeen = false;
  const queuedCandidates = [];       // Janus -> native peer
  const queuedEnd = { value: false };
  const localCandidatesPending = []; // native peer -> Janus
  let localEndPending = false;

  try {
    const created = await janus.request({ janus: 'create' }, false);
    sessionId = created.data && created.data.id;
    if (!sessionId) throw new Error('Janus create response omitted session id');
    const attached = await janus.request({
      janus: 'attach', plugin: 'janus.plugin.echotest', session_id: sessionId,
    }, false);
    handleId = attached.data && attached.data.id;
    if (!handleId) throw new Error('Janus attach response omitted handle id');
    console.log('[public-webrtc] Janus session created; EchoTest handle attached');

    local = localSignaling(async (m, peer) => {
      if (m.type === 'join') {
        peer.send({ type: 'joined', peerId: 'native-public-janus-test' });
      } else if (m.type === 'list-peers') {
        peer.send({ type: 'peers', peers: ['public-janus-echotest'] });
      } else if (m.type === 'offer') {
        if (offerSeen) throw new Error('native peer sent more than one SDP offer');
        if (typeof m.sdp !== 'string' || !m.sdp.includes('m=application')) {
          throw new Error('native offer has no SCTP application m-line');
        }
        offerSeen = true;
        await janus.request({
          janus: 'message', session_id: sessionId, handle_id: handleId,
          body: { audio: false, video: false }, jsep: { type: 'offer', sdp: m.sdp },
        }, true);
        console.log('[public-webrtc] native SDP offer submitted to public Janus');
      } else if (m.type === 'candidate') {
        if (typeof m.candidate !== 'string' || !m.candidate.startsWith('candidate:')) return;
        // Queue native candidates until Janus has returned its SDP answer.
        if (!answerSent) {
          localCandidatesPending.push(m.candidate);
          return;
        }
        await janus.request({
          janus: 'trickle', session_id: sessionId, handle_id: handleId,
          candidate: { candidate: m.candidate, sdpMid: '0', sdpMLineIndex: 0 },
        }, true);
      } else if (m.type === 'end-of-candidates') {
        if (!answerSent) {
          localEndPending = true;
          return;
        }
        await janus.request({
          janus: 'trickle', session_id: sessionId, handle_id: handleId,
          candidate: { completed: true },
        }, true);
      } else if (m.type === 'error') {
        throw new Error('native signaling client error: ' + (m.message || 'unknown'));
      }
    });
    const port = await local.listen();

    janus.onEvent(m => {
      if (process.env.PUBLIC_JANUS_DEBUG === '1') {
        console.log('[janus-event] ' + JSON.stringify({
          janus: m.janus,
          session_id: m.session_id,
          sender: m.sender,
          transaction: m.transaction,
          error: m.error,
          has_jsep: Boolean(m.jsep),
          jsep_type: m.jsep && m.jsep.type,
          candidate_completed: m.candidate && m.candidate.completed,
        }));
      }
      if (m.janus === 'error') {
        console.error('[public-webrtc] Janus API error event: ' + JSON.stringify(m.error || m));
        return;
      }
      // Janus transports can omit session_id on session-scoped events; this
      // WebSocket is dedicated to this session. Validate it only when present.
      if (m.session_id !== undefined && String(m.session_id) !== String(sessionId)) return;
      if (m.sender !== undefined && String(m.sender) !== String(handleId)) return;
      const peer = local && local.getPeer();
      if (!peer || peer.closed) return;
      if (m.janus === 'event' && m.jsep && m.jsep.type === 'answer' && typeof m.jsep.sdp === 'string') {
        peer.send({ type: 'answer', sdp: m.jsep.sdp });
        answerSent = true;
        console.log('[public-webrtc] received public Janus SDP answer');
        for (const candidate of queuedCandidates.splice(0)) peer.send({ type: 'candidate', candidate });
        if (queuedEnd.value) peer.send({ type: 'end-of-candidates' });
        void (async () => {
          for (const candidate of localCandidatesPending.splice(0)) {
            await janus.request({
              janus: 'trickle', session_id: sessionId, handle_id: handleId,
              candidate: { candidate, sdpMid: '0', sdpMLineIndex: 0 },
            }, true);
          }
          if (localEndPending) {
            localEndPending = false;
            await janus.request({
              janus: 'trickle', session_id: sessionId, handle_id: handleId,
              candidate: { completed: true },
            }, true);
          }
          console.log('[public-webrtc] flushed local ICE candidates to public Janus');
        })().catch(error => {
          console.error('[public-webrtc] failed to trickle native ICE to Janus: ' + error.message);
          if (peer && !peer.closed) peer.send({ type: 'error', message: error.message });
        });
      } else if (m.janus === 'trickle' && m.candidate) {
        if (m.candidate.completed) {
          if (answerSent) peer.send({ type: 'end-of-candidates' });
          else queuedEnd.value = true;
        } else if (typeof m.candidate.candidate === 'string') {
          if (answerSent) peer.send({ type: 'candidate', candidate: m.candidate.candidate });
          else queuedCandidates.push(m.candidate.candidate);
        }
      } else if (m.janus === 'error') {
        console.error('[public-webrtc] Janus error event: ' + JSON.stringify(m.error || m));
      } else if (m.janus === 'hangup') {
        console.error('[public-webrtc] Janus reported hangup');
      }
    });

    console.log('[public-webrtc] starting native peer: ' + NATIVE_BIN);
    child = spawn(NATIVE_BIN, [
      '--offer', '--room', 'public-janus', '--server', '127.0.0.1',
      '--port', String(port), '--path', '/', '--force-stun', '--janus-echo',
    ], { stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '', stderr = '';
    child.stdout.on('data', b => { stdout = (stdout + b.toString()).slice(-1000000); process.stdout.write(b); });
    child.stderr.on('data', b => { stderr = (stderr + b.toString()).slice(-1000000); process.stderr.write(b); });
    const exit = await timeout(new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('close', (code, signal) => resolve({ code, signal }));
    }), TIMEOUT, 'native DataChannel to public Janus EchoTest');
    if (exit.code !== 0 || nativeTimedOut) {
      throw new Error('native public Janus DataChannel test failed: exit=' + exit.code +
        ' signal=' + exit.signal + '\n' + stderr + '\n' + stdout);
    }
    const combinedOutput = stdout + '\n' + stderr;
    if (!offerSeen || !answerSent ||
        !combinedOutput.includes('PUBLIC_JANUS_ECHO_PASS: exact DataChannel payload returned')) {
      throw new Error('native process exited without exact public Janus DataChannel echo evidence');
    }
    console.log(JSON.stringify({
      result: 'PASS',
      browser_launched: false,
      public_webrtc_server: JANUS_WS,
      janus_plugin: 'janus.plugin.echotest',
      native_sdp_offer: true,
      public_janus_sdp_answer: true,
      dtls_sctp_datachannel_echo: true,
      exact_payload: PAYLOAD,
      echoed_payload: ECHO_PREFIX + PAYLOAD,
      native_exit_code: exit.code,
      production_backend: 'usrsctp',
      note: 'This exercises the current production DataChannel path; it is not C11 SCTP cutover qualification.',
    }, null, 2));
  } finally {
    if (child && child.exitCode === null) {
      nativeTimedOut = true;
      try { child.kill('SIGTERM'); } catch {}
      setTimeout(() => { try { if (child && child.exitCode === null) child.kill('SIGKILL'); } catch {} }, 1500).unref();
    }
    if (local) await local.close();
    if (handleId !== null && sessionId !== null) {
      try { await janus.request({ janus: 'detach', session_id: sessionId, handle_id: handleId }, false); }
      catch (e) { console.error('[cleanup] Janus detach: ' + e.message); }
    }
    if (sessionId !== null) {
      try { await janus.request({ janus: 'destroy', session_id: sessionId }, false); }
      catch (e) { console.error('[cleanup] Janus destroy: ' + e.message); }
    }
    janus.close();
  }
}

main().catch(error => {
  console.error(JSON.stringify({ result: 'FAIL', browser_launched: false, error: String(error.stack || error) }, null, 2));
  process.exitCode = 1;
});
