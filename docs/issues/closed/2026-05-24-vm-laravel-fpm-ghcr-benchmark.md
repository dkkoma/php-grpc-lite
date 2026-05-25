---
Status: Closed
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
- 比較対象はgrpc-lite currentとofficial ext-grpc 1.58.0 artifact。VMの実CPUがBroadwellだったため、通常比較ではofficial ext-grpc `pecl` profileとgrpc-lite追加最適化flagなしを揃えて使う。

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
- [x] VM比較実行

## 検証条件

- VM: `grpc-lite-wire-e2micro` / `asia-northeast1-a`
- VM attached service account: `test-spanner@vast-falcon-165704.iam.gserviceaccount.com`
- auth: VM metadata ADCのみ。credential JSONは使わない。
- Spanner: `vast-falcon-165704` / `bench` / `laravel-bench-db`
- FPM: 16 workers / CPU quota 2.0（`e2-micro` のVM上限に合わせる）
- VM CPU: Intel family 6 model 79 / Broadwell。`optimized-amd64-skylake` artifactは標準比較に使わない。

## 判断ログ

- VM上buildは行わない。runtime比較ではbuild時間、build artifact差、VM CPU消費がノイズになるため、GitHub Actions / GHCRで事前buildする。
- GHCR packageは既存の `php-grpc-lite-spanner-repro` を使う。新規packageのvisibility設定を増やさず、既にpublic pull確認済みのpackageへ用途別tagを追加する。
- 既存のissue #5 wire diagnostic image workflowはこのbranchでは用途外とし、`test` branch pushではLaravel/FPM bench用tagだけをpublishする。
- VM runnerは `GOOGLE_APPLICATION_CREDENTIALS` を渡さず、metadata server ADCだけで実行する。
- GHCR publishは GitHub Actions run `26360354877` で成功。`lite` / `official` / `nginx` / `loadgen` の4 imageのみをpublishした。
- 初回smokeではFPM warmup完了前の502をready扱いしていた。runnerはHTTP 200応答を確認するまで待つ形に修正した。
- official ext-grpcは `pecl install grpc-${version}` をLaravel/FPM bench image内で実行しない。特にパッチやカスタムビルドが必要ない限り、`ghcr.io/dkkoma/ext-grpc-artifacts` の `grpc.so` artifact imageからCOPYして使う。
- artifact tagは `<grpc-version>-php<php-version>-<distro>-<arch>-<profile>`。`pecl` は全arch、`optimized-amd64-skylake` はamd64専用。Laravel/FPM benchのGitHub Actions publish defaultは `1.58.0-php8.4-trixie-amd64-pecl` とし、optimized profileは実CPU targetを満たす場合の明示比較だけに使う。
- Dockerfileは `EXT_GRPC_ARTIFACT_ARCH` build argでartifact archを明示する。GitHub Actionsは `amd64` 固定、arm64ローカル確認は `EXT_GRPC_ARTIFACT_ARCH=arm64` + `GRPC_OFFICIAL_PROFILE=pecl` を指定する。
- `tools/diagnostics/issue5-spanner-repro/Dockerfile` の通常official variantもartifact COPYへ切替済み。`grpc-official-frame-trace` は公式 `ext-grpc` にtrace patchを当てるため、例外としてsource buildを維持する。
- local smokeとして `docker build --target grpc-official -f tools/diagnostics/issue5-spanner-repro/Dockerfile --build-arg EXT_GRPC_ARTIFACT_ARCH=arm64 --build-arg GRPC_OFFICIAL_PROFILE=pecl ...` を実行し、artifact `grpc.so` が `phpversion("grpc") === 1.58.0` でロードできることを確認した。
- `--platform linux/amd64` + `EXT_GRPC_ARTIFACT_ARCH=amd64` + `GRPC_OFFICIAL_PROFILE=optimized-amd64-skylake` でも同じsmokeを実行し、current GCP/GitHub Actions用tagがロードできることを確認した。
- `test` branch push後の GitHub Actions run `26376549711` は成功。Laravel/FPM bench image publishは現行tag `1.58.0-php8.4-trixie-amd64-optimized-amd64-skylake` を使う経路で完了した。
- 公式 `gcr.io/google.com/cloudsdktool/google-cloud-cli:stable` container + `openssh-client` でVMのCPUを確認した。`gcloud` はホストinstallではなく公式containerで実行する。
- VM CPUは `GenuineIntel` / family `6` / model `79` / `Intel(R) Xeon(R) CPU @ 2.20GHz` で、SkylakeではなくBroadwell系だった。以後このVMの標準比較はofficial `pecl` profileに戻す。
- `test` branch push後の GitHub Actions run `26377329903` は成功。Laravel/FPM bench image publishはofficial `pecl` profile defaultへ戻した状態で完了した。
- pecl profile imageでVM比較 `laravel-fpm-pecl-compare-20260525T004000Z` を実行した。VM上のrunner表示は古いpercentile parseでp50/p90が0になったため、保存済み `hey-*.log` からp50/p90/p99を再集計した。
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

## VM比較結果(pecl profile)

条件:

- VM: `grpc-lite-wire-e2micro`
- VM CPU: Intel family 6 model 79 / Broadwell
- auth: metadata ADC
- FPM CPU quota: `2.0`
- requests/concurrency: `192` / `16`
- run id: `laravel-fpm-pecl-compare-20260525T004000Z`
- official ext-grpc artifact: `1.58.0-php8.4-trixie-amd64-pecl`
- grpc-lite build: Laravel/FPM bench Dockerfileの通常build。追加 `-O3` / LTO / CPU target flagなし。

