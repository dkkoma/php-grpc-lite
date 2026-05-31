# お手本化: grpc_call field layout hot path最適化

- Status: Open
- Created: 2026-05-31
- Branch: main
- Owner: Unassigned
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md
- Depends-On: docs/issues/closed/2026-05-31-exemplar-grpc-call-exchange-state-map.md

## Background

`docs/design/grpc-call-exchange-state.md` で `grpc_call` のfield責務、lifetime、hotnessを整理した。次の段階として、現在の `struct _grpc_call` のfield orderがhot pathに対して妥当かを測定し、必要ならlayoutを調整する。

`grpc_call` は1 RPC over 1 HTTP/2 streamの交換状態であり、unaryではstack上、server streamingでは `server_streaming_call_state` resource内に置かれる。nghttp2 callback、DATA chunk parser、request body callback、status resolution、server streaming queueで頻繁に参照されるため、field order変更は単なる可読性改善ではなく性能変更として扱う。

## Current Layout Observations

現状の `src/grpc_exchange_state.h` の `struct _grpc_call` は、おおむね責務順に並んでいる。

1. stream identity / connection link
   - `connection`, `next_active_stream`, `method_path`, `stream_id`, `stream_registered`, `connection_owned`, `stream_closed`
2. status / validation flags
   - `grpc_status`, `grpc_message`, `stream_error_code`, `stream_reset_seen`, `stream_refused_seen`, `http_status`, validation bools
3. response header values / size limits
   - `content_type`, `grpc_encoding`, `response_message_count`, `max_response_messages`, `max_receive_message_bytes`
4. deadline / I/O error detail
   - `timed_out`, `last_io_errno`, `last_ssl_error`, `last_io_error_detail[256]`, `deadline_abs_us`
5. response delivery / metadata / parser
   - `decode_response_incrementally`, queue pointers/counters, metadata list/counters, parser offsets/header buffer/payload
6. response body and request writer
   - `body`, `grpc_header`, request pointer/offsets, pending write iov
7. bench-only observation
   - `PHP_GRPC_LITE_ENABLE_BENCH` buildでは先頭付近と中盤にdiagnostic fields、後半に `grpc_bench_call bench`

この並びは教材としては読みやすい。一方で、性能観点では次が気になる。

- `last_io_error_detail[256]` はcold寄りのerror detailだが、中盤にあり、後続のresponse delivery/parser fieldsとの距離を広げている。
- 多数の `bool` が `int` / pointer / `size_t` と混在しており、paddingが増えている可能性がある。
- inbound hot pathで一緒に触るfieldが分散している。
  - stream lookup/lifecycle: `stream_id`, `stream_closed`, `stream_reset_seen`, `stream_error_code`
  - validation/status: `grpc_status`, `grpc_status_seen`, `invalid_grpc_status`, `metadata_too_large`, `malformed_response_frame`, `response_message_too_large`
  - parser: `response_parse_offset`, `response_header_len`, `response_payload_len`, `response_payload_offset`, `response_current_compressed`, `response_payload`
  - server streaming queue: `response_queue_head`, `response_queue_tail`, `response_queue_count`, `response_queue_bytes`
- outbound hot pathのrequest writer fieldsは末尾にまとまっているが、status/parser fieldsからは離れている。unary request send中心のworkloadではこのまとまり自体は悪くない可能性がある。
- bench buildではproductionとlayoutが変わる。bench-only fieldsを中盤に挟むと、bench buildの観測値がproduction layoutからずれる可能性がある。

## Goals

- 現在の `grpc_call` layoutを数値で観測する。
  - `sizeof(grpc_call)`
  - field offset
  - padding / cache line境界
  - production build と `PHP_GRPC_LITE_ENABLE_BENCH` build の差
- hot pathごとのfield access groupを明確にする。
- field order変更またはhot/cold splitの候補を作る。
- before/after benchmarkで採否判断する。
- 効果が小さい場合は実装を採用せず、分析結果だけを残す。

