'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { hashCanonical } = require('../src/canonical_json');
const { createFakeLab } = require('./fixtures/fake_lab');
const { CLEANUP_STEPS, runCase, runManifest } = require('../src/controller');

function relayContract(){return {schema_version:1,ip_family:'ipv4',protocol:'udp',relay_protocol:'tcp',remote_candidate_types:['host']};}
function caseDef(id='baseline', workflow={credential_expiry:false,topology_transition:false}){
  return {case_key:`chrome/${id}`,browser:{name:'chrome',version:'127.0.0',platform:'Windows 11'},scenario_id:id,topology_id:'restricted-nat-ipv4',relay_contract:relayContract(),scenario:{scenario_id:id,topology_id:'restricted-nat-ipv4',workflow,sample_interval_ms:250,duration_ms:1000}};
}
function manifest(cases, overrides={}){
  const base={
    schema_version:2,profile:'diagnostic',
    source:{commit:'0'.repeat(40),require_clean_tree:true,test_page_url:'https://acceptance.example.test/client.html',test_page_sha256:'a'.repeat(64)},
    grid:{endpoint_env:'TURBO_MEDIA_ACCEPTANCE_GRID_URL',browsers:[{name:'chrome',version:'127.0.0',platform:'Windows 11'}]},
    sfu:{base_url:'https://sfu.example.test',whip_endpoint:'/whip',whep_endpoint:'/whep',token_provider:{command:['sfu-token-provider','--json']}},
    turn:{credential_provider:{command:['turn-credential-provider','--json']}},
    topologies:[{topology_id:'restricted-nat-ipv4',relay_contract:relayContract(),hooks:{setup:{command:['topology-hook','setup']},transition:{command:['topology-hook','transition']},teardown:{command:['topology-hook','teardown']}}}],
    scenarios:cases.map((entry)=>({...entry.scenario})),
    phase_deadlines:{preflight_ms:40,connect_ms:40,stable_ms:40,transition_ms:40,recovery_ms:40,drain_ms:40},
    threshold_profile:{profile_id:'controller-test',stable:{minimum_samples:2,max_rtt_ms:100,min_media_delta:1},recovery:{minimum_samples:2,max_rtt_ms:100,min_media_delta:1},drain:{maximum_duration_ms:1000}},
    artifact_directory:'artifacts/webrtc-acceptance',
  };
  return {...base,...overrides};
}
function context(fake,m,identity={run_id:'run-test',manifest_hash:'a'.repeat(64),started_at:'2026-09-23T00:00:00.000Z'}){return {lab:fake.lab,manifest:m,run_identity:identity};}
function directContext(fake,m,c,ordinal=0){return {...context(fake,m),case_identity:{run_id:'run-test',case_id:`case-${ordinal}`,case_key:c.case_key,ordinal}};}
function cleanupEvents(fake){return fake.events.filter(e=>CLEANUP_STEPS.includes(e.method)).map(e=>e.method);}
function browserEvidence(role,generation,pairId,localHash,remoteHash,staleEtagStatus=null,relay={verified:true,pairs:[{pair_id:pairId}]}){
  return {snapshot:{role,generation,ice:{generation,local_sha256:localHash,remote_sha256:remoteHash},
    api:{local_media_order:role==='publisher'?['audio','video']:[],stale_etag_status:staleEtagStatus}},relay};
}


test('baseline case passes and always drains in the fixed reverse-resource order', async()=>{
  const fake=createFakeLab(); const c=caseDef(); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'PASS');
  assert.deepEqual(cleanupEvents(fake),CLEANUP_STEPS);
  assert.equal(result.generation,0);
  assert.equal(result.browser_ice_generation,1);
  assert.deepEqual(result.confirmed_track_ids,['publisher-audio','publisher-video-1']);
});

test('transition workflow binds the effective receipt to the explicit relay contract hash', async()=>{
  const fake=createFakeLab(); const c=caseDef('migration',{credential_expiry:true,topology_transition:true}); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'PASS');
  assert.equal(result.generation,1);
  assert.equal(result.browser_ice_generation,2);
  const transition=fake.events.find(e=>e.method==='transitionTopology');
  assert.equal(transition.generation,1);
  const restart=fake.events.find(e=>e.method==='restartIce');
  assert.equal(restart.generation,1);
  assert.equal(restart.extra,2);
  assert.equal(hashCanonical(c.relay_contract).length,64);
});

test('a topology receipt with a different relay contract hash is harness ERROR and still fully drains', async()=>{
  const fake=createFakeLab({behavior:{transitionTopology:(runtime,expectedSequence)=>({
    schema_version:2,effective:true,evidence_id:'wrong-contract',
    topology_id:runtime.definition.topology_id,action:'transition',
    generation:runtime.generation,sequence:expectedSequence,
    relay_contract_hash:'b'.repeat(64),
  })}});
  const c=caseDef('migration',{credential_expiry:false,topology_transition:true}); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'ERROR');
  assert.equal(result.primary.reason.code,'TOPOLOGY_RECEIPT_MISMATCH');
  assert.deepEqual(cleanupEvents(fake),CLEANUP_STEPS);
});

