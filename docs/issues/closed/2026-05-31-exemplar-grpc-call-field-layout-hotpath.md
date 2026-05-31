# お手本化: grpc_call field layout hot path最適化

- Status: Closed
- Created: 2026-05-31
- Branch: codex/exemplar-grpc-call-field-layout-hotpath
- Owner: Codex
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

## Implementation Hypothesis

最初に試す変更は、semantic helperやsub-struct化ではなく、`src/grpc_exchange_state.h` のfield orderだけに限定する。

仮説:

- `last_io_error_detail[256]` はsuccess hot pathではほぼ読まれないcold detailである。
- 現在はproduction layoutでoffset `132` にあり、parser / queue / request writer fieldsを後方へ押し出している。
- これをrequest writer blockの後ろへ移すと、response mode / queue / metadata / parser / request writer blockが前方へ寄り、tiny unary、small server streaming、metadata/header pathでcache line localityが少し改善する可能性がある。
- `memset(&call, 0, sizeof(call))` はstruct全体を触るため、改善幅は小さい可能性が高い。afterが測定誤差内なら採用しない。

採否基準:

- `cpu-micro` の `small_unary_100b`、`small_streaming_1x100b`、`small_streaming_100x100b` が悪化しない。
- `metadata-header` の代表case、`spanner-shape` の代表case、payload unary / streamingが悪化しない。
- PHPT / C unitが通る。
- HTTP/2/gRPC domain model上、ownership、status taxonomy、RST_STREAM / GOAWAY / EOF lifecycleに影響しないことを確認する。
- 改善が明確でなくても、コード複雑性が増えず、代表benchで悪化がない場合だけ採用候補にする。悪化またはノイズ内ならrevertして調査結果だけ残す。

## Before Measurements

### Layout

2026-05-31、Docker compose `dev` container、aarch64、`offsetof()` probeで確認。

| Item | Production | Bench build | Note |
|---|---:|---:|---|
| `sizeof(grpc_call)` | 688 | 1712 | bench buildは `grpc_bench_call bench` が大きい |
| `last_io_error_detail` offset | 132 | 228 | 256 bytes。success pathではcold寄り |
| response mode start | 400 | 496 | `decode_response_incrementally` |
| queue field range | 408-439 | 504-535 | `response_queue_*` |
| metadata/parser range | 440-535 | 536-631 | metadata pointers/counters + parser |
| request writer range | 552-687 | 1576-1711 | bench buildでは `bench` blockで大きく後方化 |

### Benchmark

Before run ids:

- `grpc-call-layout-before-cpu-micro-20260531`
- `grpc-call-layout-before-metadata-header-20260531`
- `grpc-call-layout-before-spanner-shape-20260531`
- `grpc-call-layout-before-payload-unary-20260531`
- `grpc-call-layout-before-payload-streaming-20260531`

代表値:

| Suite / measurement | Before |
|---|---:|
| `cpu-micro small_unary_100b` | 11.2 CPU us/call |
| `cpu-micro small_streaming_1x100b` | 11.3 CPU us/call |
| `cpu-micro small_streaming_100x100b` | 62.1 CPU us/call |
| `metadata-header req0/resp0` | p50 40.5 us, p99 279.8 us |
| `metadata-header req10/resp10` | p50 50.5 us, p99 701.8 us |
| `metadata-header req50/resp50` | p50 157.8 us, p99 1069.0 us |
| `spanner-shape begin_txn_unary` | p50 37.2 us, p99 576.7 us |
| `spanner-shape commit_txn_unary` | p50 30.1 us, p99 132.4 us |
| `spanner-shape select_1row_10col_streaming` | p50 32.6 us, p99 154.1 us |
| `payload-unary 0B` | p50 32.9 us, p99 374.0 us |
| `payload-unary 100B` | p50 30.0 us, p99 156.2 us |
| `payload-unary 100KiB` | p50 80.2 us, p99 1571.1 us |
| `payload-streaming 0B` | p50 120.7 us, p99 1537.4 us |
| `payload-streaming 100B` | p50 155.3 us, p99 403.5 us |
| `payload-streaming 1024B` | p50 160.7 us, p99 1160.5 us |