## Non-Goals

- 計測なしでfield orderを変更しない。
- `grpc_call` のsemantic ownershipを変えない。
- status/metadata/deadline/RST_STREAM/GOAWAY/EOF behaviorを変えない。
- `grpc_call` をsub-struct化する場合でも、先にlayout-only変更として評価する。semantic helper抽出と混ぜない。
- ext-grpcの数値へ近づけること自体を目的にしない。

## Suggested Investigation

### 1. Layout dump

まずproduction buildとbench buildのlayoutを観測する。候補:

- `clang -Xclang -fdump-record-layouts` を使って `struct _grpc_call` のoffsetを出す。
- debug symbol付きbuildで `pahole` が使える環境なら `pahole -C _grpc_call modules/grpc.so` を使う。
- 難しければ、一時的なdiagnostic C unitまたはdebug-only helperで `sizeof(grpc_call)` と `offsetof()` を出す。これはcommitしないか、commitする場合はdiagnostic-onlyに限定する。

記録したい項目:

| Item | Production | Bench build | Note |
|---|---:|---:|---|
| `sizeof(grpc_call)` | TBD | TBD | cache line個数も見る |
| `last_io_error_detail` offset | TBD | TBD | cold large fieldの位置 |
| parser fields offset range | TBD | TBD | DATA chunk hot path |
| request writer offset range | TBD | TBD | outbound DATA hot path |
| queue fields offset range | TBD | TBD | server streaming read-ahead |
| bench fields offset range | n/a | TBD | production layoutとの差 |

### 2. Hot path access groups

次の関数を中心に、field accessを表にする。

| Path | Main functions | Important fields |
|---|---|---|
| stream registration / close | `register_grpc_call_stream()`, `unregister_grpc_call_stream()`, `on_stream_close_callback()` | `connection`, `next_active_stream`, `stream_id`, `stream_registered`, `connection_owned`, `stream_closed`, `stream_error_code`, `stream_reset_seen`, `stream_refused_seen` |
| inbound header/status | `on_header_callback()`, `grpc_protocol_add_response_metadata_entry()`, `resolve_grpc_call_status()` | `grpc_status`, `grpc_message`, `http_status`, `grpc_status_seen`, `invalid_grpc_status`, `content_type_seen`, `content_type`, `grpc_encoding`, metadata counters/list |
| inbound DATA parser | `on_data_chunk_recv_callback()`, `grpc_protocol_process_response_data_direct()`, `grpc_protocol_validate_response_message_lengths()` | parser offsets/header buffer/payload, message limits, compression flags, body/queue fields |
| server streaming delivery | `enqueue_response_payload()`, `server_streaming_call_next_resource()`, `free_queued_response_payloads()` | queue head/tail/count/bytes, `queue_response_payloads`, `direct_response_payload`, metadata/status flags |
| outbound request writer | `data_source_read_callback()`, `copy_request_bytes()`, `send_pending_h2_frames()` | `grpc_header`, `grpc_header_len`, `request`, `request_len`, `request_offset`, pending write fields, `deadline_abs_us`, `timed_out` |
| error/status detail | `connection_send()`, `connection_recv()`, `resolve_grpc_call_status()` | `timed_out`, `last_io_errno`, `last_ssl_error`, `last_io_error_detail`, status flags |

### 3. Candidate layout directions

候補は最初から複数案にし、1案だけに決め打ちしない。

#### Candidate A: hot parser/status fieldsを前方へ寄せる

- stream lifecycle、status flags、parser offsets、message limitsを近くに寄せる。
- `last_io_error_detail[256]`、metadata list、body、request writerは後方へ寄せる。
- unary small response / metadata validationに効く可能性がある。
- request send pathのlocalityを悪化させないか確認が必要。

#### Candidate B: request writer blockを明示的にまとめる

