'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');

const statePath = process.argv[2];
const chunks = [];
const timer = setTimeout(() => process.stdin.destroy(), 250);

function readState() {
  return JSON.parse(fs.readFileSync(statePath, 'utf8'));
}
function writeState(value) {
  const temp = `${statePath}.tmp-${process.pid}`;
  fs.writeFileSync(temp, JSON.stringify(value), { encoding: 'utf8', mode: 0o600 });
  fs.renameSync(temp, statePath);
}
function digest(value) {
  return crypto.createHash('sha256').update(JSON.stringify(value)).digest('hex').slice(0, 16);
}

process.stdin.on('data', (chunk) => {
  clearTimeout(timer);
  chunks.push(Buffer.from(chunk));
});
process.stdin.on('end', () => {
  if (!statePath || chunks.length === 0) return;
  const request = JSON.parse(Buffer.concat(chunks).toString('utf8'));
  const state = readState();

  if (typeof request.audience === 'string') {
    const tokenId = `sfu-${digest({
      audience: request.audience,
      scope: request.scope,
      binding: request.binding,
      case_id: request.case_id,
    })}`;
    const token = `contract-sfu-secret-${tokenId}`;
    const envelope = {
      schema_version: 1,
      provider_id: 'contract-lab-sfu',
      token_id: tokenId,
      audience: request.audience,
      scope: request.scope,
      subject: request.subject,
      binding: request.binding,
      issued_at: new Date(request.now_ms).toISOString(),
      expires_at: new Date(Math.max(
        request.required_valid_until_ms + 60_000,
        request.now_ms + 60_000
      )).toISOString(),
      token,
    };
    state.tokens ||= {};
    state.tokens[token] = {
      audience: envelope.audience,
      scope: envelope.scope,
      binding: envelope.binding,
    };
    writeState(state);
    process.stdout.write(JSON.stringify(envelope));
    return;
  }

  state.turn_counter = (state.turn_counter || 0) + 1;
  const credentialId = `turn-${state.turn_counter}`;
  const fresh = typeof request.previous_credential_id === 'string';
  const expiresMs = request.short_ttl === true && !fresh
    ? request.required_valid_until_ms + 100
    : Math.max(request.required_valid_until_ms + 60_000, request.now_ms + 60_000);
  const envelope = {
    schema_version: 1,
    provider_id: 'contract-lab-turn',
    credential_id: credentialId,
    urls: ['turns:turn.contract.invalid:5349?transport=tcp'],
    username: `${Math.floor(expiresMs / 1000)}:contract-user`,
    credential: `contract-turn-secret-${credentialId}`,
    issued_at: new Date(request.now_ms).toISOString(),
    expires_at: new Date(expiresMs).toISOString(),
    coturn_version: '4.6.3-contract',
  };
  state.turn ||= {};
  state.turn[credentialId] = { expires_at_ms: expiresMs };
  writeState(state);
  process.stdout.write(JSON.stringify(envelope));
});
