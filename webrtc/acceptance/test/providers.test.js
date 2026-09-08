'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');

const { LIMITS } = require('../src/constants');
const { runBoundedCommand } = require('../src/process_adapter');
const {
  issueTurnCredential,
  issueSfuToken,
  invokeTopologyHook,
} = require('../src/providers');

const providerFixture = path.join(__dirname, 'fixtures', 'provider_process.js');
const hookFixture = path.join(__dirname, 'fixtures', 'hook_process.js');
const NOW_MS = Date.parse('2026-08-25T08:01:00.000Z');

function commandOptions(request, overrides = {}) {
  return {
    executable: process.execPath,
    args: [providerFixture],
    request,
    operation: 'provider_fixture',
    caseId: 'case-safe',
    timeoutMs: 2_000,
    killGraceMs: 50,
    maxStdoutBytes: 4_096,
    maxStderrBytes: 4_096,
    ...overrides,
  };
}

function processAdapter(fixturePath, operation) {
  return (request) => runBoundedCommand({
    ...commandOptions(request),
    args: [fixturePath],
    operation,
    caseId: request.case_id,
  });
}

function turnRequest(extra = {}) {
  return {
    case_id: 'case-turn',
    mode: 'turn_success',
    now_ms: NOW_MS,
    required_valid_until_ms: NOW_MS + 30_000,
    ...extra,
  };
}

function sfuRequest(extra = {}) {
  return {
    case_id: 'case-sfu',
    mode: 'sfu_success',
    now_ms: NOW_MS,
    required_valid_until_ms: NOW_MS + 30_000,
    audience: 'turbo-sfu',
    scope: ['publish', 'subscribe'],
    subject: 'acceptance-runner',
    binding: { room_id: 'room-1', participant_id: 'participant-1' },
    ...extra,
  };
}

function hookRequest(extra = {}) {
  return {
    run_id: 'run-1',
    case_id: 'case-hook',
    generation: 2,
    sequence: 4,
    topology_id: 'restricted-nat-ipv4',
    action: 'transition',
    mode: 'success',
    ...extra,
  };
}

async function assertSafeRejection(promise, code, forbiddenValues = []) {
  await assert.rejects(promise, (error) => {
    assert.equal(error.code, code);
    const exposed = JSON.stringify({
      message: error.message,
      code: error.code,
      operation: error.operation,
      case_id: error.case_id,
      stage: error.stage,
      exit_code: error.exit_code,
      signal: error.signal,
      schema_paths: error.schema_paths,
    });
    for (const forbidden of forbiddenValues) {
      assert.ok(!exposed.includes(forbidden), `error leaked ${forbidden}`);
    }
    return true;
  });
}

function waitForCleanFixtureDiscoveryExit(fixturePath) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [fixturePath], {
      windowsHide: true,
      stdio: ['pipe', 'ignore', 'ignore'],
    });
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      reject(new Error('fixture did not exit when discovered without a request'));
    }, 500);
    child.once('error', (error) => {
      clearTimeout(timer);
      reject(error);
    });
    child.once('close', (code) => {
      clearTimeout(timer);
      if (code === 0) resolve();
      else reject(new Error(`fixture discovery exit code was ${code}`));
    });
  });
}

test('process fixtures exit cleanly when the Node test runner discovers them without stdin', async () => {
  await waitForCleanFixtureDiscoveryExit(providerFixture);
  await waitForCleanFixtureDiscoveryExit(hookFixture);
});

test('bounded command uses split executable args and returns one parsed object', async () => {
  const request = { mode: 'success', nested: { value: 1 } };
  const result = await runBoundedCommand(commandOptions(request, {
    args: [providerFixture, '&', 'definitely-not-a-command'],
  }));

  assert.deepEqual(result, { schema_version: 1, ok: true });
  assert.deepEqual(request, { mode: 'success', nested: { value: 1 } });
});