- request writer fieldsは現在も末尾にまとまっているため、大きく動かさない。
- parser/status側だけ冷たいdetail fieldを後方へ逃がす。
- 変更範囲が小さく、最初に試しやすい。

#### Candidate C: cold detail blockを最後尾へ寄せる

- `last_io_error_detail[256]`, `grpc_message`, `content_type`, `grpc_encoding`, metadata listなど、error/detail/result construction寄りのfieldを後方へ寄せる。
- hot bool/size/pointer群のpadding改善を狙う。
- status resolutionでdetail fieldを読む場合はcold化しすぎないよう注意する。

#### Candidate D: bench-only fieldsをbench blockへ集約する

- `PHP_GRPC_LITE_ENABLE_BENCH` fieldsを中盤に挟まず、可能な限り `grpc_bench_call bench` 側へ集める。
- production layoutとbench layoutの乖離を減らす。
- bench diagnostic codeが直接読むfieldの移動だけならsemantic changeはないが、bench結果の解釈に影響するため記録する。

## Measurement Plan

実装前にbeforeを保存する。run idはissueに記録する。

### Primary gate

まず既存suiteで、実アプリ寄りと制御可能なRPC shapeの両方を見る。

- `./bench/run.sh spanner-shape`
- `./bench/run.sh metadata-header`
- `./bench/run.sh cpu-micro`
- `./bench/run.sh payload-unary --payload-sizes=0,100,1024,102400`
- `./bench/run.sh payload-streaming --streams=20 --message-count=100 --payload-sizes=0,100,1024`
- parser/queueを大きく動かす場合は `./bench/run.sh large-streaming`

`cpu-micro` はこのissueで特に重要。`small_unary_100b`、`new_client_unary_100b`、`small_streaming_1x100b`、`new_client_streaming_1x100b`、`small_streaming_100x100b` が、`grpc_call` のstack object / resource embedded object / per-call client lifecycle / streaming queueを分けて見る入口になる。

### Existing suiteで足りない観測

既存suiteだけだと、次の差は埋もれやすい。

- 0B / 100Bの極小unaryを長く回したときの、payload処理より `grpc_call` 初期化・status/header処理が支配的な差。
- server streamingのmessage countが `1`、`2`、`10`、`100` のときの、stream setup固定費とper-message parser/queue費用の分離。
- DATA frameやgRPC 5B headerがchunk境界をまたぐケース。通常のGo gRPC serverでは安定して狙いにくい。
- error/status path。`last_io_error_detail[256]` を後方へ寄せる候補では、success hot pathだけでなくerror detail構築の悪化も確認したい。
- production buildと `PHP_GRPC_LITE_ENABLE_BENCH` buildのlayout差。bench buildだけで改善に見えていないか確認が必要。

### 追加を検討するsmall bench

採否に迷う程度の差しか出ない場合は、新規suiteを増やす前に `tools/benchmark/cpu-micro.php` を拡張する。理由は、production wrapper経路のまま固定回数のCPU/callを見られ、`PHP_GRPC_LITE_ENABLE_BENCH` によるlayout差を避けられるため。

1. `cpu-micro` の引数固定run
   - 例: `./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500`
   - 目的: duration型のthroughputより、同じcall shapeを固定回数で回したCPU/requestを見る。
   - 採否: `small_unary_100b` と `small_streaming_1x100b` が両方改善し、`new_client_*` が悪化しないこと。

2. `cpu-micro` に追加する最小case
   - `tiny_unary_0b`: request/response payloadを0Bにして、`grpc_call` 初期化、header/status、stream lifecycleの固定費を見やすくする。
   - `tiny_streaming_1x0b`: server streamingのresource embedded `grpc_call` を、payload copyをほぼ消した状態で見る。
   - `small_streaming_10x100b`: 1 messageと100 messagesの中間として、parser/queue fieldsのper-message費用を見る。
   - `metadata_unary_req10_resp10_32b`: request/response metadataを伴うunaryをCPU/callで見る。`metadata-header` はlatency span中心なので、小さいlayout差の採否には `cpu-micro` 側にも置く。

