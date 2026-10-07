'use strict';

const path = require('node:path');
const { createContractValidator, validateContract } = require('./contracts');

const validator = createContractValidator(path.join(__dirname, '..', 'schemas'));
const SAFE_LABEL = /^[A-Za-z0-9_.:-]+$/;
const SHA256 = /^[0-9a-f]{64}$/;
const ISO_TIMESTAMP = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?(Z|([+-])(\d{2}):(\d{2}))$/;
const SAFE_PROCESS_ERROR = Symbol.for('turbonet.webrtcAcceptance.safeError');
const PROCESS_ERROR_CODES = new Set([
  'INVALID_PROCESS_OPTIONS',
  'PROCESS_ABORTED',
  'PROCESS_EXIT_ERROR',
  'PROCESS_SPAWN_ERROR',
  'PROCESS_STDIN_ERROR',
  'PROCESS_STREAM_ERROR',
  'PROCESS_TIMEOUT',
  'STDERR_LIMIT_EXCEEDED',
  'STDOUT_LIMIT_EXCEEDED',
  'INVALID_PROCESS_OUTPUT',
]);
const PROCESS_ERROR_STAGES = new Set([
  'abort',
  'exit',
  'json',
  'output_stream',
  'spawn',
  'stderr_limit',
  'stdin',
  'stdout_limit',
  'timeout',
  'utf8',
  'validate_options',
]);
const PROCESS_SIGNALS = new Set([
  'SIGABRT',
  'SIGALRM',
  'SIGBREAK',
  'SIGHUP',
  'SIGINT',
  'SIGKILL',
  'SIGPIPE',
  'SIGQUIT',
  'SIGSEGV',
  'SIGTERM',
  'SIGUSR1',
  'SIGUSR2',
]);

async function issueTurnCredential(adapter, request) {
  return issueCredential('turn-credential', 'issue_turn_credential', adapter, request);
}

async function issueSfuToken(adapter, request) {
  const { copiedRequest, output } = await invokeProvider(
    'sfu-token',
    'issue_sfu_token',
    adapter,
    request
  );
  validateCredentialTime(output, copiedRequest, 'issue_sfu_token');
  validateOptionalCorrelation(output, copiedRequest, 'issue_sfu_token');
  return output;
}

async function invokeTopologyHook(adapter, request) {
  const operation = 'invoke_topology_hook';
  if (typeof adapter !== 'function') {
    throw safeError('INVALID_HOOK_REQUEST', operation, undefined, 'request');
  }

  let copiedRequest;
  try {
    copiedRequest = freezeJsonCopy(request);
  } catch {
    throw safeError('INVALID_HOOK_REQUEST', operation, undefined, 'request');
  }
  const caseId = safeCaseId(copiedRequest.case_id);
  if (!isSafeLabel(copiedRequest.run_id) || !caseId ||
      !isSafeLabel(copiedRequest.topology_id) ||
      !['setup', 'transition', 'teardown', 'probe'].includes(copiedRequest.action) ||
      !Number.isInteger(copiedRequest.generation) || copiedRequest.generation < 0 ||
      !Number.isInteger(copiedRequest.sequence) || copiedRequest.sequence < 0 ||
      !SHA256.test(copiedRequest.relay_contract_hash)) {
    throw safeError('INVALID_HOOK_REQUEST', operation, caseId, 'request');
  }

  let rawOutput;
  try {
    rawOutput = await adapter(copiedRequest);
  } catch (error) {
    throw sanitizeAdapterError(error, operation, caseId, 'HOOK_ADAPTER_FAILED');
  }
  let output;
  try {
    output = freezeJsonCopy(rawOutput);
  } catch {
    throw safeError('INVALID_HOOK_OUTPUT', operation, caseId, 'copy_output');
  }
  validateSchema('hook-receipt', output, operation, caseId, 'HOOK_SCHEMA_INVALID');
  if (output.topology_id !== copiedRequest.topology_id ||
      output.action !== copiedRequest.action ||
      output.generation !== copiedRequest.generation ||
      output.sequence !== copiedRequest.sequence ||
      output.relay_contract_hash !== copiedRequest.relay_contract_hash) {
    throw safeError('HOOK_CORRELATION_MISMATCH', operation, caseId, 'correlation');
  }
  if (output.effective !== true) {
    throw safeError('HOOK_NOT_EFFECTIVE', operation, caseId, 'effective');
  }
  return output;
}

async function issueCredential(schemaName, operation, adapter, request) {
  const { copiedRequest, output } = await invokeProvider(schemaName, operation, adapter, request);
  validateCredentialTime(output, copiedRequest, operation);
  return output;
}

