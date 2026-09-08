'use strict';

const chunks = [];
const noRequestTimer = setTimeout(() => {
  process.stdin.destroy();
}, 200);
process.stdin.on('data', (chunk) => {
  clearTimeout(noRequestTimer);
  chunks.push(Buffer.from(chunk));
});
process.stdin.on('end', () => {
  const request = JSON.parse(Buffer.concat(chunks).toString('utf8'));
  const receipt = {
    schema_version: 1,
    hook_id: 'restricted-nat-ipv4-transition',
    topology_id: request.topology_id,
    action: request.action,
    generation: request.generation,
    sequence: request.sequence,
    started_at: '2026-08-25T08:01:00.000Z',
    finished_at: '2026-08-25T08:01:01.000Z',
    observed_at: '2026-08-25T08:01:01.000Z',
    status: 'ok',
    effective: true,
    evidence_id: 'lab-receipt-123',
  };
  if (request.mode === 'sequence_mismatch') {
    receipt.sequence += 1;
  }
  process.stdout.write(JSON.stringify(receipt));
});
