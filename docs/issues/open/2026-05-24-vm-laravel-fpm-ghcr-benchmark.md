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
- 比較対象はgrpc-lite currentとofficial ext-grpc 1.58.0 artifact。amd64 performance comparatorでは `optimized-amd64-skylake` profileを使う。

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
- [x] artifact tag/profile変更をDockerfile、workflow、docsへ反映
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
- artifact tagは `<grpc-version>-php<php-version>-<distro>-<arch>-<profile>`。`pecl` は全arch、`optimized-amd64-skylake` はamd64専用。Laravel/FPM benchのGitHub Actions publishではamd64向けに `1.58.0-php8.4-trixie-amd64-optimized-amd64-skylake` を使う。
- Dockerfileは `EXT_GRPC_ARTIFACT_ARCH` build argでartifact archを明示する。GitHub Actionsは `amd64` 固定、arm64ローカル確認は `EXT_GRPC_ARTIFACT_ARCH=arm64` + `GRPC_OFFICIAL_PROFILE=pecl` を指定する。
- `tools/diagnostics/issue5-spanner-repro/Dockerfile` の通常official variantもartifact COPYへ切替済み。`grpc-official-frame-trace` は公式 `ext-grpc` にtrace patchを当てるため、例外としてsource buildを維持する。
- local smokeとして `docker build --target grpc-official -f tools/diagnostics/issue5-spanner-repro/Dockerfile --build-arg EXT_GRPC_ARTIFACT_ARCH=arm64 --build-arg GRPC_OFFICIAL_PROFILE=pecl ...` を実行し、artifact `grpc.so` が `phpversion("grpc") === 1.58.0` でロードできることを確認した。
- `--platform linux/amd64` + `EXT_GRPC_ARTIFACT_ARCH=amd64` + `GRPC_OFFICIAL_PROFILE=optimized-amd64-skylake` でも同じsmokeを実行し、current GCP/GitHub Actions用tagがロードできることを確認した。
- 旧 `optimized` profile tagを使ったrun `26375271620` / `26375466928` はartifact COPY経路確認としては有効だが、現行tag方針の最終確認ではない。
- `test` branch push後の GitHub Actions run `26375271620` は成功。`official` image buildはartifact COPY経路で完了した。
- VM smoke `laravel-fpm-artifact-smoke-20260524T230700Z` は `select_1row_10col` / `16 requests` / `c4` で `lite` / `official` ともにHTTP 200で完走した。
- 初回VM比較 `laravel-fpm-artifact-compare-20260524T230811Z` は完走。runnerのpercentile parseが `hey` の `50%%` 表記に合っておらず、実行時表示のp50/p90は0になったため、保存済み `hey-*.log` から値を確認し、runnerを修正した。

## VM比較結果

条件:

- VM: `grpc-lite-wire-e2micro`
- auth: metadata ADC
- FPM CPU quota: `2.0`
- requests/concurrency: `192` / `16`
- images:
  - `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:laravel-fpm-lite` digest `sha256:12b6265c7549a52fbb224fd658465ee4c46d3fed63e8d9c49b589b9e61f3f394`
  - `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:laravel-fpm-official` digest `sha256:050a93d330a91c47d0c6b60079eda5bae89612a9f5b2d03e4009e33ccd80fea5`

| variant | action | rps | cpu_us/req | avg_ms | p50_ms | p90_ms | p99_ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| lite | `transaction_select2_update1_insert1` | 9.6267 | 25,904.9 | 1,646.0 | 1,885.1 | 2,336.3 | 2,932.4 |
| official | `transaction_select2_update1_insert1` | 7.1552 | 211,233.8 | 2,126.5 | 2,065.8 | 3,001.4 | 3,579.8 |
| lite | `select_1row_10col` | 9.1327 | 146,035.0 | 1,650.7 | 1,554.6 | 2,568.8 | 3,514.5 |
| official | `select_1row_10col` | 9.1044 | 167,110.6 | 1,637.2 | 1,534.7 | 2,500.4 | 4,001.7 |

このrunでは全体latencyはSpanner側待ちが大きく、CPU/requestの値もaction間で大きく揺れる。runtime比較の継続には同一imageで複数runを取り、外れ値とSpanner待ちの影響を分ける必要がある。

## 完了条件

- VM上でGHCR imageをpullしてLaravel/FPM Spanner比較が実行できる。
- 結果にCPU/request、RPS、avg/p50/p90/max latencyが含まれる。
- 実行条件と結果がこのissueに記録されている。
