# PR #6 HTTP/2 specialist review 2026-05-20

## Scope

- `main..issue5-bdp-ping-settings`
- `ext/grpc/internal.h`
- `ext/grpc/main.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/002-ini.phpt`
- `ext/grpc/tests/029-trace-file.phpt`
- `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md`
- `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md`
- BDP PING / SETTINGS update / flow-control-window / max-frame assumptions / trace interpretation / benchmark conclusions

## Reviewer Role

- HTTP/2 transport specialist

## Review Prompt Summary

- Review PR #6 primarily for whether the investigation and validation are technically sound. Code changes were checked as supporting evidence, but the focus is on whether the issue/docs conclusions are justified by the observed traces and benchmarks.

## Issues

### REVIEW-20260520-001: Active BDP PING cannot explain the same RPC's first-response latency without a cross-RPC connection-state model

- Severity: `High`
- Status: `Open`
- Reviewer role: `HTTP/2 transport specialist`
- Finding: `validation/inference issue`: The docs record that response-DATA-triggered client PING improves later observed Spanner SELECT 1 latency, but the issue does not consistently state the required temporal model: a PING submitted after receiving response DATA cannot causally reduce the request-write-to-first-response time of that same RPC. Any effect must be via connection-level state that influences later streams on the same HTTP/2 connection.
- Evidence: `ext/grpc/transport.c:maybe_submit_active_bdp_probe()` is called from `on_data_chunk_recv_callback()` after inbound DATA is already being processed; `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` and `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md` discuss `request write -> first response` improvements from active PING; the real Spanner SELECT 1 table compares repeated iterations but does not explicitly separate first affected stream from post-probe streams.
- Expected model: HTTP/2 PING is connection-level control traffic. A response-DATA-triggered PING can only affect future behavior after it is written, processed by the peer, and ACKed. For a measured first-response gap, validation must distinguish same-stream causality from cross-stream persistent connection effects.
- Why it matters: Without this distinction, the investigation can over-attribute the SELECT 1 first-response gap to BDP PING. This makes the PR look like a direct fix for the current RPC even though the mechanism can only act on subsequent streams, unless the server/frontend uses the PING side effect for later stream scheduling.
- Recommended fix: Update the issue and PR description to explicitly state that active BDP PING is a cross-RPC connection-state experiment. Re-run or re-summarize the Spanner measurement as warmup streams that establish probe state followed by measured streams, or split results into pre-probe and post-ACK phases.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: This does not prove the observed improvement is false; it limits what the evidence can claim.

### REVIEW-20260520-002: `grpc.http2.bdp_probe=0` evidence weakens BDP as the root-cause hypothesis, but the PR still defaults active probe on

