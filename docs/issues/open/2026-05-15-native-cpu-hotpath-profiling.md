---
Status: Open
Owner: Codex
Created: 2026-05-15
Branch: main
---

# native単体CPU hot pathの特定と改善計画

## 目的

実アプリ条件で再現した `php-grpc-lite` nativeのCPU消費増について、`ext-grpc` 比較ではなくnative単体のCPU使用傾向をプロファイルし、改善すべき処理を洗い出す。

## 背景

Laravel + `colopl/laravel-spanner` + Cloud Spanner + FPM worker32 / CPU quota 4.0 / client concurrency 32 で、nativeはCPU quotaに張り付き、throttlingとtail latency悪化が観測された。比較値は十分に得られたため、以後はnative単体でCPU hot pathを特定し改善する。

## スコープ

- native FPM workerのCPU profileを取得する。
- 高負荷時のFPM workerごとのCPU偏り、context switch、cgroup throttlingを確認する。
- PHP userland / protobuf / C extension / OpenSSL / nghttp2 / socket I/O のどこにCPUが乗るか分類する。
- 改善候補を個別issueに分解し、改善・計測・レビューのループへ進める。

## 非スコープ

- ext-grpcとの追加比較を主目的にしない。
- Spanner自体やアプリケーション業務ロジックの最適化を主目的にしない。
- 計測基盤の過剰整備を目的化しない。

## 方針

1. profiling用FPM環境を作り、`perf` / `callgrind` / `strace` / process sampling のうち実行可能なものを確認する。
2. まず `4 CPUs / 32 workers / client concurrency 32` を主条件にする。c64は不安定な飽和条件として補助扱いにする。
3. profile取得はnativeのみ。比較表ではなくhot pathを洗い出す。
4. 洗い出したhot pathを、改善可能性とリスクで分類する。
5. 改善候補ごとにissueを作り、計測→実装→再計測→レビューを繰り返す。

## 計測対象

- Laravel route: `/bench?action=select_1row_10col`
- backend: native `ext/grpc` built from this repository
- FPM workers: 32
- CPU quota: 4.0
- load: client concurrency 32, sustained 30sを基本条件
- Spanner: Cloud Spanner `vast-falcon-165704` / `bench` / `laravel-bench-db`

## 検証ログ

- `Dockerfile.fpm-profile` を追加し、profiling用FPM imageへ `linux-perf` / `procps` / `strace` / `valgrind` を導入した。
- `perf record -p <worker>` は `perf_event_open(...): Operation not permitted` で失敗した。profiling専用containerへ `privileged` / `PERFMON` 等を与えれば進む可能性はあるが、権限範囲が大きいため明示承認なしには使わない。
- `bench/fpm-laravel-spanner-callgrind.sh` と `tools/benchmark/laravel-spanner-app/bin/profile-action.php` を追加し、Laravel + `colopl/laravel-spanner` のnative select経路をCLIでcallgrind取得できるようにした。
- `select_1row_10col` 500 iterations / `opcache.enable_cli=1`: `var/bench-results/native-callgrind-select500-opcache-20260515/`
- `select_1row_10col` 50 iterations strace summary: `var/bench-results/native-strace-select50-20260515/strace-summary.txt`
- `select_1row_10col` 3 iterations setsockopt trace: `var/bench-results/native-strace-setsockopt-20260515/setsockopt.log`

## 観測結果

- callgrind CLIではPHP VM / Laravel bootstrap / protobuf descriptor系が大きく、`grpc.so` のself costは小さい。CLIはFPM high-loadの正確なsampling profileではないため、native C拡張の主犯断定には使わない。
- FPM high-loadの実CPU samplingには `perf` 等が必要だが、現Docker権限ではattach不可。
- syscall集計では50 RPCで `setsockopt` が1414回発生した。約28回/RPCで、内容はほぼ `SO_RCVTIMEO` / `SO_SNDTIMEO` のdeadline timeout再設定だった。
- syscall時間の合計では `setsockopt` 自体は小さかったが、高負荷・多worker条件ではkernel境界、context switch、throttlingの増幅要因になっていた。`docs/issues/open/2026-05-15-native-deadline-socket-timeout-hotpath.md` の実装後、sustained c32でRPSが約1.97倍、p99が826.2msから263.9msへ改善した。

## 洗い出した改善候補

1. `docs/issues/open/2026-05-15-native-deadline-socket-timeout-hotpath.md`: deadline I/Oでの `setsockopt` 過多を削減する。最初の改善候補として実装済み、レビュー待ち。
2. `docs/issues/open/2026-05-15-native-cpu-profile-fpm-perf.md`: FPM workerの実CPU sampling profileを取得する。
3. PHP/protobuf/Laravel側CPUが大きい可能性は高いが、このリポジトリ単体で直接改善できる範囲は限定的。native C拡張の改善候補とは分けて扱う。

## 次に見るCPU観点

- `perf` attachなしで継続できる範囲では、`strace -c` / targeted trace / sustained cgroup samplingを組み合わせ、syscall頻度とtail latencyの相関を見る。
- `setsockopt` hot path削減後、Cloud Spanner CLI straceではsyscall totalが29ms/50RPCで、elapsed 2.08sの主因ではない。`openat` / `newfstatat` / `read` / `fcntl` はCLIのLaravel bootstrap / OPcache / generated PHP class loadが強く、FPM steady-stateのnative transport CPUとは分けて扱う。
- `docs/issues/closed/2026-05-15-native-h2-write-coalescing.md` で小さいTLS write coalescingを検証したが、controlled CPU microで悪化したため見送った。
- `perf` が必要な段階になった場合のみ、profiling専用containerの権限付与を別途判断する。

## 現時点の切り分け

- native transport単体のcontrolled CPU固定費は、small unary / 1-message streamingでおおむね10〜18µs/call。100-message streamingでも約78µs/call。
- Laravel + google/cloud-spanner + Cloud Spanner実経路のCPU/requestは桁が違うため、残るCPUはこのリポジトリのHTTP/2 transport hot pathだけでは説明しにくい。
- ただし、FPM高負荷では小さいkernel境界でも増幅することが `setsockopt` 削減で実証された。以後の改善候補は、controlled microで悪化しないことを先に確認してからCloud Spanner c32 sustainedへ進める。

## 完了条件

- native CPU hot pathが関数/処理カテゴリ単位で説明できる。
- 改善候補が個別issueへ分解されている。
- 少なくとも最優先候補について、改善前後のCPU/throttling/latency結果が記録されている。

## 2026-05-15 worker warmup再計測

FPM worker32条件では、startup時のsession warmupとは別に、各FPM workerへHTTP requestを流してPHP/opcache/protobuf/native channelを暖める必要がある。`bench/fpm-laravel-spanner-load-compare.sh` と `bench/fpm-laravel-spanner-cpu-sustain.sh` にworker数ベースのwarmupを追加した。

条件: Cloud Spanner / native / FPM worker32 / CPU quota 4.0 / client concurrency 32 / 30s。

| 条件 | RPS | avg | p50 | p90 | p99 | throttled_usec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| deadline I/O改善後、worker HTTP warmupなし | 259.7230 | 123.0ms | 115.4ms | 172.6ms | 263.9ms | 30,241,007 |
| deadline I/O改善後、worker HTTP warmupあり | 333.5908 | 95.8ms | 94.9ms | 126.3ms | 163.5ms | 34,087,066 |

warmupありではRPSがさらに約1.28倍、p99が約38%低下した。これはnative transportの追加改善ではなく、計測条件の妥当化である。以後のFPM CPU計測では、startup session warmupとFPM worker HTTP warmupを分離して必須にする。
