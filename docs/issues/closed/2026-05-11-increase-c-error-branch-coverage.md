# increase C extension error branch PHPT coverage

Status: Closed

## 目的

`server_streaming_call.c`、`unary_call.c`、`transport.c` の簡単に踏める error branch をPHPTで追加し、Codecov上の低カバレッジを改善する。

## 背景

`check-c-coverage.sh` の実測で、対象3ファイルは以下のline coverageだった。

- `server_streaming_call.c`: 166/279 lines = 59.5%
- `transport.c`: 1146/1612 lines = 71.1%
- `unary_call.c`: 107/145 lines = 73.8%

未カバーにはsocket/TLS/nghttp2 fault injectionが必要な分岐もあるが、入力validation、metadata境界、raw fixtureで作れるHTTP/2応答異常、server streaming lifecycleはPHPTで追加できる。

## スコープ

- PHPTで自然に踏めるerror pathを追加する。
- 必要ならGo test-server raw fixtureを小さく拡張する。
- 追加テストの妥当性を自己レビューして記録する。

## 非スコープ

- C fault injection専用機構の追加。
- TLS/SSL内部エラーの人工注入。
- coverage目標値の固定。

## 計画

- 未カバー行をPHPTで踏める候補に分類する。
- PHPTとfixtureを追加する。
- `check-phpt.sh` と `check-c-coverage.sh` を実行する。
- 追加ケースが仕様・実装責務に対して妥当かレビューする。

## 進捗

- `020-request-metadata-control.phpt` にrequest metadata value型エラー、metadata value数上限、server streaming側metadata validationを追加した。
- `024-control-semantics.phpt` にserver streaming connection refused status化を追加した。
- `024-control-semantics.phpt` のEOF fixture確認を、fixtureのglobal parityに依存しない形へ修正した。
- server streaming cancelのidempotenceを確認するケースを追加した。
- scalar metadata正常系はAPI surfaceとして曖昧なため採用しない判断にした。
- 追加テストのdomain/error semantics self reviewを `docs/reviews/issues/2026-05-11-c-error-branch-phpt-self-review.md` に記録した。

## 検証

- `docker compose run --rm dev sh -lc 'cd ext/grpc && TEST_PHP_ARGS="-q" make test TESTS="tests/020-request-metadata-control.phpt tests/024-control-semantics.phpt"'`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`

Coverage:

- C全体: 2497/3303 lines = 75.6% → 2537/3303 lines = 76.8%
- `server_streaming_call.c`: 166/279 lines = 59.5% → 178/279 lines = 63.8%
- `transport.c`: 1146/1612 lines = 71.1% → 1146/1612 lines = 71.1%
- `unary_call.c`: 107/145 lines = 73.8% → 110/145 lines = 75.9%

## 判断ログ

- まずはfault injectionなしで踏める分岐だけを対象にする。

## 完了条件

- 追加PHPTが通る。
- C coverageが再計測される。
- 追加ケースの妥当性レビューが記録される。

## 修正コミット

このコミット。
