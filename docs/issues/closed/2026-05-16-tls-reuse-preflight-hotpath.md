---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# TLS persistent reuse preflightの固定費を確認する

## 目的

persistent TLS connection reuse時の `SSL_peek()` とfd mode切替がCPU/request差に寄与しているかを確認し、削減可能なら修正する。

## 背景

`preflight_persistent_connection()` はidle TLS connectionのreuse前にnonblocking modeへ切り替え、`SSL_peek()` でpending dataを確認してから元のfd modeへ戻す。mixed transactionではRPCごとにreuseされるため、このpreflight固定費が乗る可能性がある。

## スコープ

- straceで`fcntl`/read系の発生を確認する。
- 安全に削れるfd mode切替があるか確認する。
- EOF/GOAWAY/control semanticsを壊さない範囲で検証する。

## 非スコープ

- persistent reuse preflight自体を無条件に削除すること。
- event loop/thread設計変更。

## 完了条件

- 採用/棄却判断と計測結果を残す。

## 実装

- TLS connectionはhandshake後もnonblocking fdのまま運用されるため、reuse preflight内の `fcntl(F_GETFL)` → `set_fd_nonblocking_mode(true)` → `fcntl(F_SETFL, previous_mode)` を削除した。
- `SSL_peek()` によるpending TLS data / EOF / error検出は維持した。

## 検証

- `make test TESTS="tests/010-unary.phpt tests/011-server-streaming.phpt tests/024-control-semantics.phpt tests/030-tls.phpt"`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS

## ベンチマーク

real Spanner Laravel/FPM, `transaction_select2_update1_insert1`, 256 requests, concurrency 16, 16 FPM workers, cloud Spanner.

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| identity canonical digest | 9215.4 | 25.9296 | 603.5 | 611.4 | 678.7 |
| TLS preflight fcntl削減 | 9141.4 | 26.3599 | 593.0 | 598.2 | 683.2 |

## 判断

- 採用する。
- 改善幅は小さいが、TLS fdは既にnonblockingであり、reuse hot pathのfcntl往復は不要だった。
- EOF/pending control data検出の `SSL_peek()` は残しているため、lifecycle semanticsは維持される。
