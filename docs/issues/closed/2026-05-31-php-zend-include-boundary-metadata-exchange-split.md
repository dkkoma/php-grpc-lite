# PHP/Zend include boundary: metadata and exchange state split

- Status: Closed
- Created: 2026-05-31
- Branch: codex/php-zend-metadata-exchange-split
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
- 2026-05-31: 実装前方針を固定する。A/Bはまずsource boundary splitとして試す。`h2_request_headers` のstorageとrequest metadata `zval` conversionを別source/headerへ分け、response metadata listとPHP array conversionもtransport本体から分ける。既存のallocation policy、`zend_string` ownership、public function boundaryはできるだけ変えない。
- 2026-05-31: Cの `grpc_call` sub-struct化はこのissueでは試さない。field layout / cache localityへ直接影響し、既存field layout issueでも採用に足る改善が出ていないため、明確なruntime目的が出るまでreject寄りの判断にする。
- 2026-05-31: 採用基準は、`metadata-header` 代表caseと `spanner-shape` 代表caseで構造的な悪化が見えないこと。source splitによる見通し改善は必要条件だが、metadata-heavyやsmall unaryで一貫した悪化が見える場合は採用しない。

## Trial Plan

### Candidate A: request header storage / PHP metadata conversion split

- `h2_request_headers` はnghttp2へ渡すheader storage、inline capacity、Zend string owner listを持つ。
- `append_custom_request_headers()` はuser metadata `zval` を読み、key/value validation、binary metadata base64 encode、reserved header除外を行う。
- 試す変更:
  - storage操作は `src/h2_request_headers.*` へ置く。
  - PHP metadata conversionは `src/request_metadata.*` へ置く。
  - 呼び出し元は `append_custom_request_headers()` を従来どおり呼ぶ。
  - allocation policyとheader順序は変えない。

### Candidate B: response metadata list / PHP result map split

- response metadata listはtransport callbackで蓄積し、unary/server streaming result bridgeでPHP arrayへ変換する。
- 試す変更:
  - response metadata entry listの追加、trailing mark、free、PHP map copyを `src/response_metadata.*` へ分ける。
  - `grpc_call` field layoutは変えない。
  - status/details優先順位、binary metadata decode、metadata limit処理は変えない。

### Candidate C: `grpc_call` exchange state sub-struct

- 今回は実装しない。
- parser state、queue state、metadata state、request writer stateのsub-struct化は教材として読みやすくなる可能性はあるが、field layoutとhot path accessを変える。
- 採用判断にはより強い目的とlayout-specific benchmarkが必要。

## Implementation

- `src/h2_request_headers.c` / `src/h2_request_headers.h`:
  - nghttp2 header storage、inline storage、owned `zend_string` list、grow/free、fixed library-owned header appendを担当する。
  - `append_owned_request_header()` をstorage APIとして追加し、request metadata conversion側からcapacity arrayの実装詳細を隠した。
- `src/request_metadata.c` / `src/request_metadata.h`:
  - user request metadata `zval` のHashTable走査、reserved key filtering、ASCII/binary value validation、`*-bin` base64 encodeを担当する。
  - `user-agent` はlibrary-owned headerとして従来どおりuser metadataから送らない。
- `src/response_metadata.c` / `src/response_metadata.h`:
  - response metadata entry listの追加、metadata limit超過時のstream-local cancel、trailing mark、free、PHP array map conversionを担当する。
  - response metadata listは `zend_string` ownerなのでpure C化しない。
- `src/metadata_key.h`:
  - request/responseで共通に使う `*-bin` key判定を小さいinline helperとして分離する。
- `src/transport.c`:
  - socket/TLS/nghttp2 callback、connection lifecycle、gRPC DATA parser中心に戻した。
  - `grpc_call` field layoutは変更していない。
- `config.m4` と `tools/test/check-c-static-analysis.sh`:
  - 新しいproduction C sourceを通常buildとbench buildの両方へ追加した。
- `docs/guides/code-reading-guide.md`:
  - metadata boundary split後の読み順と責務を反映した。

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

判断:

- `metadata-header` のrequest-only caseはrun間の揺れが残るが、metadata-heavy response caseや `spanner-shape` 代表caseで一貫した構造的悪化は見えない。
- `cpu-micro` のmetadata付きunaryは 15us/call前後で、既存のmetadata付き固定費として不自然な悪化は見えない。
- 今回の目的は性能改善ではなくdomain boundary整理であり、field layoutやallocation policyは変えていないため採用する。

## Review Result

- Domain model review: `docs/reviews/issues/2026-05-31-metadata-exchange-split-domain-self-review.md`
- Result: Blocker / High / Medium / Low は none。

## Verification Result

- `git diff --check`: PASS
- 通常build: `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-metadata-split-build2-distclean.log 2>&1 || true; rm -rf .libs modules *.lo *.o *.dep src/*.lo src/*.o src/*.dep src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep Makefile config.h config.log config.status autom4te.cache include; phpize >/tmp/grpc-metadata-split-build2-phpize.log; ./configure --enable-grpc >/tmp/grpc-metadata-split-build2-configure.log; make -j$(nproc) >/tmp/grpc-metadata-split-build2-make.log'`: PASS
- bench build: `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-metadata-split-final-bench-distclean.log 2>&1 || true; rm -rf .libs modules *.lo *.o *.dep src/*.lo src/*.o src/*.dep src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep Makefile config.h config.log config.status autom4te.cache include; phpize >/tmp/grpc-metadata-split-final-bench-phpize.log; ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-metadata-split-final-bench-configure.log; make -j$(nproc) >/tmp/grpc-metadata-split-final-bench-make.log; php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15

## Close Criteria

- 採用する場合はbefore/afterとdomain model reviewが記録されている。
- 採用しない場合も、試行理由、検証、reject理由が記録されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
