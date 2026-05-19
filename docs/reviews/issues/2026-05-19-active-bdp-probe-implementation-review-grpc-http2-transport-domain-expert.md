# Active BDP Probe Implementation Review 2026-05-19

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/main.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/002-ini.phpt`
- `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md`

## Reviewer Role

- `gRPC + HTTP/2 transport domain expert`

## Review Prompt Summary

- active BDP probe PING が HTTP/2 connection-level control state として正しくモデル化されているかを確認した。特に connection-level state、connection単位でoutstanding PING 1つ、client-origin PING ACKのopaque照合、inline waitなし、通常send scheduling、server-origin PING ACK処理との分離、INI semantics、flow-control/window変更との非結合を確認した。

## Domain Check Result

- `active_bdp_probe_outstanding`、`active_bdp_probe_opaque`、`active_bdp_probe_sent_at_us` は `h2_connection` にあり、gRPC CallやHTTP/2 StreamではなくHTTP/2 Connection scopeに置かれている。
- `maybe_submit_active_bdp_probe()` は outstanding 中に新規PINGをsubmitせず、connection単位で1つのclient-origin probeだけを保持している。
- `complete_active_bdp_probe_if_matching()` は inbound `PING ACK` かつopaque一致時だけoutstandingを解除し、server-origin non-ACK `PING` ではprobe stateを変更しない。
- DATA callbackでは `nghttp2_submit_ping()` のみを行い、ACK待ちはしていない。unary/server streamingのrecv loopはいずれも `nghttp2_session_mem_recv()` 後の `nghttp2_session_want_write()` / `send_pending_h2_frames()` により通常のcontrol frame flushへ乗せている。
- 実装はwindow sizeやflow-control設定を変更しておらず、active PINGはfixed windowのconnection control behaviorとして分離されている。
- INIは `grpc_lite.active_bdp_probe=1`、`grpc_lite.active_bdp_probe_min_interval_ms=0` が `PHP_INI_SYSTEM` として登録され、PHPTでdefault値が確認されている。

## Issues

### REVIEW-20260519-001: active BDP probeのcontrol-frame不変条件が自動テストで固定されていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `gRPC + HTTP/2 transport domain expert`
- Finding: `実装自体はconnection-level state、opaque照合、server-origin PINGとの分離、通常send schedulingを満たしているが、002-ini.phptはINI defaultだけを確認しており、active BDP probeのHTTP/2 control-frame不変条件を自動テストで固定していない。`
- Evidence: `ext/grpc/tests/002-ini.phpt:17`, `ext/grpc/transport.c:348`, `ext/grpc/transport.c:383`, `ext/grpc/transport.c:1875`, `ext/grpc/transport.c:1999`, `ext/grpc/unary_call.c:179`, `ext/grpc/server_streaming_call.c:314`
- Expected model: `active BDP probe PINGはHTTP/2 Connectionのcontrol behaviorなので、connection単位でone outstanding、client-origin ACKのopaque一致だけで解除、server-origin PING ACKとは独立、inline waitなし、flow-control/window変更なし、という不変条件がfixtureまたはPHPTで回帰防止されるべき。`
- Why it matters: `この経路はSpanner実経路の性能差に効くproduction behaviorであり、将来のtransport修正でserver PING処理、WINDOW_UPDATE、read loop flush順序へ偶発的に結合しても、default INIテストだけでは検出できない。`
- Recommended fix: `raw lifecycle fixtureまたは既存のHTTP/2 test-serverに、response DATA後のclient-origin PING送信、ACK opaque一致時だけのre-arm、server-origin PING受信時のprobe outstanding非解除、PING submit後にinline waitしないこと、WINDOW_UPDATE増分が変わらないことを観測するprotocol regression testを追加する。`
- Fix summary: `ext/grpc/tests/029-trace-file.phpt` に active BDP probe の protocol regression を追加した。`GRPC_LITE_TRACE_WIRE_BYTES=1` で outbound client-origin PING payload と inbound PING ACK payload を記録し、少なくとも1つのACK payloadがoutbound PING payloadと一致することを確認する。inbound PING payload traceも `ext/grpc/transport.c` に追加した。
- Fix commit: `this commit`
- Verification: `./tools/test/check-phpt.sh` pass, `./tools/test/check-c-static-analysis.sh` pass
- Notes: `production codeのBlocker/High/Medium指摘はない。`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1`
- Design Decision: `none`