## Progress

- 2026-05-31: production / bench buildの `grpc_call` layoutを `offsetof()` probeで確認。
- 2026-05-31: 既存benchmarkでbefore計測を実施。
- 2026-05-31: 最初の実装候補を `last_io_error_detail[256]` のcold寄せに限定。
- 2026-05-31: `last_io_error_detail[256]` をrequest writer block後方へ移すlayout-only実験を実施。
- 2026-05-31: C unitとPHPTはPASSしたが、after benchmarkで主要固定費が改善せず、metadata/header代表caseも安定しなかったため、layout変更は採用せずrevert。
- 2026-05-31: 最小候補だけで閉じず、`grpc_call` 全体のfield order候補を複数試す方針へ変更。
- 2026-05-31: Candidate B-Eとbaseline repeatを実施。全体layout変更は採用しない判断にした。
- 2026-05-31: 既存suite単発runではfield layout差を検出する感度に不安が残るため、`cpu-micro` にlayout-specific caseと `--repeat-runs` を追加。
- 2026-05-31: 追加したrepeat計測でbaselineとCandidate A-Eを同条件再比較。現行layoutを上回る候補はなく、C header変更はすべてrevert。

## Trial Results

### Candidate A: `last_io_error_detail` cold tail

実験内容:

- `src/grpc_exchange_state.h` のfield orderだけを変更。
- `last_io_error_detail[256]` を `pending_write_payload_len` の後ろへ移動。
- semantic helper、ownership、status taxonomy、parser、transport actionは変更なし。

after layout:

| Item | Before production | After production | Note |
|---|---:|---:|---|
| `sizeof(grpc_call)` | 688 | 688 | sizeは不変 |
| response mode start | 400 | 144 | `decode_response_incrementally` |
| metadata/parser range | 440-535 | 184-279 | parser hot blockは前方化 |
| request writer range | 552-687 | 296-431 | request writerも前方化 |
| `last_io_error_detail` offset | 132 | 432 | cold detailを後方化 |

after benchmark run ids:

- `grpc-call-layout-after-cpu-micro-20260531`
- `grpc-call-layout-after-metadata-header-20260531`

代表比較:

| Suite / measurement | Before | After | Judgment |
|---|---:|---:|---|
| `cpu-micro small_unary_100b` | 11.2 CPU us/call | 11.4 CPU us/call | reject |
| `cpu-micro small_streaming_1x100b` | 11.3 CPU us/call | 11.5 CPU us/call | reject |
| `cpu-micro small_streaming_100x100b` | 62.1 CPU us/call | 62.1 CPU us/call | neutral |
| `cpu-micro new_client_unary_100b` | 12.9 CPU us/call | 13.3 CPU us/call | reject |
| `cpu-micro new_client_streaming_1x100b` | 13.3 CPU us/call | 13.9 CPU us/call | reject |
| `metadata-header req0/resp0` | p50 40.5 us | p50 36.2 us | better but not enough |
| `metadata-header req10/resp10` | p50 50.5 us | p50 55.9 us | reject |
| `metadata-header req50/resp0` | p50 91.9 us | p50 99.2 us | reject |
| `metadata-header req50/resp50` | p50 157.8 us | p50 164.2 us | reject |

### Candidate B: cold strings and error detail tail

狙い:

- `method_path`、`grpc_message`、`content_type`、`grpc_encoding`、`last_io_error_detail[256]` を後方へ寄せる。
- stream identity、status / validation flags、parser、queue、request writerを前方へ寄せる。

trial log:

- 初回patchでは `last_io_error_detail` の元位置削除漏れによりduplicate memberでbuild失敗。修正後にbuild成功。
- `grpc-call-layout-candidate-b-cpu-micro-20260531`: 一部改善、一部悪化。
- `grpc-call-layout-candidate-b2-cpu-micro-20260531`: 改善が再現せず、全体的にbaseline repeatと同等または悪化。

