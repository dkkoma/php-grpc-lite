# PHP/Zend include boundary: metadata and exchange state split

- Status: Open
- Created: 2026-05-31
- Branch: TBD
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-php-zend-include-boundary.md
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

## Close Criteria

- 採用する場合はbefore/afterとdomain model reviewが記録されている。
- 採用しない場合も、試行理由、検証、reject理由が記録されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
