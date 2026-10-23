#!/usr/bin/env node
'use strict';

// Public-service preflight: direct RFC 5389 UDP probes, the repository's native
// ICE agent against public STUN, and Janus REST/session/plugin availability.
// This deliberately does not launch a browser or claim a DataChannel E2E pass.
const crypto = require('node:crypto');
const dgram = require('node:dgram');
const dns = require('node:dns').promises;
const { spawn } = require('node:child_process');

const MAGIC_COOKIE = 0x2112a442;
const STUN_SERVERS = [
  { host: 'stun.l.google.com', port: 19302 },
  { host: 'stun.cloudflare.com', port: 3478 },
];
const JANUS_API = (process.env.PUBLIC_JANUS_API || 'https://janus.conf.meetecho.com/janus').replace(/\/+$/, '');
const ICE_PROBE_BIN = process.env.WEBRTC_ICE_PROBE_BIN || process.argv[2] || '';
const TIMEOUT_MS = Number(process.env.PUBLIC_SERVICE_TIMEOUT_MS || 5000);

function makeBindingRequest(txid) {
  const request = Buffer.alloc(20);
  request.writeUInt16BE(0x0001, 0);
  request.writeUInt16BE(0, 2);
  request.writeUInt32BE(MAGIC_COOKIE, 4);
  txid.copy(request, 8);
  return request;
}

function decodeMappedAddress(packet) {
  if (packet.length < 20 || packet.readUInt16BE(0) !== 0x0101) {
    throw new Error('not a STUN Binding Success Response');
  }
  if (packet.readUInt32BE(4) !== MAGIC_COOKIE) throw new Error('wrong STUN magic cookie');
  const bodyLength = packet.readUInt16BE(2);
  if ((bodyLength & 3) !== 0 || packet.length < 20 + bodyLength) {
    throw new Error('invalid STUN message length');
  }
  let offset = 20;
  const end = 20 + bodyLength;
  while (offset + 4 <= end) {
    const type = packet.readUInt16BE(offset);
    const length = packet.readUInt16BE(offset + 2);
    const value = offset + 4;
    if (value + length > end) throw new Error('truncated STUN attribute');
    if ((type === 0x0020 || type === 0x0001) && length >= 8) {
      const family = packet[value + 1];
      if (family === 1 && length === 8) {
        const xor = type === 0x0020;
        const port = packet.readUInt16BE(value + 2) ^ (xor ? (MAGIC_COOKIE >>> 16) : 0);
        const cookie = Buffer.from([0x21, 0x12, 0xa4, 0x42]);
        const address = [];
        for (let i = 0; i < 4; i++) {
          address.push(packet[value + 4 + i] ^ (xor ? cookie[i] : 0));
        }
        return {
          address: address.join('.'),
          port,
          attribute: type === 0x0020 ? 'XOR-MAPPED-ADDRESS' : 'MAPPED-ADDRESS',
        };
      }
    }
    offset = value + ((length + 3) & ~3);
  }
  throw new Error('STUN response omitted an IPv4 mapped-address attribute');
}

async function probeStun(server) {
  const started = process.hrtime.bigint();
  let socket;
  try {
    const resolved = await dns.lookup(server.host, { family: 4 });
    const txid = crypto.randomBytes(12);
    const request = makeBindingRequest(txid);
    const result = await new Promise((resolve, reject) => {
      socket = dgram.createSocket('udp4');
      let settled = false;
      const finish = (error, value) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        try { socket.close(); } catch {}
        if (error) reject(error); else resolve(value);
      };
      const timer = setTimeout(() => finish(new Error('STUN response timeout')), TIMEOUT_MS);
      socket.on('error', (error) => finish(error));
      socket.on('message', (packet, remote) => {
        try {
          if (remote.port !== server.port) throw new Error('STUN response came from an unexpected port');
          if (packet.length < 20 || !packet.subarray(8, 20).equals(txid)) {
            throw new Error('STUN transaction ID mismatch');
          }
          const mapped = decodeMappedAddress(packet);
          finish(null, {
            server: server.host + ':' + server.port,
            status: 'PASS',
            rtt_ms: Number(process.hrtime.bigint() - started) / 1e6,
            mapped_address_received: true,
            mapped_address_family: 'IPv4',
            mapped_attribute: mapped.attribute,
          });
        } catch (error) {
          finish(error);
        }
      });
      socket.send(request, server.port, resolved.address, (error) => {
        if (error) finish(error);
      });
    });
    return result;
  } catch (error) {
    if (socket) { try { socket.close(); } catch {} }
    return {
      server: server.host + ':' + server.port,
      status: 'FAIL',
      error: String(error.message || error),
    };
  }
}

