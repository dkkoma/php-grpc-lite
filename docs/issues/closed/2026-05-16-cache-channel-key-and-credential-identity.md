---
Status: Closed
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

## 進捗

- `Grpc\\Channel` に `connection_key` を追加し、Channel construction時に一度だけpersistent connection keyを生成するようにした。
- unary / server streaming / `Channel::close()` はcached keyを使うようにした。
- RPCごとのdefault roots PEM hashとkey string allocationを削除した。

## 検証結果

- Build: `docker compose run --rm dev sh -lc 'cd /workspace/ext/grpc && phpize >/tmp/grpc-key-phpize.log && ./configure --enable-grpc >/tmp/grpc-key-configure.log && make -j$(nproc) >/tmp/grpc-key-make.log && php -n -d extension=/workspace/ext/grpc/modules/grpc.so -r \"echo phpversion(\\\"grpc\\\"), PHP_EOL;\"'`
- PHPT: `docker compose run --rm dev sh -lc 'cd /workspace/ext/grpc && TEST_PHP_ARGS=\"-q\" make test TESTS=\"tests/004-object-lifecycle.phpt tests/010-unary.phpt tests/011-server-streaming.phpt tests/030-tls.phpt\"'`
- PHPT result: 4 passed, 0 failed.
- Benchmark: real Spanner / Laravel FPM / `transaction_select2_update1_insert1` / 16 workers / 4 CPU limit / concurrency 16 / 256 requests.

| variant | cpu_us/req | ratio vs ext-grpc optimized |
|---|---:|---:|
| before: grpc-lite default | 12909.9 | 1.24x |
| after: grpc-lite key cache | 11431.5 | 1.15x |
| ext-grpc 1.58 optimized | 9932.8 | 1.00x |

判断:

- CPU/requestは `12909.9us` から `11431.5us` へ約11.5%改善した。
- ext-grpc optimizedとの差は `1.24x` から `1.15x` まで縮小した。
- この修正候補は採用する価値が高い。

## レビュー

- `docs/reviews/issues/2026-05-16-channel-key-cache-domain-self-review.md`

## 完了条件

- RPCごとのPEM全体hashがなくなる。
- CPU/request改善または非改善が記録される。
