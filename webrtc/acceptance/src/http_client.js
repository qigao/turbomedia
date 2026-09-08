'use strict';

const { LIMITS } = require('./constants');

const DEFAULT_REQUEST_TIMEOUT_MS = 10_000;
const JSON_TYPE = /^application\/json(?:\s*;\s*charset\s*=\s*(?:utf-8|"utf-8"))?\s*$/i;
const TEXT_TYPE = /^text\/plain(?:\s*;\s*charset\s*=\s*(?:utf-8|"utf-8"))?\s*$/i;

function httpError(code, operation, stage, status) {
  const error = new Error(`code=${code} operation=${operation} stage=${stage}${status === undefined ? '' : ` status=${status}`}`);
  Object.assign(error, { code, operation, stage });
  if (status !== undefined) error.status = status;
  return error;
}

function positiveLimit(value, name) {
  if (!Number.isSafeInteger(value) || value <= 0 || value > LIMITS.MAX_CASE_DURATION_MS) {
    throw httpError('HTTP_INVALID_OPTIONS', name, 'options');
  }
  return value;
}

function createHttpClient(options = {}) {
  let base;
  try { base = new URL(options.baseUrl); } catch { throw httpError('HTTP_INVALID_URL', 'http', 'options'); }
  if (!['https:', 'http:'].includes(base.protocol) || base.username || base.password ||
      base.search || base.hash || base.pathname !== '/') {
    throw httpError('HTTP_INVALID_URL', 'http', 'options');
  }
  const requestTimeoutMs = positiveLimit(options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS, 'request_timeout');
  const maxBodyBytes = options.maxBodyBytes ?? LIMITS.MAX_PROVIDER_OUTPUT_BYTES;
  if (!Number.isSafeInteger(maxBodyBytes) || maxBodyBytes <= 0 || maxBodyBytes > LIMITS.MAX_ARTIFACT_BYTES_PER_CASE) {
    throw httpError('HTTP_INVALID_OPTIONS', 'body_limit', 'options');
  }

  function resourceKey(segments) {
    if (!Array.isArray(segments) || segments.length === 0 || segments.some((part) =>
      typeof part !== 'string' || !part || part === '.' || part === '..' || !part.isWellFormed())) {
      throw httpError('HTTP_INVALID_PATH', 'http', 'path');
    }
    return `${base.origin}/${segments.map(encodeURIComponent).join('/')}`;
  }

  async function request({ segments, method = 'GET', body, token, signal, statuses = [200], responseType = 'json', operation = 'http' }) {
    const url = resourceKey(segments);
    const controller = new AbortController();
    let timedOut = false;
    const abort = () => controller.abort();
    if (signal?.aborted) throw httpError('HTTP_ABORTED', operation, 'abort');
    signal?.addEventListener('abort', abort, { once: true });
    const timer = setTimeout(() => { timedOut = true; controller.abort(); }, requestTimeoutMs);
    let response;
    let reader;
    try {
      const headers = { Accept: responseType === 'json' ? 'application/json' : 'text/plain' };
      if (token !== undefined) {
        if (typeof token !== 'string' || !token || /[\r\n]/.test(token)) throw httpError('HTTP_INVALID_TOKEN', operation, 'request');
        headers.Authorization = `Bearer ${token}`;
      }
      let encodedBody;
      if (body !== undefined) {
        encodedBody = JSON.stringify(body);
        if (Buffer.byteLength(encodedBody) > maxBodyBytes) throw httpError('HTTP_BODY_LIMIT', operation, 'request');
        headers['Content-Type'] = 'application/json';
      }
      response = await fetch(url, { method, headers, body: encodedBody, signal: controller.signal, redirect: 'manual' });
      const length = response.headers.get('content-length');
      if (length !== null && (!/^\d+$/.test(length) || Number(length) > maxBodyBytes)) {
        throw httpError('HTTP_BODY_LIMIT', operation, 'response', response.status);
      }
      const chunks = [];
      let size = 0;
      reader = response.body?.getReader();
      if (reader) {
        while (true) {
          const chunk = await reader.read();
          if (chunk.done) break;
          size += chunk.value.byteLength;
          if (size > maxBodyBytes) throw httpError('HTTP_BODY_LIMIT', operation, 'response', response.status);
          chunks.push(chunk.value);
        }
      }
      if (!statuses.includes(response.status)) throw httpError('HTTP_STATUS', operation, 'status', response.status);
      if (response.status === 204) return Object.freeze({ status: response.status, body: null });
      const type = response.headers.get('content-type') || '';
      if (!(responseType === 'json' ? JSON_TYPE : TEXT_TYPE).test(type)) {
        throw httpError('HTTP_CONTENT_TYPE', operation, 'content_type', response.status);
      }
      let text;
      try { text = new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks, size)); }
      catch { throw httpError('HTTP_INVALID_UTF8', operation, 'utf8', response.status); }
      let parsed = text;
      if (responseType === 'json') {
        try { parsed = JSON.parse(text); }
        catch { throw httpError('HTTP_INVALID_JSON', operation, 'json', response.status); }
      }
      return Object.freeze({ status: response.status, body: parsed });
    } catch (error) {
      if (timedOut) throw httpError('HTTP_TIMEOUT', operation, 'timeout');
      if (signal?.aborted) throw httpError('HTTP_ABORTED', operation, 'abort');
      if (error instanceof Error && error.code?.startsWith('HTTP_')) throw error;
      throw httpError('HTTP_TRANSPORT', operation, 'transport');
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
      controller.abort();
      reader?.releaseLock();
    }
  }
  return Object.freeze({ request, resourceKey });
}

module.exports = { createHttpClient, httpError, positiveLimit };
