'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const modulePath = path.join(__dirname, '../src/sdpfrag.js');
function api() {
  assert.ok(fs.existsSync(modulePath), 'Task 8 SDP fragment implementation must exist');
  return require(modulePath);
}
const candidate = 'candidate:1 1 UDP 2122260223 192.0.2.1 50000 typ relay raddr 0.0.0.0 rport 0 generation 1';
const fragment = 'a=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuv\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\na=' + candidate + '\r\na=end-of-candidates\r\n';
const model = { ufrag: 'abcd', pwd: 'abcdefghijklmnopqrstuv', media: [
  { mLine: 'm=audio 9 UDP/TLS/RTP/SAVPF 111', mid: '0', candidates: [candidate], endOfCandidates: true },
] };

test('SDP fragment builder emits canonical CRLF and parser preserves generation data', () => {
  assert.equal(api().buildSdpfrag(model), fragment);
  assert.deepEqual(api().parseSdpfrag(fragment), model);
  assert.deepEqual(api().parseSdpfrag('a=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuv\r\n'),
    { ufrag: 'abcd', pwd: 'abcdefghijklmnopqrstuv', media: [] });
});

test('SDP fragment accepts separate media and candidate-only trickle', () => {
  const value = 'm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=candidate:a 1 tcp 123 2001:db8::1 9 typ relay tcptype passive\r\n';
  const parsed = api().parseSdpfrag(value);
  assert.equal(parsed.ufrag, null);
  assert.equal(parsed.media[0].mid, 'video');
  assert.equal(api().buildSdpfrag(parsed), value);
});

test('SDP fragment rejects unknown, duplicate, malformed, unsupported and misordered input without echo', () => {
  const invalid = [
    '', fragment.replaceAll('\r\n', '\n'), fragment.slice(0, -2), fragment + '\r\n',
    fragment + 'a=unknown:SECRET\r\n', fragment.replace('m=audio', 'a=ice-ufrag:abcd\r\nm=audio'),
    fragment.replace('m=audio', 'a=ice-pwd:abcdefghijklmnopqrstuv\r\nm=audio'),
    fragment.replace('a=mid:0', 'a=mid:0\r\na=mid:0'), fragment + 'a=end-of-candidates\r\n',
    fragment.replace('a=mid:0\r\n', ''), fragment.replace('m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n', ''),
    fragment.replace('a=ice-ufrag:abcd\r\n', ''), fragment.replace('a=ice-pwd:abcdefghijklmnopqrstuv\r\n', ''),
    fragment.replace('abcd', 'a\tbc'), fragment.replace('abcd', 'abéé'),
    fragment.replace('a=mid:0', 'a=mid:SECRET x'), fragment.replace('m=audio', 'm=application'),
    fragment.replace(' 50000 ', ' 65536 '), fragment.replace(' 1 UDP ', ' 0 UDP '),
    fragment.replace(' 2122260223 ', ' 4294967296 '), fragment.replace('typ relay', 'typ unknown'),
    fragment.replace('192.0.2.1', '999.0.2.1'), fragment.replace('generation 1', 'bogus SECRET'),
    fragment.replace('generation 1', 'generation 1 generation 1'), fragment.replace(' UDP ', ' TCP '),
    fragment.replace('a=end-of-candidates', 'a=end-of-candidates\r\na=' + candidate),
    fragment + 'm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\n',
  ];
  for (const value of invalid) {
    assert.throws(() => api().parseSdpfrag(value), (error) => {
      assert.equal(error.code, 'INVALID_SDP_FRAGMENT');
      assert.ok(!JSON.stringify(error).includes('SECRET'));
      return true;
    });
  }
  assert.throws(() => api().buildSdpfrag({ ...model, extra: 'SECRET' }), { code: 'INVALID_SDP_FRAGMENT' });
  assert.throws(() => api().buildSdpfrag({ ...model, media: [{ ...model.media[0], mid: '0\r\na=SECRET' }] }),
    { code: 'INVALID_SDP_FRAGMENT' });
  assert.throws(() => api().buildSdpfrag({ ...model, media: [{ ...model.media[0], candidates: [],
    mid: '0\r\na=candidate:1 1 UDP 123 192.0.2.1 50000 typ relay' }] }), { code: 'INVALID_SDP_FRAGMENT' });
  assert.throws(() => api().parseSdpfrag(fragment.replace('generation 1', 'ufrag:bad 1')),
    { code: 'INVALID_SDP_FRAGMENT' });
  assert.throws(() => api().parseSdpfrag(fragment.replace('generation 1', 'ufrag other')),
    { code: 'INVALID_SDP_FRAGMENT' });
});

test('SDP fragment enforces named byte cap before parsing or building', () => {
  const { MAX_SDPFRAG_BYTES, parseSdpfrag, buildSdpfrag } = api();
  assert.throws(() => parseSdpfrag('x'.repeat(MAX_SDPFRAG_BYTES + 1)), { code: 'SDP_FRAGMENT_LIMIT' });
  assert.throws(() => buildSdpfrag({ ...model, pwd: 'x'.repeat(MAX_SDPFRAG_BYTES + 1) }),
    { code: 'SDP_FRAGMENT_LIMIT' });
});
