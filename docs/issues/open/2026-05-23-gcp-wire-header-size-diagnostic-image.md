# GCP wire/header-size diagnostic image

Status: Open

## 目的

GCP VM上で、Cloud Spanner `ExecuteStreamingSql` の request HEADERS size / outbound packet size / first inbound latency の関係を、ローカル回線依存を避けて計測する。

この作業の目的は「公開imageが起動すること」ではない。目的は、header sizeを変えたときにGoogle側からの応答開始時刻がどう変わるかを、tcpdumpとextension traceを突き合わせて判断できるデータを取ること。

## 背景

ローカル環境では回線差・テザリング・p99揺れが大きく、`HEADERS out -> first inbound frame` の差分がクライアント実装差なのかネットワーク揺れなのか切り分けにくい。

既存調査では、`ExecuteStreamingSql` の小さいSELECT系で grpc-lite の request HEADERS/TLS packet shape が ext-grpc と異なり、`x-bench-padding` + no-index でsteady HEADERS payloadを一定範囲へ寄せるとfirst inbound latencyが改善する可能性が見えている。ただしローカル回線依存が残るため、GCP VM上で再計測する。

## スコープ

- GHCR診断imageに、現在ブランチのgrpc-lite extensionをsource buildして入れる。
- 診断imageに `tcpdump` を同梱する。
- Spanner table SELECT 1 row 10 columnsのmarked scriptをimageへ同梱する。
- VMで実行する1コマンドの診断runnerを追加し、以下を `/results` に保存する。
  - `markers.log`: PHP marker wall time
  - `trace.jsonl`: grpc-lite HTTP/2 frame trace
  - `tcpdump.pcap`: packet capture
  - `summary.txt`: streamごとのHEADERS sizeとfirst inbound latency集計
- paddingなし / paddingあり の最低限のシナリオをローカルでsmoke確認する。

## 非スコープ

- production採用判断。
- paddingを正式機能化すること。
- ext-grpcの完全なHTTP/2 frame traceをこのimageで再現すること。ext-grpcは比較wall timeとtcpdump上のpacket timing参照に留める。ext-grpcのHEADERS sizeは別途instrumented buildの調査記録を参照する。
- Pub/Sub / Secret Managerのcross-check。

## 計測モデル

- extension traceの `wire.frame_out HEADERS` で stream id と HEADERS payload length を取る。
- extension traceの最初の同stream `wire.frame_in` までを `client observed first inbound latency` とする。
- tcpdumpでは同じRPC期間の outbound TLS packet length と inbound TLS packet timing を確認する。
- 厳密なserver-side wall timeではなく、GCP VM上で観測した `client outbound observed -> client inbound observed` をGoogle側応答開始に近い指標として扱う。

## 計画

1. 既存GHCR workflow / Dockerfileを、repo root contextからsource-built grpc-liteを作る形に変える。
2. `tcpdump` と wire diagnostic runnerを追加する。
3. local dockerで `select-table` のtrace/marker出力が作れることを確認する。
4. 権限が必要なtcpdumpは、ローカルで可能なら確認し、VMでは `--cap-add NET_RAW --cap-add NET_ADMIN` を前提にする。
5. 確認結果とVM実行コマンドをこのissueに記録する。

## 進捗

- 2026-05-23: issue作成。直前のimage smokeが目的から外れていたため、wire/header-size diagnostic imageとしてスコープを切り直した。

## 検証

未実施。

## 判断ログ

- `official` imageはPECL ext-grpc 1.58.0の比較用として残すが、HTTP/2 HEADERS sizeの一次ソースにはしない。暗号化TLSのtcpdumpだけではHEADERS block lengthは見えないため。
- `lite` imageはPackagist releaseではなく、このブランチのsource-built extensionを使う。今回必要なtrace/padding/no-index実験ノブがrelease版に入っていないため。

## 完了条件