test('bounded command returns the single parsed JSON value without imposing a schema', async () => {
  for (const value of [null, false, 0, 'response', ['response']]) {
    const result = await runBoundedCommand(commandOptions({ mode: 'json_value', value }));
    assert.deepEqual(result, value);
  }
});

test('bounded command rejects unsafe process options before spawning', async () => {
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'success' }, { args: 'provider --json' })),
    'INVALID_PROCESS_OPTIONS'
  );
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'success' }, {
      maxStdoutBytes: LIMITS.MAX_PROVIDER_OUTPUT_BYTES + 1,
    })),
    'INVALID_PROCESS_OPTIONS'
  );
});

test('bounded command sanitizes spawn failures without exposing executable or argv', async () => {
  const missingExecutable = path.join(__dirname, 'missing-secret-executable.exe');
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'success' }, {
      executable: missingExecutable,
      args: ['secret-argument-value'],
    })),
    'PROCESS_SPAWN_ERROR',
    [missingExecutable, 'secret-argument-value']
  );
});

test('bounded command rejects timeout abort and nonzero exit with safe metadata', async () => {
  const secret = 'must-not-leak-timeout-request';
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'timeout', secret }, { timeoutMs: 100 })),
    'PROCESS_TIMEOUT',
    [secret, providerFixture, process.execPath]
  );

  const controller = new AbortController();
  const aborted = runBoundedCommand(commandOptions({ mode: 'timeout', secret }, {
    timeoutMs: 2_000,
    signal: controller.signal,
  }));
  setTimeout(() => controller.abort(), 50);
  await assertSafeRejection(aborted, 'PROCESS_ABORTED', [secret, providerFixture, process.execPath]);

  const racingController = new AbortController();
  const racing = runBoundedCommand(commandOptions({ mode: 'timeout', secret }, {
    timeoutMs: 75,
    killGraceMs: 25,
    signal: racingController.signal,
  }));
  setTimeout(() => racingController.abort(), 75);
  await assert.rejects(racing, (error) => {
    assert.ok(['PROCESS_ABORTED', 'PROCESS_TIMEOUT'].includes(error.code));
    assert.ok(!error.message.includes(secret));
    return true;
  });

  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'nonzero', secret })),
    'PROCESS_EXIT_ERROR',
    [secret, providerFixture, process.execPath]
  );
});

test('bounded command rejects malformed multiple and empty JSON without payload text', async () => {
  for (const mode of ['malformed', 'multiple', 'empty']) {
    await assertSafeRejection(
      runBoundedCommand(commandOptions({ mode })),
      'INVALID_PROCESS_OUTPUT',
      ['broken']
    );
  }
});

test('bounded command rejects fatal UTF-8 and byte-counted stdout or stderr overflow', async () => {
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'invalid_utf8' })),
    'INVALID_PROCESS_OUTPUT'
  );
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'invalid_stderr_utf8' })),
    'INVALID_PROCESS_OUTPUT'
  );
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'stdout_unicode_overflow', character_count: 2 }, {
      maxStdoutBytes: 7,
    })),
    'STDOUT_LIMIT_EXCEEDED'
  );
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'stdout_overflow', byte_count: 9 }, {
      maxStdoutBytes: 8,
    })),
    'STDOUT_LIMIT_EXCEEDED'
  );
  await assertSafeRejection(
    runBoundedCommand(commandOptions({ mode: 'stderr_overflow', byte_count: 9 }, {
      maxStderrBytes: 8,
    })),
    'STDERR_LIMIT_EXCEEDED'
  );
});

test('bounded command rejects invalid stdout or stderr UTF-8 before a later timeout', async () => {
  for (const mode of ['invalid_utf8_then_timeout', 'invalid_stderr_utf8_then_timeout']) {
    await assertSafeRejection(
      runBoundedCommand(commandOptions({ mode }, { timeoutMs: 300 })),
      'INVALID_PROCESS_OUTPUT'
    );
  }
});

