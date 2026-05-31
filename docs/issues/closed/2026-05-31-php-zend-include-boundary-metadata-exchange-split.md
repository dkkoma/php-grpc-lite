# PHP/Zend include boundary: metadata and exchange state split

- Status: Closed
- Created: 2026-05-31
- Trial-Branch: codex/php-zend-metadata-exchange-split
- Decision-Branch: codex/reject-metadata-exchange-split
- Decision: Rejected
- Owner: Codex
- Parent: docs/issues/closed/2026-05-31-php-zend-include-boundary.md
- Related-Design: docs/design/php-zend-include-boundary.md

## Background

`src/h2_request_headers.h` と `src/grpc_exchange_state.h` はPHP/Zend依存を持つ。これは単純に悪いわけではない。request metadataは `zval` からHTTP/2 headerへ変換され、response payloadやmetadataは `zend_string` がownerになるためである。

一方で、HTTP/2 header storage、PHP metadata conversion、response parser state、PHP result bridgeが同じ大きなtransport周辺に集まっており、C/PHP boundaryとしてはさらに分けられる可能性がある。

## Goals

- PHP metadata conversionとHTTP/2 header storageを分けるべきか判断する。
- `grpc_exchange_state.h` のPHP-owned buffers、parser state、request writer、metadata listを分けるべきか判断する。
- 実装する場合はbefore/after benchmarkとdomain model reviewで採否を決める。

## Candidate Directions

### A. request header storage / PHP metadata conversion split

- `h2_request_headers` はnghttp2 header storageとZend string ownershipを持つ。
- `append_custom_request_headers()` は `zval` を読むPHP metadata conversionである。
- storageとconversionを分けるとboundaryはきれいになるが、function boundaryやallocation policyを変えるとrequest hot pathへ影響する。

### B. response metadata list / PHP result map split

- response metadata entryはtransport callbackで作られ、PHP result bridgeでarrayへ変換される。
- list ownershipとPHP map conversionを明確に分けられる可能性がある。
- status/details優先順位やbinary metadata decodeの互換性を壊さないことが前提。

### C. `grpc_call` exchange state sub-struct

- parser state、queue state、metadata state、request writer stateをsub-struct化する案。
- 読みやすさは上がる可能性があるが、field layout / cache localityに影響する。
- 既にfield layout issueで、効果が薄いlayout変更は採用しない判断をしている。再検討するなら明確な目的が必要。

## Performance Plan

実装前にbeforeを取る。

候補:

- `./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500 --repeat-runs=3`
- `./bench/run.sh metadata-header`
- `./bench/run.sh payload-unary --payload-sizes=0,100,1024,102400`
- `./bench/run.sh payload-streaming --streams=20 --message-count=100 --payload-sizes=0,100,1024`
- parser/queueへ触る場合は `./bench/run.sh large-streaming`

既存benchで判断できない場合は、実装前にsmall caseを追加する。

## Non-Goals

- benchmarkなしでmetadata conversionやexchange state layoutを変えない。
- `zend_string` ownershipを曖昧にしない。
- PHP metadata shape、binary metadata、status details、deadline semanticsを変えない。

## Verification

- before/after benchmark
- `git diff --check`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- affected PHPUnit integration
- HTTP/2/gRPC domain model review

## Decision Log

- 2026-05-31: このissueはinclude整理ではなくdomain boundary splitであり、performance-sensitiveとして扱う。
- 2026-05-31: `codex/php-zend-metadata-exchange-split` でsource分割を試した。request header storage、request metadata conversion、response metadata list / PHP array conversionを `transport.c` から分け、`grpc_call` field layoutとallocation policyは変えない方針にした。
- 2026-05-31: Candidate Cの `grpc_call` exchange state sub-struct化は試さなかった。field layout / cache localityへ直接影響し、既存field layout issueでも採用に足る改善が出ていないため。
- 2026-05-31: 試行結果として、metadata-heavy caseや `spanner-shape` 代表caseで一貫した大きな悪化は見えなかった。一方で、この変更は性能改善を狙うものではなく、理論上の改善余地もほぼない。translation unit分割やfunction boundary増加でhot pathが微小に悪化する可能性は残る。
- 2026-05-31: 性能リスクを重く見る判断により、source分割実装は採用しない。境界整理の方向性や計測結果は有用だが、mainへ入れる価値がhot path悪化リスクを明確に上回るとは判断しない。

## Trial Summary

