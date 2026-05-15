---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# transport loop / scheduler差のCPU影響を追加調査する

## 目的

同期 `SSL_read` / `SSL_write` + `poll` 方式と、ext-grpcのthreaded event loop方式の差がCPU/requestにどの程度効くかを確認する。

## 背景

cgroup sustainでは `grpc-lite` がCPU約1.32倍、nonvoluntary context switchも多かった。CLI `strace -c` では `grpc-lite` は `read` / `write` / `ppoll` 型、ext-grpcはbackground thread / `epoll_pwait` / `futex` 型の構造差が見えた。ただし、straceの時間値はwaitを含むためCPU根拠としては弱い。

## スコープ

- Docker Desktopで可能な範囲のscheduler/cgroup/strace観測を追加する。
- Linux VMやGCEなどperf/eBPF可能な環境で取るべきcounterを定義する。
- transport loop改善候補を、C/PHP bridge固定費の修正後に再評価する。

## 非スコープ

- すぐに専用thread/event loopへ設計変更すること。
- ext-grpc Coreの模倣。

## 検証

- cgroup CPU sustain。
- strace syscall count。
- perf/eBPF可能環境では task-clock / cycles / instructions / context-switches / cache-misses。

## 完了条件

- transport scheduler由来の差か、bridge/metadata/copy固定費由来の差かが、修正後再計測で切り分けられる。
