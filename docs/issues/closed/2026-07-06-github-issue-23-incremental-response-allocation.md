---
Status: Closed
Owner: Codex
Created: 2026-07-06
Related-Issue: https://github.com/dkkoma/php-grpc-lite/issues/23
Branch: codex/fix-issue-23-incremental-response-allocation
---

# response length-prefixで巨大bufferを先行確保しない

## 目的

gRPC response direct decode経路で、5 byteのmessage prefixだけを受信した時点では宣言payload長の全量を確保せず、実際に受信済みのpayload bytesに比例してbufferを伸ばす。

## 背景

GitHub issue #23で、`grpc.max_receive_message_length = -1` の場合に、悪意あるserverが巨大なlength-prefixだけを送ることで、payload bytesを送らずにmulti-GiB allocationを誘発できることが報告された。

現行の `grpc_protocol_process_response_data_direct()` はprefix parse直後に `zend_string_alloc(response_payload_len)` を実行する。unary response direct decodeとserver streamingの両方がこの経路を使うため、response message size guardが無制限の場合は5 byte入力から巨大確保に進んでしまう。

## スコープ

- `grpc_protocol_process_response_data_direct()` のpayload buffer確保を受信済みpayload量に比例させる。
- raw h2c fixtureに、巨大宣言長 + payloadなしのtruncated responseを追加する。
- PHPTでunary/server streamingのstatus taxonomyとpeak memoryを固定する。
- HTTP/2/gRPC domain model reviewを実施する。

## 非スコープ

- `grpc.max_receive_message_length = -1` の意味変更。
- non-direct response length validation経路の変更。
- libcurl fallbackやtransport selectionの追加。

## 計画

1. direct decodeのprefix parse後に全量確保せず、初期確保量を現在chunk内のpayload bytes以下にする。
2. copy直前に必要な分だけ `zend_string_realloc()` で伸ばす。
3. `poc/test-server` に巨大宣言長truncated fixtureを追加する。
4. `022-error-and-http-validation.phpt` にstatusとpeak memoryの回帰ケースを追加する。
5. 対象PHPT、C unit、静的解析を実行する。
6. HTTP/2/gRPC domain model reviewを記録する。

## 進捗

- 2026-07-06: GitHub issue #23を確認し、対象が `grpc_protocol_process_response_data_direct()` の先行確保であることを確認した。
- 2026-07-06: direct decodeのpayload buffer初期確保を現在chunk内のpayload bytes以下にし、後続DATA受信時に必要分だけ `zend_string_realloc()` で伸ばすようにした。
- 2026-07-06: `ZSTR_MAX_LEN` を超える宣言長は、32-bit buildでも安全に扱えるようmessage-too-large taxonomyへ寄せた。
- 2026-07-06: raw h2c fixtureに `x-bench-grpc-response: declared-large-truncated` を追加し、48MiB payload長を宣言してpayloadなしでtrailersへ進むケースを作った。
- 2026-07-06: PHPTでunary/server streamingのstatusが `malformed gRPC response frame` になることと、peak memory deltaが16MiB未満に収まることを固定した。
- 2026-07-06: HTTP/2/gRPC domain model reviewを実施し、Blocker / High / Medium / Low findingsなしを確認した。

## 検証

- `docker compose up -d --build test-server`
  - PASS。fixture追加後のtest-serverをrebuild/recreate。
- `./tools/test/check-phpt.sh tests/phpt/022-error-and-http-validation.phpt tests/phpt/025-resource-limits.phpt`
  - PASS。preflightを含むPHPT 16件がPASS。
- `./tools/test/check-c-unit.sh`
  - PASS。protocol_core / status_core / transport_core unit testsがPASS。
- `./tools/test/check-c-static-analysis.sh`
  - PASS。
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist`
  - PASS。31 tests / 116 assertions。
- Domain model review
  - `docs/reviews/issues/2026-07-06-issue23-incremental-response-allocation-domain-model-review-codex.md`: Blocker / High / Medium / Low findingsなし。
  - `docs/reviews/issues/2026-07-06-issue23-incremental-response-allocation-domain-self-review.md`: Blocker / High / Medium / Low findingsなし。

## 修正コミット

- PR branch: `codex/fix-issue-23-incremental-response-allocation`

## 判断ログ

- declared lengthはgRPC frame completion targetとして維持し、message count / max receive size / compression flag / read-ahead limitの検証順序は変えない。
- stream-localなmalformed/truncated responseとして扱い、connection lifecycleやHTTP/2 connection cacheの責務は変更しない。

## 完了条件

- 巨大length-prefix 5 byteだけでは巨大bufferを確保しない。
- truncated responseは従来のmalformed gRPC response frame taxonomyで失敗する。
- 正常なunary/server streaming response decodeは回帰しない。
