'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { LIMITS, createContractValidator, validateContract } = require('./contracts');
const { canonicalStringify, hashCanonical } = require('./canonical_json');

const SCHEMA_DIRECTORY = path.join(__dirname, '..', 'schemas');
const runtimeOverrideNames = new Set(['outputDirectory', 'runLabel']);

function loadManifest(filePath, runtimeOverrides = {}) {
  if (typeof filePath !== 'string' || filePath.length === 0) {
    throw new TypeError('manifest filePath must be a non-empty string');
  }

  let parsed;
  try {
    parsed = JSON.parse(fs.readFileSync(filePath, 'utf8'));
  } catch (error) {
    throw new Error(`unable to load manifest ${filePath}: ${error.message}`, { cause: error });
  }

  const validator = createContractValidator(SCHEMA_DIRECTORY);
  const validation = validateContract(validator, 'manifest', parsed);
  if (!validation.valid) {
    const details = validation.errors.map((error) => `${error.instancePath || '/'} ${error.message}`).join('; ');
    throw new Error(`manifest validation failed: ${details}`);
  }

  const runtime = normalizeRuntimeOverrides(runtimeOverrides, parsed.artifact_directory);
  const effectiveManifest = {
    ...parsed,
    artifact_directory: runtime.outputDirectory,
    runtime,
  };
  return deepFreeze(effectiveManifest);
}

function normalizeRuntimeOverrides(runtimeOverrides, defaultOutputDirectory) {
  if (!runtimeOverrides || typeof runtimeOverrides !== 'object' || Array.isArray(runtimeOverrides)) {
    throw new TypeError('runtime overrides must be an object');
  }
  for (const name of Object.keys(runtimeOverrides)) {
    if (!runtimeOverrideNames.has(name)) {
      throw new RangeError(`runtime override is not allowed: ${name}`);
    }
  }

  const outputDirectory = runtimeOverrides.outputDirectory === undefined
    ? defaultOutputDirectory
    : requireNonEmptyString(runtimeOverrides.outputDirectory, 'runtime override outputDirectory');
  const runtime = { outputDirectory };
  if (runtimeOverrides.runLabel !== undefined) {
    runtime.runLabel = requireNonEmptyString(runtimeOverrides.runLabel, 'runtime override runLabel');
  }
  return runtime;
}

function expandCases(manifest) {
  if (!manifest || typeof manifest !== 'object') {
    throw new TypeError('manifest must be an object');
  }
  const browsers = manifest.grid && manifest.grid.browsers;
  const scenarios = manifest.scenarios;
  if (!Array.isArray(browsers) || browsers.length === 0 || !Array.isArray(scenarios) || scenarios.length === 0) {
    throw new RangeError('manifest matrix must contain at least one browser and scenario');
  }
  const topologiesById = createTopologyIndex(manifest.topologies);

  const maxCases = manifest.limits && manifest.limits.max_cases !== undefined
    ? manifest.limits.max_cases
    : LIMITS.MAX_CASES;
  if (!Number.isInteger(maxCases) || maxCases < 1 || maxCases > LIMITS.MAX_CASES) {
    throw new RangeError(`manifest max_cases must be between 1 and ${LIMITS.MAX_CASES}`);
  }

  const credentialProfile = hashCanonical(manifest.turn.credential_provider.command);
  const seenCaseKeys = new Set();
  const cases = [];
  for (const browser of browsers) {
    for (const scenario of scenarios) {
      if (!topologiesById.has(scenario.topology_id)) {
        throw new RangeError(`scenario references undeclared topology_id: ${scenario.topology_id}`);
      }
      const topology = topologiesById.get(scenario.topology_id);
      if (cases.length >= maxCases) {
        throw new RangeError(`expanded case count exceeds ${maxCases}`);
      }
      const caseKey = canonicalStringify({
        browser: {
          name: browser.name,
          platform: browser.platform,
          version: browser.version,
        },
        credential_profile: credentialProfile,
        publisher: 'publisher',
        scenario: scenario.scenario_id,
        topology_id: scenario.topology_id,
        viewer: 'viewer',
      });
      if (seenCaseKeys.has(caseKey)) {
        throw new Error(`duplicate case key: ${caseKey}`);
      }
      seenCaseKeys.add(caseKey);
      cases.push(Object.freeze({
        browser: Object.freeze({ ...browser }),
        case_key: caseKey,
        credential_profile: credentialProfile,
        publisher: 'publisher',
        scenario,
        scenario_id: scenario.scenario_id,
        topology_id: scenario.topology_id,
        relay_contract: topology.relay_contract,
        viewer: 'viewer',
      }));
    }
  }
  return Object.freeze(cases);
}

function createTopologyIndex(topologies) {
  if (!Array.isArray(topologies) || topologies.length === 0) {
    throw new RangeError('manifest must declare at least one topology');
  }
  const topologiesById = new Map();
  for (const topology of topologies) {
    if (topologiesById.has(topology.topology_id)) {
      throw new Error(`duplicate topology_id: ${topology.topology_id}`);
    }
    topologiesById.set(topology.topology_id, topology);
  }
  return topologiesById;
}

function requireNonEmptyString(value, name) {
  if (typeof value !== 'string' || value.length === 0) {
    throw new TypeError(`${name} must be a non-empty string`);
  }
  return value;
}

function deepFreeze(value, seen = new Set()) {
  if (!value || typeof value !== 'object' || seen.has(value)) {
    return value;
  }
  seen.add(value);
  for (const key of Object.keys(value)) {
    deepFreeze(value[key], seen);
  }
  return Object.freeze(value);
}

module.exports = { loadManifest, expandCases };
