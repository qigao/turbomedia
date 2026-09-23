'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { canonicalStringify, hashCanonical } = require('../src/canonical_json');
const { loadManifest, expandCases } = require('../src/manifest');

const fixturePath = path.join(__dirname, 'fixtures', 'minimal-manifest.json');

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function loadFixture() {
  return JSON.parse(fs.readFileSync(fixturePath, 'utf8'));
}

function writeManifest(t, manifest) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'webrtc-acceptance-manifest-'));
  const filePath = path.join(directory, 'manifest.json');
  fs.writeFileSync(filePath, JSON.stringify(manifest), 'utf8');
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return filePath;
}

test('canonical JSON hashes equivalent object key orders identically', () => {
  const left = { scenario: { id: 'relay', threshold: 1 }, browser: ['chrome', '127'] };
  const right = { browser: ['chrome', '127'], scenario: { threshold: 1, id: 'relay' } };

  assert.equal(canonicalStringify(left), canonicalStringify(right));
  assert.equal(hashCanonical(left), hashCanonical(right));
  assert.equal(
    hashCanonical(['turn-credential-provider', '--json']),
    '2d25ee65992e02dbb8c6046b49413c1ea03f8a49071d1c04e50f9b4ae7cc1ae8'
  );
});

test('canonical JSON rejects an array accessor without executing its getter', () => {
  let getterReads = 0;
  const value = [];
  Object.defineProperty(value, 0, {
    enumerable: true,
    get() {
      getterReads += 1;
      return 'must-not-be-read';
    },
  });
  value.length = 1;

  assert.throws(() => canonicalStringify(value), /accessor properties/i);
  assert.equal(getterReads, 0);
});

test('canonical JSON rejects an object accessor without executing its getter', () => {
  let getterReads = 0;
  const value = {};
  Object.defineProperty(value, 'secret', {
    enumerable: true,
    get() {
      getterReads += 1;
      return 'must-not-be-read';
    },
  });

  assert.throws(() => canonicalStringify(value), /accessor properties/i);
  assert.equal(getterReads, 0);
});

test('canonical JSON rejects cyclic values', () => {
  const value = {};
  value.self = value;

  assert.throws(() => canonicalStringify(value), /cyclic values/i);
});

test('canonical JSON rejects sparse arrays', () => {
  const value = [];
  value.length = 1;

  assert.throws(() => canonicalStringify(value), /sparse arrays/i);
});

test('manifest expansion retains declared browser then scenario order', () => {
  const manifest = loadManifest(fixturePath);
  const cases = expandCases(manifest);

  assert.deepEqual(cases.map((entry) => [entry.browser.name, entry.scenario_id]), [
    ['chrome', 'relay-baseline'],
    ['chrome', 'network-migration'],
    ['firefox', 'relay-baseline'],
    ['firefox', 'network-migration'],
  ]);
  assert.ok(cases.every((entry) => entry.publisher === 'publisher'));
  assert.ok(cases.every((entry) => entry.viewer === 'viewer'));
  assert.ok(cases.every((entry) => entry.topology_id === 'restricted-nat-ipv4'));
  assert.ok(cases.every((entry) => entry.relay_contract === manifest.topologies[0].relay_contract));
  assert.deepEqual(cases[0].relay_contract, {
    schema_version: 1,
    ip_family: 'ipv4',
    protocol: 'udp',
    relay_protocol: 'tcp',
    remote_candidate_types: ['host'],
  });
  assert.ok(cases.every((entry) => /^[0-9a-f]{64}$/.test(entry.credential_profile)));
  assert.deepEqual(cases.map((entry) => entry.scenario.duration_ms), [1000, 1000, 1000, 1000]);
  assert.deepEqual(cases.map((entry) => entry.scenario.workflow.topology_transition),
    [false, true, false, true]);
});

test('manifest without a run label remains canonical JSON data', () => {
  const manifest = loadManifest(fixturePath);

  assert.equal(hashCanonical(manifest).length, 64);
  assert.deepEqual(manifest.runtime, { outputDirectory: 'artifacts/webrtc-acceptance' });
});

test('manifest expansion rejects duplicate case keys', (t) => {
  const manifest = loadFixture();
  manifest.grid.browsers.push(clone(manifest.grid.browsers[0]));

  assert.throws(
    () => expandCases(loadManifest(writeManifest(t, manifest))),
    /duplicate case key/i
  );
});

test('manifest expansion accepts a scenario that references a declared topology', () => {
  const manifest = loadManifest(fixturePath);

  assert.equal(expandCases(manifest).length, 4);
});

test('manifest expansion rejects a scenario with an undeclared topology', (t) => {
  const manifest = loadFixture();
  manifest.scenarios[0].topology_id = 'missing-topology';

  assert.throws(
    () => expandCases(loadManifest(writeManifest(t, manifest))),
    /undeclared topology_id.*missing-topology/i
  );
});

test('manifest expansion rejects duplicate topology identifiers', (t) => {
  const manifest = loadFixture();
  manifest.topologies.push(clone(manifest.topologies[0]));

  assert.throws(
    () => expandCases(loadManifest(writeManifest(t, manifest))),
    /duplicate topology_id.*restricted-nat-ipv4/i
  );
});

test('manifest expansion rejects more than the configured case limit', (t) => {
  const manifest = loadFixture();
  manifest.scenarios = Array.from({ length: 129 }, (_, index) => ({
    ...manifest.scenarios[0],
    scenario_id: `scenario-${index}`,
  }));

  assert.throws(
    () => expandCases(loadManifest(writeManifest(t, manifest))),
    /expanded case count.*256/i
  );
});

test('manifest runtime overrides only change the output directory and run label', () => {
  const manifest = loadManifest(fixturePath, {
    outputDirectory: 'artifacts/override',
    runLabel: 'nightly-turn',
  });

  assert.equal(manifest.artifact_directory, 'artifacts/override');
  assert.deepEqual(manifest.runtime, {
    outputDirectory: 'artifacts/override',
    runLabel: 'nightly-turn',
  });
  assert.throws(() => { manifest.grid.browsers.push({}); }, TypeError);
  assert.throws(
    () => loadManifest(fixturePath, { threshold_profile: { profile_id: 'weakened' } }),
    /runtime override.*threshold_profile/i
  );
  assert.throws(
    () => loadManifest(fixturePath, { unrecognizedOverride: true }),
    /runtime override.*unrecognizedOverride/i
  );
});
