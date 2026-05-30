---
Status: Open
Owner: Codex
Created: 2026-05-28
Branch: feature/zts-formal-support
---

# ZTS正式サポート

## 目的

`php-grpc-lite` のZTS PHP環境を正式サポート対象にし、source-built `ext/grpc` がZTS buildで継続的にbuild/load/testされる状態にする。

## 背景

`composer.json` の `php-ext.support-zts` は `true` だが、`docs/SPEC.md` はZTSを将来検討としていた。HTTP/2 transportのpersistent connection cacheはmodule globals上にあり、ZTSではthread-local cacheとして扱う設計にする必要がある。

現行コードは `ZEND_DECLARE_MODULE_GLOBALS(grpc_lite)` / `ZEND_MODULE_GLOBALS_ACCESSOR` を使い、`COMPILE_DL_GRPC && ZTS` では `ZEND_TSRMLS_CACHE_UPDATE()` / `ZEND_TSRMLS_CACHE_DEFINE()` も持つ。一方で、ZTS PHP上でのbuild/load/PHPTを再現可能なrunnerとCI gateがなかったため、正式サポートとしては検証線が不足していた。

## スコープ

- ZTS PHP Docker image / compose serviceを追加する。
- ZTS PHP上で `phpize && ./configure --enable-grpc && make`、extension load、PHPTを実行するrunnerを追加する。
- CIのNative QAにZTS PHPT jobを追加する。
- ZTS/NTSの代表性能比較QAを追加する。
- ZTS thread並列でPHP userland call pathを通すQAを追加する。
- SPEC / native test framework / release QA checklistのZTS記述を更新する。
- C拡張内のZTSリスクを棚卸しし、必要なら追加修正する。

## 非スコープ

- PHP 8.4未満のZTS対応。
- 1つのHTTP/2 session/socketを複数threadで共有する設計。
- transport専用threadやasync event loopの導入。
- FrankenPHP grpc-go backendのZTS互換性保証。

## 計画

1. ZTS検証環境を追加する。
2. ZTS PHPT runnerを追加し、ZTS buildであることをpreflightする。
3. CIにZTS PHPT jobを追加する。
4. NTS/ZTSの代表性能比較runnerを追加する。
5. NTS multi-process / ZTS threadのPHP call path並列runnerを追加する。
6. module globals、persistent connection cache、static mutable state、resource lifecycleをZTS観点でレビューする。
7. ZTS PHPTと通常PHPTをDocker内で実行する。
8. NTS/ZTS代表性能比較を実行し、OTEL summaryを記録する。
9. ZTS thread並列のunary/server streaming比較を実行し、結果を記録する。
10. 必要に応じてHTTP/2/gRPCドメインモデルレビューを残し、Blocker / High / Medium / Lowがnoneになるまで修正する。

## 進捗

- 2026-05-28: ZTS正式サポートの親issueを作成。
- 2026-05-28: `Dockerfile.zts` と `dev-zts` compose serviceを追加。
- 2026-05-28: `tools/test/check-zts-phpt.sh` を追加。
- 2026-05-28: CI `Native QA` に `ZTS PHPT` jobを追加。
- 2026-05-28: SPEC / native test framework / release QA checklistのZTS gate記述を更新。
- 2026-05-28: NTS/ZTS代表性能比較用 `tools/test/check-zts-performance.sh` を追加。
- 2026-05-28: ZTS thread並列比較用 `tools/test/check-zts-parallel-performance.sh` とPHP call path worker `tools/benchmark/zts-parallel-call-path.php` / `tools/benchmark/zts-parallel-worker.php` を追加。
- 2026-05-28: 追加レビューで、`SIGPIPE` のprocess-wide変更、runtime `getenv()` trace設定、persistent connection cacheのthread-local invariantを正式サポート前の確認タスクに追加。
- 2026-05-28: FPM NTSと同じLaravel/Spanner application経路をFrankenPHP ZTSで計測するため、`franken-zts-laravel-native` serviceと `bench/fpm-laravel-spanner-load-compare.sh` の `franken-zts` variantを追加。
- 2026-05-28: Cloud Spanner / Laravel mixed transactionでFPM NTS nativeとFrankenPHP ZTS nativeを同条件計測し、FrankenPHP ZTS側の大幅なwall time悪化を確認。
- 2026-05-28: ZTS正式サポートの性能完了条件を、FrankenPHP ZTSの同一application経路がFPM NTSと同等またはそれ以上であることへ引き上げた。現状の大幅なwall time悪化は正式サポートのblockerとして扱う。
- 2026-05-28: FrankenPHP ZTS遅延をtraceで分解し、worker targetが `/bench` requestを受けるfront controllerではなかったため、実リクエストがworker poolに乗らず直列化されていたことを確認。`public/index.php` をworker対応し、FrankenPHP worker targetを `index.php` に変更。

