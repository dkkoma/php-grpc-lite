---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# native deadline I/Oのsetsockopt hot path削減

## 目的

native transportのsend/recv hot pathで、deadline処理のために `SO_RCVTIMEO` / `SO_SNDTIMEO` を過剰に再設定している処理を削減する。

## 背景

`select_1row_10col` を3回実行したstraceで、同一HTTP/2 connection上のfdに対して `SO_RCVTIMEO` / `SO_SNDTIMEO` が多数発行されていた。50回実行では `setsockopt` が1414回、約28回/RPC 発生した。

現状は `h2_connection_send()` / `connection_recv()` が呼ばれるたびに `remaining_timeout_us_for_deadline()` を計算し、`set_socket_timeout_us()` を呼ぶ。deadlineを守る意図は正しいが、I/Oごとのsocket option更新はsyscall数とkernel境界を増やす。

## スコープ

- `h2_connection_send()` / `connection_recv()` のdeadline実装を見直す。
- 可能ならsocket timeout再設定ではなく、nonblocking I/O + `poll_fd_until_deadline()` でdeadlineを制御する。
- unary / server streaming / TLS / h2c のdeadline挙動を維持する。
- `021-deadline.phpt` を含むdeadlineテストを通す。
- straceで `setsockopt` 回数が削減されたことを確認する。

## 非スコープ

- deadline仕様を緩めること。
- ext-grpcとの差分比較。
- TLS handshake / connect timeoutの制御変更。ただし副作用が出る場合は扱う。

## 計画

1. 現状の `setsockopt` 回数を記録する。
2. nonblocking I/O + poll deadlineへ変更できる範囲を確認する。
3. 実装する。
4. PHPT deadline/control系を実行する。
5. straceでsyscall削減を確認する。
6. Cloud Spanner c32 sustained samplingでCPU/throttlingの変化を見る。

## 検証ログ

- `var/bench-results/native-strace-select50-20260515/strace-summary.txt`
- `var/bench-results/native-strace-setsockopt-20260515/setsockopt.log`
- `var/bench-results/native-strace-setsockopt-after-20260515/setsockopt.log`
- `var/bench-results/native-strace-setsockopt-after2-20260515/setsockopt.log`
- `var/bench-results/fpm-worker32-native-sustain-20260515-c32/`
- `var/bench-results/fpm-worker32-native-sustain-after-timeout-20260515-c32/`

## 実装メモ

- `h2_connection_send()` / `connection_recv()` のI/Oごとの `SO_RCVTIMEO` / `SO_SNDTIMEO` 再設定を廃止した。
- connection fdはnonblockingのまま運用し、`SSL_ERROR_WANT_READ` / `SSL_ERROR_WANT_WRITE`、`EAGAIN` / `EWOULDBLOCK` を `poll_fd_until_deadline()` で待つ。
- TLS handshakeは既存通りnonblocking handshakeでdeadlineを扱い、handshake成功後もblocking modeへ戻さない。
- h2cはconnect完了後にnonblockingへ切り替える。
- poll errorはdeadline timeoutと区別し、`*_poll failed` として `last_error_detail` に残す。
- 未使用になった `set_socket_timeout_us()` は削除した。

## 結果

### setsockopt trace

| 条件 | `setsockopt` trace行数 | 備考 |
| --- | ---: | --- |
| 変更前 | 99 | `select_1row_10col` 3 RPC、ほぼ `SO_RCVTIMEO` / `SO_SNDTIMEO` |
| send/recv hot path変更後 | 15 | 初期設定分が残存 |
| 初期per-RPC timeout削除後 | 3 | `TCP_NODELAY` / `IP_RECVERR` / exitのみ |

### sustained FPM load

条件: Cloud Spanner / Laravel / native backend / FPM worker 32 / CPU quota 4.0 / client concurrency 32 / 30s。

| 条件 | RPS | avg | p50 | p90 | p99 | throttled_usec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 変更前 | 131.9067 | 242.1ms | 202.5ms | 343.3ms | 826.2ms | 39,630,863 |
| 変更後 | 259.7230 | 123.0ms | 115.4ms | 172.6ms | 263.9ms | 30,241,007 |

RPSは約1.97倍、p99は約68%低下した。`setsockopt` syscall単体のstrace時間は小さかったが、高負荷FPMではkernel境界とdeadline再設定がCPU quota/throttling下で大きく効いていたと判断する。

## 検証

- `docker compose run --rm dev sh -lc 'cd ext/grpc && make -j$(nproc)'`: pass
- `./tools/test/check-phpt.sh`: 15 tests, 15 passed
- `./tools/test/check-c-static-analysis.sh`: pass

## 完了条件

- hot pathの `SO_RCVTIMEO` / `SO_SNDTIMEO` 再設定が大幅に減っている。
- deadline PHPTが通る。
- c32 sustained samplingでCPU/throttling/latencyの改善有無が判断できる。

## 終了判断

- 修正コミット: `pending`
- 主要検証: `./tools/test/check-phpt.sh`、`./tools/test/check-c-static-analysis.sh`
- 判断: deadline I/O hot pathの過剰なsocket timeout再設定は解消済み。残るCPU要因は `2026-05-15-native-cpu-hotpath-profiling.md` 側で継続する。
