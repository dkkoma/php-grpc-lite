# BDP flow-control SETTINGS design review 2026-05-19

## Scope

- `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`
- `ext/grpc/transport.c`
- `ext/grpc/internal.h`
- `ext/grpc/main.c`

## Reviewer Role

- gRPC/HTTP2 transport domain reviewer

## Review Prompt Summary

- BDP probe後に `SETTINGS_INITIAL_WINDOW_SIZE` / `SETTINGS_MAX_FRAME_SIZE` を送る設計が、HTTP/2 connection-level flow-control、PING/ACK lifecycle、SETTINGS semantics、production safetyに沿っているかを実装前に確認した。

## Issues

### REVIEW-20260519-001: SETTINGS連動はWINDOW_UPDATEとは別概念として明記する

- Severity: Medium
- Status: Fixed
- Reviewer role: gRPC/HTTP2 transport domain reviewer
- Finding: issue本文では `flow-control window / max frame size update` と書いているが、既存のconnection `WINDOW_UPDATE` とBDP ACK後の `SETTINGS_INITIAL_WINDOW_SIZE` 更新が混同されやすい。
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`
- Expected model: HTTP/2では、connection flow-control credit増加は `WINDOW_UPDATE`、今後のstream initial windowやmax frame size変更は `SETTINGS` であり、別のframe/lifecycleとして扱う。
- Why it matters: 実装時にDATA受信ごとの通常WINDOW_UPDATEと、BDP ACK後のSETTINGS再送を混同すると、過剰なcontrol frame送信や不正なstream/connection scopeを招く。
- Recommended fix: issueの設計候補に「通常WINDOW_UPDATEは既存nghttp2/初期window経路に任せ、このissueではBDP ACK後のSETTINGS updateだけを扱う」と明記する。
- Fix summary: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md` に、通常 `WINDOW_UPDATE` は既存nghttp2/初期window経路に任せ、このissueはBDP ACK後のSETTINGS updateだけを扱うことを明記した。
- Fix commit: pending
- Verification: design doc self-review
- Notes:

### REVIEW-20260519-002: MAX_FRAME_SIZE上限候補の意味と副作用が未説明

- Severity: High
- Status: Fixed
- Reviewer role: gRPC/HTTP2 transport domain reviewer
- Finding: 初期上限候補 `max frame size upper bound: 256KiB` はHTTP/2 `SETTINGS_MAX_FRAME_SIZE` の最大値 `16777215` 以内なので仕様上は合法だが、既存の「receive buffer 256KiB」と混同されており、実サービスへ送るSETTINGS値として妥当性が未説明。
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`
- Expected model: `SETTINGS_MAX_FRAME_SIZE` はpeerのDATA frame payload最大値であり、受信bufferサイズやcompact thresholdとは別。値はHTTP/2仕様範囲、nghttp2制約、メモリ上限、tail latency副作用を分けて決める。
- Why it matters: 大きすぎるframe sizeは小さいRPCには不要で、large responseやslow consumerでchunk granularity / memory pressureに影響し得る。
- Recommended fix: 初期実装ではmax frame size更新を独立optionにするか、まずはCore式estimate clampに基づく小さな上限として明記し、受信bufferと同一視しない。
- Fix summary: `SETTINGS_MAX_FRAME_SIZE` はreceive bufferやcompact thresholdではなくpeer DATA frame payload最大値であること、HTTP/2上限内だが副作用があり得ること、初期実装では `grpc_lite.active_bdp_update_max_frame_size=0` で独立計測することを明記した。
- Fix commit: pending
- Verification: design doc self-review
- Notes:

### REVIEW-20260519-003: ACK後SETTINGS送信のflush boundaryを明記する

- Severity: Medium
- Status: Fixed
- Reviewer role: gRPC/HTTP2 transport domain reviewer
- Finding: `nghttp2_submit_settings()` 後にどのタイミングでwireへflushするかがissue上で未定義。
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`
- Expected model: PING ACKはread pathで処理されるため、SETTINGSをqueueした後はDATA callback内でinline waitせず、既存の `nghttp2_session_want_write()` / `send_pending_h2_frames()` 境界でflushする。
- Why it matters: ACK callback内で同期writeやwaitを行うと、read callback responsibilityとwrite schedulingが混ざり、tail latencyやreentrancy riskを増やす。
- Recommended fix: issueに「ACK処理ではsubmitのみ、flushは既存read loop境界」と明記し、テストでoutbound SETTINGSがtraceされることを確認する。
- Fix summary: ACK処理では `nghttp2_submit_settings()` でqueueするだけにし、flushは既存の `nghttp2_session_want_write()` / `send_pending_h2_frames()` 境界に乗せることを明記した。
- Fix commit: pending
- Verification: design doc self-review
- Notes:

### REVIEW-20260519-004: default policyを明示的にopt-inへ固定する

- Severity: Medium
- Status: Fixed
- Reviewer role: gRPC/HTTP2 transport domain reviewer
- Finding: issueはopt-inと書いているが、既存 `grpc_lite.active_bdp_probe` は現時点でdefault onであり、SETTINGS連動がそれに従うとproduction defaultへ広がる。
- Evidence: `ext/grpc/main.c`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`
- Expected model: 低RTT synthetic / emulatorでactive PING default onの副作用が記録済みなので、SETTINGS連動は明示opt-inかつ既存probe default見直しとは独立して扱う。
- Why it matters: SETTINGS更新はpeer-visibleなconnection behavior変更であり、default onにするとGoogle/Spanner以外のサービスで予期せぬ副作用が出る。
- Recommended fix: `active_bdp_update_settings` はdefault off、かつ `active_bdp_probe` がonでも自動有効化しないことをissueに明記する。
- Fix summary: `active_bdp_update_settings` はdefault offで、`active_bdp_probe` がonでも自動有効化しないことを明記した。
- Fix commit: pending
- Verification: design doc self-review
- Notes:

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
