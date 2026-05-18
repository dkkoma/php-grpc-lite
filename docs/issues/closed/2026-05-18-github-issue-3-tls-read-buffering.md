---
Status: Closed
Owner: Codex
Created: 2026-05-18
GitHub-Issue: https://github.com/dkkoma/php-grpc-lite/issues/3
---

# GitHub issue #3: TLS read buffering

## 目的

TLS有効時の受信で、TLS record header 5 bytes + body exact length の2段読みが発生し、1 responseあたりread syscallが増える問題を調査・修正する。

## 背景

GitHub issue #3では、php-grpc-lite 0.0.4 がTLS recordごとにheader/bodyを分けてreadし、1 unary responseで6 read syscallが観測されたと報告されている。公式ext-grpcは大きめbufferで1〜2 recvmsgにまとまっている。

現行コードは `connection_recv()` が呼び出し側のbuffer長で `SSL_read()` を呼ぶ構造に見える。unaryは16KiB、server streamingは64KiB bufferを渡しているため、報告された5B exact readが現行コード由来か、OpenSSL内部BIOのsocket read挙動か、旧実装/別経路かを切り分ける。

## スコープ

- `connection_recv()` とOpenSSL read pathの実挙動を確認する。
- 呼び出し側bufferサイズとOpenSSL内部read syscall粒度の関係を確認する。
- 必要ならread buffering / read-ahead / buffer size調整を実装する。
- PHPT / static analysis / 主要ベンチ smokeを通す。

## 非スコープ

- OpenSSL内部の全面置換。
- BoringSSL/C-core移植。
- server側の挙動変更。

## 計画

- `transport.c` の `connection_recv()` と呼び出し側を読む。
- TLSで5B header readがアプリコード起因かOpenSSL内部起因かを確認する。
- 8KiB/16KiB以上のbufferで `SSL_read()` が既に呼ばれている場合、追加で改善可能な層を整理する。
- 実装可能ならread syscall削減を試す。

## 完了条件

- TLS受信syscall分割の発生層を説明できる。
- 実装可能なら改善を入れ、効果を計測する。
- 見送る場合は理由と次の調査手段を記録する。

## 実装メモ

- `SSL_set_read_ahead(connection->ssl, 1)` をTLS connectionに設定する。
- `connection_recv()` の呼び出し側bufferは既にunary 16KiB、server streaming 64KiBであるため、アプリケーション側は小さいexact readを要求していない。
- OpenSSLにTLS record単位を超えたsocket read-aheadを許可し、kernel buffer内の複数recordをまとめて取り込めるようにする。
- `SSL_MODE_AUTO_RETRY` は設定しない。php-grpc-lite側のnonblocking `SSL_read` + poll/deadline制御を維持する。

## before / after

### Before

GitHub issue #3の報告をbefore観測として採用する。

- 対象: `dkkoma/php-grpc-lite:0.0.4`, real Google Cloud Spanner TLS gRPC
- 現象: 1 unary responseでTLS record header/bodyの2段readがrecordごとに発生
- 例: `read(..., 5)`, `read(..., 68)`, `read(..., 5)`, `read(..., 359)`, `read(..., 5)`, `read(..., 61)`

### After smoke

ローカルTLS test-serverのwarm channel上で1 RPCをstraceした。

- command: `strace -e trace=read,write,sendmsg,recvmsg php -d extension=/workspace/ext/grpc/modules/grpc.so /workspace/var/tmp/tls-unary-once.php`
- 観測: measure RPCのresponse受信は `read(4, ..., 16713) = 190` 1回で、5B header + body exact lengthの2段readは発生しなかった。
- 最初に `read(...)=EAGAIN` が出るが、これはnonblocking socketでresponse到着前にpollへ進む通常経路。

## 検証

- `./tools/test/check-phpt.sh`: 15/15 PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `php -l tools/benchmark/cpu-micro.php && php -l tools/benchmark/spanner-shape.php`: PASS
- `./bench/compare.sh throughput-unary --duration=0.05 --payload-bytes=100`: php-grpc-lite / ext-grpc のOTEL summary出力まで確認
- `./bench/compare.sh spanner-shape --calls=2 --warmup-calls=1`: php-grpc-lite / ext-grpc のOTEL summary出力まで確認
- `./bench/run.sh tls-spanner-shape --calls=2 --warmup-calls=1`: TLS Spanner shape suiteの動作確認
- `./bench/run.sh tls-cpu-micro --calls=20 --warmup-calls=2`: TLS CPU micro suiteの動作確認

### 0.0.4 vs current

`var/tag-so/0.0.4/grpc.so` と現在の `ext/grpc/modules/grpc.so` を同じDocker compose環境で比較した。対向はGo test-server TLS port `50052`。

`tls-cpu-micro --calls=1000 --warmup-calls=100`:

| measurement | 0.0.4 cpu us/call | current cpu us/call | 0.0.4 wall us/call | current wall us/call |
| --- | ---: | ---: | ---: | ---: |
| small_unary_100b | 28.3 | 12.9 | 80.9 | 36.8 |
| new_client_unary_100b | 24.8 | 16.3 | 59.0 | 43.1 |
| begin_txn_unary | 14.9 | 14.5 | 39.4 | 42.4 |
| commit_txn_unary | 17.3 | 13.0 | 45.8 | 38.2 |
| small_streaming_1x100b | 17.4 | 14.4 | 43.1 | 41.2 |
| new_client_streaming_1x100b | 20.2 | 16.3 | 49.1 | 41.2 |
| small_streaming_100x100b | 71.7 | 61.3 | 173.7 | 153.0 |
| select_1row_10col_streaming | 17.9 | 14.4 | 45.5 | 45.1 |
| dml_insert_10col_streaming | 19.3 | 14.7 | 49.8 | 42.2 |
| dml_update_10col_streaming | 18.9 | 13.7 | 48.8 | 39.0 |
| dml_delete_10col_streaming | 16.3 | 13.8 | 42.2 | 40.6 |

`tls-spanner-shape --calls=500 --warmup-calls=50`:

| measurement | 0.0.4 p50 us | current p50 us | 0.0.4 p99 us | current p99 us |
| --- | ---: | ---: | ---: | ---: |
| begin_txn_unary | 40.1 | 35.1 | 435.9 | 104.7 |
| commit_txn_unary | 43.3 | 34.3 | 660.2 | 98.0 |
| select_1row_10col_streaming | 39.2 | 32.3 | 158.0 | 85.3 |
| dml_insert_10col_streaming | 36.4 | 34.7 | 91.4 | 112.2 |
| dml_update_10col_streaming | 41.2 | 32.5 | 146.2 | 100.0 |
| dml_delete_10col_streaming | 37.0 | 40.8 | 159.7 | 132.9 |

## 判断

- Status: Closed
- 判断: 採用する。アプリケーション側は小さいexact readを要求しておらず、OpenSSL read-ahead有効化後のローカルTLS straceではresponse受信が1回のsocket readにまとまった。
- 備考: `SSL_MODE_AUTO_RETRY` は入れない。nonblocking `SSL_read` + poll/deadlineの制御責務をphp-grpc-lite側に残す。