function runNativeIceProbe(binary) {
  if (!binary) return Promise.reject(new Error('WEBRTC_ICE_PROBE_BIN is required'));
  return new Promise((resolve, reject) => {
    const child = spawn(binary, [], { stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    let settled = false;
    const append = (current, chunk) => (current + chunk.toString()).slice(-1024 * 1024);
    child.stdout.on('data', (chunk) => { stdout = append(stdout, chunk); });
    child.stderr.on('data', (chunk) => { stderr = append(stderr, chunk); });
    child.on('error', (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(error);
    });
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      reject(new Error('native ICE/STUN candidate gathering exceeded ' + (TIMEOUT_MS * 4) + ' ms'));
    }, TIMEOUT_MS * 4);
    child.on('close', (code, signal) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      const output = stdout + '\n' + stderr;
      const lines = output.split(/\r?\n/).filter((line) => line.includes('[ICE] Local candidate:'));
      const types = { host: 0, srflx: 0, relay: 0, other: 0 };
      for (const line of lines) {
        const match = line.match(/\btyp\s+(host|srflx|relay)\b/i);
        if (match) types[match[1].toLowerCase()]++; else types.other++;
      }
      if (code !== 0) {
        reject(new Error('native ICE probe exited with code ' + code + ' signal ' + signal +
          '; candidate_types=' + JSON.stringify(types)));
      } else if (types.srflx === 0) {
        reject(new Error('native ICE agent did not gather a public srflx candidate; candidate_types=' +
          JSON.stringify(types)));
      } else {
        resolve({
          status: 'PASS',
          candidate_types: types,
          used_public_stun: true,
          browser_launched: false,
        });
      }
    });
  });
}

function newTransaction() {
  return crypto.randomBytes(8).toString('hex');
}

async function janusPost(url, payload) {
  const response = await fetch(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(payload),
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  let body;
  try { body = await response.json(); }
  catch { throw new Error('Janus returned non-JSON HTTP ' + response.status); }
  if (!response.ok) throw new Error('Janus HTTP ' + response.status);
  if (body.janus !== 'success' || body.transaction !== payload.transaction) {
    throw new Error('Janus rejected ' + payload.janus + ': ' + JSON.stringify(body.error || body));
  }
  return body;
}

async function probeJanus() {
  const infoResponse = await fetch(JANUS_API + '/info', {
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  if (!infoResponse.ok) throw new Error('Janus info HTTP ' + infoResponse.status);
  const info = await infoResponse.json();
  if (info.janus !== 'server_info') throw new Error('Janus info endpoint returned an unexpected response');
  if (info.data_channels !== true) throw new Error('public Janus endpoint does not advertise DataChannels');
  if (!info.plugins || !info.plugins['janus.plugin.echotest']) {
    throw new Error('public Janus endpoint does not advertise janus.plugin.echotest');
  }

  let sessionId = null;
  let handleId = null;
  let attachPassed = false;
  let cleanupError = null;
  try {
    const created = await janusPost(JANUS_API, {
      janus: 'create',
      transaction: newTransaction(),
    });
    sessionId = created.data && created.data.id;
    if (!sessionId) throw new Error('Janus create succeeded without a session ID');

    const attached = await janusPost(JANUS_API + '/' + sessionId, {
      janus: 'attach',
      plugin: 'janus.plugin.echotest',
      transaction: newTransaction(),
    });
    handleId = attached.data && attached.data.id;
    if (!handleId) throw new Error('Janus attach succeeded without a plugin handle ID');
    attachPassed = true;
  } finally {
    if (handleId !== null) {
      try {
        await janusPost(JANUS_API + '/' + sessionId + '/' + handleId, {
          janus: 'detach',
          transaction: newTransaction(),
        });
      } catch (error) {
        cleanupError = 'detach: ' + String(error.message || error);
      }
    }
    if (sessionId !== null) {
      try {
        await janusPost(JANUS_API + '/' + sessionId, {
          janus: 'destroy',
          transaction: newTransaction(),
        });
      } catch (error) {
        cleanupError = (cleanupError ? cleanupError + '; ' : '') +
          'destroy: ' + String(error.message || error);
      }
    }
  }
  if (cleanupError) throw new Error('Janus cleanup failed: ' + cleanupError);
  if (!attachPassed) throw new Error('Janus EchoTest plugin attach did not complete');
  return {
    status: 'PASS',
    endpoint: JANUS_API,
    version: info.version_string || String(info.version || 'unknown'),
    data_channels: true,
    echo_plugin_attached: true,
    session_cleanup: 'PASS',
    scope: 'Janus REST control plane only; no JSEP/DTLS/SCTP DataChannel was negotiated',
  };
}

async function main() {
  if (!ICE_PROBE_BIN) throw new Error('Pass WEBRTC_ICE_PROBE_BIN=/path/to/browser_interop');
  const stunResults = await Promise.all(STUN_SERVERS.map(probeStun));
  if (!stunResults.some((result) => result.status === 'PASS')) {
    throw new Error('all public STUN Binding Request probes failed: ' + JSON.stringify(stunResults));
  }
  const nativeIce = await runNativeIceProbe(ICE_PROBE_BIN);
  const janus = await probeJanus();
  console.log(JSON.stringify({
    result: 'PASS',
    browser_launched: false,
    public_stun: stunResults,
    native_ice_public_stun: nativeIce,
    public_webrtc_service: janus,
  }, null, 2));
}

main().catch((error) => {
  console.error(JSON.stringify({
    result: 'FAIL',
    browser_launched: false,
    error: String(error.stack || error),
  }, null, 2));
  process.exitCode = 1;
});
