# PR #6 Spanner gRPC performance validation review 2026-05-20

## Scope

- PR #6 / `main..issue5-bdp-ping-settings`
- `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md`
- `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md`
- `docs/reviews/issues/2026-05-19-active-bdp-probe-implementation-review-grpc-http2-transport-domain-expert.md`
- `docs/reviews/issues/2026-05-19-bdp-estimator-domain-review-transport-domain-expert.md`
- `ext/grpc/main.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/029-trace-file.phpt`

## Reviewer Role

- Google Cloud Spanner gRPC performance specialist

## Review Prompt Summary

- Review whether the investigation and validation behind PR #6 is sound for Cloud Spanner latency diagnosis, with emphasis on real Spanner `SELECT 1`, SA JSON vs ADC interpretation, ext-grpc 1.58 comparison, BDP PING hypotheses, server-side/GFE inference, variance/sample size, and whether issue communication asks for the right next data.
- Focus primarily on validation and inference quality, not production code style.

## Issues

### REVIEW-20260520-001: Default-on active BDP probe is not supported by the validation record

- Severity: `Blocker`
- Status: `Open`
- Reviewer role: `Google Cloud Spanner gRPC performance specialist`
- Type: `validation/inference issue with code consequence`
- Finding: The branch enables `grpc_lite.active_bdp_probe` by default, but the issue's own validation says default-on is dangerous outside the real Spanner `SELECT 1` condition. For a Spanner performance fix, the evidence supports an opt-in diagnostic or narrowly scoped experiment, not a global default behavior.
- Evidence: `ext/grpc/main.c` adds `grpc_lite.active_bdp_probe` default `1`; `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` records real Spanner improvement for SA JSON `SELECT 1`, but also records clear p99/p50 regressions in `spanner-shape`, `tls-spanner-shape`, `spanner-real-client`, and concludes that default-on is dangerous and opt-in is safer; `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md` keeps SETTINGS update default off because it does not improve real Spanner.
- Expected model: A Spanner-specific latency mitigation should be promoted to default only after it is shown to improve the target workload without unacceptable regression in representative non-target workloads, or after a defensible gating policy exists. Otherwise it should remain explicit opt-in with documentation of when to enable it.
- Why it matters: This PR would change all users' HTTP/2 control behavior based on one real Spanner SA JSON `SELECT 1` scenario while already-observed synthetic/emulator regressions indicate broader tail-latency risk. That is not a sound production default decision.
- Recommended fix: Make `grpc_lite.active_bdp_probe` default `0` for this PR, document it as opt-in for issue #5 investigation, and ask the reporter to validate `active_bdp_probe=0/1` in their failing environment. If default-on is still desired later, open a separate issue with a default policy, target workload, guardrails, and before/after acceptance criteria.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: `This is the only PR-blocking finding from the Spanner validation perspective.`

### REVIEW-20260520-002: BDP/GFE causal inference is overstated relative to the evidence

- Severity: `High`
- Status: `Open`
- Reviewer role: `Google Cloud Spanner gRPC performance specialist`
- Type: `validation/inference issue`
- Finding: The investigation has strong evidence that a DATA-callback client-origin PING changes real Spanner `SELECT 1` latency in this environment, but it does not establish that the mechanism is BDP estimation, flow-control update, or Google Frontend behavior. In particular, official ext-grpc remains fast with `grpc.http2.bdp_probe=0`, and the PR's SETTINGS-after-ACK experiment does not add improvement.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` records `official ext-grpc grpc.http2.bdp_probe=0` as still fast, while active DATA Ping improves grpc-lite but leaves a remaining p50 gap; `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md` records active PING + SETTINGS update as p50/mean equivalent to active PING only and p99 worse.
- Expected model: The conclusion should be phrased as an observed transport-control interaction: "queueing a client-origin PING from response DATA improves grpc-lite real Spanner SA JSON `SELECT 1` in this environment." Claims about BDP estimator, GFE/server scheduling, or flow-control SETTINGS should be explicitly marked as unproven hypotheses unless backed by direct wire/control evidence.
- Why it matters: Mislabeling the mechanism as BDP can send the next investigation toward flow-control/window tuning even though the measured SETTINGS variant was negative and official's BDP-disable result weakens BDP as a sole cause. This can waste iteration and create unjustified production behavior.
- Recommended fix: Update the PR/issue summary to separate facts from hypotheses: facts are response-arrival gap, active DATA PING improvement, metadata variants mostly negative, SETTINGS negative; hypotheses are C-core scheduler/control lifecycle, Spanner frontend/GFE response scheduling, and BDP-related control state. For GitHub issue communication, avoid saying the root cause is BDP; say the active PING experiment narrows the search and request reporter-side confirmation.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: `The implementation name may remain if treated as compatibility with gRPC terminology, but the validation text should not imply full BDP estimator behavior.`