async function invokeProvider(schemaName, operation, adapter, request) {
  if (typeof adapter !== 'function') {
    throw safeError('INVALID_PROVIDER_REQUEST', operation, undefined, 'request');
  }
  let copiedRequest;
  try {
    copiedRequest = freezeJsonCopy(request);
  } catch {
    throw safeError('INVALID_PROVIDER_REQUEST', operation, undefined, 'request');
  }
  const caseId = safeCaseId(copiedRequest.case_id);
  if (!Number.isFinite(copiedRequest.now_ms) ||
      (copiedRequest.required_valid_until_ms !== undefined &&
       (!Number.isFinite(copiedRequest.required_valid_until_ms) ||
        copiedRequest.required_valid_until_ms < copiedRequest.now_ms))) {
    throw safeError('INVALID_PROVIDER_REQUEST', operation, caseId, 'request_time');
  }

  let rawOutput;
  try {
    rawOutput = await adapter(copiedRequest);
  } catch (error) {
    throw sanitizeAdapterError(error, operation, caseId, 'PROVIDER_ADAPTER_FAILED');
  }
  let output;
  try {
    output = freezeJsonCopy(rawOutput);
  } catch {
    throw safeError('INVALID_PROVIDER_OUTPUT', operation, caseId, 'copy_output');
  }
  validateSchema(schemaName, output, operation, caseId, 'PROVIDER_SCHEMA_INVALID');
  return { copiedRequest, output };
}

function validateSchema(schemaName, output, operation, caseId, errorCode) {
  const validation = validateContract(validator, schemaName, output);
  if (validation.valid) return;
  const schemaPaths = [...new Set(validation.errors.map((entry) => entry.schemaPath))].sort();
  throw safeError(errorCode, operation, caseId, 'schema', {
    schema_paths: Object.freeze(schemaPaths),
  });
}

function validateCredentialTime(output, request, operation) {
  const caseId = safeCaseId(request.case_id);
  const issuedAt = parseIsoTimestampMs(output.issued_at);
  const expiresAt = parseIsoTimestampMs(output.expires_at);
  if (!Number.isFinite(issuedAt) || !Number.isFinite(expiresAt) || issuedAt >= expiresAt) {
    throw safeError('INVALID_CREDENTIAL_TIME', operation, caseId, 'credential_time');
  }
  if (expiresAt <= request.now_ms) {
    throw safeError('CREDENTIAL_EXPIRED', operation, caseId, 'expiry');
  }
  if (request.required_valid_until_ms !== undefined && expiresAt < request.required_valid_until_ms) {
    throw safeError('CREDENTIAL_DEADLINE_NOT_MET', operation, caseId, 'expiry_deadline');
  }
}

function parseIsoTimestampMs(value) {
  const match = ISO_TIMESTAMP.exec(value);
  if (!match) return Number.NaN;

  const year = Number(match[1]);
  const month = Number(match[2]);
  const day = Number(match[3]);
  const hour = Number(match[4]);
  const minute = Number(match[5]);
  const second = Number(match[6]);
  const millisecond = Number((match[7] || '').padEnd(3, '0').slice(0, 3));
  const local = new Date(0);
  local.setUTCFullYear(year, month - 1, day);
  local.setUTCHours(hour, minute, second, millisecond);
  if (local.getUTCFullYear() !== year || local.getUTCMonth() !== month - 1 ||
      local.getUTCDate() !== day || local.getUTCHours() !== hour ||
      local.getUTCMinutes() !== minute || local.getUTCSeconds() !== second ||
      local.getUTCMilliseconds() !== millisecond) {
    return Number.NaN;
  }

  let offsetMs = 0;
  if (match[8] !== 'Z') {
    const offsetHours = Number(match[10]);
    const offsetMinutes = Number(match[11]);
    if (offsetHours > 23 || offsetMinutes > 59) return Number.NaN;
    const direction = match[9] === '+' ? 1 : -1;
    offsetMs = direction * (offsetHours * 60 + offsetMinutes) * 60_000;
  }
  const instant = local.getTime() - offsetMs;
  return Number.isFinite(instant) ? instant : Number.NaN;
}

function validateOptionalCorrelation(output, request, operation) {
  const caseId = safeCaseId(request.case_id);
  for (const field of ['audience', 'subject']) {
    if (Object.hasOwn(request, field) && output[field] !== request[field]) {
      throw safeError('PROVIDER_CORRELATION_MISMATCH', operation, caseId, 'correlation');
    }
  }
  if (Object.hasOwn(request, 'scope') && !sameJsonValue(output.scope, request.scope)) {
    throw safeError('PROVIDER_CORRELATION_MISMATCH', operation, caseId, 'correlation');
  }
  if (Object.hasOwn(request, 'binding') && !sameJsonValue(output.binding, request.binding)) {
    throw safeError('PROVIDER_CORRELATION_MISMATCH', operation, caseId, 'correlation');
  }
}

function sameJsonValue(left, right) {
  if (left === right) return true;
  if (!left || !right || typeof left !== 'object' || typeof right !== 'object' ||
      Array.isArray(left) !== Array.isArray(right)) return false;
  if (Array.isArray(left)) {
    return left.length === right.length && left.every((value, index) => sameJsonValue(value, right[index]));
  }
  const leftKeys = Object.keys(left).sort();
  const rightKeys = Object.keys(right).sort();
  return leftKeys.length === rightKeys.length &&
    leftKeys.every((key, index) => key === rightKeys[index] && sameJsonValue(left[key], right[key]));
}

