# PR #6 gRPC protocol specialist review 2026-05-20

## Scope

- `main..issue5-bdp-ping-settings`
- `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md`
- `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md`
- `docs/reviews/issues/2026-05-19-active-bdp-probe-implementation-review-grpc-http2-transport-domain-expert.md`
- `docs/reviews/issues/2026-05-19-bdp-estimator-domain-review-transport-domain-expert.md`
- `ext/grpc/internal.h`
- `ext/grpc/main.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/002-ini.phpt`
- `ext/grpc/tests/029-trace-file.phpt`

## Reviewer Role

- `gRPC protocol specialist`

## Review Prompt Summary

- Review PR #6 / `issue5-bdp-ping-settings` primarily for whether the investigation and validation are technically sound, not just whether the code compiles.
- Focus on active BDP probe semantics, HTTP/2 PING usage, interaction with gRPC calls/deadlines/keepalive, metadata/control-frame assumptions, and whether conclusions about ext-grpc / gRPC Core behavior are justified.

## Issues

### REVIEW-20260520-001: default-on DATA-triggered PING is not justified by the PR's own validation

- Severity: `Blocker`
- Status: `Open`
- Reviewer role: `gRPC protocol specialist`
- Issue type: `code issue + validation/inference issue`
- Finding: The PR makes `grpc_lite.active_bdp_probe=1` and `grpc_lite.active_bdp_probe_min_interval_ms=0` the default, but the issue document records clear regressions in low-RTT synthetic, TLS synthetic, and emulator/high-level real-client suites. A gRPC transport must not add connection-level PING traffic by default when its benefit is currently shown only for one Cloud Spanner real endpoint condition and its downside is already demonstrated in other supported workloads.
- Evidence: `ext/grpc/main.c` adds default `grpc_lite.active_bdp_probe=1` and `grpc_lite.active_bdp_probe_min_interval_ms=0`. `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` records `active BDP probe default on` p99 regressions for `spanner-shape`, `tls-spanner-shape`, `spanner-real-client`, `rtt-unary`, and explicitly states `default onのまま進めるのは危険`. The same document later keeps `active_bdp_update_settings` default off, but does not revert active PING default.
- Expected model: HTTP/2 PING is connection-level control traffic. A gRPC client can use it for keepalive or transport probing, but default behavior must be broadly safe across peers, RTTs, and streaming shapes. If validation shows workload-specific benefit plus broad regressions, the protocol-safe default is opt-in or a conservative bounded policy, not unbounded default-on.
- Why it matters: Default-on PING changes wire behavior for every gRPC call using this extension. It can increase tail latency, create extra control-frame load, interact with proxy/server ping enforcement, and make grpc-lite behavior non-neutral for non-Spanner or low-RTT environments. This is a merge blocker because the PR's code default contradicts the PR's recorded validation.
- Recommended fix: Change `grpc_lite.active_bdp_probe` default to `0` before merging, or introduce a narrowly justified auto policy with evidence covering low-RTT, TLS, server streaming, unary, emulator, and real Cloud Spanner. If kept as a Spanner investigation aid, document it as opt-in diagnostic/experimental transport behavior rather than default gRPC behavior.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Re-run at least `spanner-shape`, `tls-spanner-shape`, and real Cloud Spanner `SELECT 1` with default config, plus an opt-in run showing the Spanner improvement remains available when explicitly enabled.
- Notes: This finding is about merge safety, not about whether the active PING experiment is useful.

### REVIEW-20260520-002: ACK-rearmed min-interval-0 behavior is materially different from gRPC Core's BDP estimator

