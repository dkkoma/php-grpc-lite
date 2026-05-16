---
Status: Open
Owner: Unassigned
Created: 2026-05-17
Branch: main
---

# CallCredentials on insecure channelを拒否する

## 目的

`Grpc\CallCredentials::createFromPlugin()` で生成されたCallCredentialsが、TLSなしのinsecure channel上でauthorization metadataを送信しないようにする。

## 背景

公式ext-grpcはCallCredentials pluginを `GRPC_PRIVACY_AND_INTEGRITY` のminimum security level付きでgRPC Coreへ登録する。そのためinsecure channelでCallCredentialsを使うRPCは、`Established channel does not have a sufficient security level to transfer call credential.` 相当のエラーになる。

php-grpc-liteはgRPC Core credential systemを持たず、RPC実行直前にCallCredentials callbackを直接実行してrequest metadataへmergeしている。そのため現状では、テスト都合でinsecure test-server上でもCallCredentials callbackが動き、authorization metadataを平文h2cで送れる可能性がある。

これは公式ext-grpcとの差分として悪い方向の差であり、安全性の観点で修正対象にする。

## スコープ

- ChannelCredentialsがinsecureで、CallCredentialsが設定されたRPCを拒否する。
- callbackは呼ばない。
- RPCは送信しない。
- unary / server streaming / franken-go backendの挙動を確認する。
- TLS channel上のCallCredentialsは引き続き動作させる。
- PHPTでinsecure拒否とTLS成功を固定する。

## 非スコープ

- CallCredentials service_url cacheなどの性能最適化は行わない。
- composite credentials対応は行わない。
- google-auth token cacheやGAX側の挙動は変更しない。

## 計画

- 公式ext-grpcのstatus code / detailsを確認する。
- php-grpc-liteのstatus mappingを決める。
- `grpc_lite_merge_call_credentials_metadata()` の前段または内部でinsecure channelを拒否する。
- insecure channelではcallbackが呼ばれないことをテストする。
- TLS channelではcallbackが呼ばれ、metadataが送信されることをテストする。
- HTTP/2/gRPC domain model reviewを実施する。

## 完了条件

- insecure channel + CallCredentials がRPC送信前にエラーになる。
- callbackが呼ばれないことがテストで確認されている。
- TLS channel + CallCredentials は成功する。
- 公式ext-grpcとの差分が悪い方向に残っていないことをissueに記録する。