- GHCRの `lite` diagnostic imageに現在ブランチのtrace/padding実装が入っている。
- VM用runnerで `markers.log`, `trace.jsonl`, `tcpdump.pcap`, `summary.txt` を出せる。
- ローカルで少数iterationのSpanner診断が完走し、traceからHEADERS sizeとfirst inbound latencyを集計できる。
- 2026-05-23: `lite` imageをPackagist installからrepo source buildへ変更。現在ブランチのtrace/padding/no-index実験ノブを含むextensionをimageへ入れる方針にした。
- 2026-05-23: `tcpdump`、`select-table-marked-app-extra-header.php`、`issue5-wire-diagnostic` runner、`wire-summary.php` を追加。
- 2026-05-24: `vast-falcon-165704` で Compute Engine APIを有効化し、Spanner instanceと同じ `asia-northeast1-a` に `e2-micro` VM `grpc-lite-wire-e2micro` を作成した。
- 2026-05-24: VM上のDockerでGHCR imageをpullし、SA JSONを `/home/daisuke/sa.json` に配置した。
- 2026-05-24: VM上では診断containerに `--network host` が必要。通常bridge networkではtcpdump filterはpacketを受けてもpcapが欠けるケースがあった。
- 2026-05-24: `tcpdump -Z root -U` と計測終了後の `TCPDUMP_STOP_GRACE=1` をrunnerへ追加し、VM上でpcapが欠けずに残ることを確認した。

## ローカル検証結果

2026-05-23に `php-grpc-lite-spanner-repro:diag-lite` をローカルbuildし、real Spanner / SA JSON / `BenchRows WHERE Id = 1` で少数iterationのwire diagnosticを実行した。

Build / static checks:

- `docker buildx build --check --build-arg GRPC_VARIANT=lite -f tools/diagnostics/issue5-spanner-repro/Dockerfile .`: OK
- `docker buildx build --load --build-arg GRPC_VARIANT=lite -f tools/diagnostics/issue5-spanner-repro/Dockerfile -t php-grpc-lite-spanner-repro:diag-lite .`: OK
- container内 `php --ri grpc` で `grpc_lite.http2_experimental_no_index_x_bench_padding` など実験INIが入っていることを確認。
- container内に `tcpdump` と `issue5-wire-diagnostic` があることを確認。

Smoke run:

```sh
docker run --rm \
  --cap-add NET_RAW --cap-add NET_ADMIN \
  -v "$SA_KEY":/sa.json:ro \
  -v "$PWD/$out":/results \
  -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
  -e GOOGLE_CLOUD_PROJECT=vast-falcon-165704 \
  -e DB_SPANNER_INSTANCE=bench \
  -e DB_SPANNER_DATABASE=laravel-bench-db \
  -e ITER=3 \
  -e SPANNER_GRPC_EXTRA_HEADER_BYTES="$pad" \
  -e PHP_INI_ARGS='-d grpc_lite.http2_experimental_no_index_x_bench_padding=1' \
  --entrypoint issue5-wire-diagnostic \
  php-grpc-lite-spanner-repro:diag-lite
```

Result dir:

- `var/bench-results/gcp-wire-header-diag-local-20260523T123740Z/`

Output files confirmed for each run:

- `markers.log`
- `trace.jsonl`
- `tcpdump.pcap`
- `tcpdump.txt`
- `summary.txt`

Observed sample:

| pad | steady HEADERS payload | steady HEADERS -> first inbound p50 | marker p50 |
|---:|---:|---:|---:|
| 0 | 642B | 124875us | 126948us |
| 510 | 1104B | 72027us | 76011us |

このローカル値はネットワーク依存を含むため性能判断には使わない。VM検証前の確認として、header sizeを変えたrunでtrace/pcap/marker/summaryが揃い、summary上でHEADERS payload差とfirst inbound差を読めることを確認した。

## GHCR publish / public image検証

2026-05-23に `test` branch pushでGHCR publish workflowを実行した。

- Workflow run: https://github.com/dkkoma/php-grpc-lite/actions/runs/26333201920
- Head SHA: `fe9a74efe35a66e35e6c15db66727c0490272cb2`
- `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:lite`: publish成功
- `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:official`: publish成功

`lite` public image verification:

- `docker pull --platform linux/amd64 ghcr.io/dkkoma/php-grpc-lite-spanner-repro:lite`: OK
- Digest: `sha256:d8832cf1162f78b2a315ce95f889998ceb29433121dc69fb711e5dd19146942c`
- `php --ri grpc` で以下の実験INIが入っていることを確認:
  - `grpc_lite.http2_experimental_ext_grpc_158_settings_profile`
  - `grpc_lite.http2_experimental_ext_grpc_158_wire_profile`
  - `grpc_lite.http2_experimental_ext_grpc_158_header_padding_target`
  - `grpc_lite.http2_experimental_no_index_x_bench_padding`

`lite` public image Spanner diagnostic smoke:

- Result dir: `var/bench-results/ghcr-wire-diag-lite-smoke-20260523T130535Z/`
- command: `issue5-wire-diagnostic`
- environment: real Spanner / SA JSON / `ITER=3` / `SPANNER_GRPC_EXTRA_HEADER_BYTES=510` / `grpc_lite.http2_experimental_no_index_x_bench_padding=1`
- output files: `markers.log`, `trace.jsonl`, `tcpdump.pcap`, `tcpdump.txt`, `summary.txt`

Observed sample:

| stream | HEADERS payload | HEADERS -> first inbound |
|---:|---:|---:|
| 3 | 1233B | 68186us |
| 5 | 1101B | 32132us |
| 7 | 1101B | 43604us |

`x-bench-padding` がstream 5以降でも圧縮で落ちず、steady HEADERS payloadが `1101B` として残ることを確認した。これにより、VM上でheader size sweepを実行できるimageとして成立している。

`official` public image verification:

- `docker pull --platform linux/amd64 ghcr.io/dkkoma/php-grpc-lite-spanner-repro:official`: OK
- Digest: `sha256:92f03356106a14b8f629a75132bbcc47542d864fdf03392773c97c7c34d14f03`
- `extension_loaded("grpc") === true`
- `Grpc\VERSION === 1.58.0`

`official` public image diagnostic smoke:

- Result dir: `var/bench-results/ghcr-wire-diag-official-smoke-20260523T131900Z/`
- command: `issue5-wire-diagnostic`
- environment: real Spanner / SA JSON / `ITER=1`
- output files: `markers.log`, `tcpdump.pcap`, `tcpdump.txt`, `summary.txt`
- `official` imageはlite traceを出さないため `select_streams=0` になるが、markerとtcpdumpは取得できる。

## GCP VM検証

2026-05-24にGCP VMを作成し、VM上でdiagnostic imageを実行できることを確認した。

VM:

- Project: `vast-falcon-165704`
- Zone: `asia-northeast1-a`
- Instance: `grpc-lite-wire-e2micro`
- Machine type: `e2-micro`
- OS: Container-Optimized OS `cos-stable`
- Boot disk: `30GB pd-standard`
- External IP at creation: `34.146.128.188`
- Spanner instance: `bench` (`regional-asia-northeast1`)

作成後確認:

- `docker --version`: `Docker version 27.5.1`
- stateful partition: 約26GB free
- `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:lite` pull OK
- `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:official` pull OK

VM上の最終lite smoke:

- Workflow run: https://github.com/dkkoma/php-grpc-lite/actions/runs/26347127675
- Head SHA: `f9d7233c21f02bffa1d709af546c2ebc583a21ba`
- Result dir on VM: `/home/daisuke/results/lite-pad510-final-20260524T001109Z`
- command: `issue5-wire-diagnostic`
- Docker options: `--network host --cap-add NET_RAW --cap-add NET_ADMIN`
- environment: real Spanner / SA JSON / `ITER=5` / `SPANNER_GRPC_EXTRA_HEADER_BYTES=510` / `grpc_lite.http2_experimental_no_index_x_bench_padding=1`
- output files: `markers.log`, `trace.jsonl`, `tcpdump.pcap`, `tcpdump.txt`, `summary.txt`
- tcpdump result: `54 packets captured`, `54 packets received by filter`, `0 packets dropped`
- `tcpdump.txt`: 54 lines

Observed sample:

| stream | HEADERS payload | HEADERS -> first inbound |
|---:|---:|---:|
| 3 | 1235B | 17837us |
| 5 | 1103B | 5346us |
| 7 | 1103B | 5471us |
| 9 | 1103B | 5652us |
| 11 | 1103B | 3958us |

VM上のofficial smoke:

- Result dir on VM: `/home/daisuke/results/official-final-20260524T001135Z`
- command: `issue5-wire-diagnostic`
- Docker options: `--network host --cap-add NET_RAW --cap-add NET_ADMIN`
- environment: real Spanner / SA JSON / `ITER=3`
- output files: `markers.log`, `tcpdump.pcap`, `tcpdump.txt`, `summary.txt`
- tcpdump result: `46 packets captured`, `46 packets received by filter`, `0 packets dropped`
- `tcpdump.txt`: 46 lines
- marker elapsed p50: `8367us`

この時点で、VM上で `lite` はgrpc-lite trace + marker + tcpdump、`official` はmarker + tcpdumpを取得できる。以降のheader-size sweepはこのVMと同じrunner条件で実施する。

## official request shape確認

2026-05-24に、VM上で `official` imageのC-core traceを有効化し、SA JSONとADC OAuthで `SELECT 1` のrequest shapeを確認した。