test('mismatched hook sequence is harness ERROR and cannot advance state', async()=>{
  const fake=createFakeLab({behavior:{transitionTopology:(runtime,expectedSequence)=>({
    schema_version:2,effective:true,evidence_id:'wrong-sequence',
    topology_id:runtime.definition.topology_id,action:'transition',
    generation:runtime.generation,sequence:expectedSequence+1,
    relay_contract_hash:runtime.relay_contract_hash,
  })}});
  const c=caseDef('migration',{credential_expiry:false,topology_transition:true}); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'ERROR');
  assert.equal(result.primary.reason.code,'TOPOLOGY_RECEIPT_MISMATCH');
  assert.equal(fake.events.some(e=>e.method==='restartIce'),false);
});

test('missing setup receipt is harness ERROR before room attachment', async()=>{
  const fake=createFakeLab({behavior:{prepareCase:()=>({})}});
  const c=caseDef(); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'ERROR');
  assert.equal(result.primary.reason.code,'TOPOLOGY_RECEIPT_MISSING');
  assert.equal(fake.events.some(e=>e.method==='attachRoom'),false);
  assert.deepEqual(cleanupEvents(fake),CLEANUP_STEPS);
});

test('sampling above the configured cap is harness ERROR', async()=>{
  const fake=createFakeLab();
  const c=caseDef(); const m=manifest([c],{limits:{max_samples_per_case:1}});
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'ERROR');
  assert.equal(result.primary.reason.code,'SAMPLE_LIMIT_EXCEEDED');
});

test('stable threshold violation is FAIL rather than harness ERROR', async()=>{
  const fake=createFakeLab({behavior:{samplePhase:(_runtime,phase)=>phase==='stable'?[{timestamp_ms:0,rtt_ms:200,jitter_ms:2,media_delta:2,loss_ratio:0},{timestamp_ms:250,rtt_ms:200,jitter_ms:2,media_delta:2,loss_ratio:0}]:[]}});
  const c=caseDef(); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'FAIL');
  assert.equal(result.primary.reason.code,'THRESHOLD_FAILED');
});

test('missing relay evidence is INCOMPLETE and never becomes PASS', async()=>{
  const fake=createFakeLab({behavior:{openPublisher:()=>browserEvidence('publisher',1,'publisher-pair-1',
    'a'.repeat(64),'b'.repeat(64),null,{verified:false,code:'RELAY_PAIR_MISSING',category:'missing_evidence'})}});
  const c=caseDef(); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'INCOMPLETE');
  assert.equal(result.primary.reason.code,'RELAY_PAIR_MISSING');
});

test('confirmed track ids are passed unchanged into viewer subscription', async()=>{
  let observed=null;
  const fake=createFakeLab({behavior:{setViewerSubscriptions:(_runtime,trackIds)=>{observed=[...trackIds]; return {subscribed:true};}}});
  const c=caseDef(); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'PASS');
  assert.deepEqual(observed,['publisher-audio','publisher-video-1']);
});

test('restart requires browser ICE generation 2 with changed hashes and selected pairs', async()=>{
  const sameHashes=createFakeLab({behavior:{restartIce:(_runtime,expected)=>({
    publisher:browserEvidence('publisher',expected,'publisher-pair-2','a'.repeat(64),'b'.repeat(64),412),
    viewer:browserEvidence('viewer',expected,'viewer-pair-2','1'.repeat(64),'2'.repeat(64),412),
  })}});
  const c=caseDef('migration',{credential_expiry:false,topology_transition:true}); const m=manifest([c]);
  const unchanged=await runCase(directContext(sameHashes,m,c),c);
  assert.equal(unchanged.outcome,'FAIL');
  assert.equal(unchanged.primary.reason.code,'UNCHANGED_ICE_HASH');

  const samePair=createFakeLab({behavior:{restartIce:(_runtime,expected)=>({
    publisher:browserEvidence('publisher',expected,'publisher-pair-1','e'.repeat(64),'f'.repeat(64),412),
    viewer:browserEvidence('viewer',expected,'viewer-pair-2','1'.repeat(64),'2'.repeat(64),412),
  })}});
  const pair=await runCase(directContext(samePair,m,c),c);
  assert.equal(pair.outcome,'FAIL');
  assert.equal(pair.primary.reason.code,'UNCHANGED_RELAY_PAIR');
});

test('credential expiry without fresh credential evidence is INCOMPLETE', async()=>{
  const fake=createFakeLab({behavior:{assertCredentialExpiry:()=>({old_credential_rejected:true})}});
  const c=caseDef('expiry',{credential_expiry:true,topology_transition:true}); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'INCOMPLETE');
  assert.equal(result.primary.reason.code,'FRESH_CREDENTIAL_EVIDENCE_MISSING');
});

