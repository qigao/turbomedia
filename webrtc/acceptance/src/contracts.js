'use strict';

const fs = require('node:fs');
const path = require('node:path');
const Ajv2020 = require('ajv/dist/2020');
const { LIMITS } = require('./constants');

const SCHEMA_FILES = Object.freeze({
  manifest: 'manifest.schema.json',
  'turn-credential': 'turn-credential.schema.json',
  'sfu-token': 'sfu-token.schema.json',
  'hook-receipt': 'hook-receipt.schema.json',
  report: 'report.schema.json',
});

function loadSchema(schemaDirectory, fileName) {
  const schemaPath = path.resolve(schemaDirectory, fileName);
  return injectLimitValues(JSON.parse(fs.readFileSync(schemaPath, 'utf8')));
}

function injectLimitValues(schema) {
  if (Array.isArray(schema)) {
    schema.forEach(injectLimitValues);
    return schema;
  }
  if (!schema || typeof schema !== 'object') {
    return schema;
  }

  if (typeof schema.$comment === 'string' && schema.$comment.startsWith('limits:')) {
    const assignments = schema.$comment.slice('limits:'.length).split(';');
    for (const assignment of assignments) {
      const [keyword, limitName] = assignment.split('=');
      if (!['maxItems', 'maximum', 'minimum'].includes(keyword) ||
          !Object.hasOwn(LIMITS, limitName)) {
        throw new Error(`invalid schema limit annotation: ${schema.$comment}`);
      }
      schema[keyword] = LIMITS[limitName];
    }
    delete schema.$comment;
  }

  for (const value of Object.values(schema)) {
    injectLimitValues(value);
  }
  return schema;
}

function createContractValidator(schemaDirectory) {
  if (typeof schemaDirectory !== 'string' || schemaDirectory.length === 0) {
    throw new TypeError('schemaDirectory must be a non-empty string');
  }

  const ajv = new Ajv2020({
    allErrors: true,
    coerceTypes: false,
    removeAdditional: false,
    useDefaults: false,
  });
  const validators = Object.create(null);

  for (const [schemaName, fileName] of Object.entries(SCHEMA_FILES)) {
    validators[schemaName] = ajv.compile(loadSchema(schemaDirectory, fileName));
  }

  return Object.freeze({ validators: Object.freeze(validators) });
}

function validateContract(validator, schemaName, value) {
  if (!validator || !validator.validators) {
    throw new TypeError('validator must be created by createContractValidator');
  }

  const validate = validator.validators[schemaName];
  if (!validate) {
    throw new RangeError(`unknown contract schema: ${schemaName}`);
  }

  if (validate(value)) {
    return { valid: true, errors: [] };
  }

  return {
    valid: false,
    errors: validate.errors.map((error) => ({
      instancePath: error.instancePath,
      keyword: error.keyword,
      message: error.message,
      params: { ...error.params },
      schemaPath: error.schemaPath,
    })),
  };
}

module.exports = { LIMITS, createContractValidator, validateContract };
