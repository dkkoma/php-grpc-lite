---
Status: Open
Owner: Codex
Created: 2026-05-19
Related-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# duplicate request metadataをgRPC metadataとして保持する

## 目的

同じmetadata keyに複数valueを持つgRPC request metadataを、php-grpc-liteの送信経路で落とさずHTTP/2 headersへ反映する。

## 背景

GitHub issue #5のreal Spanner `Spanner/Commit` wire shape比較で、ext-grpc 1.58は `x-goog-api-client` を2値として送っている一方、php-grpc-liteはCallCredentials plugin由来の `x-goog-api-client: cred-type/u` を送っていないことが分かった。

現状の `grpc_lite_merge_call_credentials_metadata()` は、plugin返却metadataを `zend_hash_merge(..., overwrite = 0)` で既存metadataへmergeしている。PHP HashTableのstring keyは同名keyを複数保持できないため、既に `x-goog-api-client` がある場合、plugin側の同名keyが追加されない。

これはgRPC metadataのモデルとずれている。gRPC metadataは同名keyの複数値を許すため、PHP内部表現でも値配列として保持し、送信時に各valueを個別headerとしてemitする必要がある。

## スコープ

- CallCredentials plugin返却metadataと既存request metadataのmerge処理。
- 同名keyの複数value保持。
- HTTP/2 request headersへのduplicate metadata emission。
- `x-goog-api-client` のような通常metadataを対象にする。
- 既存のreserved metadata filtering / validationは維持する。

## 非スコープ

- 公式ext-grpcの文字列や実装の模倣。
- latency差の主因と決め打ちすること。
- response metadata側のduplicate handling変更。ただし影響が見つかった場合は別issue化する。
- `user-agent` / `grpc-accept-encoding` の方針決定。

## 計画

1. 現在のrequest metadata内部表現を確認し、duplicate valueを保持できない箇所を洗い出す。
2. CallCredentials plugin返却metadataを既存metadataへ追加するとき、同名keyを値配列としてappendする。
3. `append_custom_request_headers()` が配列値を個別HTTP/2 headerとしてemitすることを確認し、不足があれば修正する。
4. PHPTで同名metadata keyが複数送信されることを検証する。
5. real Spanner `Spanner/Commit` wire shapeで `x-goog-api-client: cred-type/u` が送信されることを確認する。
6. HTTP/2/gRPC domain model reviewを実施する。

## 検証

- `./tools/test/check-phpt.sh ext/grpc/tests/023-metadata-and-call-credentials.phpt ext/grpc/tests/022-error-and-http-validation.phpt`
  - 実際にはpreflightによりPHPT 15件が実行され、全件PASS。

## 進捗

- `grpc_lite_merge_call_credentials_metadata()` で同名keyを上書き/無視せず、既存値をarray化してplugin返却値をappendするようにした。
- `023-metadata-and-call-credentials.phpt` に、request metadataとCallCredentials pluginが同じ `x-bench-echo-ascii` keyを返すケースを追加した。
- `grpc-accept-encoding` は別差分として計測したが、本issueの修正対象には含めない。現時点ではcompression未対応のため、`identity, deflate, gzip` をproduction defaultで送る判断はしない。

## 完了条件

- CallCredentials plugin由来のduplicate metadataが落ちない。
- `x-goog-api-client` のbase valueと `cred-type/u` がどちらも送信される。
- 既存metadata validation / filteringが壊れていない。
- PHPTとdomain model reviewが通る。