## 検証

- `./tools/test/check-zts-phpt.sh`: PASS, ZTS PHP 8.4.21, PHPT 16/16
- `./tools/test/check-phpt.sh`: PASS, NTS PHP 8.4.20, PHPT 16/16
- `./tools/test/check-c-static-analysis.sh`: PASS
- `BENCH_TAG=zts-compare-smoke-20260528 ZTS_PERF_ARGS=--calls=5 ./tools/test/check-zts-performance.sh`: PASS
  - `spanner-shape` NTS/ZTS smoke run id:
    - `zts-compare-smoke-20260528-nts-spanner-shape`
    - `zts-compare-smoke-20260528-zts-spanner-shape`
  - `metadata-header` NTS/ZTS smoke run id:
    - `zts-compare-smoke-20260528-nts-metadata-header`
    - `zts-compare-smoke-20260528-zts-metadata-header`
  - `--calls=5` のrunner smokeであり、正式な性能判断値ではない。正式QAではcalls/warmup/repeatを増やして再計測し、代表値を `docs/benchmarks/` またはこのissueへ記録する。
- `ZTS_PARALLEL_WORKERS=1 ZTS_PARALLEL_CALLS=1 ZTS_PARALLEL_WARMUP_CALLS=0 ZTS_PARALLEL_STREAM_MESSAGES=2 ./tools/test/check-zts-parallel-performance.sh`: PASS
  - NTS multi-process: unary / server streaming call path smoke PASS
  - ZTS thread: unary / server streaming call path smoke PASS
  - `server_delay_ms=10`
  - server streamingはPHP userlandの `GreeterClient->BenchServerStream(...)->responses()` を最後までdrain
  - worker `1` / calls `1` のrunner smokeであり、正式な性能判断値ではない。正式QAではdefault worker `1,2,8`、十分なcalls/warmup/repeatで再計測し、代表値を `docs/benchmarks/` またはこのissueへ記録する。
- `ZTS_PARALLEL_WORKERS=1,2,8 ZTS_PARALLEL_CALLS=20 ZTS_PARALLEL_WARMUP_CALLS=2 ZTS_PARALLEL_SERVER_DELAY_MS=10 ZTS_PARALLEL_STREAM_MESSAGES=2 ./tools/test/check-zts-parallel-performance.sh`: PASS
  - NTS multi-process:
    - unary workers=1: throughput 72.487/s, p50 10.884ms, p99 11.886ms
    - streaming workers=1: throughput 70.723/s, p50 11.655ms, p99 12.170ms
    - unary workers=2: throughput 137.447/s, p50 11.800ms, p99 12.777ms
    - streaming workers=2: throughput 135.468/s, p50 11.974ms, p99 14.769ms
    - unary workers=8: throughput 528.034/s, p50 11.198ms, p99 15.905ms
    - streaming workers=8: throughput 543.613/s, p50 11.809ms, p99 15.963ms
  - ZTS thread:
    - unary workers=1: throughput 80.182/s, p50 11.071ms, p99 11.680ms
    - streaming workers=1: throughput 75.684/s, p50 11.759ms, p99 12.371ms
    - unary workers=2: throughput 145.995/s, p50 11.927ms, p99 12.482ms
    - streaming workers=2: throughput 145.616/s, p50 11.743ms, p99 12.705ms
    - unary workers=8: throughput 587.983/s, p50 11.179ms, p99 12.926ms
    - streaming workers=8: throughput 593.156/s, p50 10.843ms, p99 12.595ms
  - この1回runでは、ZTS threadはworker数増加に応じてthroughputが伸び、p99もNTS multi-processより悪化していない。正式な採否判断ではrepeatを追加して揺れを確認する。
