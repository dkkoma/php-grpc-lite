---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# user-agentをPHP metadata配列経由ではなくHTTP/2 header buildで直接付与する

## 目的

RPCごとのuser-agent付与で発生するPHP zval配列構築・metadata merge・header変換の固定費を削減する。

## 背景

現行実装は `grpc_lite_append_user_agent()` で `call->metadata` に `user-agent => [value]` を追加し、その後 `append_custom_request_headers()` がmetadata配列を走査してHTTP/2 headerへ変換する。user-agentはwire headerであり、アプリケーションmetadataとして保持する必要は薄い。

## スコープ

- channelのprimary/default user-agentをrequest header build時に直接追加する。
- ユーザー指定metadataとの禁止key/filtering semanticsを維持する。
- `user-agent` がアプリケーションmetadataとして漏れないことを維持する。

## 非スコープ

- user-agent値そのものの変更。
- call credentials metadataの仕様変更。

## 検証

- request metadata validation/filtering PHPT。
- metadata compatibility PHPUnit。
- real Spanner mixed transaction c16。

## 完了条件

- `grpc_lite_append_user_agent()` のPHP metadata配列固定費がhot pathから消える。
- CPU/request改善または非改善が記録される。