代表比較:

| Measurement | Before | Candidate B run1 | Candidate B run2 | Baseline repeat |
|---|---:|---:|---:|---:|
| `small_unary_100b` | 11.2 | 11.0 | 12.0 | 11.9 |
| `new_client_unary_100b` | 12.9 | 12.7 | 14.1 | 13.2 |
| `small_streaming_1x100b` | 11.3 | 12.3 | 12.2 | 12.0 |
| `small_streaming_100x100b` | 62.1 | 61.0 | 63.8 | 63.7 |
| `select_1row_10col_streaming` | 11.9 | 11.9 | 12.5 | 12.4 |

判断:

- run1だけ見ると一部改善に見えるが、run2で再現しない。
- 1-message streamingは一貫してbaselineより悪い。
- 効果がnoiseを超えたとは判断できないため不採用。

### Candidate C: keep `method_path`, cold result strings and error detail tail

狙い:

- `method_path` はstream identity側へ戻す。
- `grpc_message`、`content_type`、`grpc_encoding`、`last_io_error_detail[256]` だけを後方へ寄せる。

run id:

- `grpc-call-layout-candidate-c-cpu-micro-20260531`

代表値:

| Measurement | Before | Candidate C |
|---|---:|---:|
| `small_unary_100b` | 11.2 | 12.2 |
| `new_client_unary_100b` | 12.9 | 15.2 |
| `small_streaming_1x100b` | 11.3 | 12.2 |
| `small_streaming_100x100b` | 62.1 | 63.8 |

判断:

- 主要caseが明確に悪化。不採用。

### Candidate D: request writer before response parser

狙い:

- `body` / request writer / pending write blockをdeadline直後へ移し、unary send pathを前方化する。
- response queue / parser blockはその後ろに置く。

run id:

- `grpc-call-layout-candidate-d-cpu-micro-20260531`

代表値:

| Measurement | Before | Candidate D |
|---|---:|---:|
| `small_unary_100b` | 11.2 | 11.4 |
| `new_client_unary_100b` | 12.9 | 13.2 |
| `small_streaming_1x100b` | 11.3 | 12.1 |
| `small_streaming_100x100b` | 62.1 | 63.7 |

判断:

- request writerを前方化してもunary固定費は改善しない。
- streaming側も悪化。不採用。

### Candidate E: result string tail only

狙い:

- `last_io_error_detail[256]` は元位置に残す。
- `grpc_message`、`content_type`、`grpc_encoding` だけを後方へ寄せ、status / validation flagのpacking効果を見る。

run id:

- `grpc-call-layout-candidate-e-cpu-micro-20260531`

代表値:

| Measurement | Before | Candidate E |
|---|---:|---:|
| `small_unary_100b` | 11.2 | 11.4 |
| `new_client_unary_100b` | 12.9 | 13.2 |
| `small_streaming_1x100b` | 11.3 | 12.4 |
| `small_streaming_100x100b` | 62.1 | 63.5 |

判断:

- string pointerだけの移動でも改善しない。不採用。

### Baseline Repeat

候補比較中に時間帯・container状態による揺れが見えたため、元layoutへ戻してrebuildし、同じ条件で再計測した。

run id:

- `grpc-call-layout-baseline-repeat-cpu-micro-20260531`

代表値:

| Measurement | Original before | Baseline repeat |
|---|---:|---:|
| `small_unary_100b` | 11.2 | 11.9 |
| `new_client_unary_100b` | 12.9 | 13.2 |
| `small_streaming_1x100b` | 11.3 | 12.0 |
| `small_streaming_100x100b` | 62.1 | 63.7 |

判断:

- 1回ごとの数%差はrun noiseに埋もれる。
- Candidate Bの一部改善はbaseline repeatと比較しても安定しない。
- field order変更はこの時点では採用せず、layout-specific repeat計測を追加して再確認する。

### Layout-specific `cpu-micro` repeat