- `BENCH_RUN_ID=zts-franken-app-real-mixed-c16-64req-20260528 BENCH_VARIANTS='native franken-zts' BENCH_ACTIONS='transaction_select2_update1_insert1' BENCH_HEY_TIMEOUT=60 LARAVEL_SPANNER_EMULATOR_HOST= LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 LARAVEL_SPANNER_INSTANCE_ID=bench LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db LARAVEL_SPANNER_MIN_SESSIONS=16 FRANKENPHP_WORKERS=16 ./bench/fpm-laravel-spanner-load-compare.sh 64 16`: PASS
  - log: `var/bench-results/fpm-laravel-spanner-load-zts-franken-app-real-mixed-c16-64req-20260528/`
  - FPM NTS native: throughput 10.3529/s, cpu_us/req 25414.6, avg 1471.3ms, p50 1387.4ms, p90 1996.2ms, max 2287.0ms
  - FrankenPHP ZTS native: throughput 1.5673/s, cpu_us/req 45448.8, avg 9048.4ms, p50 10190.4ms, p90 10680.5ms, max 10730.4ms
  - 同じLaravel app / Cloud Spanner / mixed transaction / client concurrency 16 / service CPU quota 4.0 / warmup後で、FrankenPHP ZTSはFPM NTS比でthroughput約0.15x、平均wall time約6.15x、CPU/request約1.79x。
- `BENCH_RUN_ID=zts-franken-app-real-mixed-c16-20260528 ... ./bench/fpm-laravel-spanner-load-compare.sh 256 16`: FAIL
  - FPM NTS nativeは256/256成功: throughput 10.7372/s, cpu_us/req 25777.6, avg 1457.8ms, p50 1561.2ms, p90 1815.7ms, max 2074.2ms
  - FrankenPHP ZTS nativeは229/256成功、27件がhey default 20s timeout。完了分の平均 15412.0ms、p50 15851.4ms、p90 18762.8ms。
- `BENCH_RUN_ID=zts-franken-app-select-c16-20260528 BENCH_VARIANTS='native franken-zts' BENCH_ACTIONS='select_1row_10col' FRANKENPHP_WORKERS=16 ./bench/fpm-laravel-spanner-load-compare.sh 256 16`: FAIL
  - local emulator条件ではFPM NTS側で1件 `ABORTED: The emulator only supports one transaction at a time.` が発生。並列application性能比較にはCloud Spannerを使う。
- `BENCH_RUN_ID=zts-franken-app-real-mixed-c16-16req-trace-20260528 BENCH_VARIANTS='native franken-zts' BENCH_ACTIONS='transaction_select2_update1_insert1' BENCH_HEY_TIMEOUT=60 LARAVEL_SPANNER_TRACE=1 ... ./bench/fpm-laravel-spanner-load-compare.sh 16 16`: PASS
  - log: `var/bench-results/fpm-laravel-spanner-load-zts-franken-app-real-mixed-c16-16req-trace-20260528/`
  - FPM NTS native: throughput 12.6876/s, cpu_us/req 18068.6, avg 1178.4ms, p50 1179.8ms, p90 1260.8ms, max 1260.8ms
  - FrankenPHP ZTS native: throughput 1.4070/s, cpu_us/req 48519.1, avg 5959.9ms, p50 6369.1ms, p90 11371.6ms, max 11371.6ms
  - Laravel trace上の `http.bench` はFrankenPHP ZTSでも平均721.1msであり、request handler内部が5.9sかかっているわけではなかった。
  - traceから計算した最大同時 `http.bench` はFPM NTSが16、FrankenPHP ZTSが1。遅延の主因はworker poolが実リクエストを処理しておらず、PHP handlerに入る前で直列queueingしていたこと。