- Severity: `High`
- Status: `Open`
- Reviewer role: `HTTP/2 transport specialist`
- Finding: `validation/inference issue`: The issue records that official ext-grpc remains fast with `grpc.http2.bdp_probe=0`, while grpc-lite still needs active PING to improve. That evidence means “gRPC Core BDP probe is the missing mechanism” is not established. Keeping `grpc_lite.active_bdp_probe=1` as the default needs stronger production-safety and benefit evidence than the current docs provide.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` records `official ext-grpc grpc.http2.bdp_probe=0` at mean 11.9ms / p50 11.1ms; `ext/grpc/main.c` sets `grpc_lite.active_bdp_probe` default to `1`; earlier diagnostic tables record tail regressions for simple PING variants.
- Expected model: A default-on HTTP/2 control behavior should be justified by broad workload safety, not only by correlation in one Spanner/JWT repeated-stream scenario. If official remains fast with BDP disabled, BDP should be treated as a diagnostic lever or opt-in mitigation until the actual differentiating mechanism is identified.
- Why it matters: PING is visible connection-level control traffic and can change scheduler, server, and middlebox behavior. Default-on can affect workloads unrelated to Spanner, especially long server-streaming responses or low-latency synthetic paths.
- Recommended fix: Either make `grpc_lite.active_bdp_probe` default off for PR #6, or add a clear acceptance record explaining why default-on is safe despite the official `bdp_probe=0` result. Include broad benchmark/smoke evidence for unary, server streaming, low-RTT, TLS, and slow-consumer cases with active probe enabled.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: The current SETTINGS update remains default-off; the concern is active PING default-on.

### REVIEW-20260520-003: Default re-arm policy allows RTT-rate PINGs on long DATA streams

- Severity: `High`
- Status: `Open`
- Reviewer role: `HTTP/2 transport specialist`
- Finding: `code issue`: The implementation has one outstanding PING per connection, but after a matching ACK clears `active_bdp_probe_outstanding`, the next inbound DATA can immediately arm another PING because `grpc_lite.active_bdp_probe_min_interval_ms` defaults to `0`. That is a continuous re-arm policy, not a bounded initial probe.
- Evidence: `ext/grpc/transport.c:maybe_submit_active_bdp_probe()` returns only for outstanding probes or a positive min interval; `ext/grpc/transport.c:complete_active_bdp_probe_if_matching()` clears outstanding on matching ACK; `ext/grpc/main.c` defaults `grpc_lite.active_bdp_probe=1` and `grpc_lite.active_bdp_probe_min_interval_ms=0`; `on_data_chunk_recv_callback()` invokes the probe on each DATA callback path.
- Expected model: BDP-style PING should be a bounded connection-level sampling lifecycle. At minimum, production defaults should prevent repeated RTT-rate probes under sustained streaming unless adaptive byte/RTT estimation and backoff are intentionally implemented.
- Why it matters: Long server streaming responses can generate repeated PINGs at roughly one per RTT. That is more conservative than per-DATA PING, but still default-on control-frame load and can create tail variance or peer-side throttling risk.
- Recommended fix: Use a non-zero conservative default interval, a one-probe-per-connection-generation mode, or make active probe opt-in. Add a regression test or trace assertion for the intended re-arm policy under multi-message server streaming.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: Existing tests assert that at least one PING/ACK exists, but do not bound the number of PINGs over a sustained stream.

### REVIEW-20260520-004: SETTINGS update validation does not prove peer-applied state before measured streams

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 transport specialist`
- Finding: `validation/inference issue`: The SETTINGS experiment concludes no additional Spanner SELECT 1 improvement, but the issue does not show whether the peer received and ACKed the updated SETTINGS before the measured RPCs whose latency is summarized. Because SETTINGS changes are peer-applied connection state, their effect must be evaluated after the peer has processed them.
- Evidence: `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md` reports active PING only vs active PING + SETTINGS update over 500 iterations; `ext/grpc/tests/029-trace-file.phpt` asserts outbound SETTINGS contents but not peer SETTINGS ACK timing; the issue says SETTINGS ACK is delegated to nghttp2 and duplicate suppression state is last submitted target, not peer-ACKed current.
- Expected model: HTTP/2 SETTINGS have a send, peer-apply, ACK lifecycle. A validation that measures the effect of SETTINGS should either wait for ACK before the measured phase or explicitly state that the measurement is “submitted SETTINGS, not confirmed applied.”
- Why it matters: Without an ACK/applied boundary, a no-improvement result is still useful but weaker. It rules out a naïve submitted-SETTINGS variant in that run, not necessarily a correctly staged peer-applied SETTINGS state.
- Recommended fix: Add trace-derived evidence for outbound SETTINGS time, inbound SETTINGS ACK time, and measured stream start times. If ACK tracking remains intentionally out of production state, implement it only in the diagnostic trace parser or benchmark harness.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: This is a validation gap, not a protocol correctness bug in nghttp2 usage.

