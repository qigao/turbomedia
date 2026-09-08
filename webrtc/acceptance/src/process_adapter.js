'use strict';

const { spawn } = require('node:child_process');
const { TextDecoder } = require('node:util');
const { LIMITS } = require('./constants');

const SAFE_ERROR = Symbol.for('turbonet.webrtcAcceptance.safeError');
const MAX_PROCESS_OUTPUT_BYTES = Math.max(
  LIMITS.MAX_PROVIDER_OUTPUT_BYTES,
  LIMITS.MAX_HOOK_OUTPUT_BYTES
);

async function runBoundedCommand(options) {
  let normalized;
  try {
    normalized = normalizeOptions(options);
  } catch {
    throw safeError('INVALID_PROCESS_OPTIONS', { stage: 'validate_options' });
  }

  if (normalized.signal && normalized.signal.aborted) {
    throw safeError('PROCESS_ABORTED', normalized, 'abort');
  }

  return new Promise((resolve, reject) => {
    let child;
    try {
      child = spawn(normalized.executable, normalized.args, {
        shell: false,
        windowsHide: true,
        stdio: ['pipe', 'pipe', 'pipe'],
      });
    } catch {
      reject(safeError('PROCESS_SPAWN_ERROR', normalized, 'spawn'));
      return;
    }

    const stdoutChunks = [];
    const stdoutDecoder = new TextDecoder('utf-8', { fatal: true });
    const stderrDecoder = new TextDecoder('utf-8', { fatal: true });
    let stdoutBytes = 0;
    let stderrBytes = 0;
    let acceptingOutput = true;
    let settled = false;
    let terminationRequested = false;
    let pendingError = null;
    let forceKillTimer = null;

    const timeoutTimer = setTimeout(() => {
      requestTermination(safeError('PROCESS_TIMEOUT', normalized, 'timeout'));
    }, normalized.timeoutMs);

    const onAbort = () => {
      requestTermination(safeError('PROCESS_ABORTED', normalized, 'abort'));
    };
    const onStdoutData = (chunk) => {
      if (!acceptingOutput) return;
      const bytes = Buffer.byteLength(chunk);
      stdoutBytes += bytes;
      if (stdoutBytes > normalized.maxStdoutBytes) {
        stdoutChunks.length = 0;
        requestTermination(safeError('STDOUT_LIMIT_EXCEEDED', normalized, 'stdout_limit'));
        return;
      }
      try {
        stdoutChunks.push(stdoutDecoder.decode(chunk, { stream: true }));
      } catch {
        requestTermination(safeError('INVALID_PROCESS_OUTPUT', normalized, 'utf8'));
      }
    };
    const onStderrData = (chunk) => {
      if (!acceptingOutput) return;
      const bytes = Buffer.byteLength(chunk);
      stderrBytes += bytes;
      if (stderrBytes > normalized.maxStderrBytes) {
        requestTermination(safeError('STDERR_LIMIT_EXCEEDED', normalized, 'stderr_limit'));
        return;
      }
      try {
        stderrDecoder.decode(chunk, { stream: true });
      } catch {
        requestTermination(safeError('INVALID_PROCESS_OUTPUT', normalized, 'utf8'));
      }
    };
    const onStreamError = () => {
      requestTermination(safeError('PROCESS_STREAM_ERROR', normalized, 'output_stream'));
    };
    const onStdinError = () => {
      requestTermination(safeError('PROCESS_STDIN_ERROR', normalized, 'stdin'));
    };
    const onChildError = () => {
      finish(safeError('PROCESS_SPAWN_ERROR', normalized, 'spawn'));
    };
    const onClose = (exitCode, signal) => {
      if (pendingError) {
        finish(pendingError);
        return;
      }
      if (exitCode !== 0 || signal !== null) {
        finish(safeError('PROCESS_EXIT_ERROR', normalized, 'exit', {
          exit_code: Number.isInteger(exitCode) ? exitCode : undefined,
          signal: typeof signal === 'string' ? signal : undefined,
        }));
        return;
      }

      let stdoutText;
      try {
        stdoutChunks.push(stdoutDecoder.decode());
        stderrDecoder.decode();
        stdoutText = stdoutChunks.join('');
      } catch {
        finish(safeError('INVALID_PROCESS_OUTPUT', normalized, 'utf8'));
        return;
      }

      let parsed;
      try {
        parsed = JSON.parse(stdoutText);
      } catch {
        finish(safeError('INVALID_PROCESS_OUTPUT', normalized, 'json'));
        return;
      }
      finish(null, parsed);
    };

    function requestTermination(error) {
      if (settled || terminationRequested) return;
      terminationRequested = true;
      acceptingOutput = false;
      pendingError = error;
      child.kill();
      forceKillTimer = setTimeout(() => {
        if (!settled) child.kill('SIGKILL');
      }, normalized.killGraceMs);
    }

    function finish(error, value) {
      if (settled) return;
      settled = true;
      acceptingOutput = false;
      clearTimeout(timeoutTimer);
      if (forceKillTimer !== null) clearTimeout(forceKillTimer);
      if (normalized.signal) normalized.signal.removeEventListener('abort', onAbort);
      child.removeListener('error', onChildError);
      child.removeListener('close', onClose);
      child.stdin.removeListener('error', onStdinError);
      child.stdout.removeListener('data', onStdoutData);
      child.stdout.removeListener('error', onStreamError);
      child.stderr.removeListener('data', onStderrData);
      child.stderr.removeListener('error', onStreamError);
      if (error) reject(error);
      else resolve(value);
    }

    child.once('error', onChildError);
    child.once('close', onClose);
    child.stdin.once('error', onStdinError);
    child.stdout.on('data', onStdoutData);
    child.stdout.once('error', onStreamError);
    child.stderr.on('data', onStderrData);
    child.stderr.once('error', onStreamError);
    if (normalized.signal) normalized.signal.addEventListener('abort', onAbort, { once: true });

    child.stdin.end(normalized.requestJson);
  });
}

