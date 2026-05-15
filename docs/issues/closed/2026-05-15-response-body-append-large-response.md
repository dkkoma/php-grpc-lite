---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# response body append構造の改善余地を再確認する

## 目的

unary large responseで `smart_str_appendl()` によるbody assemblyがCPU/コピー固定費になっていないか再確認する。

## 背景

過去のlarge response調査ではuserlandよりlibcurl/nghttp2/transport側の構造が焦点だったが、現在はnghttp2 native transportに一本化されている。small/mixed transactionでは主因ではないが、確度中の改善候補として再確認する。

## スコープ

- unary response body assemblyの現在実装を確認する。
- 既存large response benchで回帰/改善余地を確認する。
- 明確な無駄があれば修正する。

## 非スコープ

- protobuf decode最適化。
- PHP wrapper API変更。

## 計画

1. `smart_str` append箇所とpayload extractionのcopy回数を確認する。
2. large response系の既存benchで影響を確認する。
3. 採用可能な改善があれば実装する。

## 進捗

- unary responseで1 messageを直接payloadとして返し、`smart_str` body assemblyとpayload extraction copyを避けるPoCを実装した。
- partial frame / malformed responseのPHPTで破綻しないように検証した。
- small unary hot pathへの悪影響が見えたため、production codeからPoCを戻した。

## 検証

- PoC適用時の `cpu-micro`:

| case | PoC native CPU/call | note |
| --- | ---: | --- |
| `small_unary_100b` | 17.9µs | prior観測12.7µs近辺から悪化 |
| `new_client_unary_100b` | 13.6µs | 同等 |
| `begin_txn_unary` | 12.7µs | 小幅悪化 |

- PoCを戻した後:

| case | reverted native CPU/call | note |
| --- | ---: | --- |
| `small_unary_100b` | 14.0µs → header init調整後13.5µs | 悪化は縮小 |
| `new_client_unary_100b` | 13.7µs → 12.6µs | 同等〜小幅改善 |

## 判断

- direct unary payloadはcopy削減として理屈上は有効だが、small unary固定費を悪化させた。
- このリポジトリの主要ワークロードではsmall unary / small streamingの優先度が高いため採用しない。
- large response専用最適化は、small pathに分岐・状態を増やさずに実装できる設計が出るまで保留する。

## 完了条件

- large response appendが現時点の修正対象か判断できる。

## 完了

- 現時点では採用しない判断を記録したためClosed。
- 修正コミット: this commit