- `BENCH_RUN_ID=zts-franken-app-real-mixed-c16-16req-worker-index-trace-20260528 BENCH_VARIANTS='native franken-zts' BENCH_ACTIONS='transaction_select2_update1_insert1' BENCH_HEY_TIMEOUT=60 LARAVEL_SPANNER_TRACE=1 ... ./bench/fpm-laravel-spanner-load-compare.sh 16 16`: PASS
  - log: `var/bench-results/fpm-laravel-spanner-load-zts-franken-app-real-mixed-c16-16req-worker-index-trace-20260528/`
  - FPM NTS native: throughput 15.7775/s, cpu_us/req 21605.2, avg 973.3ms, p50 986.8ms, p90 1013.7ms, max 1013.7ms
  - FrankenPHP ZTS native: throughput 15.8526/s, cpu_us/req 11984.1, avg 915.2ms, p50 928.0ms, p90 1009.1ms, max 1009.1ms
  - worker target修正後、最大同時 `http.bench` はFPM NTS / FrankenPHP ZTSともに16。FrankenPHP ZTSはこの小runではFPM NTSと同等以上。
- `BENCH_RUN_ID=zts-franken-app-real-mixed-c16-64req-worker-index-20260528 BENCH_VARIANTS='native franken-zts' BENCH_ACTIONS='transaction_select2_update1_insert1' BENCH_HEY_TIMEOUT=60 ... ./bench/fpm-laravel-spanner-load-compare.sh 64 16`: PASS
  - log: `var/bench-results/fpm-laravel-spanner-load-zts-franken-app-real-mixed-c16-64req-worker-index-20260528/`
  - FPM NTS native: throughput 8.8602/s, cpu_us/req 24783.5, avg 1745.0ms, p50 1639.1ms, p90 2463.1ms, max 2656.3ms
  - FrankenPHP ZTS native: throughput 18.0943/s, cpu_us/req 12014.8, avg 857.6ms, p50 857.1ms, p90 920.3ms, max 1015.9ms
  - 修正後の64/c16ではFrankenPHP ZTSがFPM NTSより高throughput・低wall time・低CPU/request。

## 判断ログ

- ZTSでのpersistent connection cacheはmodule globals上のthread-local cacheとして扱う。threadをまたいでsocket/sessionは共有しない。
- TSanはthread-safety regression検出に有用だが、ZTS build/load互換性そのものの代替にはしない。ZTS PHPTを独立gateにする。
- package / extension runtime versionとは別に、ZTS対応可否はPIE metadataとCI gateで管理する。
- ZTS正式サポートでは機能互換だけでなくNTSとの代表性能比較をQA evidenceに含める。初期対象は `spanner-shape` と `metadata-header` とし、必要なら `tls-spanner-shape` / `large-streaming` を追加する。
- ZTS thread並列QAは、serverが速すぎて競合が見えなくなることを避けるため `server_delay_ms=10` をdefaultにする。
- thread並列の代表worker数は `1,2,8` に絞る。
- unaryだけではstream resource lifecycleや順次drainの問題が見えにくいため、同条件でserver streamingも含める。server streamingは各workerがPHP userlandの `GreeterClient->BenchServerStream(...)->responses()` を最後までdrainする。
- FrankenPHP ZTSのapplication性能確認は、FPM NTSと同じLaravel/Spanner app、同じHTTP load generator、同じactionで比較する。`ext/grpc/modules/grpc.so` はNTS/ZTSでABIが異なるため、variantごとに対応するPHP image内でbuildし直してからserviceを起動する。
- 2026-05-28のCloud Spanner mixed transaction比較では、synthetic ZTS thread parallel QAと異なり、FrankenPHP ZTS worker modeのapplication経路で顕著なwall time悪化が出た。traceで分解した結果、遅延はSpanner step内部ではなくPHP handlerに入る前のqueueingだった。
- FrankenPHP worker modeでは `worker` に指定したPHP fileだけがrequest loopとして使われる。`franken-worker.php` を別fileとして指定しつつ `php_server` が `/bench` を `index.php` にrewriteしている構成では、実リクエストはworker poolに乗らず、trace上で最大同時 `http.bench=1` になった。FPM比較のworker targetはfront controller `public/index.php` に揃える。
- `public/index.php` はFPM NTSでは従来通り1 requestで終了し、FrankenPHPでは `frankenphp_handle_request()` loopに入る形にする。これによりroutingされた `/bench` がworker poolで処理される。
- ZTS正式サポートは「ZTSでbuild/load/testできる」だけでは完了にしない。FPM NTSと同じLaravel/Spanner application pathで、FrankenPHP ZTSがFPM NTSと同等、またはFPM NTSより速いことをサポート可能な状態の条件にする。

