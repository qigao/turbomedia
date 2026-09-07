'use strict';

const REDACTED = '[REDACTED]';

function createRedactor(secretValues) {
  if (!Array.isArray(secretValues)) {
    throw new TypeError('secretValues must be an array');
  }

  const patterns = new Map();
  for (let index = 0; index < secretValues.length; index += 1) {
    const descriptor = Object.getOwnPropertyDescriptor(secretValues, index);
    if (!descriptor || !Object.hasOwn(descriptor, 'value') ||
        typeof descriptor.value !== 'string' || descriptor.value.length === 0) {
      throw new TypeError('secretValues must contain only non-empty strings');
    }
    const secret = descriptor.value;
    addLiteralPattern(patterns, secret);
    addLiteralPattern(patterns, JSON.stringify(secret).slice(1, -1));
    addEncodedPattern(patterns, encodeURIComponent(secret.toWellFormed()));
    addEncodedPattern(patterns, formEncode(secret));
  }

  const alternatives = [...patterns.values()]
    .sort((left, right) => right.literalLength - left.literalLength)
    .map((entry) => entry.source);
  const matcher = alternatives.length > 0 ? new RegExp(alternatives.join('|'), 'g') : null;

  return function redact(text) {
    if (typeof text !== 'string') {
      throw new TypeError('text must be a string');
    }
    return matcher ? text.replace(matcher, REDACTED) : text;
  };
}

function formEncode(value) {
  const encoded = new URLSearchParams([['value', value]]).toString();
  return encoded.slice('value='.length);
}

function addLiteralPattern(patterns, value) {
  const source = escapeRegularExpression(value);
  patterns.set(source, { source, literalLength: value.length });
}

function addEncodedPattern(patterns, value) {
  let source = '';
  for (let index = 0; index < value.length; index += 1) {
    const character = value[index];
    if (character === '%' && index + 2 < value.length &&
        /^[0-9A-F]{2}$/.test(value.slice(index + 1, index + 3))) {
      source += `%${hexPattern(value[index + 1])}${hexPattern(value[index + 2])}`;
      index += 2;
    } else {
      source += escapeRegularExpression(character);
    }
  }
  patterns.set(source, { source, literalLength: value.length });
}

function hexPattern(character) {
  return /[A-F]/.test(character) ? `[${character}${character.toLowerCase()}]` : character;
}

function escapeRegularExpression(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

module.exports = { createRedactor };
