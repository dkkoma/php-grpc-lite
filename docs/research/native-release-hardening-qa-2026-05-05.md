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

この変更でrelease hardening QAの入口は揃った。

release-readyにするには、release artifact相当のDocker imageまたはCI matrix上で `check-native-release-hardening.sh` を通し、結果JSONとValgrind logを保存する必要がある。