### REVIEW-20260520-003: SA JSON vs ADC interpretation is directionally sound but not isolated enough

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `Google Cloud Spanner gRPC performance specialist`
- Type: `validation/inference issue`
- Finding: The ADC/SA JSON matrix is a good diagnostic step and supports "PHP credential CPU is not the direct cause," but it does not isolate credential type from Spanner server-side auth/routing behavior. The current wording sometimes treats credential path as only a request/header-state modifier.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` shows official SA JSON fast, official ADC slow, lite SA JSON and lite ADC similar, then reasonably moves toward transport state. However SA JSON/JWT versus ADC also changes auth token type, identity/claims shape, and Spanner/GFE auth processing path, not just HPACK/header size and `cred-type/jwt`.
- Expected model: For Spanner diagnosis, credential mode is both a client-side request-shape variable and a server-visible auth semantic variable. It should be handled as an effect modifier until token/auth semantics are controlled or measured.
- Why it matters: If the Spanner frontend has different fast paths or caching for JWT service-account auth versus ADC bearer tokens, transport experiments alone may appear to explain a server-side auth/cache effect. Conversely, transport-control behavior may only matter under one auth path.
- Recommended fix: Reword the issue to say that SA JSON/JWT is the reproduction condition and effect modifier, not merely a header-size cause. Ask the reporter for four-way data under their environment: official/lite × SA JSON/ADC, with active probe off/on for lite, and with the same warmed channel/session procedure.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: `This does not invalidate the current data; it narrows what can be inferred from it.`

### REVIEW-20260520-004: p99 and tail claims need stronger variance handling

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `Google Cloud Spanner gRPC performance specialist`
- Type: `validation/inference issue`
- Finding: The p50/mean improvement for active DATA PING is credible, but p99/tail conclusions are weak because many tables use 200 or 500 iterations without confidence intervals, interleaving details, or raw distribution artifacts. A p99 from 500 samples is effectively determined by a handful of observations.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` contains several 200/500 iteration tables with volatile p99 values, including active PING variants that improve p50 while sometimes worsening p99; the Pub/Sub and Secret Manager cross-checks explicitly show large outliers and bimodality. `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md` concludes p99 worsened for SETTINGS update from 500 iterations.
- Expected model: For remote Cloud Spanner latency, p50 and p90 can be used for directional diagnosis with hundreds of samples, but p99/tail claims should be treated as tentative unless backed by larger sample sizes, repeated interleaved runs, bootstrap confidence intervals, or raw quantile/histogram output.
- Why it matters: Tail behavior is central to deciding whether active PING is safe. Over-reading p99 from small runs can either reject useful behavior or promote risky behavior incorrectly.
- Recommended fix: In the issue and PR description, mark p99 observations as preliminary unless supported by repeated interleaved runs. For reporter follow-up, request raw per-iteration elapsed data or at least repeated runs with fixed iterations, p50/p90/p95/p99, min/max, and run order. Keep acceptance focused on stable p50/p90 first, then separately validate tail safety.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: `No code change required.`

