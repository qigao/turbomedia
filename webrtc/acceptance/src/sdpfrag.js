'use strict';

// Classic-script lexical binding keeps helpers off window; Node tests use the same code.
const turboSdpfrag = (() => {
  const MAX_SDPFRAG_BYTES = 65_536;
  const MAX_MEDIA_SECTIONS = 2;
  const MAX_CANDIDATES = 64;
  const MAX_CANDIDATE_BYTES = 1_024;
  const MAX_PORT = 65_535;
  const MAX_PRIORITY = 4_294_967_295;
  const MAX_ICE_TEXT = 256;
  const MIN_UFRAG = 4;
  const MIN_PWD = 22;
  const MAX_MID = 64;
  const MAX_PAYLOAD_TYPE = 127;
  const CRLF = '\r\n';

  function fail(code = 'INVALID_SDP_FRAGMENT') {
    const error = new Error(code);
    error.name = 'SdpFragmentError'; error.code = code;
    throw error;
  }
  function bounded(value) {
    if (typeof value !== 'string') fail();
    if (value.length > MAX_SDPFRAG_BYTES || new TextEncoder().encode(value).byteLength > MAX_SDPFRAG_BYTES) {
      fail('SDP_FRAGMENT_LIMIT');
    }
    return value;
  }
  function fields(value, expected) {
    if (!value || typeof value !== 'object' || Array.isArray(value) ||
        Object.keys(value).some((key) => !expected.includes(key))) fail();
  }
  function integer(value, minimum, maximum) {
    return /^\d+$/.test(value) && Number.isSafeInteger(Number(value)) && Number(value) >= minimum && Number(value) <= maximum;
  }
  function address(value) {
    if (value.includes(':')) {
      try { return new URL(`https://[${value}]/`).hostname.startsWith('['); } catch { return false; }
    }
    if (/^[\d.]+$/.test(value)) {
      const parts = value.split('.');
      return parts.length === 4 && parts.every((part) => integer(part, 0, 255) && String(Number(part)) === part);
    }
    return value.length <= 253 && value.includes('.') && value.split('.').every((part) =>
      /^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$/.test(part));
  }
  function candidate(value, ufrag) {
    if (value.length > MAX_CANDIDATE_BYTES || !/^[\x21-\x7e]+(?: [\x21-\x7e]+)*$/.test(value)) fail();
    const tokens = value.split(' ');
    const BASE_FIELDS = 8;
    if (tokens.length < BASE_FIELDS || tokens.length % 2 !== 0 || !/^candidate:[a-zA-Z0-9+/]{1,32}$/.test(tokens[0]) ||
        !integer(tokens[1], 1, 2) || !/^(udp|tcp)$/i.test(tokens[2]) || !integer(tokens[3], 1, MAX_PRIORITY) ||
        !address(tokens[4]) || !integer(tokens[5], 1, MAX_PORT) || tokens[6] !== 'typ' ||
        !['host', 'srflx', 'prflx', 'relay'].includes(tokens[7])) fail();
    const extensions = new Map();
    for (let i = BASE_FIELDS; i < tokens.length; i += 2) {
      const name = tokens[i], data = tokens[i + 1];
      if (extensions.has(name)) fail();
      if (name === 'raddr') { if (!address(data)) fail(); }
      else if (name === 'rport') { if (!integer(data, 0, MAX_PORT)) fail(); }
      else if (name === 'tcptype') { if (!['active', 'passive', 'so'].includes(data)) fail(); }
      else if (['generation', 'network-id', 'network-cost'].includes(name)) { if (!integer(data, 0, MAX_PRIORITY)) fail(); }
      else if (name === 'ufrag') {
        if (!/^[a-zA-Z0-9+/]+$/.test(data) || data.length > MAX_ICE_TEXT || (ufrag !== null && data !== ufrag)) fail();
      }
      else fail();
      extensions.set(name, data);
    }
    if (extensions.has('raddr') !== extensions.has('rport') ||
        (tokens[2].toLowerCase() === 'tcp') !== extensions.has('tcptype')) fail();
  }
  function mediaLine(value) {
    if (!/^m=(audio|video) 9 (UDP\/TLS\/RTP\/SAVPF|RTP\/SAVPF)(?: \d+)+$/.test(value)) fail();
    const payloads = value.split(' ').slice(3);
    if (!payloads.every((id) => integer(id, 0, MAX_PAYLOAD_TYPE)) || new Set(payloads).size !== payloads.length) fail();
  }

  /** Strict supported subset: session ICE pair followed by audio/video media sections.
   * A media section is m-line, mid, candidates, optional end marker. O(bytes) space/time.
   * Credentials-only restart and candidate-only trickle are supported, SDP is not.
   */
  function parseSdpfrag(input) {
    bounded(input);
    if (!input || !input.endsWith(CRLF) || /[^\x20-\x7e\r\n]/.test(input)) fail();
    const lines = input.slice(0, -CRLF.length).split(CRLF);
    if (lines.some((line) => !line || /[\r\n]/.test(line))) fail();
    const output = { ufrag: null, pwd: null, media: [] };
    let current = null;
    for (const line of lines) {
      if (line.startsWith('a=ice-ufrag:')) {
        if (current || output.ufrag !== null || output.pwd !== null) fail();
        output.ufrag = line.slice('a=ice-ufrag:'.length);
        if (output.ufrag.length < MIN_UFRAG || output.ufrag.length > MAX_ICE_TEXT || !/^[a-zA-Z0-9+/]+$/.test(output.ufrag)) fail();
      } else if (line.startsWith('a=ice-pwd:')) {
        if (current || output.ufrag === null || output.pwd !== null) fail();
        output.pwd = line.slice('a=ice-pwd:'.length);
        if (output.pwd.length < MIN_PWD || output.pwd.length > MAX_ICE_TEXT || !/^[a-zA-Z0-9+/]+$/.test(output.pwd)) fail();
      } else if (line.startsWith('m=')) {
        if ((current && current.mid === null) || output.media.length >= MAX_MEDIA_SECTIONS) fail();
        mediaLine(line);
        current = { mLine: line, mid: null, candidates: [], endOfCandidates: false };
        output.media.push(current);
      } else if (line.startsWith('a=mid:')) {
        const mid = line.slice('a=mid:'.length);
        if (!current || current.mid !== null || !/^[a-zA-Z0-9_-]+$/.test(mid) || mid.length > MAX_MID ||
            output.media.some((media) => media.mid === mid)) fail();
        current.mid = mid;
      } else if (line.startsWith('a=candidate:')) {
        if (!current || current.mid === null || current.endOfCandidates || current.candidates.length >= MAX_CANDIDATES) fail();
        const value = line.slice(2); candidate(value, output.ufrag);
        if (current.candidates.includes(value)) fail();
        current.candidates.push(value);
      } else if (line === 'a=end-of-candidates') {
        if (!current || current.mid === null || current.endOfCandidates) fail();
        current.endOfCandidates = true;
      } else fail();
    }
    if ((output.ufrag === null) !== (output.pwd === null) || (current && current.mid === null) ||
        (output.ufrag === null && !output.media.some((media) => media.candidates.length || media.endOfCandidates))) fail();
    return output;
  }

  function buildSdpfrag(value) {
    fields(value, ['ufrag', 'pwd', 'media']);
    if (!Array.isArray(value.media) || value.media.length > MAX_MEDIA_SECTIONS) fail();
    let text = '';
    const append = (line) => {
      bounded(line); if (/[\r\n]/.test(line)) fail();
      text += line + CRLF; bounded(text);
    };
    if (value.ufrag !== null || value.pwd !== null) {
      bounded(value.ufrag); bounded(value.pwd);
      append(`a=ice-ufrag:${value.ufrag}`); append(`a=ice-pwd:${value.pwd}`);
    }
    for (const media of value.media) {
      fields(media, ['mLine', 'mid', 'candidates', 'endOfCandidates']);
      if (!Array.isArray(media.candidates) || media.candidates.length > MAX_CANDIDATES || typeof media.endOfCandidates !== 'boolean') fail();
      bounded(media.mLine); bounded(media.mid);
      append(media.mLine); append(`a=mid:${media.mid}`);
      for (const item of media.candidates) { bounded(item); append(`a=${item}`); }
      if (media.endOfCandidates) append('a=end-of-candidates');
    }
    parseSdpfrag(text);
    return text;
  }
  return Object.freeze({ MAX_SDPFRAG_BYTES, parseSdpfrag, buildSdpfrag });
})();

if (typeof module !== 'undefined' && module.exports) module.exports = turboSdpfrag;