### REVIEW-20260520-005: SELECT 1 is not a meaningful workload for validating MAX_FRAME_SIZE effects

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 transport specialist`
- Finding: `validation/inference issue`: The docs correctly state that `SETTINGS_MAX_FRAME_SIZE` controls peer response DATA frame payload size, but the primary real Spanner validation uses `SELECT 1`, whose response is far below the default 16KiB maximum frame size. Therefore the SELECT 1 result should not be used to infer that max-frame tuning has no value generally.
- Evidence: `docs/issues/closed/2026-05-19-bdp-flow-control-settings.md` uses real Cloud Spanner `SELECT 1` as the main table for `active_bdp_update_max_frame_size=1`; the issue states `MAX_FRAME_SIZE=256KiB` is expected to affect peer response DATA frame length.
- Expected model: `SETTINGS_MAX_FRAME_SIZE` can only change peer DATA frame splitting when response DATA payload would otherwise exceed the current maximum frame size and the peer chooses to use the larger maximum.
- Why it matters: The conclusion “SETTINGS update showed no additional improvement for SELECT 1” is valid. A broader conclusion “MAX_FRAME_SIZE update is not useful” would be overreach without medium/large response cases.
- Recommended fix: Keep the SELECT 1 conclusion narrow. If retaining `active_bdp_update_max_frame_size`, add a controlled large-response benchmark/trace to confirm whether peer DATA frame lengths change and whether p50/p99 changes.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: This does not block the PR if the feature is explicitly documented as experimental and not a proven optimization.

### REVIEW-20260520-006: The PR records many excluded hypotheses, but the remaining hypothesis is not yet falsifiable enough

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 transport specialist`
- Finding: `validation/inference issue`: The investigation usefully excludes metadata strings, header folding, HPACK no-index, timeout formatting, receive window size, TCP_INQ, and simple BDP-off/on checks. However, the remaining explanation is still broad: “official C-core and lite nghttp2 direct transport wire/control lifecycle differ.” That is true but not yet a falsifiable HTTP/2 hypothesis.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` lists many negative variants and then leaves remaining candidates as connection preface/settings/control lifecycle, C-core scheduler, TLS record, HPACK dynamic table, or stream lifecycle.
- Expected model: After a negative-result investigation, the next hypothesis should name the specific HTTP/2 state that differs and the expected observable outcome. Examples: peer SETTINGS ACK timing, client initial SETTINGS values, PING cadence, HPACK dynamic table evolution, stream ID sequencing, connection reuse age, server PING/ACK relation, DATA/HEADERS coalescing, or TLS record boundaries.
- Why it matters: A broad residual hypothesis makes it easy to keep adding control-frame variants without a clear stop condition. It also makes PR review hard because the PR mixes useful instrumentation, observed Spanner behavior, and speculative control changes.
- Recommended fix: Add a short “current falsifiable hypotheses” section to the open issue. For each remaining candidate, include expected trace evidence, how to disprove it, and whether PR #6 addresses it. Separate “implemented mitigation” from “investigation artifact.”
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: This is about investigation quality, not code style.

### REVIEW-20260520-007: Tests cover positive PING/SETTINGS traces but not disabled/no-op semantics

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 transport specialist`
- Finding: `code issue`: PHPT now asserts default INI values and a positive opt-in SETTINGS trace, but does not assert no outbound active PING when `grpc_lite.active_bdp_probe=0` or no SETTINGS when `grpc_lite.active_bdp_update_settings=0`.
- Evidence: `ext/grpc/tests/002-ini.phpt` checks defaults; `ext/grpc/tests/029-trace-file.phpt` enables `active_bdp_update_settings=1` and `active_bdp_update_max_frame_size=1` and asserts PING/SETTINGS existence.
- Expected model: Experimental HTTP/2 control features need both positive and disabled-path regression tests, especially when defaults and INI combinations are part of the production safety contract.
- Why it matters: Future changes could accidentally emit PING or SETTINGS even when disabled, and current tests would not catch it.
- Recommended fix: Add a small PHPT or extend an existing trace test to run with `grpc_lite.active_bdp_probe=0` and assert no client-origin active PING, and with `grpc_lite.active_bdp_update_settings=0` and assert no ACK-triggered SETTINGS update beyond the connection preface SETTINGS.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending`
- Notes: This is lower severity because nghttp2 protocol correctness is not directly at risk.

## Review Result

- Blocker: `none`
- High: `3`
- Medium: `3`
- Low: `1`
- Design Decision: `none`
