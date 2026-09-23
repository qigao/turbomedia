'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { setTimeout: delay } = require('node:timers/promises');

const { hashCanonical } = require('./canonical_json');
const { LIMITS } = require('./constants');
const { normalizeBrowserSnapshot } = require('./metrics');
const { runBoundedCommand } = require('./process_adapter');
const { issueTurnCredential, issueSfuToken, invokeTopologyHook } = require('./providers');
const { createSeleniumAdapter } = require('./selenium_adapter');
const { createSfuAdapter } = require('./sfu_adapter');

const RESULT_FILE = 'result.json';
const MAX_OPERATION_MS = 45_000;
const SAFE_ID = /^[A-Za-z0-9_.:-]+$/;

function createAcceptanceLab(options = {}) {
  const manifest = options.manifest;
  const runIdentity = options.runIdentity;
  if (!manifest || typeof manifest !== 'object' || !runIdentity ||
      typeof runIdentity.run_id !== 'string' || !SAFE_ID.test(runIdentity.run_id)) {
    throw labError('LAB_OPTIONS_INVALID', 'harness', 'init');
  }

  const dependencies = options.dependencies || {};
  const runCommand = dependencies.runBoundedCommand || runBoundedCommand;
  const seleniumFactory = dependencies.createSeleniumAdapter || createSeleniumAdapter;
  const sfuFactory = dependencies.createSfuAdapter || createSfuAdapter;
  const now = dependencies.now || Date.now;
  const env = dependencies.env || process.env;
  if (typeof runCommand !== 'function' || typeof seleniumFactory !== 'function' ||
      typeof sfuFactory !== 'function' || typeof now !== 'function' ||
      !env || typeof env !== 'object') {
    throw labError('LAB_OPTIONS_INVALID', 'harness', 'init');
  }

  const phaseMax = Math.max(...Object.values(manifest.phase_deadlines));
  const sfu = sfuFactory({
    baseUrl: manifest.sfu.base_url,
    requestTimeoutMs: Math.min(MAX_OPERATION_MS, phaseMax),
    waitTimeoutMs: Math.min(MAX_OPERATION_MS,
      Math.max(manifest.phase_deadlines.stable_ms, manifest.phase_deadlines.recovery_ms)),
  });
  const topologyById = new Map(manifest.topologies.map((entry) => [entry.topology_id, entry]));
  const cases = new Map();
  let sfuBaseline = null;

  function outputLimit(kind) {
    const configured = manifest.limits || {};
    if (kind === 'hook') {
      return Math.min(configured.max_hook_output_bytes ?? LIMITS.MAX_HOOK_OUTPUT_BYTES,
        LIMITS.MAX_HOOK_OUTPUT_BYTES);
    }
    return Math.min(configured.max_provider_output_bytes ?? LIMITS.MAX_PROVIDER_OUTPUT_BYTES,
      LIMITS.MAX_PROVIDER_OUTPUT_BYTES);
  }

  async function invokeCommand(command, request, signal, operation, kind) {
    if (!command || !Array.isArray(command.command) || command.command.length === 0 ||
        command.command.some((value) => typeof value !== 'string' || value.length === 0)) {
      throw labError('LAB_COMMAND_INVALID', 'harness', operation);
    }
    const executable = command.command[0];
    const args = command.command.slice(1);
    return runCommand({
      executable,
      args,
      request,
      operation,
      caseId: safeCaseId(request.case_id),
      timeoutMs: Math.min(MAX_OPERATION_MS, phaseMax),
      killGraceMs: Math.min(1_000, phaseMax),
      maxStdoutBytes: outputLimit(kind),
      maxStderrBytes: outputLimit(kind),
      signal,
    });
  }

  function topology(caseDefinition) {
    const value = topologyById.get(caseDefinition.topology_id);
    if (!value) throw labError('TOPOLOGY_UNDECLARED', 'harness', 'topology');
    return value;
  }

  function stateFor(runtime) {
    let state = cases.get(runtime.identity.case_id);
    if (!state) {
      const suffix = runtime.identity.case_id.replace(/^case-/, '').slice(0, 24);
      state = {
        room_id: 'room-' + suffix,
        publisher_id: 'publisher-' + suffix,
        viewer_id: 'viewer-' + suffix,
        browser: null,
        topology_setup: false,
        attached: false,
        publisher_resource: null,
        viewer_resource: null,
        publisher_write: null,
        viewer_write: null,
        publisher_dangerous: null,
        publisher_media: null,
        viewer_media: null,
        turn: null,
      };
      cases.set(runtime.identity.case_id, state);
    }
    return state;
  }

  function sfuContext(runtime, state) {
    const value = {
      run_id: runtime.identity.run_id,
      case_id: runtime.identity.case_id,
      room_id: state.room_id,
      publisher_id: state.publisher_id,
      viewer_id: state.viewer_id,
    };
    if (state.publisher_resource) value.publisher_session_id = state.publisher_resource.session_id;
    return value;
  }

  function validityDeadline(runtime) {
    const deadlines = manifest.phase_deadlines;
    const scenario = runtime.definition.scenario;
    const budget = scenario.duration_ms + deadlines.connect_ms + deadlines.stable_ms +
      deadlines.transition_ms + deadlines.recovery_ms + deadlines.drain_ms + 60_000;
    return now() + budget;
  }

  async function issueTurn(runtime, state, signal) {
    const request = {
      now_ms: now(),
      required_valid_until_ms: validityDeadline(runtime),
      run_id: runtime.identity.run_id,
      case_id: runtime.identity.case_id,
    };
    return issueTurnCredential(
      (value) => invokeCommand(manifest.turn.credential_provider, value, signal,
        'turn_credential', 'provider'),
      request
    );
  }

  async function issueToken(runtime, state, audience, scope, participantId, signal) {
    const request = {
      now_ms: now(),
      required_valid_until_ms: validityDeadline(runtime),
      run_id: runtime.identity.run_id,
      case_id: runtime.identity.case_id,
      audience,
      subject: 'webrtc-acceptance',
      scope,
      binding: { room_id: state.room_id, participant_id: participantId },
    };
    return issueSfuToken(
      (value) => invokeCommand(manifest.sfu.token_provider, value, signal,
        'sfu_token', 'provider'),
      request
    );
  }

  async function invokeCaseHook(runtime, action, sequence, signal) {
    const declared = topology(runtime.definition);
    const command = declared.hooks[action];
    if (!command) throw labError('TOPOLOGY_HOOK_UNAVAILABLE', 'missing_evidence', action);
    const request = {
      run_id: runtime.identity.run_id,
      case_id: runtime.identity.case_id,
      topology_id: runtime.definition.topology_id,
      action,
      generation: runtime.machine.generation,
      sequence,
      relay_contract_hash: runtime.relay_contract_hash,
    };
    return invokeTopologyHook(
      (value) => invokeCommand(command, value, signal, 'topology_' + action, 'hook'),
      request
    );
  }

  function browserFor(caseDefinition) {
    return seleniumFactory({
      source: manifest.source,
      gridUrl: env[manifest.grid.endpoint_env],
      profile: manifest.profile,
      allowLoopbackHttp: ['diagnostic', 'contract_lab'].includes(manifest.profile),
      commandTimeoutMs: Math.min(MAX_OPERATION_MS, phaseMax),
    });
  }

  function browserConfiguration(runtime, state, role) {
    const participantId = role === 'publisher' ? state.publisher_id : state.viewer_id;
    const token = role === 'publisher' ? state.publisher_media : state.viewer_media;
    const origin = new URL(manifest.sfu.base_url).origin;
    return {
      sfu_origin: origin,
      whip_url: new URL(manifest.sfu.whip_endpoint + '/' + state.room_id + '/' + state.publisher_id, origin).href,
      whep_url: new URL(manifest.sfu.whep_endpoint + '/' + state.room_id + '/' + state.viewer_id, origin).href,
      token: token.token,
      turn: {
        urls: Array.from(state.turn.urls),
        username: state.turn.username,
        credential: state.turn.credential,
      },
      run_id: runtime.identity.run_id,
      case_id: runtime.identity.case_id,
      participant_id: participantId,
    };
  }

  function mediaResource(evidence, kind, state) {
    const api = evidence && evidence.snapshot && evidence.snapshot.api;
    if (!api || typeof api.session_id !== 'string' || !SAFE_ID.test(api.session_id)) {
      throw labError('MEDIA_RESOURCE_EVIDENCE_MISSING', 'missing_evidence', kind);
    }
    return Object.freeze({
      kind,
      room_id: state.room_id,
      participant_id: kind === 'whip' ? state.publisher_id : state.viewer_id,
      session_id: api.session_id,
    });
  }

  function rawBrowserMetrics(evidence) {
    const snapshot = evidence && evidence.snapshot;
    if (!snapshot || !Array.isArray(snapshot.stats)) {
      throw labError('BROWSER_METRICS_MISSING', 'missing_evidence', 'sample');
    }
    const transport = snapshot.stats.find((entry) => entry && entry.type === 'transport' &&
      typeof entry.selectedCandidatePairId === 'string' && entry.selectedCandidatePairId.length > 0);
    const pair = transport && snapshot.stats.find((entry) => entry && entry.type === 'candidate-pair' &&
      entry.id === transport.selectedCandidatePairId && entry.state === 'succeeded');
    const inbound = snapshot.stats.filter((entry) => entry && entry.type === 'inbound-rtp');
    if (!pair || inbound.length === 0) {
      throw labError('BROWSER_METRICS_MISSING', 'missing_evidence', 'sample');
    }

    const rtt = finiteNonNegative(pair.currentRoundTripTime ?? pair.roundTripTime);
    const jitter = inbound.map((entry) => finiteNonNegative(entry.jitter ?? 0));
    const packets = inbound.map((entry) => safeCounter(entry.packetsReceived));
    const lost = inbound.map((entry) => safeCounter(entry.packetsLost ?? 0));
    const nack = inbound.map((entry) => safeCounter(entry.nackCount ?? 0));
    if (rtt === null || jitter.some((value) => value === null) ||
        packets.some((value) => value === null) || lost.some((value) => value === null) ||
        nack.some((value) => value === null)) {
      throw labError('BROWSER_METRICS_INVALID', 'harness', 'sample');
    }
    return {
      rtt_ms: rtt * 1_000,
      jitter_ms: Math.max(...jitter) * 1_000,
      media_packets: packets.reduce((sum, value) => sum + value, 0),
      packets_lost: lost.reduce((sum, value) => sum + value, 0),
      nack_count: nack.reduce((sum, value) => sum + value, 0),
    };
  }

  async function sample(runtime, phase, signal) {
    const state = stateFor(runtime);
    if (!state.browser) throw labError('BROWSER_ROLE_UNAVAILABLE', 'harness', phase);
    const scenario = runtime.definition.scenario;
    const profile = manifest.threshold_profile[phase];
    const minimum = profile && Number.isSafeInteger(profile.minimum_samples) ? profile.minimum_samples : 1;
    const cap = manifest.limits && manifest.limits.max_samples_per_case !== undefined
      ? manifest.limits.max_samples_per_case : LIMITS.MAX_SAMPLES_PER_CASE;
    const desired = Math.min(cap + 1,
      Math.max(minimum + 1, Math.floor(scenario.duration_ms / scenario.sample_interval_ms) + 1));
    const output = [];
    let previous = null;

    for (let index = 0; index < desired; index += 1) {
      if (index > 0) await delay(scenario.sample_interval_ms, undefined, { signal });
      const observed = await state.browser.execute('viewer', 'snapshot', undefined, signal);
      const normalized = normalizeBrowserSnapshot(rawBrowserMetrics(observed), previous, performance.now());
      previous = normalized;
      if (index > 0) output.push(normalized);
    }
    return Object.freeze(output);
  }

  return Object.freeze({
    async preflightArtifacts(_manifest, signal) {
      if (signal && signal.aborted) throw labError('PREFLIGHT_ABORTED', 'harness', 'artifacts');
      const directory = path.resolve(manifest.artifact_directory);
      const target = path.join(directory, RESULT_FILE);
      let probe = null;
      let handle = null;
      try {
        await fs.promises.mkdir(directory, { recursive: true });
        try {
          await fs.promises.access(target);
          throw labError('RUN_ARTIFACT_EXISTS', 'harness', 'artifacts');
        } catch (error) {
          if (error && error.code !== 'ENOENT') throw error;
        }
        probe = path.join(directory,
          '.preflight-' + String(process.pid) + '-' + crypto.randomUUID());
        handle = await fs.promises.open(probe, 'wx', 0o600);
        await handle.writeFile('probe');
        await handle.sync();
        await handle.close();
        handle = null;
        await fs.promises.unlink(probe);
        probe = null;
      } catch (error) {
        if (handle) {
          try { await handle.close(); } catch {}
        }
        if (probe) {
          try { await fs.promises.unlink(probe); } catch {}
        }
        if (error && error.code === 'RUN_ARTIFACT_EXISTS') throw error;
        throw labError('ARTIFACT_DIRECTORY_UNAVAILABLE', 'harness', 'artifacts');
      }
      return Object.freeze({ writable: true });
    },

    async preflightCase(caseDefinition, signal) {
      const browser = browserFor(caseDefinition);
      let primary = null;
      let observed = null;
      try {
        observed = await browser.preflightBrowser({
          browser: caseDefinition.browser,
          relay_contract: caseDefinition.relay_contract,
        }, signal);
        if (manifest.profile === 'release' && (!observed.cors || observed.cors.verified !== true)) {
          throw labError('CORS_EVIDENCE_INCOMPLETE', 'missing_evidence', 'preflightCase');
        }
      } catch (error) {
        primary = error;
      }
      let cleanup = null;
      try {
        cleanup = await browser.closeAll();
      } catch {
        if (!primary) primary = labError('BROWSER_PREFLIGHT_CLEANUP_FAILED', 'harness', 'preflightCase');
      }
      if (!primary && cleanup && cleanup.cleanup_failures.length > 0) {
        primary = labError('BROWSER_PREFLIGHT_CLEANUP_FAILED', 'harness', 'preflightCase');
      }
      if (primary) throw primary;
      return Object.freeze({ available: true });
    },

    async preflightProviders(_manifest, signal) {
      const runtime = {
        identity: { run_id: runIdentity.run_id, case_id: 'preflight-provider' },
        definition: { scenario: { duration_ms: 1_000 } },
      };
      const state = { room_id: 'preflight-room', publisher_id: 'preflight-publisher' };
      await issueToken(runtime, state, 'turbomedia-sfu-control',
        ['sfu.control.write'], state.publisher_id, signal);
      return Object.freeze({ ready: true });
    },

    async preflightTopologies(_manifest, signal) {
      for (let index = 0; index < manifest.topologies.length; index += 1) {
        const declared = manifest.topologies[index];
        if (!declared.hooks.probe) {
          if (manifest.profile === 'release') {
            throw labError('TOPOLOGY_PROBE_UNAVAILABLE', 'missing_evidence', 'preflightTopologies');
          }
          continue;
        }
        const request = {
          run_id: runIdentity.run_id,
          case_id: 'preflight-topology-' + String(index),
          topology_id: declared.topology_id,
          action: 'probe',
          generation: 0,
          sequence: 0,
          relay_contract_hash: hashCanonical(declared.relay_contract),
        };
        await invokeTopologyHook(
          (value) => invokeCommand(declared.hooks.probe, value, signal,
            'topology_probe', 'hook'),
          request
        );
      }
      return Object.freeze({ ready: true });
    },

    async preflightSfu(_manifest, signal) {
      await sfu.preflight(signal);
      sfuBaseline = await sfu.captureBaseline(signal);
      return Object.freeze({ ready: true });
    },

    async preflightTurn(_manifest, signal) {
      const current = now();
      await issueTurnCredential(
        (value) => invokeCommand(manifest.turn.credential_provider, value, signal,
          'turn_preflight', 'provider'),
        {
          now_ms: current,
          required_valid_until_ms: current + 60_000,
          run_id: runIdentity.run_id,
          case_id: 'preflight-turn',
        }
      );
      if (manifest.profile === 'release') {
        throw labError('TURN_REACHABILITY_UNVERIFIED', 'missing_evidence', 'preflightTurn');
      }
      return Object.freeze({ credential_ready: true, reachability_verified: false });
    },

    async prepareCase(runtime, sequence, signal) {
      const state = stateFor(runtime);
      state.turn = await issueTurn(runtime, state, signal);
      state.publisher_write = await issueToken(runtime, state, 'turbomedia-sfu-control',
        ['sfu.control.write'], state.publisher_id, signal);
      state.viewer_write = await issueToken(runtime, state, 'turbomedia-sfu-control',
        ['sfu.control.write'], state.viewer_id, signal);
      state.publisher_dangerous = await issueToken(runtime, state, 'turbomedia-sfu-control',
        ['sfu.control.dangerous'], state.publisher_id, signal);
      state.publisher_media = await issueToken(runtime, state, 'turbomedia-sfu-media',
        ['sfu.media.publish', 'sfu.media.trickle', 'sfu.media.delete'], state.publisher_id, signal);
      state.viewer_media = await issueToken(runtime, state, 'turbomedia-sfu-media',
        ['sfu.media.subscribe', 'sfu.media.trickle', 'sfu.media.delete'], state.viewer_id, signal);
      const receipt = await invokeCaseHook(runtime, 'setup', sequence, signal);
      state.topology_setup = true;
      return Object.freeze({ topology_receipt: receipt });
    },

    async attachRoom(runtime, signal) {
      const state = stateFor(runtime);
      const result = await sfu.attachRoom(sfuContext(runtime, state), state.publisher_write, signal);
      state.attached = true;
      return result;
    },

    async openPublisher(runtime, signal) {
      const state = stateFor(runtime);
      state.browser = browserFor(runtime.definition);
      await state.browser.openPublisher({
        browser: runtime.definition.browser,
        relay_contract: runtime.relay_contract,
      }, signal);
      await state.browser.execute('publisher', 'configure',
        browserConfiguration(runtime, state, 'publisher'), signal);
      const evidence = await state.browser.execute('publisher', 'startPublisher', undefined, signal);
      state.publisher_resource = mediaResource(evidence, 'whip', state);
      return evidence;
    },

    async confirmPublishedTracks(runtime, signal) {
      const state = stateFor(runtime);
      return sfu.waitForPublishedTracks(sfuContext(runtime, state), state.publisher_write,
        ['audio', 'video'], signal);
    },

    async setViewerSubscriptions(runtime, trackIds, signal) {
      const state = stateFor(runtime);
      return sfu.setViewerSubscriptions(sfuContext(runtime, state), state.viewer_write,
        trackIds, signal);
    },

    async openViewer(runtime, signal) {
      const state = stateFor(runtime);
      await state.browser.openViewer({
        browser: runtime.definition.browser,
        relay_contract: runtime.relay_contract,
      }, signal);
      await state.browser.execute('viewer', 'configure',
        browserConfiguration(runtime, state, 'viewer'), signal);
      const evidence = await state.browser.execute('viewer', 'startViewer', undefined, signal);
      state.viewer_resource = mediaResource(evidence, 'whep', state);
      return evidence;
    },

    samplePhase: sample,

    async assertCredentialExpiry() {
      throw labError('CREDENTIAL_EXPIRY_PROBE_UNAVAILABLE',
        'missing_evidence', 'credential_expiry');
    },

    async transitionTopology(runtime, sequence, signal) {
      return invokeCaseHook(runtime, 'transition', sequence, signal);
    },

    async restartIce(runtime, expectedIceGeneration, freshCredentialId, signal) {
      const state = stateFor(runtime);
      if (freshCredentialId !== null && freshCredentialId !== undefined) {
        throw labError('FRESH_CREDENTIAL_RESTART_UNAVAILABLE',
          'missing_evidence', 'recovery');
      }
      const input = { generation: expectedIceGeneration };
      const publisher = await state.browser.execute('publisher', 'restartIce', input, signal);
      const viewer = await state.browser.execute('viewer', 'restartIce', input, signal);
      return Object.freeze({ credential_id: null, publisher, viewer });
    },

    async stopSampling() {
      return Object.freeze({ cleanup_failures: [] });
    },

    async deleteMediaResources(runtime, signal) {
      const state = stateFor(runtime);
      const failures = [];
      if (state.browser) {
        try {
          const receipt = await state.browser.closeAll(signal);
          for (const failure of receipt.cleanup_failures) {
            failures.push(Object.freeze({ code: failure.code }));
          }
        } catch {
          failures.push(Object.freeze({ code: 'BROWSER_CLOSE_FAILED' }));
        }
      }
      if (state.attached) {
        try {
          await sfu.detachRoom(sfuContext(runtime, state), state.publisher_dangerous, signal);
          state.attached = false;
        } catch {
          failures.push(Object.freeze({ code: 'SFU_DETACH_FAILED' }));
        }
      }
      return Object.freeze({ cleanup_failures: Object.freeze(failures) });
    },

    async closeBrowsers() {
      return Object.freeze({ cleanup_failures: [] });
    },

    async waitCaseResourcesZero(_runtime, signal) {
      if (!sfuBaseline) throw labError('SFU_BASELINE_MISSING', 'harness', 'waitCaseResourcesZero');
      const snapshot = await sfu.getNodeStats(undefined, signal);
      if (snapshot.session_count !== sfuBaseline.session_count ||
          snapshot.published_track_count !== sfuBaseline.published_track_count) {
        throw labError('SFU_CASE_RESOURCES_REMAIN', 'harness', 'waitCaseResourcesZero');
      }
      return Object.freeze({ cleanup_failures: [] });
    },

    async waitGlobalBaseline(_runtime, signal) {
      if (!sfuBaseline) throw labError('SFU_BASELINE_MISSING', 'harness', 'waitGlobalBaseline');
      await sfu.waitForBaseline(sfuBaseline, undefined, signal);
      return Object.freeze({ cleanup_failures: [] });
    },

    async waitTurnBaseline() {
      if (manifest.profile === 'release') {
        return Object.freeze({
          cleanup_failures: Object.freeze([{ code: 'TURN_BASELINE_UNVERIFIED' }]),
        });
      }
      return Object.freeze({ cleanup_failures: [] });
    },

    async teardownTopology(runtime, sequence, signal) {
      const state = stateFor(runtime);
      if (!state.topology_setup) return Object.freeze({ cleanup_failures: [] });
      try {
        await invokeCaseHook(runtime, 'teardown', sequence, signal);
        state.topology_setup = false;
        return Object.freeze({ cleanup_failures: [] });
      } catch {
        return Object.freeze({
          cleanup_failures: Object.freeze([{ code: 'TOPOLOGY_TEARDOWN_FAILED' }]),
        });
      }
    },
  });
}

function safeCaseId(value) {
  return typeof value === 'string' && SAFE_ID.test(value) ? value : 'case-unknown';
}

function safeCounter(value) {
  return Number.isSafeInteger(value) && value >= 0 ? value : null;
}

function finiteNonNegative(value) {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : null;
}

function labError(code, category, stage) {
  return Object.assign(new Error(stage + ': ' + code), { code, category, stage });
}

module.exports = { createAcceptanceLab };