3. streaming message count sweep
   - 既存 `throughput-streaming` または `payload-streaming` を `message-count=1,2,10,100` で複数runする。
   - 目的: `response_queue_*` と parser offsetsを寄せる変更がper-stream固定費に効いたのか、per-message処理に効いたのかを分ける。
   - まずは `cpu-micro` の `small_streaming_10x100b` 追加で足りるかを見る。新suiteとして `call-layout-micro` を作るのは、同一run id内のsweep管理が必要になってからでよい。

4. status/error detail micro
   - `BenchUnary` に `x-bench-error-code` と長めの `x-bench-error-message` を渡す固定回数caseを `cpu-micro` または小さい専用runnerへ追加する。
   - 目的: `last_io_error_detail` / `grpc_message` をcold寄せした副作用を見る。
   - 採否gateではなくregression smoke扱い。success path改善を打ち消すほど悪化するなら採用しない。

5. DATA fragmentation diagnostic
   - production benchmarkではなく、`PHP_GRPC_LITE_ENABLE_BENCH` の `grpc_lite_bench_unary_batch` または既存 `bench.php` 系diagnosticで、`data-frame-size` / split gRPC frameを使う。
   - 目的: `response_header_len`、`response_payload_len`、`response_payload_offset`、`response_payload` のlocalityをchunk境界で見る。
   - production layoutとbench layoutが違う場合、採否の主根拠にはしない。parser candidateの説明補助に留める。

6. layout-only diagnostic
   - ベンチではないが、`sizeof(grpc_call)` とfield offsetsをproduction/benchで保存する。
   - 目的: 数%未満の差しか出ない場合に、struct size削減・padding削減・cache line境界の変化を説明できるようにする。

見る指標:

- OTEL summaryのp50 / p90 / p99
- CPU/requestまたはwall timeだけでなく、揺れ幅
- production buildの通常path
- bench buildでlayoutが違う場合は、その差を解釈に書く

採否目安:

- 改善が測定誤差に埋もれる場合は採用しない。
- 小さい改善でも、struct size削減やlayout説明性の改善が明確なら採用候補にできる。
- regressionがある場合は、field order変更を戻して分析結果だけを残す。

## Required Verification

layout変更をcommitする場合:

- `git diff --check`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`
- PHPUnit integration
- before/after benchmark
- HTTP/2/gRPC domain model review

## Risks

- `grpc_call` はunary stack objectとserver streaming resource embedded objectの両方で使われるため、片方のworkloadだけで判断すると偏る。
- bench build layoutがproductionと異なるため、bench-only fieldを動かすと観測値の解釈が難しくなる。
- field orderだけのつもりでも、initializerやcleanup漏れが出る可能性がある。`memset(&call, 0, sizeof(call))` 前提の箇所と、個別初期化箇所を確認する。
- bool packingを狙ってbitfield化するのは別issueにする。bitfieldは読み書きコストやatomicity、debuggabilityの差があり、単純なfield order変更ではない。

## Progress

- 2026-05-31: `grpc_call` field mapの後続として作成。現時点では実装・計測なし。

## Verification

- issue作成のみ。コード変更なし。

## Decision Log

- 2026-05-31: `grpc_call` field order変更はperformance-sensitive taskとして扱う。before/afterなしに採用しない。
- 2026-05-31: 最初の候補はsemantic helper抽出やsub-struct化ではなく、layout観測とfield order評価に限定する。

## Close Criteria

- production / bench buildのlayout観測結果が記録されている。
- hot path access groupと候補layoutが記録されている。
- before/after benchmarkのrun idと判断が記録されている。
- 採用する場合は検証とdomain model reviewが完了している。
- 採用しない場合も理由をDecision Logへ記録している。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
