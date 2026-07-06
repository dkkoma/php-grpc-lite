---
Status: Closed
Owner: Codex
Created: 2026-07-06
Related-Issue: https://github.com/dkkoma/php-grpc-lite/issues/22
Branch: codex/fix-issue-22-empty-x-goog-api-client
---

# 空のx-goog-api-client CallCredentials metadataで落ちないようにする

## 目的

CallCredentials callbackが `x-goog-api-client` に空配列を返した場合でも、metadata fold処理がNULL `zend_string`をPHP配列へ追加せず、プロセスを落とさないようにする。

## 背景

GitHub issue #22で、`grpc_lite_fold_x_goog_api_client()` が空配列をfoldすると `smart_str` の実体が確保されないまま `add_next_index_str()` に渡され、NULL pointer dereferenceでSIGSEGVすることが報告された。

`x-goog-api-client` はGoogle API client metrics headerとして、CallCredentials metadata merge時だけ空白区切りの1値へfoldする例外扱いをしている。空配列はその例外経路の入力として不正ではなく、一般metadataの空配列と同じく送出header 0本として扱えればよい。

## スコープ

- `grpc_lite_fold_x_goog_api_client()` のNULL-safe化。
- CallCredentials callbackが空配列を返すPHPT回帰テスト。
- 既存の非空 `x-goog-api-client` foldingと一般duplicate metadata appendの維持。

## 非スコープ

- `x-goog-api-client` folding仕様そのものの変更。
- 一般metadataの空配列入力仕様変更。
- HTTP/2 transportの変更。

## 計画

1. `smart_str` が空のままならfold成功にせず、一般metadata merge経路へ戻す。
2. `023-metadata-and-call-credentials.phpt` に空配列のCallCredentialsケースを追加する。
3. 対象PHPTと静的解析を実行する。

## 進捗

- 2026-07-06: GitHub issue #22を確認し、対象箇所と既存PHPTを特定した。
- 2026-07-06: `grpc_lite_fold_x_goog_api_client()` で空fold結果を `add_next_index_str()` に渡さず、一般metadata merge経路へ戻すようにした。
- 2026-07-06: `023-metadata-and-call-credentials.phpt` に、CallCredentials callbackが `['x-goog-api-client' => []]` を返すTLS RPCケースを追加し、送出headerが0本になることを確認した。
- 2026-07-06: PR #24コメントを受け、空fold結果を空文字header 1本ではなく一般metadata空配列と同じ0本送出へ変更した。

## 検証

- `./tools/test/check-phpt.sh tests/phpt/023-metadata-and-call-credentials.phpt`
  - PASS。preflightを含むPHPT 16件がPASS。
- `./tools/test/check-c-static-analysis.sh`
  - PASS。

## 修正コミット

- PR branch: `codex/fix-issue-22-empty-x-goog-api-client`

## 判断ログ

- 空fold結果を空文字header 1本として送ると、一般metadataの空配列がheader 0本になる挙動と食い違う。`x-goog-api-client` foldingは非空tokenを1本に畳むための例外であり、中身が無い場合は一般metadata mergeに委ねる。

## 完了条件

- 空配列の `x-goog-api-client` CallCredentials metadataでSIGSEGVせず、空値headerを送らない。
- 非空の `x-goog-api-client` foldingが従来通り1値になる。
- `023-metadata-and-call-credentials.phpt` がPASSする。
