---
Status: Closed
Owner: Codex
Created: 2026-05-18
GitHub-Issue: https://github.com/dkkoma/php-grpc-lite/issues/2
---

# GitHub issue #2: TLS write coalescing

## 目的

TLS有効時のunary RPC送信で、HTTP/2 frameごとに `SSL_write` が発生し、1 RPCあたり複数TLS record / write syscallになる問題を調査・修正する。

## 背景

GitHub issue #2では、php-grpc-lite 0.0.4 が小さなunary RPCで3回のTLS app-data writeを行い、公式ext-grpc 1.58は1回のsendmsg相当にまとまっていると報告されている。Spanner実ワークロードで約16% throughput低下、Spanner-heavy pathで約10〜32% latency増が観測されている。

既存履歴には `2026-05-15-native-h2-write-coalescing.md` があるが、0.0.4実運用straceではTLS record分割が残っているため、現行transportの送信経路を再確認する。

## スコープ

- nghttp2 send callbackからTLS writeまでの送信経路を確認する。
- 1 RPCのHTTP/2 framesを可能な範囲でまとめ、TLS write回数を削減する。
- TLSだけでなくh2cにも悪影響がないことを確認する。
- PHPT / static analysis / 主要ベンチ smokeを通す。

## 非スコープ

- 公式ext-grpcの実装模倣。
- BoringSSL/C-core移植。
- request metadataやCallCredentialsの別最適化。

## 計画

- `transport.c` の `send_callback` と `connection_write` 相当を読む。
- nghttp2が複数回呼ぶsend callbackをconnection-local bufferへcoalesceできるか確認する。
- flush境界を `nghttp2_session_send()` 呼び出し単位に置けるか確認する。
- 実装後、syscall/ベンチ観測を行う。

## 完了条件

- TLS unary送信でHTTP/2 frameごとの `SSL_write` が残るかどうかを説明できる。
- 実装可能ならcoalescingを導入し、効果を計測する。
- 見送る場合は理由と次の調査手段を記録する。

## 実装メモ

- `send_pending_h2_frames()` の1回の `nghttp2_session_send()` をflush境界にする。
- TLS connectionのみ `send_callback()` 由来の小さいHTTP/2 frame bytesをconnection-local bufferへ集約する。
- h2cではコピー固定費を増やさないためcoalescingしない。
- 16KiBを超える大きいcallback payloadは先にbufferをflushして直接writeする。large requestで余分なcopyを増やさない。
- write bufferは `h2_connection` のpersistent flagに合わせて `pemalloc` / `pefree` する。

## before / after

### Before

GitHub issue #2の報告をbefore観測として採用する。

- 対象: `dkkoma/php-grpc-lite:0.0.4`, PHP 8.4.21 NTS, real Google Cloud Spanner TLS gRPC
- 現象: 1 unary RPC requestでTLS app data writeが3回
- 例: `write(..., 39)`, `write(..., 709)`, `write(..., 664)`
- 影響: ext-grpc 1.58比で約16% throughput低下、Spanner-heavy pathで約10〜32% latency増

### After smoke

ローカルTLS test-serverのwarm channel上で1 RPCをstraceした。

- command: `strace -e trace=read,write,sendmsg,recvmsg php -d extension=/workspace/ext/grpc/modules/grpc.so /workspace/var/tmp/tls-unary-once.php`
- 観測: measure RPCのrequest送信はresponse受信前のTLS app data `write(4, ..., 77) = 77` 1回に集約された。
- response後に `write(4, ..., 39) = 39` が出るが、これはrequest body送信ではなくresponse処理後のHTTP/2制御frame送信であり、issue #2の「1 RPC requestが3 TLS recordに分割される」問題とは別のflush境界。

## 検証

- `./tools/test/check-phpt.sh`: 15/15 PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `php -l tools/benchmark/cpu-micro.php && php -l tools/benchmark/spanner-shape.php`: PASS
- `./bench/compare.sh throughput-unary --duration=0.05 --payload-bytes=100`: php-grpc-lite / ext-grpc のOTEL summary出力まで確認
- `./bench/compare.sh spanner-shape --calls=2 --warmup-calls=1`: php-grpc-lite / ext-grpc のOTEL summary出力まで確認
- `./bench/run.sh cpu-micro --calls=500 --warmup-calls=50`: h2cではTLS coalescingを無効化し、copy固定費を足さない方針に修正
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
- 判断: 採用する。TLS request write分割はローカルstrace smokeで解消し、`tls-cpu-micro` では主要ケースのCPU/wall timeが0.0.4比で概ね改善した。
- 備考: `tls-spanner-shape` の一部p99はローカルDocker環境の揺れを含むため、実Spanner / FPM負荷の比較は別issueで扱う。
