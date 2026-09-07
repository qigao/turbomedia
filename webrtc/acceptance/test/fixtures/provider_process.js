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
  const turnCredential = {
    schema_version: 1,
    provider_id: 'turn-lab-v1',
    credential_id: 'turn-credential-1',
    urls: ['turns:turn.example.test:5349?transport=tcp'],
    username: 'expiry:alice',
    credential: 'fixture-turn-secret',
    issued_at: '2026-08-25T08:00:00.000Z',
    expires_at: '2026-08-25T08:02:00.000Z',
    coturn_version: '4.6.3',
  };
  const sfuToken = {
    schema_version: 1,
    provider_id: 'sfu-lab-v1',
    token_id: 'sfu-token-1',
    audience: 'turbo-sfu',
    scope: ['publish', 'subscribe'],
    subject: 'acceptance-runner',
    binding: { room_id: 'room-1', participant_id: 'participant-1' },
    issued_at: '2026-08-25T08:00:00.000Z',
    expires_at: '2026-08-25T08:02:00.000Z',
    token: 'fixture-sfu-secret',
  };

  switch (request.mode) {
    case 'success':
      process.stdout.write(JSON.stringify({ schema_version: 1, ok: true }));
      break;
    case 'json_value':
      process.stdout.write(JSON.stringify(request.value));
      break;
    case 'turn_success':
      process.stdout.write(JSON.stringify(turnCredential));
      break;
    case 'expired_credential':
      turnCredential.expires_at = '2026-08-25T08:01:00.000Z';
      process.stdout.write(JSON.stringify(turnCredential));
      break;
    case 'sfu_success':
      process.stdout.write(JSON.stringify(sfuToken));
      break;
    case 'timeout':
      setTimeout(() => process.stdout.write('{}'), 10_000);
      break;
    case 'nonzero':
      process.exitCode = 7;
      break;
    case 'malformed':
      process.stdout.write('{"broken":');
      break;
    case 'multiple':
      process.stdout.write('{}\n{}');
      break;
    case 'empty':
      break;
    case 'non_object':
      process.stdout.write('[]');
      break;
    case 'invalid_utf8':
      process.stdout.write(Buffer.from([0xc3, 0x28]));
      break;
    case 'invalid_stderr_utf8':
      process.stderr.write(Buffer.from([0xc3, 0x28]));
      process.stdout.write('{}');
      break;
    case 'stdout_unicode_overflow':
      process.stdout.write('🔑'.repeat(request.character_count));
      break;
    case 'stdout_overflow':
      process.stdout.write(Buffer.alloc(request.byte_count, 0x61));
      break;
    case 'stderr_overflow':
      process.stderr.write(Buffer.alloc(request.byte_count, 0x62));
      process.stdout.write('{}');
      break;
    default:
      process.exitCode = 9;
      break;
  }
});
