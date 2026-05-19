---
Status: Closed
Owner: Codex
Created: 2026-05-19
Related-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# CallCredentials由来のx-goog-api-clientを既存metrics headerへfoldする

## 目的

CallCredentials callbackが返す `x-goog-api-client: cred-type/...` を、既存のGAX由来 `x-goog-api-client` と別headerとして送らず、Google Auth PHPのmetrics header意図に合わせて1本へfoldする。

## 背景

GitHub issue #5の0.0.7 traceで、grpc-liteはSpanner RPCごとに `x-goog-api-client` を2本送っていた。

- GAX由来: `gl-php/... gccl/... gax/... grpc/...`
- CallCredentials / Google Auth由来: `cred-type/u`

`cred-type/u` はgrpc-liteが生成している値ではない。Google Auth callbackが返しているcredential type metrics tokenである。Google Auth PHPの `MetricsTrait::applyServiceApiUsageMetrics()` は、既存の `x-goog-api-client` がある場合に同keyの別valueを追加せず、既存値へ空白区切りで追記する実装になっている。

一方、grpc-liteはduplicate metadata保持修正により、CallCredentials callback由来の同名keyを一般gRPC metadataとしてappendするようになった。一般metadataでは正しいが、`x-goog-api-client` はGoogle client metrics headerとして1本へfoldするのが妥当。

## スコープ

- CallCredentials metadata merge時の `x-goog-api-client` 特別処理。
- 一般duplicate metadataの保持は維持する。
- PHPTでローカルTLS test-serverに対してwire-visible metadata shapeを検証する。

## 非スコープ

- `cred-type/u` 自体を削除すること。
- real Spanner latency差の主因と決め打ちすること。
- GAX / google-auth のPHPコード変更。

## 計画

1. `x-goog-api-client` が2値で送られる現状をRed PHPTで固定する。
2. CallCredentials merge時に `x-goog-api-client` だけ既存値へ空白区切りでfoldする。
3. generic duplicate metadataはappendのまま残ることを確認する。
4. 対象PHPTを実行する。
5. issue #5への報告材料を整理する。

## 進捗

- `023-metadata-and-call-credentials.phpt` に、request metadataの `x-goog-api-client` とCallCredentials callback由来の `cred-type/u` が1値へfoldされることを検証するケースを追加した。
- テスト追加直後、現行実装では `x-goog-api-client` が2値になりPHPTがRedになることを確認した。
- `grpc_lite_merge_metadata_append_values()` 経由のCallCredentials metadata mergeで、keyが `x-goog-api-client` の場合だけ既存値とcallback返却値を空白区切りでfoldするようにした。
- `x-bench-echo-ascii` のような一般duplicate metadataは複数値appendのまま維持した。

## 検証

- Red確認: `./tools/test/check-phpt.sh ext/grpc/tests/023-metadata-and-call-credentials.phpt`
  - `023-metadata-and-call-credentials.phpt` が期待通りFAIL。
- Green確認: `./tools/test/check-phpt.sh ext/grpc/tests/023-metadata-and-call-credentials.phpt ext/grpc/tests/020-request-metadata-control.phpt`
  - preflight/default含めPHPT 16件PASS。
- 静的解析: `./tools/test/check-c-static-analysis.sh`
  - PASS。

## 完了条件

- `x-goog-api-client` が `base cred-type/u` の1値としてサーバに届く。
- `x-bench-echo-ascii` など一般duplicate metadataは複数値としてサーバに届く。
- 対象PHPTがPASSする。

## 判断ログ

- gRPC metadata一般のduplicate key許容は維持する。
- `x-goog-api-client` はGoogle client metrics headerとして単一文字列にtokenを積む意味を持つため、CallCredentials merge時だけ特別にfoldする。
- `cred-type/u` は削除しない。Google Auth callback由来のcredential type metrics tokenとして保持する。
