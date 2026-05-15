---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# request metadata build を1-pass化する

## 目的

custom request metadataを `count_custom_header_values()` と `append_custom_request_headers()` で2回走査する固定費を削減する。

## 背景

現行実装はheader配列capacityを事前確保するためにmetadataを先に数える。inline/grow可能なheader bufferへ変更できれば、事前countをなくし、validationとappendを1 passにできる。

## スコープ

- unary / server streaming の `count_custom_header_values()` 呼び出しをなくす。
- metadata value数上限はappend時に維持する。

## 非スコープ

- metadata key/value validation仕様の変更。
- binary metadata encoding仕様の変更。

## 計画

1. `init_request_headers()` をcustom count不要にする。
2. `append_custom_request_header_value()` で最大値とcapacityを検査する。
3. request metadata PHPTで互換性を確認する。

## 進捗

- `count_custom_header_values()` を削除した。
- `init_request_headers()` はcustom metadata数を受け取らず、inline bufferで開始する。
- `append_custom_request_header_value()` がappend時に `GRPC_LITE_MAX_REQUEST_METADATA_VALUES` とcapacityを検査する。
- unary / server streaming / bench direct APIの全call siteを1-pass buildへ変更した。

## 検証

- PHPT: request metadata制御、binary metadata、metadata limitを含むtarget PHPTがPASS。
- Metadata header comparison:

| case | php-grpc-lite p50 | ext-grpc p50 | note |
| --- | ---: | ---: | --- |
| `metadata_header_req_10_resp_0_value_32b` | 69.3µs | 119.2µs | request metadata 10 values |
| `metadata_header_req_50_resp_0_value_32b` | 136.1µs | 147.0µs | request metadata 50 values |
| `metadata_header_req_50_resp_50_value_32b` | 192.7µs | 283.6µs | request/response metadata heavy |

## 判断

- 2-pass走査は削除できた。
- 通常small RPCでの改善は明確ではないが、metadata-heavy p50では少なくとも悪化していない。
- metadata上限、validation、binary metadata semanticsは維持する。

## 完了条件

- production経路でcustom metadataの事前count走査がなくなる。
- metadata制御テストが通る。

## 完了

- 完了条件を満たしたためClosed。
- 修正コミット: this commit