test('TURN provider validates through the real process boundary and returns a frozen copy', async () => {
  const request = turnRequest();
  const result = await issueTurnCredential(processAdapter(providerFixture, 'issue_turn_credential'), request);

  assert.equal(result.credential_id, 'turn-credential-1');
  assert.ok(Object.isFrozen(result));
  assert.ok(Object.isFrozen(result.urls));
  assert.deepEqual(request, turnRequest());
});

test('credential providers accept finite millisecond instants without integer coercion', async () => {
  const request = turnRequest({
    now_ms: NOW_MS + 0.5,
    required_valid_until_ms: NOW_MS + 30_000.5,
  });

  const result = await issueTurnCredential(
    processAdapter(providerFixture, 'issue_turn_credential'),
    request
  );

  assert.equal(result.credential_id, 'turn-credential-1');
});

test('providers copy JSON-safe requests without invoking accessors', async () => {
  let getterReads = 0;
  const request = turnRequest();
  Object.defineProperty(request, 'secret', {
    enumerable: true,
    get() {
      getterReads += 1;
      return 'accessor-secret';
    },
  });

  await assertSafeRejection(
    issueTurnCredential(async () => { throw new Error('adapter must not run'); }, request),
    'INVALID_PROVIDER_REQUEST',
    ['accessor-secret', 'adapter must not run']
  );
  assert.equal(getterReads, 0);
});

test('providers pass an isolated immutable request snapshot to adapters', async () => {
  const request = turnRequest();
  let adapterRequest;
  const output = {
    schema_version: 1,
    provider_id: 'turn-lab-v1',
    credential_id: 'turn-credential-1',
    urls: ['turn:example.test'],
    username: 'user',
    credential: 'credential',
    issued_at: '2026-08-25T08:00:00.000Z',
    expires_at: '2026-08-25T08:02:00.000Z',
    coturn_version: '4.6.3',
  };

  await issueTurnCredential(async (received) => {
    adapterRequest = received;
    assert.throws(() => { received.now_ms = 0; }, TypeError);
    return output;
  }, request);

  assert.notStrictEqual(adapterRequest, request);
  assert.equal(request.now_ms, NOW_MS);
});

test('providers reject output accessors without executing them', async () => {
  let getterReads = 0;
  const output = {};
  Object.defineProperty(output, 'credential', {
    enumerable: true,
    get() {
      getterReads += 1;
      return 'accessor-output-secret';
    },
  });

  await assertSafeRejection(
    issueTurnCredential(async () => output, turnRequest()),
    'INVALID_PROVIDER_OUTPUT',
    ['accessor-output-secret']
  );
  assert.equal(getterReads, 0);
});

test('credential providers replace arbitrary adapter error metadata with fixed safe fields', async () => {
  const maliciousCode = `SECRET_${'TOKEN'.repeat(64)}`;
  const maliciousStage = 'secret-token-stage';
  const maliciousSignal = 'secret-token-signal';
  const adapterError = {
    code: maliciousCode,
    stage: maliciousStage,
    signal: maliciousSignal,
    exit_code: 77,
  };

  await assert.rejects(issueTurnCredential(async () => { throw adapterError; }, turnRequest()), (error) => {
    assert.equal(error.code, 'PROVIDER_ADAPTER_FAILED');
    assert.equal(error.stage, 'adapter');
    assert.equal(error.exit_code, undefined);
    assert.equal(error.signal, undefined);
    const exposed = JSON.stringify({ message: error.message, ...error });
    for (const secret of [maliciousCode, maliciousStage, maliciousSignal]) {
      assert.ok(!exposed.includes(secret), `error leaked ${secret}`);
    }
    return true;
  });
});

