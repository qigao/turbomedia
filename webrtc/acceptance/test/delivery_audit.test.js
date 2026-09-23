'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { loadManifest } = require('../src/manifest');

const acceptanceRoot = path.join(__dirname, '..');
const examples = Object.freeze([
  Object.freeze({ name: 'diagnostic', path: path.join(acceptanceRoot, 'examples', 'diagnostic.manifest.json') }),
  Object.freeze({ name: 'release', path: path.join(acceptanceRoot, 'examples', 'release.manifest.json') }),
]);
const SECRET_PATTERNS = Object.freeze([
  /-----BEGIN [A-Z ]*PRIVATE KEY-----/,
  /\bAKIA[0-9A-Z]{16}\b/,
  /\bgh[pousr]_[A-Za-z0-9]{20,}\b/,
  /\bsk-[A-Za-z0-9]{20,}\b/,
  /\bxox[baprs]-[A-Za-z0-9-]{10,}\b/,
]);
const SHIPPING_DIRECTORIES = Object.freeze(['src', 'schemas', 'examples']);
const SHIPPING_DOCS = Object.freeze(['README.md', 'CLI.md', 'CONTRACTS.md']);

async function temporaryOutput(t) {
  const directory = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'webrtc-delivery-audit-'));
  t.after(() => fs.promises.rm(directory, { recursive: true, force: true }));
  return directory;
}

function walkStrings(value, visit) {
  if (typeof value === 'string') {
    visit(value);
    return;
  }
  if (Array.isArray(value)) {
    for (const entry of value) walkStrings(entry, visit);
    return;
  }
  if (value && typeof value === 'object') {
    for (const entry of Object.values(value)) walkStrings(entry, visit);
  }
}

function assertPlaceholderNetworkValue(value) {
  if (/^https?:\/\//.test(value)) {
    const url = new URL(value);
    assert.equal(url.username, '');
    assert.equal(url.password, '');
    assert.ok(url.hostname === 'example.test' || url.hostname.endsWith('.example.test'),
      `non-placeholder HTTP endpoint in example: ${url.hostname}`);
  }
  if (/^turns?:/.test(value)) {
    const parsed = new URL(value.replace(/^turns?:/, 'https://'));
    assert.ok(parsed.hostname === 'example.test' || parsed.hostname.endsWith('.example.test') ||
      parsed.hostname.endsWith('.invalid'),
    `non-placeholder TURN endpoint in example: ${parsed.hostname}`);
  }
}

async function collectFiles(directory) {
  const output = [];
  for (const entry of await fs.promises.readdir(directory, { withFileTypes: true })) {
    const target = path.join(directory, entry.name);
    if (entry.isDirectory()) output.push(...await collectFiles(target));
    else if (entry.isFile()) output.push(target);
  }
  return output;
}

test('diagnostic and release example manifests are schema-valid placeholder-only inputs', async (t) => {
  const output = await temporaryOutput(t);
  for (const example of examples) {
    const manifest = loadManifest(example.path, {
      outputDirectory: path.join(output, example.name),
      runLabel: `audit-${example.name}`,
    });
    assert.equal(manifest.profile, example.name);
    walkStrings(manifest, (value) => {
      assertPlaceholderNetworkValue(value);
      for (const pattern of SECRET_PATTERNS) {
        assert.equal(pattern.test(value), false, `secret-like value in ${example.name} example`);
      }
    });
  }

  const release = loadManifest(examples[1].path, { outputDirectory: path.join(output, 'release-matrix') });
  assert.deepEqual(release.grid.browsers.map((entry) => entry.name),
    ['chrome', 'MicrosoftEdge', 'firefox', 'safari']);
  assert.deepEqual(release.topologies.map((entry) => entry.relay_contract.ip_family),
    ['ipv4', 'ipv6']);
});

test('native release CMake graph remains independent of the Node acceptance package', async () => {
  const repositoryRoot = path.resolve(acceptanceRoot, '..', '..');
  const cmakeFiles = [
    path.join(repositoryRoot, 'CMakeLists.txt'),
    path.join(repositoryRoot, 'webrtc', 'CMakeLists.txt'),
  ];
  const forbidden = [
    /webrtc[\\/]acceptance/,
    /add_subdirectory\s*\(\s*acceptance\b/i,
    /node_modules/i,
    /package\.json/i,
    /\bnpm\b/i,
  ];
  for (const fileName of cmakeFiles) {
    const text = await fs.promises.readFile(fileName, 'utf8');
    for (const pattern of forbidden) {
      assert.equal(pattern.test(text), false,
        `Node acceptance leaked into native CMake graph: ${path.relative(repositoryRoot, fileName)}`);
    }
  }

  const buildPresets = JSON.parse(await fs.promises.readFile(
    path.join(repositoryRoot, 'presets', 'BuildPresets.json'), 'utf8'));
  const linuxRelease = buildPresets.buildPresets.find((entry) => entry.name === 'build-default-linux');
  assert.ok(linuxRelease);
  assert.equal(linuxRelease.configurePreset, 'release-linux-ninja');
});

test('shipping acceptance sources schemas examples and docs contain no credential-like literals', async () => {
  const files = [];
  for (const directory of SHIPPING_DIRECTORIES) {
    files.push(...await collectFiles(path.join(acceptanceRoot, directory)));
  }
  for (const fileName of SHIPPING_DOCS) {
    const target = path.join(acceptanceRoot, fileName);
    try {
      await fs.promises.access(target);
      files.push(target);
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
    }
  }

  for (const fileName of files) {
    const text = await fs.promises.readFile(fileName, 'utf8');
    for (const pattern of SECRET_PATTERNS) {
      assert.equal(pattern.test(text), false,
        `secret-like literal matched in ${path.relative(acceptanceRoot, fileName)}`);
    }
  }
});