既存の単発 `cpu-micro` だけでは、field order差そのものよりrun noiseを見ている可能性が残った。そのため、runtime C実装へlayout変更を入れる前に、benchmark側を先に拡張した。

追加したcase:

- `tiny_unary_0b`: payload copyをほぼ消し、`grpc_call` 初期化、header/status、stream lifecycleの固定費を見やすくする。
- `tiny_streaming_1x0b`: server streaming resource内の `grpc_call` を、payload copyをほぼ消した状態で見る。
- `small_streaming_10x100b`: 1 messageと100 messagesの中間として、parser/queue fieldsのper-message費用を見る。
- `metadata_unary_req10_resp10_32b`: request/response metadataを伴うunaryをCPU/callで見る。
- `--repeat-runs=N`: 同じcaseを同一run id内で複数回測り、OTEL summaryでも `repeat` を分離して表示する。

測定条件:

- Docker compose `dev` container
- `./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500 --repeat-runs=3`
- 各候補の前に `make -j$(nproc)` で `modules/grpc.so` をrebuild
- 表はCPU us/callの3回repeat中央値。各repeat値はOTEL run idに保存され、summaryにも `repeat` 列で出る。

run ids:

- baseline: `grpc-call-layout-baseline-layout-micro-20260531`
- Candidate A: `grpc-call-layout-candidate-a-layout-micro-20260531`
- Candidate B: `grpc-call-layout-candidate-b-layout-micro-20260531`
- Candidate C: `grpc-call-layout-candidate-c-layout-micro-20260531`
- Candidate D: `grpc-call-layout-candidate-d-layout-micro-20260531`
- Candidate E: `grpc-call-layout-candidate-e-layout-micro-20260531`

代表比較:

| Measurement | Baseline | A | B | C | D | E |
|---|---:|---:|---:|---:|---:|---:|
| `tiny_unary_0b` | 11.6 | 11.7 | 17.7 | 17.9 | 17.6 | 16.1 |
| `small_unary_100b` | 11.5 | 12.0 | 16.1 | 16.0 | 15.7 | 15.5 |
| `new_client_unary_100b` | 14.1 | 14.4 | 18.5 | 18.2 | 17.7 | 17.9 |
| `metadata_unary_req10_resp10_32b` | 16.7 | 18.9 | 22.6 | 22.2 | 23.2 | 21.8 |
| `begin_txn_unary` | 11.9 | 16.1 | 15.3 | 15.8 | 16.2 | 15.5 |
| `commit_txn_unary` | 12.1 | 15.8 | 15.7 | 16.1 | 16.1 | 15.6 |
| `tiny_streaming_1x0b` | 12.5 | 15.8 | 16.0 | 16.5 | 20.5 | 16.0 |
| `small_streaming_1x100b` | 13.0 | 16.6 | 16.4 | 16.7 | 20.6 | 17.1 |
| `new_client_streaming_1x100b` | 15.2 | 19.6 | 19.2 | 19.1 | 21.2 | 18.7 |
| `small_streaming_10x100b` | 17.8 | 22.0 | 21.8 | 22.9 | 25.1 | 22.3 |
| `small_streaming_100x100b` | 67.1 | 83.8 | 79.9 | 84.8 | 84.7 | 88.2 |
| `select_1row_10col_streaming` | 13.1 | 17.2 | 16.9 | 17.7 | 17.5 | 17.0 |
| `dml_insert_10col_streaming` | 13.4 | 16.8 | 17.7 | 17.0 | 17.2 | 17.1 |
| `dml_update_10col_streaming` | 13.3 | 16.8 | 16.6 | 15.9 | 17.3 | 17.0 |
| `dml_delete_10col_streaming` | 13.5 | 16.6 | 16.0 | 16.1 | 17.1 | 17.0 |

判断:

- baseline repeatは新caseでも比較的安定しており、layout候補の悪化幅はrun noiseより大きい。
- Candidate Aは `tiny_unary_0b` だけはbaseline相当だが、metadata unary、Spanner shape相当unary、streamingが悪化する。
- Candidate B/C/Eのようにresult string pointerをtailへ寄せる案は、unary固定費とmetadata pathを一貫して悪化させる。
- Candidate Dのrequest writer前方化はsend path改善を示さず、server streamingの固定費とper-message費用を大きく悪化させる。
- 現行layoutが理論上の最適とは言い切らない。ただし、このissueで試したA-Eの範囲では、現行layoutを変更する実測根拠はない。
- 採用する変更はbenchmark拡張と判断記録のみ。`src/grpc_exchange_state.h` は元layoutへ戻す。

## Verification

- `./tools/test/check-c-unit.sh`: PASS on experimental layout
- `./tools/test/check-phpt.sh`: PASS, 15/15 on experimental layout
- `./tools/test/check-c-unit.sh`: PASS after restoring original layout
- `./tools/test/check-phpt.sh`: PASS, 15/15 after restoring original layout
- `docker compose run --rm dev php -l tools/benchmark/cpu-micro.php`: PASS
- `docker compose run --rm dev php -l tools/benchmark/otelop-summary.php`: PASS
- `BENCH_OTEL_RUN_ID=grpc-call-layout-cpu-micro-smoke-20260531 ./bench/run.sh cpu-micro --calls=2 --warmup-calls=1 --repeat-runs=2`: PASS
- `git diff --check`: PASS
- before benchmark: `cpu-micro`, `metadata-header`, `spanner-shape`, `payload-unary`, `payload-streaming`
- trial benchmark: Candidate A-E `cpu-micro`, Candidate A `metadata-header`, baseline repeat `cpu-micro`
- layout-specific repeat benchmark: baseline and Candidate A-E `cpu-micro --calls=20000 --warmup-calls=500 --repeat-runs=3`
- Layout experiments were reverted before closing this issue. No C source/header change is adopted.

## Decision Log

- 2026-05-31: `last_io_error_detail[256]` のcold寄せは、layout上はparser/request writerを大きく前方化できる。
- 2026-05-31: ただし `cpu-micro` の固定費が改善せず、一部代表caseで悪化した。metadata/headerでも代表metadata caseのp50が悪化した。
- 2026-05-31: 採否基準を満たさないため、field order変更は採用しない。現時点では `grpc_call` の現行field orderを維持し、必要なら将来の再検討ではsmall bench追加と複数runでノイズを減らしてから扱う。
- 2026-05-31: `last_io_error_detail[256]` 単独移動は採用しない。ただし、この結果だけでは全体layout見直しの判断として不足しているため、追加候補を試す。
- 2026-05-31: Candidate B-Eも試したが、改善は安定せず、悪化caseが残った。全体layout変更は採用しない。
- 2026-05-31: 今後再検討する場合は、まずbenchmark側に複数run集計またはより低ノイズなlayout-specific micro caseを追加してから行う。現行の単発runではfield order差の採否には揺れが大きい。
- 2026-05-31: 既存suite単発runだけではfield layout差の検出感度が足りない可能性があるため、このissueは閉じず、layout-specific `cpu-micro` caseと複数run比較を先に追加する。
- 2026-05-31: layout-specific `cpu-micro` caseと `--repeat-runs` を追加し、OTEL summaryでrepeatを分離表示できるようにした。
- 2026-05-31: baselineとCandidate A-Eを同条件で再測定した結果、A-Eは主要caseでbaselineより悪化。現行layoutを「理論上ベスト」とは断定しないが、このissueで採用できるfield order変更はない。
- 2026-05-31: benchmark拡張と調査記録だけを採用し、runtime C headerのlayout変更は採用しない。

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

## Close Criteria

- production / bench buildのlayout観測結果が記録されている。
- hot path access groupと候補layoutが記録されている。
- before/after benchmarkのrun idと判断が記録されている。
- 採用する場合は検証とdomain model reviewが完了している。
- 採用しない場合も理由をDecision Logへ記録している。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