function freezeJsonCopy(value) {
  return deepFreeze(copyJsonValue(value));
}

function copyJsonValue(value, ancestors = new Set()) {
  if (value === null || typeof value === 'boolean' || typeof value === 'string') return value;
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (!value || typeof value !== 'object' || ancestors.has(value)) {
    throw new TypeError('value must be finite acyclic JSON data');
  }
  ancestors.add(value);
  try {
    if (Array.isArray(value)) {
      if (Reflect.ownKeys(value).some((key) => key !== 'length' &&
          (typeof key !== 'string' || !/^(0|[1-9][0-9]*)$/.test(key)))) {
        throw new TypeError('arrays cannot have extra properties');
      }
      const copy = [];
      for (let index = 0; index < value.length; index += 1) {
        const descriptor = Object.getOwnPropertyDescriptor(value, index);
        if (!descriptor || !Object.hasOwn(descriptor, 'value')) {
          throw new TypeError('arrays cannot contain holes or accessors');
        }
        copy.push(copyJsonValue(descriptor.value, ancestors));
      }
      return copy;
    }
    if (!isPlainObject(value) || Object.getOwnPropertySymbols(value).length > 0) {
      throw new TypeError('objects must be plain JSON objects');
    }
    const copy = {};
    for (const key of Object.getOwnPropertyNames(value)) {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (!descriptor || !descriptor.enumerable || !Object.hasOwn(descriptor, 'value')) {
        throw new TypeError('object properties must be enumerable data properties');
      }
      Object.defineProperty(copy, key, {
        value: copyJsonValue(descriptor.value, ancestors),
        enumerable: true,
        writable: true,
        configurable: true,
      });
    }
    return copy;
  } finally {
    ancestors.delete(value);
  }
}

function deepFreeze(value) {
  if (!value || typeof value !== 'object' || Object.isFrozen(value)) return value;
  for (const key of Object.keys(value)) deepFreeze(value[key]);
  return Object.freeze(value);
}

function sanitizeAdapterError(error, operation, caseId, fallbackCode) {
  if (!isTrustedProcessError(error)) {
    return safeError(fallbackCode, operation, caseId, 'adapter');
  }
  const code = safeOwnString(error, 'code');
  const stage = safeOwnString(error, 'stage');
  const exitCode = safeOwnInteger(error, 'exit_code');
  const signal = safeOwnString(error, 'signal');
  if (!PROCESS_ERROR_CODES.has(code) || !PROCESS_ERROR_STAGES.has(stage)) {
    return safeError(fallbackCode, operation, caseId, 'adapter');
  }
  return safeError(
    code,
    operation,
    caseId,
    stage,
    {
      exit_code: Number.isSafeInteger(exitCode) ? exitCode : undefined,
      signal: PROCESS_SIGNALS.has(signal) ? signal : undefined,
    }
  );
}

function isTrustedProcessError(error) {
  if (!error || (typeof error !== 'object' && typeof error !== 'function')) return false;
  const descriptor = Object.getOwnPropertyDescriptor(error, SAFE_PROCESS_ERROR);
  return Boolean(descriptor && Object.hasOwn(descriptor, 'value') && descriptor.value === true);
}

function safeOwnString(value, key) {
  if (!value || (typeof value !== 'object' && typeof value !== 'function')) return undefined;
  const descriptor = Object.getOwnPropertyDescriptor(value, key);
  return descriptor && Object.hasOwn(descriptor, 'value') && typeof descriptor.value === 'string'
    ? descriptor.value
    : undefined;
}

function safeOwnInteger(value, key) {
  if (!value || (typeof value !== 'object' && typeof value !== 'function')) return undefined;
  const descriptor = Object.getOwnPropertyDescriptor(value, key);
  return descriptor && Object.hasOwn(descriptor, 'value') && Number.isInteger(descriptor.value)
    ? descriptor.value
    : undefined;
}

function safeError(code, operation, caseId, stage, details = {}) {
  const fields = {
    code,
    operation,
    case_id: caseId,
    stage,
    exit_code: details.exit_code,
    signal: details.signal,
  };
  const message = Object.entries(fields)
    .filter(([, value]) => value !== undefined)
    .map(([key, value]) => `${key}=${value}`)
    .join(' ');
  const error = new Error(message);
  for (const [key, value] of Object.entries(fields)) {
    if (value !== undefined) error[key] = value;
  }
  if (details.schema_paths) error.schema_paths = details.schema_paths;
  return error;
}

function safeCaseId(value) {
  return isSafeLabel(value) ? value : undefined;
}

function isSafeLabel(value) {
  return typeof value === 'string' && value.length > 0 && value.length <= 128 && SAFE_LABEL.test(value);
}

function isPlainObject(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

module.exports = { issueTurnCredential, issueSfuToken, invokeTopologyHook };
