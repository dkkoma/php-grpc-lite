---
Status: Open
Owner: Codex
Created: 2026-05-15
Branch: main
---

# FPM native workerの実CPU profile取得

## 目的

FPM高負荷時のnative workerについて、実CPU sampling profileを取得し、PHP VM / protobuf / grpc.so / OpenSSL / nghttp2 / syscall の比率を明確にする。

## 背景

`perf` をprofiling containerへインストールしたが、通常権限では `perf_event_open` が `Operation not permitted` で失敗した。`privileged` / `SYS_ADMIN` / `PERFMON` / `seccomp:unconfined` を付ければ進められる可能性があるが、権限範囲が大きいため明示承認なしには使わない。

callgrind CLIではLaravel bootstrapやPHP VMが大きく、FPM high-loadの実CPU samplingとしては不十分。正確なhot path確定にはFPM workerへのsampling profilerが必要。

## スコープ

- 安全に使えるprofiling方法を優先する。
- 必要ならユーザー明示承認の上でprofiling専用containerだけに追加capabilityを与える。
- profile結果は `var/bench-results/` に保存し、要約をissueへ転記する。

## 非スコープ

- production serviceへprivileged設定を入れること。
- profiler導入自体を本番機能にすること。

## 候補

- `perf record -g -p <worker>`: 最も直接的。現状は権限不足。
- `phpspy`: PHP stackには有効だがC extension/OpenSSL/nghttp2の詳細には弱い可能性。
- `callgrind` under FPM: 権限不要だが非常に重く、高負荷状態の再現性が落ちる。
- `strace -c/-e`: syscall傾向の確認には有効。CPU hot path全体は見えない。

## 検証ログ

- `perf_event_open(...): Operation not permitted`
- `var/bench-results/native-callgrind-select500-opcache-20260515/`
- `var/bench-results/native-strace-select50-20260515/`

## 完了条件

- FPM workerのCPU sampling profileが取得できている。
- native改善候補の優先順位をprofileで説明できる。
