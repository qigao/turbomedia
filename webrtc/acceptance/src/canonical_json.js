'use strict';

const crypto = require('node:crypto');

function canonicalStringify(value) {
  return serialize(value, new Set());
}

function serialize(value, ancestors) {
  if (value === null) return 'null';

  switch (typeof value) {
    case 'boolean':
      return value ? 'true' : 'false';
    case 'number':
      if (!Number.isFinite(value)) {
        throw new TypeError('canonical JSON does not support non-finite numbers');
      }
      return JSON.stringify(value);
    case 'string':
      return JSON.stringify(value);
    case 'object':
      break;
    default:
      throw new TypeError(`canonical JSON does not support ${typeof value}`);
  }

  if (ancestors.has(value)) {
    throw new TypeError('canonical JSON does not support cyclic values');
  }
  ancestors.add(value);
  try {
    if (Array.isArray(value)) {
      const entries = [];
      for (let index = 0; index < value.length; index += 1) {
        if (!Object.hasOwn(value, index)) {
          throw new TypeError('canonical JSON does not support sparse arrays');
        }
        entries.push(serialize(value[index], ancestors));
      }
      return `[${entries.join(',')}]`;
    }

    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) {
      throw new TypeError('canonical JSON only supports plain objects');
    }
    const entries = [];
    for (const key of Object.keys(value).sort()) {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (!descriptor || !Object.hasOwn(descriptor, 'value')) {
        throw new TypeError('canonical JSON does not support accessor properties');
      }
      entries.push(`${JSON.stringify(key)}:${serialize(descriptor.value, ancestors)}`);
    }
    return `{${entries.join(',')}}`;
  } finally {
    ancestors.delete(value);
  }
}

function hashCanonical(value) {
  return crypto.createHash('sha256').update(canonicalStringify(value), 'utf8').digest('hex');
}

module.exports = { canonicalStringify, hashCanonical };
