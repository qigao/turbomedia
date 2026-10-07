'use strict';

const fs = require('node:fs');
const { main } = require('../src/cli');

const mode = process.env.TURBO_CLI_FIXTURE_MODE || 'outcome';
const outcome = process.env.TURBO_CLI_FIXTURE_OUTCOME || 'PASS';
const tracePath = process.env.TURBO_CLI_FIXTURE_TRACE;

function trace(value) {
  if (tracePath) fs.appendFileSync(tracePath, value + '\n', 'utf8');
}

async function executeRun(options) {
  trace(JSON.stringify({
    event: 'started',
    outputDirectory: options.outputDirectory,
    runLabel: options.runLabel ?? null,
  }));

  if (mode === 'secret_error') {
    const secret = process.env.TURBO_CLI_FIXTURE_SECRET || 'fixture-secret';
    throw Object.assign(new Error(secret), { code: secret, outcome: 'ERROR', payload: secret });
  }

  if (mode === 'signal' || mode === 'double_signal') {
    // A pending Promise alone does not keep Node's event loop alive. Real
    // acceptance runs own HTTP/Grid/process timers, so this interval models an
    // active run and lets the OS signal callback be delivered deterministically.
    const keepAlive = setInterval(() => {}, 1_000);
    setTimeout(() => {
      trace(JSON.stringify({
        event: 'signal_dispatch',
        mode,
        signal: mode === 'signal' ? 'SIGTERM' : 'SIGINT',
        sigint_handlers: process.listenerCount('SIGINT'),
        sigterm_handlers: process.listenerCount('SIGTERM'),
      }));
      process.kill(process.pid, mode === 'signal' ? 'SIGTERM' : 'SIGINT');
    }, 25);
    return new Promise((resolve) => {
      const onAbort = () => {
        trace(JSON.stringify({ event: 'drain' }));
        if (mode === 'signal') {
          setTimeout(() => {
            clearInterval(keepAlive);
            resolve({ outcome: 'ERROR', run_id: 'run-signal' });
          }, 20);
        } else {
          setTimeout(() => {
            trace(JSON.stringify({ event: 'signal_dispatch', mode, signal: 'SIGTERM' }));
            // Keep the fixture active until the second-signal CLI handler calls
            // process.exit(ERROR); otherwise Node may naturally exit before the
            // queued OS signal callback is delivered.
            process.kill(process.pid, 'SIGTERM');
          }, 25);
        }
      };
      if (options.signal.aborted) onAbort();
      else options.signal.addEventListener('abort', onAbort, { once: true });
    });
  }

  return { outcome, run_id: 'run-fixture' };
}

main({
  argv: process.argv.slice(2),
  executeRun,
}).then((code) => {
  process.exitCode = code;
}).catch(() => {
  process.exitCode = 4;
});
