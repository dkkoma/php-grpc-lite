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
