---
Status: Closed
Owner: Codex
Created: 2026-05-17
Closed: 2026-05-17
Branch: main
---

# CallCredentials on insecure channelを拒否する

## 目的

`Grpc\CallCredentials::createFromPlugin()` で生成されたCallCredentialsが、TLSなしのinsecure channel上でauthorization metadataを送信しないようにする。

## 背景

CallCredentialsはrequest metadata sourceだが、authorization tokenなどのsecretを生成・付与する用途が主である。php-grpc-liteはRPC実行直前にCallCredentials callbackを実行してrequest metadataへmergeするため、secure channel boundaryをbridge層で明示的に守る必要がある。

公式ext-grpcの詳細文字列を互換契約として模倣することは目的にしない。php-grpc-liteとしての正しいモデルは、insecure channelではCallCredentials callbackを実行せず、request metadataも生成せず、RPC/backendへ委譲せず、PHP Call surfaceへ `UNAUTHENTICATED` statusを返すことである。

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
- 公式ext-grpcのエラーdetails文字列への完全一致は要求しない。

## 実装

- `grpc_lite_call_has_credentials_plugin()` でCallCredentials callbackの有無を判定する。
- `grpc_lite_channel_is_secure()` でchannel credentialsがinsecureではないことをsecure channel条件にする。
- `grpc_lite_fail_if_call_credentials_require_secure_channel()` で、CallCredentials + insecure channelを `UNAUTHENTICATED` statusへ畳む。
- native unary / native server streaming / franken-go unary / franken-go server streaming の全入口で、metadata callback実行前かつtransport/backend委譲前にsecure channel gateを通す。

## 検証

- `./tools/test/check-phpt.sh`
  - 15/15 PASS
- `./tools/test/check-c-static-analysis.sh`
  - PASS
- `BENCH_TAG=call-credentials-smoke-20260517085842 ./bench/compare.sh spanner-shape --calls=2 --warmup-calls=1`
  - php-grpc-lite / ext-grpc のOTEL summary出力まで確認
- `BENCH_TAG=call-credentials-throughput-smoke-20260517085905 ./bench/compare.sh throughput-unary --duration=0.05 --payload-bytes=100`
  - php-grpc-lite / ext-grpc のOTEL summary出力まで確認

## レビュー

- `docs/reviews/issues/2026-05-17-call-credentials-secure-channel-domain-review.md`
- 初回レビュー: Low 1件
  - franken-go backendでinsecure CallCredentials gateが明示的に検証されていない。
- 対応:
  - `ext/grpc/tests/026-franken-go-backend.phpt` にfranken-go unary/server streamingのinsecure CallCredentialsケースを追加した。
  - callback未実行、backend `start()` 未到達、`UNAUTHENTICATED` statusを固定した。

## 完了条件

- insecure channel + CallCredentials がRPC送信前にエラーになる。完了。
- callbackが呼ばれないことがテストで確認されている。完了。
- TLS channel + CallCredentials は成功する。完了。
- 公式ext-grpcとの差分が悪い方向に残っていないことをissueに記録する。完了。

## 修正コミット

- pending
