---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# RPCごとのchannel key生成とcredential PEM hashをキャッシュする

## 目的

`grpc_lite_channel_key()` のRPCごとの文字列生成とcredential PEM hashを削減し、real Spanner mixed transactionのCPU/requestを下げる。

## 背景

`vendor/grpc/grpc/etc/roots.pem` は 264,440 bytes。GAX/BaseStubはdefault rootsを `ChannelCredentials::setDefaultRootsPem()` に設定し、`ChannelCredentials::createSsl()` はdefault rootsをcredentialsに持つ。現行実装はunary / server streaming開始時に毎回 `grpc_lite_channel_key()` を呼び、`root_certs` / `cert_chain` / `private_key` を毎回hashしてpersistent connection keyを作る。

## スコープ

- `ChannelCredentials` または `Channel` にcredential identity hashを保持する。
- `Channel` 初期化時にpersistent connection keyを一度だけ生成し、RPC開始時はコピーまたは参照だけにする。
- `Channel::close()` も同じcached keyを使う。

## 非スコープ

- connection poolの設計変更。
- key semanticsの変更。
- TLS/mTLS検証挙動の変更。

## 検証

- PHPT / PHPUnit。
- real Spanner mixed transaction c16で `grpc-lite default` と比較。
- channel keyにroot/cert/private keyの内容差が反映されることを既存テストまたは追加テストで確認する。

## 完了条件

- RPCごとのPEM全体hashがなくなる。
- CPU/request改善または非改善が記録される。
