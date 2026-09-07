'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const { createRedactor } = require('../src/redaction');

test('redactor removes raw JSON URL and form encodings without mutating input', () => {
  const secrets = Object.freeze(['a+b c', '秘密/🔑']);
  const redactor = createRedactor(secrets);
  const text = [
    'a+b c',
    JSON.stringify('a+b c').slice(1, -1),
    'a%2Bb%20c',
    'a%2bb%20c',
    'a%2Bb+c',
    encodeURIComponent('秘密/🔑'),
    encodeURIComponent('秘密/🔑').toLowerCase(),
  ].join('|');

  const result = redactor(text);

  assert.equal(secrets.length, 2);
  assert.ok(!result.includes('a+b c'));
  assert.ok(!result.toLowerCase().includes('a%2bb%20c'));
  assert.ok(!result.toLowerCase().includes(encodeURIComponent('秘密/🔑').toLowerCase()));
  assert.equal(result.split('[REDACTED]').length - 1, 7);
});

test('redactor handles regex characters repeats and overlapping secrets longest first', () => {
  const redactor = createRedactor(['token', 'token-long.*', 'token']);
  const result = redactor('token-long.* token token-long.*');

  assert.equal(result, '[REDACTED] [REDACTED] [REDACTED]');
  assert.ok(!result.includes('-long.*'));
});

test('redactor removes application/x-www-form-urlencoded punctuation encoding', () => {
  const secret = "a~b!c'd(e) f";
  const formEncoded = new URLSearchParams({ value: secret }).toString().slice('value='.length);
  const redactor = createRedactor([secret]);

  assert.equal(redactor(formEncoded), '[REDACTED]');
});

test('redactor rejects empty non-string or non-array secret registries', () => {
  assert.throws(() => createRedactor(['']), /non-empty strings/i);
  assert.throws(() => createRedactor([42]), /non-empty strings/i);
  assert.throws(() => createRedactor('secret'), /array/i);
});
