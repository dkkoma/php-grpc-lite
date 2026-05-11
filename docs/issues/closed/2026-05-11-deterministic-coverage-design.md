# deterministic coverage design for C transport error paths

Status: Closed

## 目的

C拡張のcoverageを、timing依存やflakyな実ネットワーク挙動に頼らず引き上げる。

## 背景

現状のC line coverageは `75.6%` から `76.8%` まで改善したが、90%には追加で約436行が必要。主な未カバーは `transport.c` のsocket/TLS/nghttp2/persistent connection error branchで、通常PHPTだけでは決定的に踏みにくい。

## スコープ

- ドメインモデルに沿ったassertion policyを明記する。
- PHPTで安定再現できるpublic semanticsを追加する。
- C unitで自然に叩けるprotocol/status helperを追加する。
- fault injectionが必要な領域を分類する。

## 非スコープ

- timing依存のtimeoutテスト追加。
- OS/kernel/TLS実装の偶然に依存するfailure再現。
- coverage数値だけを目的にしたpublic API semanticsの固定。

## Assertion policy

- gRPC semantics: status code、metadata、message framing、deadline、cancel、RST/GOAWAY後のlifecycleをassertする。
- HTTP/2 semantics: connection継続可否、stream単位の失敗、flow-control/limit、ALPN h2、END_STREAMをassertする。
- PHP API semantics: 入力不正の例外、`wait()` / `responses()` / `cancel()` の公開契約をassertする。
- 内部flag、内部関数名、read/write回数、error string全文は原則assertしない。
- coverage目的のdiagnosticはpublic behaviorのassertと分離する。

## 計画

- coverage reportからPHPT/C unitで安定して踏める未カバー分岐を分類する。
- pure helperのC unitを追加する。
- 追加ケースをdomain model観点でレビューする。
- fault injectionが必要な残領域を明文化する。

## 進捗

- `transport.c` から副作用のないtransport helperを `transport_core.c` に分離した。
- `test_transport_core.c` を追加し、message size、HTTP/2 window、metadata limit、authority identity、path/channel validationをC unitで検証した。
- `022-error-and-http-validation.phpt` にunary partial frame、server streaming compressed response、unsupported encoding、invalid grpc-status、HTTP 503 fallbackを追加した。
- 追加テストのdomain model self reviewを `docs/reviews/issues/2026-05-11-deterministic-coverage-domain-review.md` に記録した。

## 検証

- `./tools/test/check-c-unit.sh`
- `docker compose run --rm dev bash -lc 'cd /workspace/ext/grpc && make -j$(nproc) >/tmp/grpc-make.log && TEST_PHP_ARGS="-q" make test TESTS="tests/022-error-and-http-validation.phpt"'`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`
- `./tools/test/check-c-static-analysis.sh`

Coverage:

- C全体: 2537/3303 lines = 76.8% → 2565/3303 lines = 77.7%
- `transport_core.c`: 80/81 lines = 98.8%
- `transport.c`: 1146/1612 lines = 71.1% → 1089/1531 lines = 71.1%
- `bridge.c`: 454/579 lines = 78.4% → 459/579 lines = 79.3%
- `server_streaming_call.c`: 178/279 lines = 63.8% → 178/279 lines = 63.8%
- `unary_call.c`: 110/145 lines = 75.9% → 110/145 lines = 75.9%

## 判断ログ

- 90%到達には、PHPTを増やすだけでなく、socket/TLS/nghttp2 failureを決定的に返すfault injection buildが必要。
- 今回はflakyを避けるため、実ネットワークのtimingやkernel buffer状態に依存するケースは追加しない。
- `transport_core.c` の `effective_max_response_metadata_bytes()` overflow guardは64bit環境では現実的に到達不能なので、coverage目的の不自然なassertは追加しない。
- `x-bench-grpc-status: abc` のserver streaming testは、payload delivery後のinvalid trailerを表すためmessage count `1` をassertする。これは「status invalidでも既に配送済みのmessageを巻き戻さない」というstreaming semanticsの確認であり、内部実装追認ではない。

## 残障害

- `transport.c` のsocket/TLS/nghttp2/persistent connection error branchは、PHPTだけでは決定的に踏めない。
- 90%近辺を狙う次ステップは、production pathとは分離したtest-only fault injection layerを設計すること。
- fault injection対象候補: `connect_tcp`、`poll_fd_until_deadline`、`connection_send`、`connection_recv`、TLS handshake/verify、`nghttp2_submit_request`、`nghttp2_session_send`、`nghttp2_session_mem_recv`。

## 完了条件

- 追加テストが通る。
- coverage差分が記録される。
- 追加テストの妥当性レビューが記録される。
- fault injectionが必要な残領域が説明される。

## 修正コミット

このコミット。