test('topology hooks replace arbitrary adapter error metadata with fixed safe fields', async () => {
  const maliciousCode = 'SECRET_TOKEN_ABC';
  const maliciousStage = 'secret-token-abc';
  const maliciousSignal = 'secret-token-abc';

  await assert.rejects(invokeTopologyHook(async () => {
    throw { code: maliciousCode, stage: maliciousStage, signal: maliciousSignal, exit_code: 91 };
  }, hookRequest()), (error) => {
    assert.equal(error.code, 'HOOK_ADAPTER_FAILED');
    assert.equal(error.stage, 'adapter');
    assert.equal(error.exit_code, undefined);
    assert.equal(error.signal, undefined);
    const exposed = JSON.stringify({ message: error.message, ...error });
    for (const secret of [maliciousCode, maliciousStage, maliciousSignal]) {
      assert.ok(!exposed.includes(secret), `error leaked ${secret}`);
    }
    return true;
  });
});

test('providers preserve allowlisted metadata from trusted bounded process errors', async () => {
  await assert.rejects(
    issueTurnCredential(
      processAdapter(providerFixture, 'issue_turn_credential'),
      turnRequest({ mode: 'nonzero' })
    ),
    (error) => {
      assert.equal(error.code, 'PROCESS_EXIT_ERROR');
      assert.equal(error.stage, 'exit');
      assert.equal(error.exit_code, 7);
      assert.equal(error.signal, undefined);
      return true;
    }
  );
});

test('TURN provider rejects expired invalid-order and required-deadline credentials', async () => {
  const base = {
    schema_version: 1,
    provider_id: 'turn-lab-v1',
    credential_id: 'turn-credential-1',
    urls: ['turn:example.test'],
    username: 'user-secret',
    credential: 'credential-secret',
    issued_at: '2026-08-25T08:00:00.000Z',
    expires_at: '2026-08-25T08:02:00.000Z',
    coturn_version: '4.6.3',
  };
  const cases = [
    [{ ...base, expires_at: '2026-08-25T08:01:00.000Z' }, 'CREDENTIAL_EXPIRED'],
    [{ ...base, issued_at: '2026-08-25T08:03:00.000Z' }, 'INVALID_CREDENTIAL_TIME'],
    [{ ...base, expires_at: '2026-08-25T08:01:20.000Z' }, 'CREDENTIAL_DEADLINE_NOT_MET'],
  ];
  for (const [output, code] of cases) {
    await assertSafeRejection(
      issueTurnCredential(async () => output, turnRequest()),
      code,
      ['user-secret', 'credential-secret']
    );
  }
});

test('credential providers reject non-timestamp and normalized calendar dates', async () => {
  const base = {
    schema_version: 1,
    provider_id: 'turn-lab-v1',
    credential_id: 'turn-credential-1',
    urls: ['turn:example.test'],
    username: 'user-secret',
    credential: 'credential-secret',
    issued_at: '2026-08-25T08:00:00.000Z',
    expires_at: '2026-08-25T08:02:00.000Z',
    coturn_version: '4.6.3',
  };
  for (const output of [
    { ...base, issued_at: '2026', expires_at: '2027' },
    { ...base, issued_at: '2026-02-30T08:00:00Z', expires_at: '2027-08-25T08:02:00Z' },
  ]) {
    await assertSafeRejection(
      issueTurnCredential(async () => output, turnRequest()),
      'INVALID_CREDENTIAL_TIME',
      ['user-secret', 'credential-secret']
    );
  }
});

test('TURN provider rejects an expired credential from the real process fixture', async () => {
  const request = turnRequest({ mode: 'expired_credential' });

  await assertSafeRejection(
    issueTurnCredential(processAdapter(providerFixture, 'issue_turn_credential'), request),
    'CREDENTIAL_EXPIRED',
    ['fixture-turn-secret']
  );
});

test('provider schema errors expose schema paths but never rejected values', async () => {
  const invalid = {
    schema_version: 1,
    credential: 'schema-secret-value',
    unexpected: 'payload-marker',
  };
  await assert.rejects(issueTurnCredential(async () => invalid, turnRequest()), (error) => {
    assert.equal(error.code, 'PROVIDER_SCHEMA_INVALID');
    assert.ok(error.schema_paths.length > 0);
    assert.ok(error.schema_paths.every((entry) => typeof entry === 'string'));
    assert.ok(!error.message.includes('schema-secret-value'));
    assert.ok(!error.message.includes('payload-marker'));
    return true;
  });
});