function normalizeOptions(options) {
  if (!isPlainObject(options)) throw new TypeError('options must be a plain object');
  const executable = ownData(options, 'executable');
  const args = ownData(options, 'args');
  const request = ownData(options, 'request');
  const operation = ownData(options, 'operation');
  const caseId = ownData(options, 'caseId');
  const timeoutMs = ownData(options, 'timeoutMs');
  const killGraceMs = ownData(options, 'killGraceMs');
  const maxStdoutBytes = ownData(options, 'maxStdoutBytes');
  const maxStderrBytes = ownData(options, 'maxStderrBytes');
  const signal = ownData(options, 'signal');
  if (![executable, args, request, operation, caseId, timeoutMs, killGraceMs,
    maxStdoutBytes, maxStderrBytes, signal].every((field) => field.valid)) {
    throw new TypeError('options must use data properties');
  }
  if (!isNonEmptyString(executable.value) || !isSafeLabel(operation.value) ||
      !isSafeLabel(caseId.value) || !Array.isArray(args.value)) {
    throw new TypeError('invalid command identity');
  }

  const copiedArgs = [];
  for (let index = 0; index < args.value.length; index += 1) {
    const descriptor = Object.getOwnPropertyDescriptor(args.value, index);
    if (!descriptor || !Object.hasOwn(descriptor, 'value') || typeof descriptor.value !== 'string') {
      throw new TypeError('args must be an array of strings');
    }
    copiedArgs.push(descriptor.value);
  }
  for (const value of [timeoutMs.value, killGraceMs.value]) {
    if (!Number.isInteger(value) || value <= 0 || value > LIMITS.MAX_CASE_DURATION_MS) {
      throw new RangeError('invalid process deadline');
    }
  }
  for (const value of [maxStdoutBytes.value, maxStderrBytes.value]) {
    if (!Number.isInteger(value) || value <= 0 || value > MAX_PROCESS_OUTPUT_BYTES) {
      throw new RangeError('invalid process output limit');
    }
  }
  if (signal.present && signal.value !== undefined &&
      (!signal.value || typeof signal.value.aborted !== 'boolean' ||
       typeof signal.value.addEventListener !== 'function' ||
       typeof signal.value.removeEventListener !== 'function')) {
    throw new TypeError('signal must be an AbortSignal');
  }

  const requestCopy = copyJsonValue(request.value);
  if (!isPlainObject(requestCopy)) throw new TypeError('request must be a plain JSON object');
  return Object.freeze({
    executable: executable.value,
    args: Object.freeze(copiedArgs),
    requestJson: `${JSON.stringify(requestCopy)}\n`,
    operation: operation.value,
    caseId: caseId.value,
    timeoutMs: timeoutMs.value,
    killGraceMs: killGraceMs.value,
    maxStdoutBytes: maxStdoutBytes.value,
    maxStderrBytes: maxStderrBytes.value,
    signal: signal.present ? signal.value : undefined,
  });
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

function safeError(code, context = {}, stage, details = {}) {
  const fields = {
    code,
    operation: context.operation,
    case_id: context.caseId,
    stage: stage || context.stage,
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
  Object.defineProperty(error, SAFE_ERROR, { value: true });
  return error;
}

function ownData(object, key) {
  const descriptor = Object.getOwnPropertyDescriptor(object, key);
  if (!descriptor) return { valid: true, present: false, value: undefined };
  return Object.hasOwn(descriptor, 'value')
    ? { valid: true, present: true, value: descriptor.value }
    : { valid: false, present: true, value: undefined };
}

function isPlainObject(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function isNonEmptyString(value) {
  return typeof value === 'string' && value.length > 0;
}

function isSafeLabel(value) {
  return isNonEmptyString(value) && value.length <= 128 && /^[A-Za-z0-9_.:-]+$/.test(value);
}

module.exports = { runBoundedCommand };
