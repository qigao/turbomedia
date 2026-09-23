'use strict';

const { canonicalStringify } = require('./canonical_json');
const { loadManifest } = require('./manifest');

const EXIT_CODES = Object.freeze({ PASS: 0, FAIL: 2, INCOMPLETE: 3, ERROR: 4 });
const MAX_LABEL_LENGTH = 128;
const PUBLIC_CODES = new Set([
  'CLI_USAGE',
  'CLI_COMMAND_INVALID',
  'CLI_ARGUMENT_INVALID',
  'CLI_ARGUMENT_DUPLICATE',
  'CLI_ARGUMENT_MISSING_VALUE',
  'CLI_MANIFEST_REQUIRED',
  'CLI_LABEL_INVALID',
  'MANIFEST_INVALID',
  'RUN_ERROR',
  'SECOND_SIGNAL',
  'CLI_FATAL',
]);

function parseArgs(argv) {
  if (!Array.isArray(argv) || argv.length < 1) throw cliError('CLI_USAGE');
  const command = argv[0];
  if (!['validate', 'run'].includes(command)) throw cliError('CLI_COMMAND_INVALID');

  const values = Object.create(null);
  for (let index = 1; index < argv.length; index += 1) {
    const flag = argv[index];
    if (!['--manifest', '--output', '--label'].includes(flag)) throw cliError('CLI_ARGUMENT_INVALID');
    if (Object.hasOwn(values, flag)) throw cliError('CLI_ARGUMENT_DUPLICATE');
    if (index + 1 >= argv.length || argv[index + 1].startsWith('--')) throw cliError('CLI_ARGUMENT_MISSING_VALUE');
    const value = argv[++index];
    if (typeof value !== 'string' || value.length === 0) throw cliError('CLI_ARGUMENT_MISSING_VALUE');
    values[flag] = value;
  }

  if (!Object.hasOwn(values, '--manifest')) throw cliError('CLI_MANIFEST_REQUIRED');
  if (command === 'validate' && (Object.hasOwn(values, '--output') || Object.hasOwn(values, '--label'))) {
    throw cliError('CLI_ARGUMENT_INVALID');
  }
  if (Object.hasOwn(values, '--label') &&
      (values['--label'].length > MAX_LABEL_LENGTH || /[\x00-\x1f\x7f]/.test(values['--label']))) {
    throw cliError('CLI_LABEL_INVALID');
  }

  return Object.freeze({
    command,
    manifestPath: values['--manifest'],
    outputDirectory: values['--output'],
    runLabel: values['--label'],
  });
}