## 追加確認タスク

- `ext/grpc/main.c` の `signal(SIGPIPE, SIG_IGN)` はthreaded SAPIではprocess-wide状態変更になる。ZTS正式サポート前に、維持するなら明示的なprocess-wide policyとして文書化し、可能ならsocket/TLS write側のエラー処理で代替できるか確認する。
- `GRPC_LITE_TRACE_FILE` / `GRPC_LITE_TRACE_WIRE_BYTES` / `GRPC_LITE_TRACE_CALLS` のruntime `getenv()` はprocess-global環境に依存する。ZTSでの正式サポート前にINI/module globals化またはrequest/thread-local cache化を検討する。
- persistent connection cacheは `PHP_GRPC_LITE_G(persistent_connections)` 経由のthread-local所有を不変条件にする。`h2_connection` / `nghttp2_session` / socket / `SSL*` をthread間共有しないことをコメントまたはテストで明示する。
- FrankenPHP ZTS application経路の遅延要因を分解する。初期仮説は、FrankenPHP worker modeでのLaravel request lifecycle/reset不足、Spanner session/transaction lifecycleのthread間分散、ZTS thread-local persistent connection cacheの再利用不足、HTTP/FrankenPHP worker scheduling、native transportのZTS固有lock/connection再確立。
- FrankenPHP worker modeはLaravel application stateがrequestをまたいで残る。現fixtureでは性能比較のため `index.php` をworker loop化したが、正式サポート前にOctane相当のrequest reset要否、`DatabaseManager` / `colopl/laravel-spanner` connection/session stateのlong-lived worker安全性、request-specific static state (`SpannerTraceRecorder`) のresetを確認する。

## 完了条件

- `./tools/test/check-zts-phpt.sh` がDocker内で通る。
- 通常NTSの `./tools/test/check-phpt.sh` が引き続き通る。
- C static analysisが通る。
- `./tools/test/check-zts-performance.sh` を実行し、NTS/ZTSの代表性能比較結果をこのissueまたは `docs/benchmarks/` に記録する。
- `./tools/test/check-zts-parallel-performance.sh` を実行し、NTS multi-process / ZTS threadのunary/server streaming call path比較結果をこのissueまたは `docs/benchmarks/` に記録する。
- Cloud Spanner / Laravel mixed transaction / concurrency 16 の同一application比較で、FrankenPHP ZTS nativeのthroughput、wall time、CPU/requestがFPM NTS nativeと同等またはそれ以上である。最低条件として、FPM NTS比で明確な劣化が残っていないことをrepeat runで示す。
- 上記application比較の達成前は、ZTS正式サポートを完了扱いにしない。
- CI `Native QA` の `ZTS PHPT` jobが通る。
- ZTS観点のコードレビューでBlocker / High / Medium / Lowがnoneになる。
- 必要なレビュー記録と検証結果をこのissueに追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。