test('old credential unexpectedly succeeding is product FAIL', async()=>{
  const fake=createFakeLab({behavior:{assertCredentialExpiry:()=>({old_credential_rejected:false})}});
  const c=caseDef('expiry',{credential_expiry:true,topology_transition:true}); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'FAIL');
  assert.equal(result.primary.reason.code,'EXPIRED_CREDENTIAL_ACCEPTED');
});

test('transition hook timeout is ERROR, while recovery timeout is FAIL', async()=>{
  const c=caseDef('migration',{credential_expiry:false,topology_transition:true}); const m=manifest([c],{phase_deadlines:{preflight_ms:25,connect_ms:25,stable_ms:25,transition_ms:15,recovery_ms:15,drain_ms:25}});
  const transitionFake=createFakeLab({behavior:{transitionTopology:'hang'}});
  const transition=await runCase(directContext(transitionFake,m,c),c);
  assert.equal(transition.outcome,'ERROR');
  assert.equal(transition.primary.reason.code,'PHASE_TIMEOUT');
  const recoveryFake=createFakeLab({behavior:{restartIce:'hang'}});
  const recovery=await runCase(directContext(recoveryFake,m,c),c);
  assert.equal(recovery.outcome,'FAIL');
  assert.equal(recovery.primary.reason.code,'PHASE_TIMEOUT');
});

test('cancellation latches ERROR but cleanup ignores the cancelled parent and completes all steps', async()=>{
  const fake=createFakeLab({behavior:{openPublisher:'hang'}}); const c=caseDef(); const m=manifest([c]);
  const controller=new AbortController();
  const pending=runCase(directContext(fake,m,c),c,controller.signal);
  setTimeout(()=>controller.abort(),5);
  const result=await pending;
  assert.equal(result.outcome,'ERROR');
  assert.equal(result.primary.reason.code,'CANCELLED');
  assert.deepEqual(cleanupEvents(fake),CLEANUP_STEPS);
});

test('cleanup failures never replace the first primary failure', async()=>{
  const fake=createFakeLab({behavior:{
    assertCredentialExpiry:()=>({old_credential_rejected:false}),
    closeBrowsers:{throw:Object.assign(new Error('external secret'),{code:'BROWSER_CLOSE_FAILED',category:'harness',stage:'close'})},
  }});
  const c=caseDef('expiry',{credential_expiry:true,topology_transition:true}); const m=manifest([c]);
  const result=await runCase(directContext(fake,m,c),c);
  assert.equal(result.outcome,'FAIL');
  assert.equal(result.primary.reason.code,'EXPIRED_CREDENTIAL_ACCEPTED');
  assert.ok(result.cleanup_failures.some(f=>f.step==='closeBrowsers'));
});

test('run preflight provider failure is ERROR before any media resource is created', async()=>{
  const fake=createFakeLab({behavior:{preflightProviders:{throw:Object.assign(new Error('provider-secret'),{code:'PROVIDER_ADAPTER_FAILED',category:'harness',stage:'preflightProviders'})}}});
  const c=caseDef(); const m=manifest([c]);
  const result=await runManifest(context(fake,m),m);
  assert.equal(result.outcome,'ERROR');
  assert.equal(result.primary.reason.code,'PROVIDER_ADAPTER_FAILED');
  assert.equal(result.cases.length,0);
  assert.equal(fake.events.some(e=>['prepareCase','attachRoom','openPublisher','openViewer'].includes(e.method)),false);
});

test('run preflight missing browser returns INCOMPLETE before any media resource is created', async()=>{
  const fake=createFakeLab({behavior:{preflightCase:()=>({available:false})}}); const c=caseDef(); const m=manifest([c]);
  const result=await runManifest(context(fake,m),m);
  assert.equal(result.outcome,'INCOMPLETE');
  assert.equal(result.cases.length,0);
  assert.equal(fake.events.some(e=>['prepareCase','attachRoom','openPublisher','openViewer'].includes(e.method)),false);
});

test('runManifest continues later cases after one case FAIL and aggregates without hiding it', async()=>{
  let expiryCalls=0;
  const fake=createFakeLab({behavior:{assertCredentialExpiry:()=>({
    old_credential_rejected:++expiryCalls>1,
    fresh_credential_id:expiryCalls>1?'turn-credential-2':undefined,
  })}});
  const cases=[caseDef('first',{credential_expiry:true,topology_transition:true}),caseDef('second',{credential_expiry:true,topology_transition:true})];
  const m=manifest(cases); const result=await runManifest(context(fake,m),m);
  assert.equal(result.outcome,'FAIL');
  assert.deepEqual(result.cases.map(c=>c.outcome),['FAIL','PASS']);
  assert.equal(result.cases.length,2);
});
