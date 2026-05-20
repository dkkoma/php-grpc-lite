# BDP estimator domain review 2026-05-19

## Scope

- `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md`
- BDP Ping / BDP estimator production candidate only

## Reviewer Role

- gRPC + HTTP/2 transport domain expert

## Review Prompt Summary

- Review whether the proposed BDP direction is a sound domain model: response DATA-triggered active PING, one outstanding probe per connection, ACK-based completion, no unconditional per-DATA PING, no immediate flush/wait, and whether flow-control/window update should be coupled initially.

## Issues

### REVIEW-20260519-001: ACK completion must match the client-origin probe opaque

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `gRPC + HTTP/2 transport domain expert`
- Finding: The candidate says ACK receipt clears `BDP probe outstanding`, but does not state that completion is only for an ACK whose opaque payload matches the active client-origin probe.
- Evidence: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` production候補 says `ACK受信でoutstandingを解除する`; the same issue records server-origin PINGs around response/trailers, so PING traffic is bidirectional in this workload.
- Expected model: HTTP/2 Connection owns PING probe state; a BDP probe completes only when an inbound PING frame has ACK flag set and the exact 8-byte opaque data of the outstanding client-origin PING. Server-origin PING without ACK must be answered but must not complete the BDP probe; unrelated ACKs must not mutate BDP state.
- Why it matters: Clearing outstanding on the wrong PING frame corrupts the connection-level probe lifecycle, can re-enable a probe too early, and turns the design back toward uncontrolled DATA-adjacent PING behavior under servers that also send PINGs.
- Recommended fix: Specify connection state as at least `bdp_probe_outstanding`, `bdp_probe_opaque`, and `bdp_probe_sent_at`; clear only on matching ACK, ignore non-matching ACK for BDP, and keep server PING ACK handling separate from BDP estimator state.
- Fix summary: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` のproduction候補に `BDP probe outstanding` / `BDP probe opaque` / `BDP probe sent_at` をconnection stateとして明記し、client-origin probeのopaqueと一致するACKだけでoutstandingを解除する方針へ修正した。server-origin PINGや無関係なPING ACKはBDP probe stateを変更しないことも明記した。
- Fix commit: `pending`
- Verification: `documentation review`
- Notes: `none`

### REVIEW-20260519-002: Active PING needs asynchronous flush scheduling, not synchronous wait

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `gRPC + HTTP/2 transport domain expert`
- Finding: `nghttp2_submit_ping()` without immediate flush/wait is conceptually right, but the candidate does not state the required scheduler invariant: once a PING is queued from response DATA, the connection must arrange a timely write opportunity without blocking for ACK.
- Evidence: The issue distinguishes the reporter-style variant as queuing PING inside `on_data_chunk_recv_callback()` and not doing additional flush/wait; earlier variants that flushed/waited around recv damaged tail latency.
- Expected model: HTTP/2 transport owns pending control frames and must flush them through the normal nonblocking send path. BDP probing must not synchronously wait in the DATA callback, but queued control frames must set/retain connection write interest or be flushed at a safe recv-loop boundary.
- Why it matters: If queued PING is not flushed until the next unrelated application send, the probe is not an active connection signal and the measured/reported benefit may disappear. If it waits synchronously, the receive path can add head-of-line delay and recreate the bad tail behavior already observed.
- Recommended fix: Document and implement the production invariant as “submit PING from DATA callback, mark connection has pending control output, flush via existing send cycle or immediate nonblocking drain point, never wait for ACK inline.” Add trace evidence for PING submit time, wire write time, and ACK time.
- Fix summary: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` のproduction候補に、DATA callback内では `nghttp2_submit_ping()` だけを行いACKをinline waitしないこと、queued control frameは既存のnonblocking send pathで速やかにflushすること、PING submit時刻・wire write時刻・ACK時刻をtraceで確認することを明記した。
- Fix commit: `pending`
- Verification: `documentation review`
- Notes: `none`

### REVIEW-20260519-003: DATA-triggered probe needs a re-arm policy, not just outstanding false

- Severity: `Low`
- Status: `Open`
- Reviewer role: `gRPC + HTTP/2 transport domain expert`
- Finding: “response DATA受信で未probeなら1回だけPING” plus “ACKでoutstanding解除” prevents simultaneous PINGs, but still permits one probe per RTT while response DATA continues unless a re-arm/delay/sample policy is defined.
- Evidence: The issue rejects unlimited per-DATA PING and earlier notes a real BDP estimator shape includes `incoming bytes accumulator`, `one outstanding ping`, `ACK completion`, `next probe delay`, and `flow-control target update`; the production候補 retains only outstanding/ACK and intentionally omits window update.
- Expected model: A BDP probe is a connection-level sampling lifecycle, not a per-frame reaction. DATA should make the connection eligible to probe; ACK completion should finish one sample and then re-arm according to a clear policy such as first DATA after connection setup, minimum interval, accumulated byte threshold, or explicit “single initial active probe” mode.
- Why it matters: Without a re-arm rule, the design can be interpreted as continuous RTT-rate PING under streaming responses. That is better than per-DATA but still not a bounded estimator and may create avoidable control-frame load or tail variance.
- Recommended fix: For the initial production candidate, define the narrowest policy explicitly. Suggested first step: one active DATA-triggered probe per connection generation, or a conservative minimum probe interval, with counters/traces for submitted, suppressed-outstanding, suppressed-interval, and ACKed probes. Defer adaptive sampling until it is tied to measured bytes/RTT and window policy.
- Fix summary: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` のproduction候補に、初期re-arm policyは「connection generationごとに1回」または「最小intervalあり」の保守的なものにし、継続streamingでRTTごとに投げ続ける設計にしないことを明記した。
- Fix commit: `pending`
- Verification: `documentation review`
- Notes: `none`

### REVIEW-20260519-004: Initial decoupling from flow-control/window update is acceptable but must be named honestly

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `gRPC + HTTP/2 transport domain expert`
- Finding: It is sound to avoid automatic flow-control/window changes in the first production candidate, but then the feature is not yet a complete BDP estimator; it is an active DATA-triggered connection probe.
- Evidence: `docs/SPEC.md` and `docs/code-reading-guide.md` already define fixed 8MiB stream and connection receive windows. The issue’s production候補 says to skip window size auto-change initially and focus on reproducing the Spanner frontend response difference caused by active PING presence.
- Expected model: Flow-control window policy is an HTTP/2 Connection concern, separate from gRPC Call semantics. A BDP estimator conceptually measures bytes over an RTT and may use that to adjust connection/stream receive windows, but introducing window mutation changes production behavior and should be a separate decision gate.
- Why it matters: Coupling PING probes to window updates immediately would mix two variables in a performance investigation and could regress memory/backpressure behavior. Conversely, calling a PING-only feature “BDP estimator” overstates the invariant it maintains.
- Recommended fix: Keep initial window update decoupled. Name the first production step `active_bdp_probe` or `bdp_probe_ping` in docs/code, record bytes/RTT as telemetry if cheap, and open a later issue for adaptive flow-control only after before/after evidence shows the fixed 8MiB policy is the limiting factor.
- Fix summary: `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md` のproduction候補を「完全なBDP estimator」ではなく `active BDP probe PING` として扱う記述に変更し、window size自動変更やadaptive flow-controlは別issueで扱う方針を明記した。
- Fix commit: `pending`
- Verification: `documentation review`
- Notes: `none`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `2`
- Low: `1`
- Design Decision: `1`