- Severity: `High`
- Status: `Open`
- Reviewer role: `gRPC protocol specialist`
- Issue type: `code issue + validation/inference issue`
- Finding: The implemented behavior is a DATA-triggered client PING with one outstanding probe and immediate re-arm on matching ACK. gRPC Core's BDP mechanism is an estimator: it accumulates incoming bytes, schedules a ping, records when the ping is on the wire, completes the sample on ACK, computes BDP/bandwidth, schedules the next probe with an adaptive delay, and runs flow-control updates. The PR sometimes distinguishes this, but the default behavior and naming still imply a BDP/Core-like mechanism while omitting the estimator and adaptive re-arm semantics.
- Evidence: `ext/grpc/transport.c` queues PING from `on_data_chunk_recv_callback()` and clears outstanding on matching ACK; it does not accumulate bytes, compute BDP, apply adaptive inter-ping delay, or use ACK completion to update flow-control. Local gRPC Core reference `_research/grpc/src/core/lib/transport/bdp_estimator.{h,cc}` and `_research/grpc/src/core/ext/transport/chttp2/transport/{parsing.cc,chttp2_transport.cc,flow_control.cc}` show `AddIncomingBytes`, `SchedulePing`, `StartPing`, `CompletePing`, next ping timer, and `PeriodicUpdate()`. `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` acknowledges this is not a complete estimator, but then selects `min_interval_ms=0` as most effective and defaults it on.
- Expected model: If the feature is called BDP, its state machine should match the BDP sampling lifecycle closely enough to preserve the protocol invariant: bounded probes based on sampled bytes/time, not one PING per response-DATA/ACK cycle. If the implementation intentionally does not do that, it should be modeled and named as a Spanner-oriented active PING experiment and kept opt-in.
- Why it matters: The validation cannot be used as evidence that grpc-lite now has a gRPC Core-like BDP estimator. It proves that one class of DATA-triggered PING changes Spanner latency. Those are different claims with different production risks.
- Recommended fix: Either implement a real bounded estimator policy before production enablement, or rename/document this as `active_response_data_ping_probe`-style behavior and keep it off by default. The issue docs should avoid saying this models gRPC Core BDP except where a specific sub-behavior is identical.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Review docs and INI names/defaults after the fix; if an estimator is implemented, add protocol tests for byte accumulator, ACK completion, next probe delay, and flow-control update separation.
- Notes: The current one-outstanding opaque matching is a correct subcomponent, but not sufficient to claim BDP estimator semantics.

### REVIEW-20260520-003: ext-grpc/Core causality is not established by the current measurements

- Severity: `High`
- Status: `Open`
- Reviewer role: `gRPC protocol specialist`
- Issue type: `validation/inference issue`
- Finding: The issue documents support the narrower conclusion that "DATA-callback PING improves grpc-lite on real Spanner SELECT 1". They do not establish that ext-grpc's faster behavior is caused by gRPC Core BDP probing. In fact, the document records `official ext-grpc grpc.http2.bdp_probe=0` as still fast. That result directly weakens a causal explanation based on Core BDP alone.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` records `official ext-grpc grpc.http2.bdp_probe=0 | mean 11.9ms / p50 11.1ms | BDP probe offでもofficial SA JSONは速い`. Later it records grpc-lite DATA Ping improvement from roughly `p50 21.485ms` to `14.706ms`, still slower than official `10.387ms`. It also records remaining unexplained differences: official request write size, TLS/write primitive, C-core scheduler/control lifecycle, and Spanner frontend behavior.
- Expected model: A validation chain should distinguish symptom reproduction from root-cause attribution. For gRPC/Core claims, the evidence should show either that disabling the relevant Core behavior removes the official advantage, or that grpc-lite reproduces a specific Core frame/control sequence and gains the corresponding effect.
- Why it matters: If the PR is merged under the belief that it implements the missing ext-grpc/Core behavior, future work may stop prematurely. The real delta may still be in C-core scheduler, write batching, HPACK/TLS record shape, Spanner frontend handling, or another control-frame lifecycle.
- Recommended fix: Reframe the conclusion as: "DATA-triggered active PING is an empirically useful grpc-lite variant for Spanner SELECT 1, but not proven to be the cause of ext-grpc's advantage." Keep the remaining root-cause investigation open. If Core causality is still desired, add same-run official `grpc.http2.bdp_probe=0/1` traces and correlate BDP PING timing, SETTINGS/WINDOW_UPDATE, request write, and first response for the same minimal SELECT 1 workload.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Updated issue docs should separate "observed improvement" from "root cause", and should explicitly mention that official remains fast with BDP probe disabled.
- Notes: This is not an argument against the experiment; it is an argument against overclaiming the result.