実行条件:

- VM: `grpc-lite-wire-e2micro` / `asia-northeast1-a`
- image: `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:official`
- `Grpc\VERSION`: `1.58.0`
- RPC: `CreateSession` -> `ExecuteStreamingSql SELECT 1` x 3 -> `DeleteSession`
- trace: `GRPC_TRACE=http,api`, `GRPC_VERBOSITY=DEBUG`
- tcpdump: `--network host --cap-add NET_RAW --cap-add NET_ADMIN`
- result archive: `var/gcp-vm-results/official-request-shape-20260524T023928Z.tar.gz`

Stock `ext-grpc` / gRPC C-core traceで取れたもの:

- `SEND_INITIAL_METADATA` のmetadata name/value、重複metadata、credential由来metadata、`SEND_MESSAGE len`
- inbound `SETTINGS` / `WINDOW_UPDATE` / `HEADERS` / `DATA` / `PING` のframe typeとpayload length
- tcpdump上のTLS packet lengthとtiming

Stock traceで取れなかったもの:

- client outboundのHPACK encoded HEADERS frame payload length
- client outbound HEADERS / DATA / PING のHTTP/2 frame境界

そのため、officialの「metadata shape」と「TLS packet shape」は確認できるが、grpc-lite traceと同じ粒度の `wire.frame_out HEADERS payload_len` はstock official imageでは取得できない。exactなofficial outbound HEADERS sizeが必要な場合は、official gRPC C-coreをinstrumentしてsource buildする必要がある。

SA JSON metadata summary:

| RPC | message len | metadata entries | metadata name+value bytes | auth value len | credential marker |
|---|---:|---:|---:|---:|---|
| `CreateSession` | 74 | 15 | 1344 | 739 | `x-goog-api-client: cred-type/jwt` |
| `ExecuteStreamingSql` | 164 | 14 | 1394 | 739 | `x-goog-api-client: cred-type/jwt` |
| `ExecuteStreamingSql` | 164 | 14 | 1394 | 739 | `x-goog-api-client: cred-type/jwt` |
| `DeleteSession` | 146 | 14 | 1383 | 739 | `x-goog-api-client: cred-type/jwt` |

ADC OAuth metadata summary:

| RPC | message len | metadata entries | metadata name+value bytes | auth value len | credential marker | extra |
|---|---:|---:|---:|---:|---|---|
| `CreateSession` | 74 | 16 | 901 | 261 | `x-goog-api-client: cred-type/u` | `x-goog-user-project: vast-falcon-165704` |
| `ExecuteStreamingSql` | 164 | 14 | 923 | 261 | none on this captured stream | `x-goog-user-project: vast-falcon-165704` |
| `DeleteSession` | 146 | 14 | 912 | 261 | none on this captured stream | `x-goog-user-project: vast-falcon-165704` |

Inbound server SETTINGSはSA JSON / ADC OAuthで同じ:

| setting | value |
|---|---:|
| `SETTINGS_MAX_CONCURRENT_STREAMS` | 100 |
| `SETTINGS_INITIAL_WINDOW_SIZE` | 1048576 |
| `SETTINGS_MAX_HEADER_LIST_SIZE` | 65536 |

tcpdump上のrequest packet例:

| credential | phase | outbound TCP payload length |
|---|---|---:|
| SA JSON | CreateSession request | 1554 |
| SA JSON | ExecuteStreamingSql request 1 | 1472 |
| SA JSON | ExecuteStreamingSql request 2 | 1400 |
| ADC OAuth | CreateSession request | 1114 |
| ADC OAuth | ExecuteStreamingSql request 1 | 998 |
| ADC OAuth | ExecuteStreamingSql request 2 | 948 |

確認結果:

- official SA JSONはJWT auth valueが約739Bで、ADC OAuthのBearer token約261Bより大きい。
- official SA JSONはcredential markerとして `cred-type/jwt` を送る。ADC OAuthは `cred-type/u` と `x-goog-user-project` が見える。
- official SA JSONとADC OAuthでinbound SETTINGSは同じなので、少なくともserver SETTINGS差では説明できない。
- officialのoutbound request packetはSA JSONのほうがADC OAuthより明確に大きい。ただしこの値はTLS record/TCP payloadであり、HPACK HEADERS payload lengthそのものではない。
- 以降、officialのexact outbound HEADERS sizeを比較軸にするなら、stock traceではなくinstrumented official buildを使う。