test('SFU provider validates schema expiry and request correlation', async () => {
  const result = await issueSfuToken(processAdapter(providerFixture, 'issue_sfu_token'), sfuRequest());
  assert.equal(result.token_id, 'sfu-token-1');
  assert.ok(Object.isFrozen(result.binding));
  assert.ok(Object.isFrozen(result.scope));

  const validOutput = { ...result, binding: { ...result.binding }, scope: [...result.scope] };
  for (const mismatch of [
    { ...validOutput, audience: 'wrong-audience' },
    { ...validOutput, subject: 'wrong-subject' },
    { ...validOutput, scope: ['publish'] },
    { ...validOutput, binding: { room_id: 'wrong-room', participant_id: 'participant-1' } },
  ]) {
    await assertSafeRejection(
      issueSfuToken(async () => mismatch, sfuRequest()),
      'PROVIDER_CORRELATION_MISMATCH',
      ['wrong-audience', 'wrong-subject', 'wrong-room', 'fixture-sfu-secret']
    );
  }
});

test('topology hook validates real receipt and exact sequence topology action correlation', async () => {
  const adapter = processAdapter(hookFixture, 'invoke_topology_hook');
  const result = await invokeTopologyHook(adapter, hookRequest());
  assert.equal(result.evidence_id, 'lab-receipt-123');
  assert.ok(Object.isFrozen(result));

  await assertSafeRejection(
    invokeTopologyHook(adapter, hookRequest({ mode: 'sequence_mismatch' })),
    'HOOK_CORRELATION_MISMATCH',
    ['lab-receipt-123']
  );

  const base = { ...result };
  for (const mismatch of [
    { ...base, topology_id: 'wrong-topology' },
    { ...base, action: 'teardown' },
    { ...base, generation: 3 },
  ]) {
    await assertSafeRejection(
      invokeTopologyHook(async () => mismatch, hookRequest()),
      'HOOK_CORRELATION_MISMATCH',
      ['wrong-topology', 'lab-receipt-123']
    );
  }
});

test('topology hook requires full request correlation and effective receipts', async () => {
  const valid = {
    schema_version: 1,
    hook_id: 'hook-1',
    topology_id: 'restricted-nat-ipv4',
    action: 'transition',
    generation: 2,
    sequence: 4,
    started_at: '2026-08-25T08:01:00.000Z',
    finished_at: '2026-08-25T08:01:01.000Z',
    observed_at: '2026-08-25T08:01:01.000Z',
    status: 'ok',
    effective: true,
    evidence_id: 'receipt-secret-marker',
  };
  const missingRunId = hookRequest();
  delete missingRunId.run_id;
  await assertSafeRejection(
    invokeTopologyHook(async () => valid, missingRunId),
    'INVALID_HOOK_REQUEST',
    ['receipt-secret-marker']
  );
  await assertSafeRejection(
    invokeTopologyHook(async () => ({ ...valid, effective: false }), hookRequest()),
    'HOOK_NOT_EFFECTIVE',
    ['receipt-secret-marker']
  );
});

test('topology hook schema errors expose only hook schema paths', async () => {
  const invalid = {
    schema_version: 1,
    evidence_id: 'hook-payload-marker',
    unexpected: 'hook-secret-value',
  };
  await assert.rejects(invokeTopologyHook(async () => invalid, hookRequest()), (error) => {
    assert.equal(error.code, 'HOOK_SCHEMA_INVALID');
    assert.ok(error.schema_paths.length > 0);
    assert.ok(!error.message.includes('hook-payload-marker'));
    assert.ok(!error.message.includes('hook-secret-value'));
    return true;
  });
});
