'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const { createRunIdentity, createCaseIdentity } = require('../src/ids');

test('identities are stable for an injected clock UUID and case definition', () => {
  const runIdentity = createRunIdentity(
    () => Date.parse('2026-08-25T08:00:00.000Z'),
    () => '11111111-2222-4333-8444-555555555555',
    'a'.repeat(64)
  );
  const caseDefinition = {
    case_key: '{"browser":{"name":"chrome"}}',
    browser: { name: 'chrome', version: '127.0.0', platform: 'Windows 11' },
  };

  assert.deepEqual(runIdentity, {
    run_id: 'run-11111111-2222-4333-8444-555555555555',
    manifest_hash: 'a'.repeat(64),
    started_at: '2026-08-25T08:00:00.000Z',
  });
  assert.deepEqual(createCaseIdentity(runIdentity, caseDefinition, 0), {
    run_id: 'run-11111111-2222-4333-8444-555555555555',
    case_id: 'case-97a39653b5fbee84',
    case_key: '{"browser":{"name":"chrome"}}',
    ordinal: 0,
  });
});
