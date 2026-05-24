---
Status: Open
Owner: Codex
Created: 2026-05-24
Related:
  - docs/issues/open/2026-05-20-laravel-fpm-sa-json-cpu-reproduction.md
  - docs/issues/open/2026-05-23-gcp-wire-header-size-diagnostic-image.md
---

# VM上のLaravel/FPM Spanner比較をGHCR imageで実行する

## 目的

GCP VM上でLaravel + colopl/laravel-spanner + PHP-FPMのgrpc-lite / official ext-grpc比較を行う。VM上ではbuildせず、GitHub Actionsで用途専用imageをGHCRへpublishし、VMはpullして実行するだけにする。

## 背景

issue #5のwire diagnosticでは、`test` branch pushでGHCR imageをbuildし、VMでpull/runする運用が機能している。一方、Laravel/FPM benchでVM上buildを試したところ、PHP / protobuf / test-server / ext-grpc buildがVM上で走り、時間とノイズが大きい。今回の目的はVM上のruntime比較であり、VM上buildは不要。

## スコープ

- `test` branch pushでLaravel/FPM Spanner bench専用imageだけをbuild/pushする。
- imageは既存のpublic GHCR package `ghcr.io/dkkoma/php-grpc-lite-spanner-repro` のtagとしてpublishする。
- VM上の実行scriptを追加し、metadata server ADCでSpannerへ接続する。
- 比較対象はgrpc-lite currentとofficial ext-grpc 1.58.0 optimized。

## 非スコープ

- issue #5 wire/header-size diagnostic imageの再build。
- SA JSON / user ADC JSONを使う計測。
- VM上でのPHP extension build。
- production codeの変更。

## 計画

1. Laravel/FPM bench用Dockerfileを追加する。
2. `test` branch専用workflowで `laravel-fpm-lite` / `laravel-fpm-official` / `laravel-fpm-nginx` / `laravel-fpm-loadgen` をpushする。
3. VM runner scriptを追加する。
4. `test` branchへpushし、image publishを確認する。
5. VMでmetadata ADC条件のFPM比較を実行する。

## 進捗

- [x] Dockerfile追加
- [x] workflow追加
- [x] VM runner追加
- [x] GHCR publish確認
- [x] official ext-grpc imageをartifact `grpc.so` 利用へ切替
- [ ] VM比較実行

## 検証条件

- VM: `grpc-lite-wire-e2micro` / `asia-northeast1-a`
- VM attached service account: `test-spanner@vast-falcon-165704.iam.gserviceaccount.com`
- auth: VM metadata ADCのみ。credential JSONは使わない。
- Spanner: `vast-falcon-165704` / `bench` / `laravel-bench-db`
- FPM: 16 workers / CPU quota 2.0（`e2-micro` のVM上限に合わせる）

## 判断ログ

- VM上buildは行わない。runtime比較ではbuild時間、build artifact差、VM CPU消費がノイズになるため、GitHub Actions / GHCRで事前buildする。
- GHCR packageは既存の `php-grpc-lite-spanner-repro` を使う。新規packageのvisibility設定を増やさず、既にpublic pull確認済みのpackageへ用途別tagを追加する。
- 既存のissue #5 wire diagnostic image workflowはこのbranchでは用途外とし、`test` branch pushではLaravel/FPM bench用tagだけをpublishする。
- VM runnerは `GOOGLE_APPLICATION_CREDENTIALS` を渡さず、metadata server ADCだけで実行する。
- GHCR publishは GitHub Actions run `26360354877` で成功。`lite` / `official` / `nginx` / `loadgen` の4 imageのみをpublishした。
- 初回smokeではFPM warmup完了前の502をready扱いしていた。runnerはHTTP 200応答を確認するまで待つ形に修正した。
- official ext-grpcは `pecl install grpc-${version}` をLaravel/FPM bench image内で実行しない。特にパッチやカスタムビルドが必要ない限り、`ghcr.io/dkkoma/ext-grpc-artifacts` の `grpc.so` artifact imageからCOPYして使う。
- artifact利用への切替はlocal Docker buildで確認済み。`tools/diagnostics/laravel-fpm-spanner-bench/Dockerfile` の `official` targetで `1.58.0-php8.4-trixie-arm64-optimized` をCOPYし、`php -m` と `phpversion("grpc")` が `grpc` / `1.58.0` を返すことを確認した。

## 完了条件

- VM上でGHCR imageをpullしてLaravel/FPM Spanner比較が実行できる。
- 結果にCPU/request、RPS、avg/p50/p90/max latencyが含まれる。
- 実行条件と結果がこのissueに記録されている。