### REVIEW-20260520-005: Reporter follow-up should request exact reproducer controls, not just result confirmation

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `Google Cloud Spanner gRPC performance specialist`
- Type: `validation/inference issue`
- Finding: The issue communication needs to ask for data that can distinguish environment, Spanner session/channel state, auth mode, and control-frame behavior. The docs identify many unresolved variables, but the next external request should be more prescriptive.
- Evidence: The docs have identified that the gap sits between request write and first response, that Pub/Sub/Secret Manager do not reproduce it, that Spanner `SELECT 1` does, and that active DATA PING helps. Remaining unresolved variables include channel/session warm state, connection generation, Spanner frontend IP, SA JSON/ADC credential mode, official BDP option, and grpc-lite active probe settings.
- Expected model: External issue communication should collect a minimal matrix that can be compared directly to local results, while protecting credentials and avoiding raw authorization traces.
- Why it matters: Without a precise request, another issue round-trip may only confirm "it is faster/slower" and still not identify whether the remaining difference is Spanner data plane, auth path, connection state, or grpc-lite control behavior.
- Recommended fix: Ask the reporter for a single minimized `SELECT 1` matrix: official ext-grpc 1.58, official with `grpc.http2.bdp_probe=0`, grpc-lite with `active_bdp_probe=0`, grpc-lite with `active_bdp_probe=1`, and if possible grpc-lite with `active_bdp_probe_min_interval_ms=1000`; run under SA JSON and ADC if both are available; keep the same warmed Spanner session/channel lifecycle; report per-run p50/p90/p99/min/max and sample count; include redacted grpc-lite trace markers for request TLS write, first TLS read, outbound PING, inbound PING ACK, and server PING timing.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: `This is a process/documentation issue, not a production code issue.`

### REVIEW-20260520-006: Cross-service checks are useful but should not be used as strong proof of Spanner specificity

- Severity: `Low`
- Status: `Open`
- Reviewer role: `Google Cloud Spanner gRPC performance specialist`
- Type: `validation/inference issue`
- Finding: Pub/Sub and Secret Manager cross-checks are useful negative controls, but the docs should keep them as weak evidence only. They differ from Spanner in service frontend, method semantics, auth handling, request routing, and latency distribution.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` records Pub/Sub `ListTopics`/`GetTopic` with large variance and Secret Manager `GetSecret` as stable and equal. The docs already mostly treat these as cross-checks rather than performance evidence.
- Expected model: Cross-service tests can rule out "grpc-lite is always slower for Google APIs," but they cannot prove a Spanner-specific server mechanism or validate a Spanner-specific fix.
- Why it matters: Over-weighting cross-service negatives can hide a Spanner-only interaction; under-weighting them misses useful evidence that the bug is not a universal gRPC transport issue.
- Recommended fix: Leave the cross-service sections, but phrase the conclusion narrowly: "no evidence of a general Google API gRPC regression in the sampled services; the remaining issue appears triggered by Spanner `ExecuteStreamingSql`/session/auth/control conditions."
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: `Current wording is close; this is a clarity adjustment.`

## Review Result

- Blocker: `1`
- High: `1`
- Medium: `3`
- Low: `1`
- Design Decision: `none`


## Resolution Update 2026-05-20

- REVIEW-20260520-001 is addressed by changing `grpc_lite.active_bdp_probe` default to `0` and documenting active PING as a Cloud Spanner issue #5 opt-in diagnostic path, not a global default behavior.
- REVIEW-20260520-002 is partially addressed by rewording the issue docs so BDP/GFE/server scheduling remains a hypothesis rather than a proven root cause.
- REVIEW-20260520-003/004/005/006 remain open validation-quality items for the next reporter/local matrix pass.
- Verification: `./tools/test/check-phpt.sh` PASS 17/17, `./tools/test/check-c-unit.sh` PASS, `./tools/test/check-c-static-analysis.sh` PASS.