async function main(options = {}) {
  const argv = options.argv ?? process.argv.slice(2);
  const stdout = options.stdout ?? process.stdout;
  const stderr = options.stderr ?? process.stderr;
  const processObject = options.processObject ?? process;
  const manifestLoader = options.loadManifest ?? loadManifest;
  const executeRun = options.executeRun ?? defaultExecuteRun;
  const installSignals = options.installSignalHandlers !== false;

  let parsed;
  try {
    parsed = parseArgs(argv);
  } catch (error) {
    const code = safeCode(error, 'CLI_USAGE');
    writeSummary(stdout, { command: 'unknown', outcome: 'ERROR', exit_code: EXIT_CODES.ERROR, code });
    writeDiagnostic(stderr, code);
    return EXIT_CODES.ERROR;
  }

  if (parsed.command === 'validate') {
    try {
      manifestLoader(parsed.manifestPath);
      writeSummary(stdout, { command: 'validate', outcome: 'PASS', exit_code: EXIT_CODES.PASS });
      return EXIT_CODES.PASS;
    } catch {
      writeSummary(stdout, { command: 'validate', outcome: 'ERROR', exit_code: EXIT_CODES.ERROR, code: 'MANIFEST_INVALID' });
      writeDiagnostic(stderr, 'MANIFEST_INVALID');
      return EXIT_CODES.ERROR;
    }
  }

  const abortController = new AbortController();
  let firstSignal = null;
  let summaryWritten = false;
  let handlersInstalled = false;

  const emitSummary = (value) => {
    if (summaryWritten) return;
    summaryWritten = true;
    writeSummary(stdout, value);
  };

  const onSignal = (signalName) => {
    if (firstSignal === null) {
      firstSignal = signalName;
      abortController.abort();
      return;
    }
    emitSummary({ command: 'run', outcome: 'ERROR', exit_code: EXIT_CODES.ERROR, code: 'SECOND_SIGNAL' });
    writeDiagnostic(stderr, 'SECOND_SIGNAL_ARTIFACTS_MAY_BE_INCOMPLETE');
    removeSignalHandlers();
    if (typeof processObject.exit === 'function') processObject.exit(EXIT_CODES.ERROR);
    else processObject.exitCode = EXIT_CODES.ERROR;
  };
  const signalHandlers = Object.freeze({
    SIGINT: () => onSignal('SIGINT'),
    SIGTERM: () => onSignal('SIGTERM'),
  });

  function installSignalHandlers() {
    if (!installSignals || typeof processObject.on !== 'function') return;
    processObject.on('SIGINT', signalHandlers.SIGINT);
    processObject.on('SIGTERM', signalHandlers.SIGTERM);
    handlersInstalled = true;
  }

  function removeSignalHandlers() {
    if (!handlersInstalled || typeof processObject.removeListener !== 'function') return;
    processObject.removeListener('SIGINT', signalHandlers.SIGINT);
    processObject.removeListener('SIGTERM', signalHandlers.SIGTERM);
    handlersInstalled = false;
  }

  try {
    const overrides = {};
    if (parsed.outputDirectory !== undefined) overrides.outputDirectory = parsed.outputDirectory;
    if (parsed.runLabel !== undefined) overrides.runLabel = parsed.runLabel;

    let manifest;
    try {
      manifest = manifestLoader(parsed.manifestPath, overrides);
    } catch {
      emitSummary({ command: 'run', outcome: 'ERROR', exit_code: EXIT_CODES.ERROR, code: 'MANIFEST_INVALID' });
      writeDiagnostic(stderr, 'MANIFEST_INVALID');
      return EXIT_CODES.ERROR;
    }

    installSignalHandlers();
    let result;
    try {
      result = await executeRun({
        manifest,
        signal: abortController.signal,
        outputDirectory: manifest.artifact_directory,
        runLabel: manifest.runtime && manifest.runtime.runLabel,
      });
    } catch (error) {
      const outcome = safeOutcome(error && error.outcome);
      const code = safeCode(error, 'RUN_ERROR');
      const exitCode = EXIT_CODES[outcome];
      emitSummary({ command: 'run', outcome, exit_code: exitCode, code });
      writeDiagnostic(stderr, code);
      return exitCode;
    }

    const outcome = safeOutcome(result && result.outcome);
    const exitCode = EXIT_CODES[outcome];
    const summary = { command: 'run', outcome, exit_code: exitCode };
    if (result && typeof result.run_id === 'string' && /^[A-Za-z0-9_.:-]+$/.test(result.run_id)) summary.run_id = result.run_id;
    emitSummary(summary);
    return exitCode;
  } finally {
    removeSignalHandlers();
  }
}

async function defaultExecuteRun(options) {
  const { executeAcceptanceRun } = require('./runner');
  return executeAcceptanceRun(options);
}

function safeOutcome(value) {
  return Object.hasOwn(EXIT_CODES, value) ? value : 'ERROR';
}

function safeCode(error, fallback) {
  const value = error && typeof error === 'object' && typeof error.code === 'string' ? error.code : fallback;
  return PUBLIC_CODES.has(value) ? value : fallback;
}

function cliError(code) {
  const error = new Error(code);
  error.code = code;
  return error;
}

function writeSummary(stream, value) {
  stream.write(canonicalStringify({ schema_version: 1, ...value }) + '\n');
}

function writeDiagnostic(stream, code) {
  stream.write(`webrtc-acceptance: ${code}\n`);
}

if (require.main === module) {
  main().then((code) => {
    process.exitCode = code;
  }).catch(() => {
    process.stdout.write(canonicalStringify({
      schema_version: 1,
      command: 'run',
      outcome: 'ERROR',
      exit_code: EXIT_CODES.ERROR,
      code: 'CLI_FATAL',
    }) + '\n');
    process.stderr.write('webrtc-acceptance: CLI_FATAL\n');
    process.exitCode = EXIT_CODES.ERROR;
  });
}

module.exports = { EXIT_CODES, main, parseArgs };
