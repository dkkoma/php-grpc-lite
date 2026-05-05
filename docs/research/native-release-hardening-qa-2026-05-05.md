# Native release hardening QA runner (2026-05-05)

## 目的

native transportをrelease defaultにする前のblockerである memory / lifecycle / FPM worker request boundary を、手元とCIで再実行できる形にする。

## 追加した入口

- `bench/phase2/check-native-lifecycle-stress.sh`
  - `MAX_FD_DELTA`
  - `MAX_RSS_DELTA_KIB`
  - `MAX_PHP_MEMORY_DELTA_BYTES`
  - `VALGRIND=1`
- `bench/phase2/check-native-fpm-lifecycle.sh`
  - single-worker PHP-FPMへFastCGI requestを複数回送り、同一worker pidで2回目以降 `persistent_reused=true` になることを確認する。
- `bench/phase2/check-native-release-hardening.sh`
  - lifecycle smoke
  - Valgrind lifecycle smoke
  - long lifecycle stress
  - FPM request-boundary lifecycle

## Docker image

- `Dockerfile`
  - `valgrind`
  - `libfcgi-bin`
- `Dockerfile.fpm`
  - `php:8.4-fpm-trixie`
  - `grpc.so` をloadするsingle-worker PHP-FPM fixture
- `compose.yaml`
  - `fpm-lifecycle` service

## 実行コマンド

```bash
docker compose build dev fpm-lifecycle
BENCH_TAG=release-hardening ./bench/phase2/check-native-release-hardening.sh
```

長時間stressを強める場合:

```bash
LONG_ITERATIONS=10000 BENCH_TAG=release-hardening-long ./bench/phase2/check-native-release-hardening.sh
```

## 判定

この変更でrelease hardening QAの入口は揃い、Docker上の短縮設定では通過した。

実行済み:

```bash
docker compose build dev fpm-lifecycle
docker compose run --rm dev sh -lc 'command -v valgrind && command -v cgi-fcgi && php -v | head -1'
docker compose run --rm dev sh -lc 'cd ext/grpc && make -j$(nproc) >/dev/null && cd /workspace && php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit'
BENCH_TAG=20260505-hardening SMOKE_ITERATIONS=20 VALGRIND_ITERATIONS=2 LONG_ITERATIONS=50 FPM_REQUESTS=5 ./bench/phase2/check-native-release-hardening.sh
```

結果:

- PHPUnit: `82 tests`, `280 assertions`, `1 skipped`
- lifecycle smoke: `20` iterations, failures `0`
- Valgrind lifecycle smoke: `2` iterations, failures `0`, `ERROR SUMMARY: 0 errors`, `in use at exit: 0 bytes`
- long lifecycle stress: `50` iterations, failures `0`
- FPM request-boundary lifecycle: `5` requests, single worker pid, `reused_after_first=4`

保存先:

- `var/bench-results/phase2-native-lifecycle-stress-20260505-hardening-lifecycle-smoke.json`
- `var/bench-results/phase2-native-lifecycle-stress-20260505-hardening-valgrind.json`
- `var/bench-results/phase2-native-lifecycle-stress-20260505-hardening-valgrind.valgrind.log`
- `var/bench-results/phase2-native-lifecycle-stress-20260505-hardening-lifecycle-long.json`
- `var/bench-results/phase2-native-fpm-lifecycle-20260505-hardening-fpm.json`

release-readyにするには、同じrunnerをrelease artifact相当のDocker imageまたはCI matrix上で長時間設定にして通し、結果JSONとValgrind logを保存する必要がある。
