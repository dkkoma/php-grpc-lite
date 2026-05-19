# BDP probe後のflow-control SETTINGS連動検証

- Status: Closed
- Owner: Codex
- Created: 2026-05-19
- Related:
  - `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md`
  - GitHub issue #5

## 目的

Cloud Spanner real endpointで観測されている `ext-grpc 1.58.0` と `php-grpc-lite` の `request write -> first response` 差について、gRPC Core v1.58.0 のBDP probe実装に含まれる `SETTINGS_INITIAL_WINDOW_SIZE` / `SETTINGS_MAX_FRAME_SIZE` 更新連動が性能差へ寄与するか検証する。

## 背景

既存調査では、SA JSON + real Cloud Spanner `ExecuteStreamingSql SELECT 1` において、BDPなしのgrpc-liteは `request write -> first response` が `ext-grpc 1.58.0` より遅い。response DATA受信後にclient-origin PINGを送る active probe で差は縮むが、まだext-grpcとの差が残っている。

`ext-grpc 1.58.0` が使う gRPC Core v1.58.0 では、`GRPC_ARG_HTTP2_BDP_PROBE` が未指定なら有効で、DATA frame受信時にBDP PINGをscheduleし、ACK完了後にBDP estimateを更新して `flow_control.PeriodicUpdate()` を実行する。その中で `SETTINGS_INITIAL_WINDOW_SIZE` と `SETTINGS_MAX_FRAME_SIZE` の更新候補が生成される。

grpc-liteの現行実装は、response DATA後にPINGを送ってACKを照合するだけで、incoming bytes accumulator、BDP estimate、window / max frame size更新を持たない。

## スコープ

- gRPC Core v1.58.0のBDP estimator / flow-control updateモデルを確認する。
- grpc-liteにproduction-safeなopt-in実験として、BDP probe ACK後にHTTP/2 SETTINGS updateを送る経路を追加する。
- real Cloud Spanner `SELECT 1` を中心に、active PINGのみとの差分を計測する。
- 低RTT synthetic / emulatorで副作用が出ないか主要ベンチで確認する。

## 非スコープ

- gRPC Coreの完全なBDP estimator再実装。
- keepalive PINGの実装。
- Google / Spanner専用の自動判定をproduction defaultへ入れること。
- client streaming / bidi streaming対応。

## 設計候補

初期実装は明示opt-inとする。既存 `grpc_lite.active_bdp_probe` が有効でも、`grpc_lite.active_bdp_update_settings` が無効ならSETTINGS再送は行わない。

このissueで扱うのは、BDP probe ACK後にpeer-visibleなHTTP/2 SETTINGSを更新する経路だけである。DATA受信に伴う通常のconnection / stream `WINDOW_UPDATE` は既存のnghttp2 flow-controlと初期connection window拡張に任せ、このissueでは変更しない。

- `grpc_lite.active_bdp_update_settings=0` を追加する。
- `grpc_lite.active_bdp_update_settings=1` かつ `grpc_lite.active_bdp_probe=1` の場合だけ、client-origin PING ACK一致後にSETTINGS候補を評価する。
- `grpc_lite.active_bdp_probe=0` の場合、`grpc_lite.active_bdp_update_settings=1` でも完全no-opとする。
- 初期実装ではincoming bytes accumulatorやRTTに基づくBDP estimateは持たない。これはBDP estimatorではなく、active PING ACK後に静的target SETTINGSを送る実験機能である。
- target値がlast submitted targetより大きい場合だけ、`SETTINGS_INITIAL_WINDOW_SIZE` / `SETTINGS_MAX_FRAME_SIZE` を `nghttp2_submit_settings()` で送る。縮小更新と同じtargetの再送は抑制する。SETTINGS ACKはnghttp2に委譲し、grpc-lite側のduplicate suppression stateはpeer-ACKed currentではなくlast submitted targetとして扱う。
- draining / dead connectionではactive PINGとSETTINGSを送らない。
- defaultはoff。実験・Spanner向けopt-inでのみ有効にする。
- ACK処理では `nghttp2_submit_settings()` でqueueするだけにし、callback内でinline waitしない。flushは既存の `nghttp2_session_want_write()` / `send_pending_h2_frames()` 境界に乗せる。

### INI matrix

| `active_bdp_probe` | `active_bdp_update_settings` | 挙動 |
|---:|---:|---|
| 0 | 0 | PINGもSETTINGS updateも行わない |
| 0 | 1 | SETTINGS updateは完全no-op |
| 1 | 0 | response DATA後のactive PINGのみ |
| 1 | 1 | client-origin PING ACK一致後にSETTINGS候補を評価 |

初期上限候補:

- initial window upper bound: 8MiB
- max frame size upper bound: 256KiB

`SETTINGS_INITIAL_WINDOW_SIZE` はstream単位の受信flow-control初期window、`SETTINGS_MAX_FRAME_SIZE` はpeerが送ってよいDATA frame payload最大値である。どちらもreceive buffer sizeやcompact thresholdとは別概念として扱う。

