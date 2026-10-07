'use strict';

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

process.stdin.on('data', (chunk) => {
  clearTimeout(timer);
  chunks.push(Buffer.from(chunk));
});
process.stdin.on('end', () => {
  if (!statePath || chunks.length === 0) return;
  const request = JSON.parse(Buffer.concat(chunks).toString('utf8'));
  const state = readState();
  const inject = state.inject || {};
  let effective = true;

  if (request.action === 'probe' && request.probe_kind === 'credential_expiry') {
    const turn = state.turn && state.turn[request.credential_id];
    const expired = turn && Date.now() >= turn.expires_at_ms;
    effective = Boolean(expired) && inject.expired_credential_accepted !== true;
  } else if (request.action === 'probe' && request.probe_kind === 'turn_reachability') {
    effective = inject.turn_unreachable !== true;
  } else if (request.action === 'probe' && request.probe_kind === 'turn_baseline') {
    effective = inject.cleanup_residue !== true;
  } else if (request.action === 'transition') {
    effective = inject.transition_ineffective !== true;
  }

  state.hooks ||= [];
  state.hooks.push({
    action: request.action,
    probe_kind: request.probe_kind || null,
    generation: request.generation,
    sequence: request.sequence,
    effective,
  });
  writeState(state);

  const now = new Date().toISOString();
  process.stdout.write(JSON.stringify({
    schema_version: 2,
    hook_id: `contract-${request.action}`,
    topology_id: request.topology_id,
    action: request.action,
    generation: request.generation,
    sequence: request.sequence,
    relay_contract_hash: request.relay_contract_hash,
    started_at: now,
    finished_at: now,
    observed_at: now,
    status: effective ? 'ok' : 'failed',
    effective,
    evidence_id: `contract-${request.action}-${request.sequence}`,
  }));
});