| variant | action | rps | cpu_us/req | avg_ms | p50_ms | p90_ms | p99_ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| lite | `transaction_select2_update1_insert1` | 23.0666 | 27,349.3 | 678.6 | 683.2 | 741.2 | 893.2 |
| official | `transaction_select2_update1_insert1` | 7.1029 | 208,932.3 | 2,129.7 | 2,100.7 | 2,994.8 | 4,505.7 |
| lite | `select_1row_10col` | 8.8370 | 143,021.5 | 1,722.5 | 1,548.7 | 2,774.9 | 5,857.2 |
| official | `select_1row_10col` | 9.3933 | 161,870.0 | 1,564.9 | 1,382.7 | 2,905.1 | 4,005.6 |

このrunでもtransaction系はliteが大きく速く、small selectはofficialが少し速い。e2-micro + real SpannerではSpanner待ちとVM CPU制約の揺れが大きいため、採否判断には複数runか、より安定したGCE machine typeでの再測定が必要。

## VM比較結果(minimal SELECT 1)

条件:

- VM: `grpc-lite-wire-e2micro`
- VM CPU: Intel family 6 model 79 / Broadwell
- auth: metadata ADC
- images: `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:official` / `:lite`
- script: `select1-bench.php`
- workload: `Database::execute('SELECT 1')->rows()->current()` のみ。warmupで同じ `SELECT 1` を1回実行後に計測。
- official ext-grpc: `1.58.0`
- grpc-lite: `0.1.0`
- google/cloud-spanner: `1.106.0.0`

| iter | variant | mean_us | p50_us | p90_us | p99_us | min_us | max_us |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 200 | official | 5,151 | 5,460 | 6,130 | 7,258 | 3,421 | 10,180 |
| 200 | lite | 5,341 | 5,479 | 6,414 | 9,230 | 3,127 | 10,523 |
| 1000 | official | 5,359 | 5,672 | 6,276 | 7,812 | 3,439 | 11,215 |
| 1000 | lite | 4,918 | 5,201 | 5,855 | 6,789 | 3,077 | 11,073 |

metadata ADC条件のminimal `SELECT 1` では、official peclとlite通常buildの差は小さい。1000 iterationではliteがp50/p90/p99で上回ったが、200 iterationではほぼ同等でofficialがやや良い値もあるため、現時点では「大きな性能差は再現しない」と扱う。

## VM比較結果(issue #5 original minimal repro)

条件:

- VM: `grpc-lite-wire-e2micro`
- VM CPU: Intel family 6 model 79 / Broadwell
- auth: metadata ADC
- images: `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:official` / `:lite`
- script: `cli-bench.php`
- workload: warmup `SELECT 1` 後、各iterationで `Database::runTransaction()` 内から `Transaction::execute('SELECT @i')` と `Transaction::commit()` を実行する。
- official ext-grpc: `1.58.0`
- grpc-lite: `0.1.0`
- google/cloud-spanner: `1.106.0.0`

| iter | variant | mean_us | p50_us | p90_us | p99_us | min_us | max_us |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 200 | official | 11,381 | 10,255 | 11,690 | 19,787 | 8,737 | 191,677 |
| 200 | lite | 10,989 | 10,195 | 11,281 | 15,653 | 9,007 | 134,935 |
| 1000 | official | 10,857 | 10,679 | 12,158 | 14,761 | 8,006 | 46,087 |
| 1000 | lite | 9,356 | 9,275 | 10,170 | 11,155 | 7,510 | 39,455 |

metadata ADC条件のissue #5 original reproでは、official peclよりlite通常buildのほうが速い。1000 iterationではliteがp50で約1.4ms、p99で約3.6ms良い。少なくともこのVM + metadata ADC条件では、issue #5の発端だった「liteがofficialより大きく遅い」状態は再現しない。

## 最終判断

- official ext-grpc pecl artifactとgrpc-lite通常buildを、GCP VM上のmetadata ADC条件で比較できる状態を作れた。
- VM CPUはBroadwell系だったため、`optimized-amd64-skylake` artifactは標準比較から外し、official peclとgrpc-lite通常buildで比較条件を揃えた。
- `SELECT 1` 単体、issue #5 original minimal repro、Laravel/FPM transaction系のいずれでも、issue #5の発端だった「grpc-liteがofficial ext-grpcより大きく遅い」状態は再現しなかった。
- Laravel/FPM small selectではofficialがやや速いrunもあるが、差は小さく、Spanner待ちとe2-microの揺れを超える強い結論にはしない。
- SA JSONによるheaders size増加、headers padding、特定header size境界による速度差は、VM上で再現せず、ローカル測定環境・ネットワーク経路依存の可能性が高い。今後の本流の改善候補から外す。
- したがってこのissueの目的は完了。今後同種の性能差を見る場合は、GCP VM上、metadata ADC、official artifact profileとCPU targetを揃えた条件を基準に、再現条件から改めて切る。

## 完了条件

- VM上でGHCR imageをpullしてLaravel/FPM Spanner比較が実行できる。
- 結果にCPU/request、RPS、avg/p50/p90/max latencyが含まれる。
- 実行条件と結果がこのissueに記録されている。

## 完了記録

- Closed: 2026-05-25
- Fix / verification commits:
  - `1046016` Issue 5: official ext-grpc artifact利用方針を全体へ反映
  - `fb2b43b` Issue 5: VM比較のofficial profileをpeclへ戻す
  - `d8ece49` Issue 5: pecl profileでVM比較を再計測
  - `ecf8c99` Issue 5: minimal SELECT 1のVM比較を記録
  - `9e45e77` Issue 5: original minimal reproのVM比較を記録