### REVIEW-20260520-004: gRPC keepalive and server ping enforcement risk is not validated

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `gRPC protocol specialist`
- Issue type: `validation/inference issue`
- Finding: The PR correctly separates server-origin PING handling from client-origin active probe ACK matching, but it does not validate the broader gRPC operational rule that excessive client PINGs can be rejected by servers/proxies. With `min_interval_ms=0`, a response-heavy stream can send repeated client-origin PINGs at approximately ACK cadence. That is not keepalive, but many gRPC deployments enforce ping frequency regardless of the application label.
- Evidence: `ext/grpc/transport.c` submits a new probe whenever response DATA arrives and no probe is outstanding; ACK completion immediately allows another probe when min interval is 0. gRPC Core local tests under `_research/grpc/test/core/transport/chttp2/too_many_pings_test.cc` indicate ping frequency enforcement is a real protocol/operational concern in the gRPC ecosystem. The PR validation focuses on Cloud Spanner and local benchmark suites, not on a server/proxy that enforces too-many-pings behavior.
- Expected model: Connection-level PING probes should have a bounded policy that is safe under long-lived server streaming and under peers that enforce ping limits. A gRPC implementation should document whether the PING is keepalive, BDP probe, or diagnostic probe, and ensure it does not accidentally violate common server policies.
- Why it matters: A feature that improves one Spanner workload can still cause GOAWAY / ENHANCE_YOUR_CALM / transport closure in other environments if it increases client-origin PING frequency. This is especially important for a drop-in gRPC client library.
- Recommended fix: Before default enablement, add a conservative minimum interval or one-shot policy, and add a fixture/test that simulates a peer rejecting too-frequent PINGs. If the feature remains opt-in, document that it may increase client-origin PING traffic and should be enabled only after workload validation.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Protocol test or integration fixture showing bounded PING behavior and correct handling of peer GOAWAY/RST/close under excessive ping policy.
- Notes: This can drop to Low if the feature is default-off and documented as opt-in experimental behavior.

### REVIEW-20260520-005: SETTINGS update validation is a negative experiment, not evidence for BDP flow-control parity

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `gRPC protocol specialist`
- Issue type: `validation/inference issue`
- Finding: The SETTINGS portion is implemented as a static target update after a matching PING ACK, but gRPC Core's BDP flow-control update derives target values from measured bytes, RTT/bandwidth, and memory pressure, then emits flow-control actions. The current validation correctly shows no Spanner improvement, but the PR should not treat this as validating or implementing the Core BDP SETTINGS path.
- Evidence: `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md` says no incoming-byte accumulator or RTT estimator is implemented and classifies the feature as an opt-in experiment. `ext/grpc/transport.c` sends `SETTINGS_INITIAL_WINDOW_SIZE` and optional `SETTINGS_MAX_FRAME_SIZE` only if configured targets exceed last submitted values. Local Core reference `_research/grpc/src/core/ext/transport/chttp2/transport/flow_control.cc` computes target window/frame size from BDP estimate and memory pressure in `PeriodicUpdate()`.
- Expected model: BDP-driven SETTINGS updates are the output of an estimator and flow-control policy. Static SETTINGS after ACK is a control-frame experiment. Its negative result is useful, but it does not prove the Core SETTINGS path is irrelevant unless the same preconditions and target values are shown to match.
- Why it matters: The investigation could incorrectly close off adaptive flow-control as a candidate. The current experiment only says "this static SETTINGS update did not help real Spanner SELECT 1."
- Recommended fix: Keep `active_bdp_update_settings` default off and document it as a negative static-SETTINGS experiment. Do not merge it as part of a claimed BDP implementation unless either the estimator inputs are implemented or the issue explicitly states this is not Core parity.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Docs should preserve the negative result but avoid treating it as evidence that adaptive Core flow-control behavior is irrelevant.
- Notes: Because the option is default-off, this is not a merge blocker if the docs are precise.

### REVIEW-20260520-006: trace regression confirms opaque matching but not the performance-critical flush timing

- Severity: `Low`
- Status: `Open`
- Reviewer role: `gRPC protocol specialist`
- Issue type: `validation/inference issue`
- Finding: `029-trace-file.phpt` confirms that an outbound PING and a matching inbound ACK payload exist, and the SETTINGS test confirms a queued SETTINGS frame in the opt-in case. It does not assert the timing invariant that matters for the investigation: DATA callback queues PING, the PING is flushed promptly by the existing nonblocking send path, and RPC completion/deadline behavior does not wait on the ACK.
- Evidence: `ext/grpc/tests/029-trace-file.phpt` checks PING payload matching and SETTINGS frame presence. The docs state submit time, wire write time, and ACK time should be trace-confirmed, but the automated regression does not lock those relationships down.
- Expected model: Active probe semantics need three separate invariants: identity matching, nonblocking/no-inline-wait, and timely flush through the transport scheduler. The test currently covers only identity and partial control-frame emission.
- Why it matters: A future refactor could preserve matching ACKs but delay PING flush until a later application write, removing the observed Spanner effect, or could accidentally wait for ACK and damage tail latency.
- Recommended fix: Extend trace validation to assert event ordering for one server-streaming response: response DATA frame observed, client-origin PING frame written before RPC end or before the next application request boundary, matching ACK observed later, and no synchronous wait is required for delivering the response.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: PHPT or diagnostic fixture with trace-order assertions.
- Notes: Low severity because the current production code appears nonblocking; the gap is regression strength.

## Review Result

- Blocker: `1`
- High: `2`
- Medium: `2`
- Low: `1`
- Design Decision: `none`