`codex/php-zend-metadata-exchange-split` では次を試した。

- `src/h2_request_headers.c` を追加し、nghttp2 header storage、inline storage、owned `zend_string` list、grow/free、fixed library-owned header appendを担当させる。
- `src/request_metadata.c` を追加し、user request metadata `zval` のHashTable走査、reserved key filtering、ASCII/binary value validation、`*-bin` base64 encodeを担当させる。
- `src/response_metadata.c` を追加し、response metadata entry listの追加、metadata limit超過時のstream-local cancel、trailing mark、free、PHP array map conversionを担当させる。
- request/response共通の `*-bin` key判定を小さいinline helperへ分ける。
- `src/transport.c` はsocket/TLS/nghttp2 callback、connection lifecycle、gRPC DATA parser中心に戻す。

明示的に変えなかったもの:

- `grpc_call` field layout。
- request headerのallocation policy。
- request header order。
- `zend_string` ownership。
- PHP metadata shape、binary metadata、status/details、deadline semantics。

## Benchmark Result

before:

- `metadata-exchange-before-metadata-header-20260531`: `BENCH_OTEL_RUN_ID=metadata-exchange-before-metadata-header-20260531 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/run.sh metadata-header --calls=500`
- `metadata-exchange-before-spanner-shape-20260531`: `BENCH_OTEL_RUN_ID=metadata-exchange-before-spanner-shape-20260531 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/run.sh spanner-shape --calls=500 --warmup-calls=50`

after:

- `metadata-exchange-after2-metadata-header-20260531`: `BENCH_OTEL_RUN_ID=metadata-exchange-after2-metadata-header-20260531 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/run.sh metadata-header --calls=500`
- `metadata-exchange-after2-spanner-shape-20260531`: `BENCH_OTEL_RUN_ID=metadata-exchange-after2-spanner-shape-20260531 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/run.sh spanner-shape --calls=500 --warmup-calls=50`
- `metadata-exchange-after2-cpu-micro-20260531`: `BENCH_OTEL_RUN_ID=metadata-exchange-after2-cpu-micro-20260531 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500 --repeat-runs=3`

代表値:

| suite / measurement | before | after | judgment |
|---|---:|---:|---|
| `metadata-header req0/resp0` p50 | 39.6 us | 40.3 us | neutral |
| `metadata-header req10/resp0` p50 | 38.4 us | 46.0 us | noisy / watch |
| `metadata-header req10/resp10` p50 | 44.5 us | 42.7 us | neutral |
| `metadata-header req50/resp0` p50 | 87.3 us | 89.9 us | neutral |
| `metadata-header req50/resp50` p50 | 166.1 us | 158.9 us | neutral |
| `spanner-shape begin_txn_unary` p50 | 32.0 us | 29.2 us | neutral |
| `spanner-shape commit_txn_unary` p50 | 36.8 us | 35.8 us | neutral |
| `spanner-shape dml_insert_10col_streaming` p50 | 31.8 us | 37.3 us | noisy / watch |
| `spanner-shape select_1row_10col_streaming` p50 | 36.6 us | 33.3 us | neutral |
| `cpu-micro metadata_unary_req10_resp10_32b` | n/a | 14.9-15.3 CPU us/call | no abnormal fixed cost |

## Verification Result

trial branchで確認した内容:

- `git diff --check`: PASS
- normal build: PASS
- bench build: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- domain model self-review: Blocker / High / Medium / Low は none

## Rejection Reason

この変更は責務分離としては整理されているが、性能改善の理論値はほぼない。期待できる改善があるとすればtranslation unitやcode layoutの偶然の変化に限られ、採用理由にはしない。

一方で、source分割により小さいhot path helperのinline余地や呼び出し境界が変わる。今回のbenchでは致命的な悪化は出ていないが、`metadata-header req10/resp0` や `spanner-shape dml_insert_10col_streaming` のように注意すべき揺れもある。

このissueの目的は「お手本として納得できる境界」だが、`transport.c` からmetadata exchangeを分けるだけでは、PHP/Zend依存そのものは残る。`zend_string` ownershipを持つ以上、`h2_request_headers` とresponse metadata listは完全なpure C componentにはならない。見通し改善は限定的で、性能リスクを取ってmainへ入れるほどではない。

## Close Criteria

- 採用する場合はbefore/afterとdomain model reviewが記録されている。
- 採用しない場合も、試行理由、検証、reject理由が記録されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