connection-level receive windowはこのissueでは動的更新しない。grpc-liteは接続確立時に固定8MiBのconnection windowへ拡張済みであり、BDP ACK後の追加 `WINDOW_UPDATE` 自動化は別issueで扱う。

`MAX_FRAME_SIZE=256KiB` はHTTP/2仕様上の上限 `16777215` より十分小さいが、小さいRPCには不要であり、large response / slow consumer / memory pressureへの副作用があり得る。そのため初期実装では `grpc_lite.active_bdp_update_max_frame_size=0` を別optionとして、まず `INITIAL_WINDOW_SIZE` 更新単独と、`MAX_FRAME_SIZE` 更新込みを分けて計測できるようにする。

`SETTINGS_MAX_FRAME_SIZE` はpeerのresponse DATA frame payload最大値に作用する。client request HEADERS/DATA packing、HPACK、TLS record schedulingを直接変えるものではない。検証ではserver response DATA frame lengthが変わるかをtraceで確認する。

## 判断ログ

- 2026-05-19: v1.58.0のgRPC CoreではBDP probeがdefault trueであり、ACK完了後にflow-control updateが走ることを確認した。
- 2026-05-19: 現行grpc-liteのactive probeはBDP estimatorではなく、HTTP/2 PING probeに近い。SETTINGS連動は別issueで検証する。

## 計画

- [x] 設計レビューを実施する。
- [x] レビュー結果に応じて設計を修正する。
- [x] opt-in実装を追加する。
- [x] PHPT / C unit / static analysisを通す。
- [x] 実装後レビューを実施する。
- [x] real Cloud Spannerと主要ベンチを計測する。

## 検証

- `./tools/test/check-phpt.sh`: PASS, 16/16
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `tools/diagnostics/issue5-spanner-repro/select1-bench.php` または既存runnerによるreal Cloud Spanner `SELECT 1`
- `./bench/compare.sh spanner-shape`
- `./bench/compare.sh tls-spanner-shape`

### real Cloud Spanner SELECT 1

fixture:

- target: real Cloud Spanner `vast-falcon-165704 / bench / laravel-bench-db`
- credentials: service account JSON via `GOOGLE_APPLICATION_CREDENTIALS=/sa.json`
- script: `/private/tmp/select1-bench-workspace-vendor.php`
- iterations: 500
- grpc-lite build: current source-built `ext/grpc/modules/grpc.so`

| variant | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| official ext-grpc 1.58.0 | 500 | 11.856ms | 11.671ms | 13.640ms | 18.901ms | 比較基準 |
| grpc-lite active PING only | 500 | 15.918ms | 15.448ms | 17.573ms | 21.977ms | SETTINGSなし |
| grpc-lite active PING + SETTINGS update | 500 | 15.906ms | 15.443ms | 17.592ms | 25.481ms | p50/mean同等、p99は悪化 |

200 iterationの事前確認でも、SETTINGS updateありは `mean=16.035ms / p50=15.326ms / p99=21.700ms`、SETTINGSなしは `mean=15.156ms / p50=14.447ms / p99=20.395ms` で、追加改善は見えなかった。

判断:

- `SETTINGS_MAX_FRAME_SIZE=256KiB` のACK後送信は、real Cloud Spanner `SELECT 1` の `request -> first response` 差をさらに縮める効果を示さなかった。
- grpc-liteとofficial ext-grpc 1.58.0の残差は、少なくともこの単純なSETTINGS再送では説明できない。
- この機能はdefault offのままにする。採用する場合もCloud Spanner向け性能改善策ではなく、HTTP/2 SETTINGS挙動検証用のopt-in実験機能として扱う。

### 主要ベンチ

`active_bdp_update_settings` はdefault offのため、通常主要ベンチは既存経路のまま動くことを確認した。

- `./bench/compare.sh spanner-shape`: PASS, run id `20260519-235307`
- `./bench/compare.sh tls-spanner-shape`: PASS, run id `20260519-235329`

代表値:

| suite | measurement | native p50/p99 | ext-grpc p50/p99 |
|---|---|---:|---:|
| spanner-shape | commit_txn_unary | 23.0us / 209.6us | 67.3us / 122.4us |
| spanner-shape | select_1row_10col_streaming | 24.9us / 379.8us | 70.1us / 104.9us |
| tls-spanner-shape | commit_txn_unary | 27.9us / 1258.0us | 85.2us / 116.5us |
| tls-spanner-shape | select_1row_10col_streaming | 30.3us / 1341.3us | 89.8us / 335.0us |

## 完了条件

- 設計レビューでBlocker / High / Medium / Lowがnoneになる。
- opt-in実装のテストが通る。
- active PINGのみ / SETTINGS連動あり / ext-grpc 1.58.0 の比較がdocsに記録される。
- 副作用が大きい場合は実装をdefault offの実験機能として残すか、撤回する判断を明記する。

## 完了判断

2026-05-19に完了。ACK後SETTINGS updateは実装・検証・計測済みだが、real Cloud Spanner `SELECT 1` ではactive PING単独からの追加改善は見えなかった。したがってdefault offのopt-in実験機能として残し、Cloud Spanner性能改善の主対策とは扱わない。
