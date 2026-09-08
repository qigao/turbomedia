# WebRTC Acceptance Report

- Run ID: `run-20260825-001`
- Outcome: `FAIL`
- Started: `2026-08-25T08:00:00Z`
- Finished: `2026-08-25T08:00:09Z`

## Counts

| Total | PASS | FAIL | INCOMPLETE | ERROR |
| ---: | ---: | ---: | ---: | ---: |
| 2 | 1 | 1 | 0 | 0 |

## Cases

| Case | Outcome | Requested browser | Observed browser | Candidate types | Selected pair | Phase timestamps | Threshold inputs/results | Stale events | Primary evidence | Cleanup evidence | Artifact hashes |
| --- | --- | --- | --- | --- | --- | --- | --- | ---: | --- | --- | --- |
| case-001 | FAIL | {"browserName":"firefox","platformName":"linux"} | {"browserName":"firefox","browserVersion":"128.0"} | {"publisher_local":["relay"],"viewer_local":["relay"]} | {"local_candidate_type":"relay","protocol":"udp","remote_candidate_type":"relay"} | {"connecting_at":"2026-08-25T08:00:02Z","draining_at":"2026-08-25T08:00:08Z","stable_at":"2026-08-25T08:00:04Z"} | {"inputs":{"max_rtt_ms":40,"minimum_samples":2},"results":{"errors":[],"failures":[{"metric":"rtt_ms","statistic":"p95","threshold":40,"value":41}],"incomplete":[],"outcome":"FAIL","summaries":{"rtt_ms":{"max":41,"min":20,"missing_count":0,"p50":20,"p95":41,"p99":41,"sample_count":2}}}} | 2 | {"code":"THRESHOLD_EXCEEDED","message":"raw=[REDACTED]; json=[REDACTED]; url=[REDACTED]"} | [{"outcome":"PASS","step":"turn-allocation-check"}] | [{"path":"cases/case-001.json","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size_bytes":512}] |
| case-002 | PASS | {"browserName":"firefox","platformName":"linux"} | {"browserName":"firefox","browserVersion":"128.0"} | {"publisher_local":["relay"],"viewer_local":["relay"]} | {"local_candidate_type":"relay","protocol":"udp","remote_candidate_type":"relay"} | {"connecting_at":"2026-08-25T08:00:02Z","draining_at":"2026-08-25T08:00:08Z","stable_at":"2026-08-25T08:00:04Z"} | {"inputs":{"max_rtt_ms":40,"minimum_samples":2},"results":{"errors":[],"failures":[],"incomplete":[],"outcome":"PASS","summaries":{"rtt_ms":{"max":22,"min":18,"missing_count":0,"p50":18,"p95":22,"p99":22,"sample_count":2}}}} | 0 | null | [{"outcome":"PASS","step":"turn-allocation-check"}] | [{"path":"cases/case-002.json","sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","size_bytes":384}] |
