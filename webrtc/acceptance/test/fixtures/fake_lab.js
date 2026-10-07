'use strict';

function createFakeLab(options = {}) {
  const events = [];
  const cleanupMethods = new Set([
    'stopSampling', 'deleteMediaResources', 'closeBrowsers', 'waitCaseResourcesZero',
    'waitGlobalBaseline', 'waitTurnBaseline', 'teardownTopology',
  ]);

  function record(method, runtime, extra) {
    events.push(Object.freeze({
      method,
      case_id: runtime && runtime.identity ? runtime.identity.case_id : null,
      ordinal: runtime && runtime.identity ? runtime.identity.ordinal : null,
      generation: runtime && Number.isInteger(runtime.generation) ? runtime.generation : null,
      extra: extra || null,
    }));
  }

  async function invoke(method, args, fallback) {
    const runtime = args[0] && args[0].identity ? args[0] : null;
    const extra = Array.isArray(args[1]) ? Object.freeze([...args[1]]) :
      (typeof args[1] === 'string' || typeof args[1] === 'number' ? args[1] : null);
    record(method, runtime, extra);
    const behavior = options.behavior && options.behavior[method];
    if (behavior === 'hang') return new Promise(() => {});
    if (typeof behavior === 'function') return behavior(...args);
    if (behavior && behavior.throw) throw behavior.throw;
    return typeof fallback === 'function' ? fallback(...args) : fallback;
  }

  const browserEvidence = (role, generation, pairId, localHash, remoteHash, staleEtagStatus = null) => ({
    snapshot: {
      role,
      generation,
      ice: { generation, local_sha256: localHash, remote_sha256: remoteHash },
      api: {
        local_media_order: role === 'publisher' ? ['audio', 'video'] : [],
        stale_etag_status: staleEtagStatus,
      },
    },
    relay: { verified: true, pairs: [{ pair_id: pairId }] },
  });
  const sample = (phase) => phase === 'recovery'
    ? [
      { timestamp_ms: 500, rtt_ms: 15, jitter_ms: 2, media_delta: 2, loss_ratio: 0 },
      { timestamp_ms: 750, rtt_ms: 14, jitter_ms: 2, media_delta: 2, loss_ratio: 0 },
    ]
    : [
      { timestamp_ms: 0, rtt_ms: 12, jitter_ms: 2, media_delta: 2, loss_ratio: 0 },
      { timestamp_ms: 250, rtt_ms: 11, jitter_ms: 2, media_delta: 2, loss_ratio: 0 },
    ];

  const lab = {
    preflightArtifacts: (...args) => invoke('preflightArtifacts', args, { writable: true }),
    preflightProviders: (...args) => invoke('preflightProviders', args, { ready: true }),
    preflightTopologies: (...args) => invoke('preflightTopologies', args, { ready: true }),
    preflightSfu: (...args) => invoke('preflightSfu', args, { ready: true }),
    preflightTurn: (...args) => invoke('preflightTurn', args, { reachable: true }),
    preflightCase: (...args) => invoke('preflightCase', args, { available: true }),
    prepareCase: (...args) => invoke('prepareCase', args, (runtime, expectedSequence) => ({
      topology_receipt: {
        schema_version: 2,
        evidence_id: `setup-${runtime.identity.case_id}`,
        topology_id: runtime.definition.topology_id,
        action: 'setup',
        generation: 0,
        sequence: expectedSequence,
        effective: true,
        relay_contract_hash: runtime.relay_contract_hash,
      },
    })),
    attachRoom: (...args) => invoke('attachRoom', args, { attached: true }),
    openPublisher: (...args) => invoke('openPublisher', args,
      browserEvidence('publisher', 1, 'publisher-pair-1', 'a'.repeat(64), 'b'.repeat(64))),
    confirmPublishedTracks: (...args) => invoke('confirmPublishedTracks', args,
      ['publisher-audio', 'publisher-video-1']),
    setViewerSubscriptions: (...args) => invoke('setViewerSubscriptions', args, { subscribed: true }),
    openViewer: (...args) => invoke('openViewer', args,
      browserEvidence('viewer', 1, 'viewer-pair-1', 'c'.repeat(64), 'd'.repeat(64))),
    samplePhase: (...args) => invoke('samplePhase', args, (_runtime, phase) => sample(phase)),
    assertCredentialExpiry: (...args) => invoke('assertCredentialExpiry', args, { old_credential_rejected: true, fresh_credential_id: 'turn-credential-2' }),
    transitionTopology: (...args) => invoke('transitionTopology', args, (runtime, expectedSequence) => ({
      schema_version: 2,
      evidence_id: `transition-${runtime.identity.case_id}`,
      topology_id: runtime.definition.topology_id,
      action: 'transition',
      generation: runtime.generation,
      sequence: expectedSequence,
      effective: true,
      relay_contract_hash: runtime.relay_contract_hash,
    })),
    restartIce: (...args) => invoke('restartIce', args, (_runtime, expectedIceGeneration, freshCredentialId) => ({
      credential_id: freshCredentialId,
      publisher: browserEvidence('publisher', expectedIceGeneration, `publisher-pair-${expectedIceGeneration}`,
        'e'.repeat(64), 'f'.repeat(64), 412),
      viewer: browserEvidence('viewer', expectedIceGeneration, `viewer-pair-${expectedIceGeneration}`,
        '1'.repeat(64), '2'.repeat(64), 412),
    })),
    stopSampling: (...args) => invoke('stopSampling', args, { cleanup_failures: [] }),
    deleteMediaResources: (...args) => invoke('deleteMediaResources', args, { cleanup_failures: [] }),
    closeBrowsers: (...args) => invoke('closeBrowsers', args, { cleanup_failures: [] }),
    waitCaseResourcesZero: (...args) => invoke('waitCaseResourcesZero', args, { cleanup_failures: [] }),
    waitGlobalBaseline: (...args) => invoke('waitGlobalBaseline', args, { cleanup_failures: [] }),
    waitTurnBaseline: (...args) => invoke('waitTurnBaseline', args, { cleanup_failures: [] }),
    teardownTopology: (...args) => invoke('teardownTopology', args, { cleanup_failures: [] }),
  };

  return Object.freeze({
    lab: Object.freeze(lab),
    events,
    cleanupMethods,
    sample,
  });
}

module.exports = { createFakeLab };
