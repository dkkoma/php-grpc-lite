---
Status: Open
Owner: Codex
Created: 2026-05-21
Related:
  - https://github.com/dkkoma/php-grpc-lite/issues/5
  - docs/issues/open/2026-05-21-http2-control-lifecycle-experiments.md
---

# ext-grpc 1.58 SA JSON wire shape 最大寄せ実験

## 目的

real Spanner `ExecuteStreamingSql SELECT 1` のSA JSON条件に絞り、捨て実装前提でgrpc-liteのwire shapeをext-grpc 1.58に可能な限り寄せ、残る差がHTTP/2 semantic / HPACK / TLS/TCP timingのどこにあるか確認する。

## 背景

既存実験ではinitial `SETTINGS`、response後connection-level `WINDOW_UPDATE`、initial `SETTINGS ACK`待ちを個別に試したが、どれも20ms級の差を単独で説明するほどではなかった。

一方、既存traceではext-grpc 1.58 SA JSONとgrpc-lite SA JSONに少なくとも次の差が残っている。

- ext-grpc initial `SETTINGS` profile。
- ext-grpcはpeer `SETTINGS`を受け取り、client `SETTINGS ACK`を送った直後に最初のstreamを送る。grpc-lite defaultはpeer `SETTINGS`を受け取る前に最初のstreamを送る。
- ext-grpcはresponse `DATA` 後に小さいconnection-level `WINDOW_UPDATE`を送る。
- ext-grpcのrequest HEADERS block lengthはgrpc-liteより大きい。SA JSONでは大きい`authorization` metadataがあり、HPACK indexing/no-index差の可能性がある。

## スコープ

- 計測対象はSA JSONのみ。
- ext-grpc 1.58観測profileへ寄せるdiagnostic flagを追加する。
- initial SETTINGS / initial connection WINDOW_UPDATEを寄せる。
- peer SETTINGS受信後、client SETTINGS ACKをflushしてから最初のstreamを送る。
- response DATA chunk受信時にconnection-level WINDOW_UPDATEを送る。
- authorization metadataをHPACK no-index扱いにする実験を含める。
- ext-grpc 1.58 SA JSON と grpc-lite SA JSON 最大寄せprofileをtrace/tcpdump付きで比較する。

## 非スコープ

- production defaultへの採用。
- ext-grpc Core実装のコピー。
- TLS record境界やTCP packet timestampの完全一致。
- ADC条件の計測。

## 計画

- [x] diagnostic flagを追加する。
- [x] ext-grpc 1.58 initial control profileを合成する。
- [x] peer SETTINGS受信後stream開始のsetup lifecycleを実装する。
- [x] authorization no-indexを実装する。
- [x] HPACK送信dynamic table無効化を実装する。
- [x] CreateSession後のactive PINGを実装する。
- [ ] PHPT/static analysisを通す。
- [x] SA JSONでext-grpc/lite最大寄せprofileを比較する。
- [x] 残差をHTTP/2 semantic / HPACK / timingに分類する。
- [x] TLS packet length差がHEADERS block length差で説明できるか確認する。
- [x] nghttp2 public APIを超えたHPACK encoder制御が必要か判断する。
- [x] HTTP/2 HEADERS paddingでpacket lengthだけをext-grpcに合わせる。

## 進捗

- `grpc_lite.http2_experimental_ext_grpc_158_wire_profile=1` を追加した。default off。
- このprofileは次をまとめて有効にする。
  - ext-grpc 1.58観測initial `SETTINGS` profile。
  - initial connection-level `WINDOW_UPDATE=4128769`。
  - peer `SETTINGS` 受信後、client `SETTINGS ACK` をflushしてから最初のstreamを送る。client initial `SETTINGS`へのpeer ACKまでは待たない。
  - response `DATA` chunk受信時のconnection-level `WINDOW_UPDATE`。
  - `authorization` metadataの `NGHTTP2_NV_FLAG_NO_INDEX`。
  - nghttp2送信HPACK dynamic table size 0。
  - CreateSession後、最初のserver PING受信時にclient-origin active `PING`を1回送る。
  - diagnosticとして、`ExecuteStreamingSql` のHEADERS payload lengthをHTTP/2 paddingでext-grpc観測値に合わせる。

## 検証

- Raw data: `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/`
- 対象はSA JSONのみ。

### latency

tcpdumpなし / traceあり / n=100 の `ExecuteStreamingSql SELECT 1`。値はPHP markerのelapsed。

| variant | p50 us | p90 us | p99 us | note |
|---|---:|---:|---:|---|
| ext-grpc 1.58 SA | 11382 | 13466 | 16312 | reference |
| lite baseline SA | 22294 | 23961 | 26903 | default lite |
| lite wire-profile SA | 22053 | 23973 | 25876 | auth no-index + HPACK table 0 + control order寄せ |
| lite wire-profile SA | 22215 | 25681 | 47186 | 非pseudo header全no-index追加。改善なし |
| lite wire-profile SA | 13167 | 16350 | 23608 | HEADERS paddingでpacket length一致 |

### request DATA -> first response HEADERS/DATA

tcpdumpあり / traceあり / n=20。stream id 1のCreateSessionを除外した `ExecuteStreamingSql` streams。

| variant | p50 us | p90 us | p99 us |
|---|---:|---:|---:|
| ext-grpc 1.58 SA | 8889 | 10877 | 44622 |
| lite baseline SA | 17889 | 22507 | 43671 |
| lite wire-profile SA | 18034 | 22051 | 43438 |
| lite wire-profile SA + HEADERS padding | 10339 | 15854 | 48903 |

### HEADERS padding target sweep

全部盛りwire-profileのまま、`ExecuteStreamingSql` HEADERS payload length targetだけを変えた。tcpdumpなし / traceあり / n=100。ただし `1191-n300`、`official/ext/baseline-n300` は同時刻 n=300。

| variant | target payload | expected steady packet | n | p50 us | p90 us | p99 us |
|---|---:|---:|---:|---:|---:|---:|
| ext-grpc 1.58 SA | observed 1191 steady | 1400 | 300 | 12686 | 14874 | 17122 |
| lite baseline SA | 639 steady | 848 | 300 | 22653 | 24799 | 29466 |
| lite wire-profile | 0 / no padding | 1231 | 100 | 21536 | 23765 | 25820 |
| lite wire-profile | 1100 | 1309 | 100 | 23471 | 26002 | 28032 |
| lite wire-profile | 1150 | 1359 | 100 | 22906 | 25367 | 29904 |
| lite wire-profile | 1175 | 1384 | 100 | 23456 | 26922 | 32323 |
| lite wire-profile | 1191 | 1400 | 100 | 14296 | 16301 | 21284 |
| lite wire-profile | 1191 | 1400 | 300 | 12767 | 15556 | 17314 |
| lite wire-profile | 1260 | 1469 | 300 | 13832 | 16949 | 28340 |
| lite wire-profile | ext pattern 1260/1210/1191 | 1469/1419/1400 | 300 | 13769 | 16147 | 18501 |

観測:

- `1100`、`1150`、`1175` は改善しない。
- `1191` で急に改善し、同時刻n=300ではext-grpcとほぼ同等のp50/p99になる。
- `1191` targetのtcpdumpではsteady outbound request packetが `1400B` になった。
- `1260` やext patternは改善するが、`1191`固定よりtailが悪い。大きければ大きいほど良いわけではない。

因果仮説:

- 改善はHPACK内容の意味ではなく、TLS/TCP packet sizeがGoogle Frontend側の処理境界に乗ることに強く相関している。
- 閾値は `HEADERS payload 1191B`、tcpdump上のsteady outbound packet `1400B` 付近に見える。
- `1175 -> 1191` の16B差でp50が大きく変わるため、header countやmetadata valueの意味差ではなく、packetization / TLS record / GFE ingress buffering の境界条件である可能性が高い。
- ただしpaddingは診断手段であり、production実装としてはHPACK encoderを制御して自然に同じpacket shapeを作るか、Google向けheuristicとして明示的に採用判断する必要がある。

### RPC shape variation: unary ExecuteSql / different streaming SQL

`ExecuteStreamingSql SELECT 1` 以外でも同じ現象かを確認するため、GAPIC `SpannerClient` 経由で次を追加計測した。

- unary: `ExecuteSql SELECT 1 AS n`
- server streaming: `ExecuteStreamingSql SELECT n FROM UNNEST(GENERATE_ARRAY(1, 10)) AS n ORDER BY n`

latency: tcpdumpなし / traceあり / n=100。

| case | variant | p50 us | p90 us | p99 us |
|---|---|---:|---:|---:|
| unary ExecuteSql | ext-grpc 1.58 SA | 21821 | 23782 | 27269 |
| unary ExecuteSql | lite baseline SA | 21864 | 24842 | 27704 |
| unary ExecuteSql | lite wire-profile target 1191 | 21437 | 23184 | 26801 |
| unary ExecuteSql | lite wire-profile max reachable padding 1126 | 22874 | 26642 | 32874 |
| streaming SQL 10 rows | ext-grpc 1.58 SA | 22341 | 25069 | 36405 |
| streaming SQL 10 rows | lite baseline SA | 21846 | 24161 | 27023 |
| streaming SQL 10 rows | lite wire-profile target 1191 | 22744 | 25770 | 30029 |
| streaming SQL 10 rows | lite wire-profile max reachable padding 1131 | 22432 | 24711 | 28509 |

HEADERS size: tcpdumpあり / traceあり / n=20。

| case | variant | first HEADERS payload | steady HEADERS payload | steady packet approx |
|---|---|---:|---:|---:|
| unary ExecuteSql | ext-grpc 1.58 SA | 1060 | 996 | 1202 |
| unary ExecuteSql | lite baseline SA | 703 | 577 | 783 |
| unary ExecuteSql | lite wire-profile target 1191 | 870 | 870 | 1076 |
| unary ExecuteSql | lite wire-profile max reachable padding | 1126 | 1126 | 1332 |
| streaming SQL 10 rows | ext-grpc 1.58 SA | 1068 | 996 | 1248 |
| streaming SQL 10 rows | lite baseline SA | 705 | 574 | 826 |
| streaming SQL 10 rows | lite wire-profile target 1191 | 875 | 875 | 1127 |
| streaming SQL 10 rows | lite wire-profile max reachable padding | 1131 | 1131 | approx 1383 |

観測:

- ext-grpcのHEADERS sizeはRPC method / SQL / request metadata shapeで変わる。`ExecuteStreamingSql SELECT 1` のsteady `1191B` は一般値ではない。
- unary `ExecuteSql` と別streaming SQLでは、ext-grpc steady HEADERSはどちらも `996B` 付近で、`SELECT 1` streamingの `1191B` より小さい。
- HTTP/2 HEADERS paddingはPad Lengthが1 byteなので、1つのHEADERS frameで増やせるpayloadは最大 `+256B`。liteのHPACK blockが `870B/875B` 程度の場合、1191Bまではpaddingだけでは届かない。
- これら2ケースでは、paddingで明確な改善は出ていない。特にunary `ExecuteSql` はext-grpcとlite baselineが元からほぼ同等で、paddingによる改善余地が小さい。

判断:

- `1191B` はSpanner `ExecuteStreamingSql SELECT 1` の特定shapeで効いた境界であり、全RPCへ機械適用する値ではない。
- production候補にするなら、固定1191 paddingではなく、RPC/metadata shapeごとのpacket-size thresholdまたはHPACK encoder方針として扱う必要がある。
- 今回の追加結果は、paddingそのものが常に速いのではなく、「特定のrequest HEADERS/TLS packet shapeが遅いケースで、1400B付近に乗せると改善する」という仮説を補強する。

### Laravel/FPM real Spanner check

実アプリ経路でも差が残るかを確認した。条件は real Spanner / SA JSON / nginx + php-fpm 16 workers / container cpus 4 / client concurrency 16。`native-pad` は全部盛りwire-profile + `grpc_lite.http2_experimental_ext_grpc_158_header_padding_target=1191`。

Raw logs:

- `var/bench-results/fpm-laravel-spanner-load-issue5-padding-fpm-20260521-104451/`
- `var/bench-results/fpm-laravel-spanner-load-issue5-padding-fpm-select-repeat-20260521-104708/`
- `var/bench-results/fpm-laravel-spanner-load-issue5-padding-fpm-mixed-repeat-20260521-104750/`

#### select_1row_10col repeat

requests=256 / concurrency=16。

| variant | rps | cpu_us/req | avg ms | p50 ms | p90 ms | max ms |
|---|---:|---:|---:|---:|---:|---:|
| ext-grpc | 400.4 | 8472.2 | 38.2 | 36.6 | 45.9 | 63.1 |
| native | 308.9 | 6969.6 | 49.1 | 47.8 | 57.8 | 78.7 |
| native-pad | 393.2 | 7846.9 | 38.1 | 36.8 | 47.6 | 62.0 |

観測:

- `select_1row_10col` ではnative baselineだけwall-timeが遅く、native-padはext-grpcとほぼ同等まで戻った。
- `cpu_us/req` はnative baselineが低く見えるが、wall-timeが長いため、Spanner待ちやloadgen側のスループット差の影響を受けている。CPU効率の単純比較には追加のsustained CPU profileが必要。

#### transaction_select2_update1_insert1 repeat

requests=128 / concurrency=16。

| variant | rps | cpu_us/req | avg ms | p50 ms | p90 ms | max ms |
|---|---:|---:|---:|---:|---:|---:|
| ext-grpc | 24.0 | 16900.7 | 583.4 | 569.3 | 1079.1 | 1432.6 |
| native | 27.5 | 14781.8 | 523.5 | 507.5 | 823.3 | 1048.5 |
| native-pad | 28.9 | 15803.4 | 530.3 | 544.3 | 635.9 | 870.1 |

観測:

- mixed transactionではnative baselineがext-grpcより速く、paddingはp50では明確な改善ではない。p90/maxはnative-padが最も低い。
- padding効果は全Laravel/FPM workloadへ一律に出るわけではなく、microの追加計測と同じくRPC/header shape依存。

判断:

- Laravel/FPM実経路でも、`select_1row_10col` のようなselect中心ケースではpaddingでext-grpcとの差がほぼ消える。
- mixed transactionではもともとnativeがext-grpcに勝っており、paddingを入れる必要性は低い。
- production方針は「常時1191 padding」ではなく、Spanner select系の遅いshapeに限定したpacket-size heuristic、またはHPACK encoder制御による自然なpacket shape調整として検討する。

### frame shape

initial sequence:

- ext-grpc:
  - `OUT SETTINGS` 36B profile
  - `OUT WINDOW_UPDATE inc=4128769`
  - `IN SETTINGS`
  - `IN WINDOW_UPDATE`
  - `OUT SETTINGS ACK`
  - `OUT HEADERS/DATA stream=1`
- lite baseline:
  - `OUT SETTINGS` 12B default profile
  - `OUT WINDOW_UPDATE inc=8323073`
  - `OUT HEADERS/DATA stream=1`
  - `IN SETTINGS`
  - `IN WINDOW_UPDATE`
  - `OUT SETTINGS ACK`
- lite wire-profile:
  - `OUT SETTINGS` 36B profile
  - `OUT WINDOW_UPDATE inc=4128769`
  - `IN SETTINGS`
  - `IN WINDOW_UPDATE`
  - `OUT SETTINGS ACK`
  - `OUT HEADERS/DATA stream=1`

CreateSession後:

- ext-grpc: server `PING`受信後、client `PING ACK`、connection-level `WINDOW_UPDATE`、client-origin `PING`、次streamの `HEADERS/DATA` が出る。
- lite wire-profile: server `PING`受信後、client `PING ACK`、client-origin `PING`、connection-level `WINDOW_UPDATE`、次streamの `HEADERS/DATA` が出る。control frame種別は寄っているが、`PING` と `WINDOW_UPDATE` の順序は完全一致していない。

HEADERS block length:

| variant | first ExecuteStreamingSql | steady ExecuteStreamingSql |
|---|---:|---:|
| ext-grpc 1.58 SA | 1260 | 1191 |
| lite baseline SA | 771 | 639 |
| lite wire-profile SA | 1022 | 1022 |
| lite wire-profile SA + all non-pseudo no-index | 1022 | 1022 |
| lite wire-profile SA + HEADERS padding | 1260 | 1191 |

## 判断ログ

- initial control frameと最初のstream開始順序はかなり寄せられた。
- response後control frameもconnection-level `WINDOW_UPDATE` とactive `PING` までは寄せたが、ext-grpcの正確な送信順序とはまだ完全一致していない。
- HEADERS block lengthは `lite baseline 639B steady` から `wire-profile 1022B steady` までext-grpcの `1191B steady` に近づいたが、まだ約169B差が残る。
- 非pseudo header全no-indexを全部盛りprofileに追加してもHEADERS長は `1022B` のままで、latencyも改善しなかった。nghttp2のpublic flag/table-size調整だけではext-grpc HPACK shapeに届いていない。
- tcpdump上のTLS packet長差はHEADERS block length差でほぼ説明できる。steady request packetはext-grpcが約 `1400B`、lite wire-profileが約 `1231B` で、差 `169B` はHEADERS block length差 `1191B - 1022B` と一致する。TLS write coalescingそのものより、HPACK/header byte shapeが残差の主因候補。
- HEADERS paddingで `ExecuteStreamingSql` のpayload lengthを ext-grpc と同じ `1260B -> 1210B -> 1191B steady` に合わせると、tcpdump上のoutbound packet lengthも `1469B -> 1419B -> 1400B steady` に揃った。
- HEADERS padding版は `request DATA -> first response` p50が `18.0ms -> 10.3ms` まで改善し、ext-grpc `8.9ms` にかなり近づいた。PHP marker latencyも `p50 13.2ms` まで改善した。
- paddingなしの最大寄せprofileではliteはext-grpc 1.58 SAのp50に届かない。`p50 22.1ms` でbaseline `22.3ms` とほぼ同等。
- packet lengthをpaddingで合わせた最大寄せprofileでは `p50 13.2ms` まで改善し、ext-grpc `11.4ms` に近づく。追加のn=300では `1191` 固定paddingが `p50 12.8ms / p99 17.3ms` で、同時刻ext-grpc `p50 12.7ms / p99 17.1ms` とほぼ同等。したがって残差の大半はheader block / TLS packet size shapeにある。
- HTTP/2 paddingはwire byte lengthを合わせる診断であり、ext-grpcのHPACK encoderを再現したものではない。production採用候補にするなら、paddingではなくHPACK encoder方針そのものを制御する必要がある。
- このprofileは捨て実装前提の調査用であり、production default候補ではない。

## 完了条件

- SA JSONのみでext-grpc 1.58とgrpc-lite最大寄せprofileのframe sequenceを比較できる。
- どこまで一致し、どこが残っているかを記録している。
- 採用候補ではなく調査用捨て実装として扱うべき範囲が明確になっている。

### External real workload validation plan

目的:

- Laravel/FPM + real Spanner の実ワークロードで、`select_1row_10col` 相当の遅延差が再現するかを確認する。
- 診断用paddingが実アプリでも ext-grpc 相当まで差を縮めるかを確認する。
- transaction / DML 系ではpaddingが不要または逆効果にならないかを確認する。

比較対象:

| variant | 設定 | 目的 |
|---|---|---|
| ext-grpc 1.58 | 報告者環境の既存ext-grpc | baseline |
| grpc-lite native | default | 現行挙動 |
| grpc-lite native-pad | `grpc_lite.http2_experimental_ext_grpc_158_wire_profile=1`, `grpc_lite.http2_experimental_ext_grpc_158_header_padding_target=1191` | HEADERS/TLS packet shape診断 |

計測条件:

- credentialはまずSA JSONで固定する。
- FPM worker数、client concurrency、Spanner session warmup条件はext-grpc/native/native-padで同一にする。
- 代表endpointは少なくとも以下2つを含める。
  - small select系: 1 row / 10 columns 程度のSELECT。
  - mixed transaction系: transaction内でselect 2回、update 1回、insert 1回。
- RPC間に人工sleepは入れない。sleepは現象を隠すため、実ワークロード検証には使わない。

判定:

- small select系で `native > ext-grpc` のwall-time差があり、`native-pad ≒ ext-grpc` になるなら、micro/FPM双方でHEADERS/TLS packet shape仮説を支持する。
- transaction系でnative baselineがext-grpc同等以上なら、paddingは全RPC defaultではなくselect系shape限定の診断・設計材料として扱う。
- native-padがtransaction系で悪化する場合、固定paddingのproduction採用は不可。HPACK/header shape制御か、RPC shape別heuristicが必要。

注意:

- `native-pad` はproduction候補ではなく診断用。ext-grpcがHTTP/2 paddingを使っているわけではない。
- 実際に採用する場合は、paddingではなくHPACK encoder方針、metadata shape、packet-size thresholdのどれを制御するべきかを別途設計する。

### Padding-only variant

`grpc_lite.http2_experimental_ext_grpc_158_header_padding_target` の固定正数指定を `wire_profile` から分離した。

追加比較対象:

| variant | 設定 | 目的 |
|---|---|---|
| native-padding-only | `grpc_lite.http2_experimental_ext_grpc_158_wire_profile=0`, `grpc_lite.http2_experimental_ext_grpc_158_header_padding_target=1191` | lite default SETTINGS/HPACK/control frameのまま、HEADERS/TLS packet sizeだけ寄せる |

意味:

- `header_padding_target > 0` は固定targetとして単独で有効。
- `header_padding_target <= 0` はpadding無効。特殊なstream-id別patternは持たない。
- defaultの `wire_profile=0`, `header_padding_target=0` はpadding無効で、send pathの追加frame parseも避ける。

これで、遅延改善の主因がwire-profile全体なのか、HEADERS/TLS packet sizeだけなのかを切り分ける。

### Removed stream-id pattern padding

`header_padding_target=-1` による `1260/1210/1191` のstream-id別padding modeは削除した。

理由:

- ext-grpc 1.58の特定観測値を模倣するだけで、HTTP/2/gRPC仕様上の意味がない。
- 固定 `1191` のほうが `ExecuteStreamingSql SELECT 1` の計測結果も良い。
- 外部実ワークロード検証に渡す設定として混乱する。
- production候補としても、stream idに依存する特殊挙動は採用できない。

現仕様:

- `grpc_lite.http2_experimental_ext_grpc_158_header_padding_target=0`: padding無効。
- `grpc_lite.http2_experimental_ext_grpc_158_header_padding_target=N`: odd stream id `>=3` のHEADERS payloadを固定target `N` までpaddingする。
- `wire_profile` はSETTINGS/HPACK/control frame寄せだけを担当し、固定paddingは `wire_profile` から独立して有効化できる。

### Padding-only major benchmark run

`grpc_lite.http2_experimental_ext_grpc_158_header_padding_target=1191` のみを有効化した `php-grpc-lite-padding-only` を、主要benchで `native` / `ext-grpc` と比較した。`wire_profile=0` のため SETTINGS / HPACK table / control frame寄せは入っていない。

実行条件:

- Docker compose / Go test-server / OTEL summary。
- `php-grpc-lite-padding-only`: `BENCH_PHP_EXTRA_INI_ARGS='-d grpc_lite.http2_experimental_ext_grpc_158_header_padding_target=1191'`。
- `spanner-real-client` はSpanner emulator経路で、real Cloud Spannerではない。

Raw summaries:

- `var/bench-results/issue5-padding-only-throughput-unary-short-20260521-182607-summary.txt`
- `var/bench-results/issue5-padding-only-payload-unary-20260521-182504-summary.txt`
- `var/bench-results/issue5-padding-only-throughput-streaming-20260521-182509-summary.txt`
- `var/bench-results/issue5-padding-only-payload-streaming-20260521-182516-summary.txt`
- `var/bench-results/issue5-padding-only-spanner-shape-20260521-182520-summary.txt`
- `var/bench-results/issue5-padding-only-tls-spanner-shape-20260521-182525-summary.txt`
- `var/bench-results/issue5-padding-only-spanner-real-client-20260521-182640-summary.txt`

代表結果:

| suite/case | ext-grpc p50/p99 us | native p50/p99 us | padding-only p50/p99 us | 観測 |
|---|---:|---:|---:|---|
| throughput-unary 100B | 62.0 / 330.7 | 25.9 / 112.3 | 26.9 / 121.1 | padding-onlyは小幅悪化 |
| payload-unary 100B | 67.6 / 537.4 | 28.8 / 213.8 | 28.4 / 320.2 | p50同等、p99悪化 |
| payload-unary 100KiB | 115.5 / 1583.4 | 69.8 / 2550.6 | 70.6 / 1642.4 | p50同等、p99改善 |
| throughput-streaming 1000x100B | 2523.5 / 3661.3 | 914.8 / 2664.3 | 937.8 / 2495.6 | p50小幅悪化、p99小幅改善 |
| payload-streaming 100x100B | 379.9 / 1564.1 | 131.5 / 878.7 | 138.0 / 845.2 | p50小幅悪化、p99小幅改善 |
| payload-streaming 100x10KiB | 918.7 / 5643.5 | 521.7 / 2886.2 | 514.4 / 3358.8 | p50同等、p99悪化 |
| spanner-shape select streaming | 65.1 / 137.8 | 29.6 / 117.0 | 31.1 / 209.9 | synthetic h2cでは悪化 |
| tls-spanner-shape select streaming | 75.5 / 122.7 | 30.2 / 90.1 | 29.8 / 82.9 | synthetic TLSでは小幅改善 |
| spanner-real-client small select emulator | 933.3 / 1688.1 | 748.8 / 918.5 | 869.1 / 1828.8 | emulator高レベル経路では悪化 |

判断:

- padding-onlyは主要bench全体では一貫した改善ではない。
- Go test-server h2c / synthetic spanner-shapeでは、むしろ固定費またはtailが悪化するケースがある。
- TLS synthetic spanner-shapeのselectでは小幅改善したが、real Cloud Spannerの `ExecuteStreamingSql SELECT 1` で見えた大きな改善とは規模が違う。
- `1191` 固定paddingはglobal default候補ではない。real Cloud Spannerの特定packet-size境界を確認するための診断ノブとして扱う。
- 外部実ワークロード検証では、`native` と `padding-only` を並べて、small select系に限定して差が消えるかを見るのが妥当。

### Real Spanner SELECT 1 current branch comparison

`ExecuteStreamingSql SELECT 1` の最小reproを、現在のブランチ状態で再計測した。SA JSON / real Cloud Spanner / n=300。比較対象は ext-grpc 1.58、native、padding-only、wire+padding。

Raw files:

- `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-ext-n300-current.markers.log`
- `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-native-n300-current.markers.log`
- `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-padding-only-n300-current.markers.log`
- `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-wire-padding-n300-current.markers.log`

Latency:

| variant | count | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|
| ext-grpc 1.58 | 300 | 13780 | 17069 | 21342 | 53509 |
| native | 300 | 23581 | 27135 | 37579 | 359717 |
| padding-only | 300 | 23792 | 27371 | 58165 | 106507 |
| wire+padding | 300 | 15524 | 18718 | 25915 | 29043 |

Outbound HEADERS payload length from trace:

| variant | first/steady HEADERS payload | PADDED |
|---|---|---:|
| ext-grpc 1.58 | first mostly `1263B`, steady `1191B` | no |
| native | first `774B`, steady `641B` | no |
| padding-only | first `776B`, steady `643B` | no |
| wire+padding | `1191B` for all measured RPC streams | yes |

判断:

- `header_padding_target=1191` 単独では、native defaultのHEADERSが小さすぎるためpaddingが実際には入っていない。HTTP/2 paddingはPad Lengthが1 byteなので、1つのHEADERS frameで増やせるpayloadは最大 `+256B`。native steady `643B` から `1191B` へは届かない。
- `wire_profile=1` でHPACK/control shapeを変えたうえでpaddingすると、HEADERS payloadが `1191B` に到達し、p50は `23.6ms -> 15.5ms` まで改善する。ただし今回のrunではext-grpc `13.8ms` にはまだ届かない。
- したがって、real Spanner SELECT 1で効いているのは「padding単独」ではなく、少なくとも `wire_profile` によってHEADERS blockがpadding可能なサイズまで大きくなることと、固定1191B化の組み合わせ。
- production検討では、単純なHTTP/2 paddingノブだけでは不十分。HPACK/header block shapeをどう作るか、または複数frame/別packetizationで1191B付近へ乗せるかを別途検討する必要がある。

### Padding-only max reachable behavior

`header_padding_target=1191` でtargetに届かない場合、従来はskipしていた。これを、HTTP/2 HEADERS paddingで到達できる最大payload長までpaddingする挙動へ変更した。

HTTP/2 HEADERS paddingの上限:

- PADDED flagを立てるとpayload先頭にPad Length 1Bが追加される。
- Pad Lengthの値は1 byteなので、実padding bodyは `0..255B`。
- したがってHEADERS frame payload lengthは最大 `current + 1 + 255 = current + 256B` まで増やせる。

検証:

- Docker build: pass
- `tests/002-ini.phpt`: pass

Real Spanner `ExecuteStreamingSql SELECT 1` / SA JSON / n=300 再計測:

| variant | count | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|
| ext-grpc 1.58 | 300 | 13413 | 15564 | 18019 | 27962 |
| native | 300 | 23274 | 26197 | 31262 | 363151 |
| padding-only max | 300 | 22546 | 24628 | 29019 | 33059 |
| wire+padding | 300 | 14745 | 17227 | 30974 | 67542 |

Outbound HEADERS payload length:

| variant | first/steady HEADERS payload | PADDED |
|---|---|---:|
| ext-grpc 1.58 | first mostly `1263B`, steady `1191B` | no |
| native | first `773B`, steady `641B` | no |
| padding-only max | first `1027B`, steady `894B` | yes |
| wire+padding | `1191B` for all measured RPC streams | yes |

判断:

- padding-only maxはnativeより少し改善するが、ext-grpcやwire+paddingには届かない。
- native default HEADERSはsteady `641B` で、最大paddingしても `894B` 程度にしかならない。`1191B` にはまだ約297B足りない。
- `wire_profile=1` はHEADERS block自体を大きくし、paddingで `1191B` に到達可能にする点で効いている。
- したがって、real Spanner SELECT 1の大きな差を埋めるには、padding上限内での調整だけでは不足。HPACK/header block shapeそのもの、または複数frame/packetizationの制御が必要。

### Lite ADC comparison

SA JSONではなく、hostのgcloud ADCをreadonly mountして `GOOGLE_APPLICATION_CREDENTIALS` を外したlite ADCでも同じ傾向になるか確認した。real Spanner `ExecuteStreamingSql SELECT 1` / n=300。

Raw files:

- `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-lite-adc-native-n300.markers.log`
- `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-lite-adc-padding-only-n300.markers.log`
- `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-lite-adc-wire-padding-n300.markers.log`

Latency:

| variant | count | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|
| lite ADC native | 300 | 23323 | 26181 | 30371 | 46700 |
| lite ADC padding-only max | 300 | 22930 | 26966 | 33377 | 35584 |
| lite ADC wire+padding | 300 | 23092 | 26184 | 37083 | 45594 |
| lite SA native | 300 | 23274 | 26197 | 31262 | 363151 |
| lite SA padding-only max | 300 | 22546 | 24628 | 29019 | 33059 |
| lite SA wire+padding | 300 | 14745 | 17227 | 30974 | 67542 |

Outbound HEADERS payload length:

| variant | first/steady HEADERS payload | PADDED |
|---|---|---:|
| lite ADC native | first `450B`, steady `257B` | no |
| lite ADC padding-only max | first `705B`, steady `514B` | yes |
| lite ADC wire+padding | about `919B` steady | yes |

判断:

- lite ADCではSA JSONの `wire+padding` と同じ改善は出ない。
- ADCのrequest HEADERSはSA JSONよりかなり小さい。steadyで native `257B`、padding-only max `514B`、wire+paddingでも約 `919B` で、`1191B` に届かない。
- lite ADC nativeのlatencyはlite SA nativeとほぼ同じ。今回のlite側ではcredential provider差そのものより、wire/header payload sizeとGoogle側の反応境界が主因に見える。
- SA JSONで `wire+padding` が効くのは、SA JSONのauthorization metadata shapeが大きく、wire_profile後にHEADERS payloadを1191Bまで到達させられるため。ADCでは同じtargetへ到達できないため、同じ効果にならない。

### HPACK/header size candidate status

official SA JSON と lite SA JSON の間で、なぜpaddingが必要になるかを候補別に整理する。

現時点で分かっている事実:

- 同じ `ExecuteStreamingSql SELECT 1` でも、steady request HEADERS payloadは official ext-grpc 1.58 が `1191B`、lite nativeが `641B`。
- lite `wire_profile=1` ではSETTINGS profile、initial control sequence、HPACK dynamic table size 0、authorization no-indexを寄せたうえで、paddingなしHEADERS payloadは過去計測で約 `1022B`。
- lite `wire+padding` ではHEADERS payloadを `1191B` に到達させられ、latencyはnativeより大きく改善する。
- lite ADCではHEADERS payloadがSA JSONより小さく、`wire+padding` でも約 `919B` にしかならず、SA JSONと同じ改善は出ない。

候補別の判定:

| candidate | 現状判定 | 根拠 | 次に必要な確認 |
|---|---|---|---|
| request metadataの論理内容差 | 未確定 | official traceはframe levelのみで、official側のheader name/value/orderをまだ取れていない | official ext-grpc側にpre-HPACK metadata traceを追加する |
| header order差 | 未確定 | lite側orderは取れているがofficial側orderが未取得 | official pre-HPACK traceで確認 |
| HPACK dynamic table設定 | 一部寄与あり | `wire_profile=1` でHEADERS payloadが `641B -> 約1022B` まで増える | 個別on/off sweepで寄与を分離 |
| never-index / no-index対象差 | 一部寄与ありそう | authorization no-index等を寄せたprofileでpayloadが増える。ただし1191Bには届かない | headerごとのflags sweep、official metadata trace |
| grpc-timeout/user-agent等value差 | 小さい可能性が高い | value長差は数byte〜数十byteで、残差約169Bを単独では説明しにくい | official pre-HPACK traceでvalue長を比較 |
| SETTINGS/control frame差 | latencyには関係し得るがHEADERS size差の直接原因ではない | SETTINGSはHEADERS block payloadを直接大きくしない。wire_profileでcontrol sequenceを寄せてもpaddingなしではlatency改善が小さい | latency因果のon/off sweep |
| nghttp2 vs C-core HPACK encoder差 | 有力 | 同じようなmetadata shapeでもHPACK block sizeが大きく違う。nghttp2 public knobsだけでは1191Bへ届いていない | official encoder input/flagsを取得し、liteのHPACK入力と比較 |
| TLS packetization/GFE ingress境界 | 有力な結果側要因 | HEADERS/TLS packet sizeを1191B/約1400Bに合わせるとlatencyが改善する | tcpdumpでpayload size thresholdを追加検証 |

現時点の結論:

- 「paddingが必要」なのは、liteのHPACK outputがofficialより小さいためで、SA JSON credentialそのものだけでは説明できない。
- ただし「なぜofficialのHPACK outputが1191Bになるか」は、official側のpre-HPACK metadata traceがないため完全には確定していない。
- 次の最短経路は、official ext-grpc 1.58 frame-trace buildに、送信前metadataの name / value length / flags / order を出すtraceを追加すること。
- そのうえで、lite側でheader order/flags/valueをofficialに合わせるsweepを行い、paddingなしで1191Bへ近づくかを見る。

## 2026-05-21 official ext-grpc pre-HPACK metadata trace

Official ext-grpc 1.58.0 trace build に pre-HPACK request metadata trace を追加し、`ExecuteStreamingSql SELECT 1` を SA JSON credential で再計測した。trace は header name と value length のみを出力し、credential 値は出力しない。

### Trace source

- Official trace: `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-official-metadata-n100.trace.jsonl`
- Lite native trace: `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-native-n300-maxpad.trace.jsonl`
- Lite wire profile trace: `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-wire-n100.trace.jsonl`
- Lite wire profile + padding trace: `var/bench-results/select1-ext-grpc-wire-align-sa-20260521/select1-wire-padding-n300-maxpad.trace.jsonl`

### Request header shape

| variant | stream | request headers | HEADERS payload | flags | notable logical metadata |
|---|---:|---:|---:|---:|---|
| official ext-grpc 1.58 | 3 | 14 | 1260 | END_HEADERS | `grpc-accept-encoding`, longer `user-agent`, duplicated `x-goog-api-client` |
| official ext-grpc 1.58 | 5 | 14 | 1210 | END_HEADERS | same |
| official ext-grpc 1.58 | 7+ | 14 | 1191 | END_HEADERS | same steady shape |
| lite native | 3 | 12 | 773 | END_HEADERS | folded `x-goog-api-client`, no `grpc-accept-encoding`, shorter `user-agent` |
| lite native | 5+ | 12 | 649 -> 641 | END_HEADERS | same steady shape |
| lite wire profile | 3+ | 12 | 1023 | END_HEADERS | authorization no-index / ext-grpc-like HPACK policy, logical metadata unchanged |
| lite wire profile + padding | 3+ | 12 | 1191 | PADDED + END_HEADERS | reaches official steady payload size by padding |
| lite padding only | 7+ | 12 | 894 | PADDED + END_HEADERS | cannot reach 1191 because one-byte Pad Length caps added bytes at 256 |

Official steady request metadata order for `ExecuteStreamingSql` is:

```text
:path, :authority, :method, :scheme,
content-type, te, grpc-accept-encoding, grpc-timeout, user-agent,
x-goog-api-client, x-goog-request-params,
google-cloud-resource-prefix, authorization, x-goog-api-client
```

Lite steady request metadata order is:

```text
:method, :scheme, :authority, :path,
content-type, te, grpc-timeout, user-agent,
x-goog-api-client, x-goog-request-params,
google-cloud-resource-prefix, authorization
```

### Latency observed in this trace run

| variant | n | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|
| official ext-grpc metadata trace | 100 | 16110 | 22520 | 32384 | 42457 |
| lite native | 300 | 23272 | 26197 | 31262 | 363151 |
| lite wire profile only | 100 | 21353 | 24751 | 30389 | 40145 |
| lite padding only | 300 | 22521 | 24628 | 29019 | 33059 |
| lite wire profile + padding | 300 | 14726 | 17227 | 30974 | 67542 |

The official metadata trace build has additional trace I/O, so absolute latency should not be compared as a release-quality benchmark. The shape comparison is still useful.

### Findings

- The candidate set is now concrete. The remaining wire-shape differences are not a single unknown bucket.
- Logical metadata differs:
  - official sends `grpc-accept-encoding`; lite does not.
  - official sends a longer built-in `user-agent`; lite sends the wrapper/userland value only.
  - official keeps `x-goog-api-client` as two header fields; lite folds it into one value.
  - pseudo-header order differs.
- HPACK policy differs independently from logical metadata:
  - lite native steady HEADERS payload is about 641B.
  - lite wire profile, without logical metadata changes, raises it to about 1023B.
  - official steady HEADERS payload is 1191B.
  - Logical metadata length difference alone is too small to explain the full 641B -> 1191B gap.
- Padding-only is not enough. It reaches only about 894B in the steady stream because HTTP/2 PADDED gives a one-byte Pad Length and at most 255 padding bytes.
- Wire profile + padding is the only tested lite variant that reaches the official steady HEADERS payload size and also shows the large SELECT 1 latency improvement for SA JSON.
- ADC does not get the same benefit because its authorization value is much shorter, and even wire profile + max padding reaches only about 919B.

### Updated candidate explanations

1. **HPACK encoding / indexing policy**: high-confidence contributor. It changes lite SA JSON steady HEADERS from about 641B to about 1023B without changing logical metadata.
2. **HEADERS payload size / TLS record shape**: high-confidence contributor for Spanner SELECT 1. Reaching roughly the official 1191B steady HEADERS payload correlates with the large latency improvement.
3. **Logical metadata shape**: concrete remaining delta. Need targeted tests for `grpc-accept-encoding`, split `x-goog-api-client`, built-in `user-agent`, and pseudo-header order.
4. **Credential type alone**: not sufficient. lite SA JSON and lite ADC are similar without wire-shape changes; SA JSON only benefits from wire profile + padding because its metadata is large enough to reach the observed payload size range.

### Next checks

- Test logical metadata alignment one by one without padding:
  - add `grpc-accept-encoding` request header;
  - keep duplicate `x-goog-api-client` fields instead of folding;
  - match official-style user-agent length/content class without copying exact branding;
  - reorder pseudo headers to official order.
- For each step, record HEADERS payload length and SELECT 1 latency.
- Do not treat exact ext-grpc branding or padding as a final design goal. Use these as diagnostic controls to identify which wire properties Google Spanner reacts to.

## 2026-05-21 arbitrary request metadata padding check

`user-agent` を `grpc.primary_user_agent` で増やす案は、Spanner high-level client / GAX の user-agent 合成経路に吸収され、今回の `Database::execute()` 経路では HEADERS size を変えられなかった。代わりに `Database::execute('SELECT 1', ['headers' => ...])` の CallOptions headers に `x-bench-padding` を追加し、任意 request metadata で HEADERS payload を増やせるかを確認した。

All variants below use SA JSON + `grpc_lite.http2_experimental_ext_grpc_158_wire_profile=1` and no HTTP/2 PADDED flag unless noted.

| variant | n | steady HEADERS payload | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|---:|
| wire profile only | 100 | 1023 | 21353 | 24751 | 30389 | 40145 |
| wire profile + HTTP/2 padding | 300 | 1191 | 14726 | 17227 | 30974 | 67542 |
| `x-bench-padding` 100B | 60 | 1124 | 25563 | 32717 | 107124 | 113209 |
| `x-bench-padding` 150B | 60 | 1169 | 24668 | 26531 | 104817 | 106166 |
| `x-bench-padding` 180B | 60 | 1197 | 14620 | 18322 | 99800 | 357890 |
| `x-bench-padding` 200B | 60 | 1216 | 14121 | 17016 | 21510 | 23261 |
| `x-bench-padding` 250B | 60 | 1255 | 14218 | 18252 | 22376 | 30653 |
| `x-bench-padding` 1000B | 100 | 1915 | 15364 | 20085 | 97429 | 101808 |

Findings:

- Arbitrary request metadata can push HEADERS beyond official's 1191B without using HTTP/2 PADDED.
- The latency improvement appears once the steady HEADERS payload reaches roughly `1197B+`, which supports the hypothesis that Spanner/GFE behavior is sensitive to the request HEADERS/TLS packet size range rather than the semantic content of the extra header.
- Overshooting to `1915B` still keeps p50 close to the improved range, but p99 becomes noisy. The smallest useful diagnostic region is around `180B-250B` extra metadata value length.
- This is diagnostic only. Sending dummy metadata is not a production design; it establishes that the missing 168B can be supplied by ordinary request metadata and that the size/packet-shape hypothesis is testable without HTTP/2 padding.

## 2026-05-21 combined metadata and HTTP/2 padding sweep

`x-bench-padding` による ordinary metadata 増量と HTTP/2 PADDED を組み合わせて、`wire_profile=1` の 1023B から official steady 1191B 近辺へ寄せた。

| variant | n | steady HEADERS payload | flags | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|---:|---:|
| official ext-grpc metadata trace | 100 | 1191 | END_HEADERS | 16110 | 22520 | 32384 | 42457 |
| lite native | 300 | 641 | END_HEADERS | 23272 | 26197 | 31262 | 363151 |
| lite wire profile only | 100 | 1023 | END_HEADERS | 21353 | 24751 | 30389 | 40145 |
| lite wire profile + HTTP/2 padding | 300 | 1191 | PADDED + END_HEADERS | 14726 | 17227 | 30974 | 67542 |
| lite wire + `x-bench-padding` 100B + HTTP/2 padding | 300 | 1191 | PADDED + END_HEADERS | 15823 | 19656 | 24690 | 32126 |
| lite wire + `x-bench-padding` 200B | 300 | 1216 | END_HEADERS | 15296 | 19826 | 28699 | 41109 |
| lite wire + `x-bench-padding` 250B | 300 | 1259 | END_HEADERS | 15042 | 18757 | 27146 | 31411 |

Findings:

- `wire_profile=1` の 1023B に、ordinary metadata と HTTP/2 PADDED のどちらでサイズを足しても、1191B 近辺では p50 が `14.7ms〜15.8ms` に入る。
- `x-bench-padding` 100B + HTTP/2 padding は、semantic metadata を少しだけ増やし、残りを protocol padding で埋める形。p99/max は今回の n=300 では最も安定した。
- `x-bench-padding` 200B/250B は HTTP/2 PADDED を使わずに近い性能になる。これは HEADERS/TLS packet size 仮説をさらに支持する。
- 一方、wire_profileなしの native に ordinary metadata を足しても steady HEADERS は約641Bに戻る。nghttp2 dynamic table によって追加 metadata が圧縮されるため、サイズ維持には HPACK policy 側の制御が必要。

Interim conclusion:

- 「dummy metadataにするべきか、HTTP/2 PADDEDにするべきか」という二択ではなく、性能因子は **steady HEADERS payload / TLS packet shape** と見てよい。
- 本番候補としては dummy metadata は避けるべき。意味のない application metadata をサーバーへ露出するため。
- もし採用するなら、少なくとも semantic metadata を汚さない HTTP/2 PADDED の方が筋は良い。
- ただし HTTP/2 PADDED も相手依存最適化なので、default化ではなく Spanner/TLS/SA JSON など条件を絞った experimental option として扱うべき。

Pending:

- `grpc-accept-encoding` 追加、`user-agent` 長、`x-goog-api-client` split を個別に試すための診断INIを追加したが、Docker API が詰まったため、この時点ではビルド・計測未完了。

## 2026-05-22 ADC request metadata size sweep

SA JSON ではなく ADC credential でも、ordinary request metadata で HEADERS payload を official steady 1191B 近辺へ寄せられるかを確認した。すべて lite + ADC + `wire_profile=1`。追加 metadata は診断用の `x-bench-padding`。

| variant | n | steady HEADERS payload | flags | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|---:|---:|
| ADC native | 300 | 257 | END_HEADERS | 23318 | 26181 | 30371 | 46700 |
| ADC wire + HTTP/2 max padding | 300 | 919 | PADDED + END_HEADERS | 23089 | 26184 | 37083 | 45594 |
| ADC wire + `x-bench-padding` 500B | 100 | 1117 | END_HEADERS | 21814 | 23970 | 27299 | 33620 |
| ADC wire + `x-bench-padding` 550B | 100 | 1157 | END_HEADERS | 22308 | 24386 | 28032 | 28073 |
| ADC wire + `x-bench-padding` 600B | 100 | 1200 | END_HEADERS | 12890 | 14700 | 17434 | 19445 |
| ADC wire + `x-bench-padding` 650B | 100 | 1244 | END_HEADERS | 13033 | 15058 | 16846 | 19117 |
| ADC wire + `x-bench-padding` 700B | 100 | 1290 | END_HEADERS | 14937 | 16872 | 21132 | 38342 |
| ADC wire + `x-bench-padding` 900B | 100 | 1467 | END_HEADERS | 13981 | 18638 | 36044 | 57859 |
| ADC wire + `x-bench-padding` 1200B | 100 | 1731 | END_HEADERS | 14023 | 17615 | 24529 | 27283 |
| ADC wire + `x-bench-padding` 500B + HTTP/2 padding | 100 | 1191 | PADDED + END_HEADERS | 13261 | 15240 | 17580 | 19182 |
| ADC wire + `x-bench-padding` 550B + HTTP/2 padding | 100 | 1191 | PADDED + END_HEADERS | 13634 | 15568 | 17573 | 18164 |
| ADC wire + `x-bench-padding` 600B + HTTP/2 padding | 100 | 1204 | END_HEADERS | 14057 | 15837 | 17089 | 20144 |

Findings:

- ADCでも、HEADERS payload が `1191B` 近辺に到達すると p50 は `13ms` 台まで改善した。
- ADCの `wire + HTTP/2 max padding` は `919B` にしか届かず改善しない。これは PADDED の最大追加量が 256B であるため。
- `x-bench-padding` 500B + HTTP/2 padding は `1191B` に到達し、p50/p90/p99/max がすべて安定して改善した。
- したがって、SA JSON 特有ではなく ADC でも同じ HEADERS/TLS packet size 仮説が成立する。
- credential type の差は「authorization value length によって、HTTP/2 padding だけで閾値に届くかどうか」の差として説明できる。

Updated interpretation:

- SA JSON: authorization が長いため、`wire_profile=1` + HTTP/2 padding だけで `1191B` に届く。
- ADC: authorization が短いため、HTTP/2 padding だけでは `919B` までしか届かない。ordinary metadata か別の自然な header shape 増加がないと閾値に届かない。
- これは、Spanner/GFE 側が credential type を直接見て遅くしているというより、request HEADERS/TLS packet shape に反応している可能性をさらに強める。

## 2026-05-22 native HPACK dynamic table vs metadata value uniqueness

User question: native/wire_profileなしで遅いのは、metadataの選び方が悪く、同じ値がHPACK dynamic tableで強く圧縮されているだけではないか。

To test this, ADC + native/wire_profileなしで `x-bench-padding` value を毎RPC変え、dynamic table value reuse を避けた。

| variant | n | steady HEADERS payload | flags | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|---:|---:|
| ADC native | 300 | 257 | END_HEADERS | 23318 | 26181 | 30371 | 46700 |
| ADC native + unique `x-bench-padding` 600B | 100 | 783 | END_HEADERS | 22836 | 24380 | 27832 | 28479 |
| ADC native + unique `x-bench-padding` 900B | 100 | 1050 | END_HEADERS | 20208 | 22544 | 24498 | 26994 |
| ADC native + unique `x-bench-padding` 1100B | 100 | 1221 | END_HEADERS | 11122 | 14308 | 16452 | 17165 |
| ADC native + unique `x-bench-padding` 1200B | 100 | 1380 | END_HEADERS | 13846 | 15849 | 21214 | 30590 |
| ADC wire + repeated `x-bench-padding` 600B | 100 | 1200 | END_HEADERS | 12890 | 14700 | 17434 | 19445 |

Findings:

- The previous “native + metadata does not help” result was affected by metadata reuse. Repeated dummy metadata is indexed/compressed by HPACK and the steady HEADERS payload shrinks back.
- If the metadata value is unique per RPC, native/wire_profileなしでも HEADERS payload can be kept large.
- Once native/wire_profileなし reaches the same size range (`~1221B`), it also becomes fast. This means `wire_profile` itself is not inherently required for latency improvement; the required property is maintaining HEADERS/TLS packet size near the fast range.
- However, using unique dummy metadata is not a production design. It is only a diagnostic to prove that HPACK dynamic table reuse was the reason repeated metadata did not increase steady HEADERS size.

## 2026-05-22 fixed metadata with NO_INDEX

User correction: for this heuristic experiment, unique metadata was unnecessary. A fixed dummy metadata value with `NGHTTP2_NV_FLAG_NO_INDEX` should be enough to avoid HPACK dynamic table reuse.

Implemented diagnostic INI:

- `grpc_lite.http2_experimental_no_index_x_bench_padding=1`
- It applies `NGHTTP2_NV_FLAG_NO_INDEX` only to `x-bench-padding`.
- Trace confirmed `x-bench-padding` is sent with `flags=1`.

ADC + native/wire_profileなし + fixed `x-bench-padding` + NO_INDEX:

| variant | n | steady HEADERS payload | flags | p50 us | p90 us | p99 us | max us |
|---|---:|---:|---:|---:|---:|---:|---:|
| ADC native | 300 | 257 | END_HEADERS | 23318 | 26181 | 30371 | 46700 |
| fixed 600B + NO_INDEX | 100 | 799 | END_HEADERS | 23151 | 25790 | 30041 | 30854 |
| fixed 900B + NO_INDEX | 100 | 1062 | END_HEADERS | 23692 | 26535 | 42703 | 57788 |
| fixed 1100B + NO_INDEX | 100 | 1236 | END_HEADERS | 14981 | 16843 | 19378 | 66998 |
| fixed 1200B + NO_INDEX | 100 | 1323 | END_HEADERS | 13201 | 15103 | 16168 | 29345 |
| fixed 1125B + NO_INDEX | 150 | 1261 | END_HEADERS | 12359 | 14897 | 15972 | 18029 |
| fixed 1150B + NO_INDEX | 150 | 1281 | END_HEADERS | 12905 | 16316 | 20027 | 32402 |
| fixed 1175B + NO_INDEX | 150 | 1302 | END_HEADERS | 13013 | 15683 | 18082 | 18746 |
| fixed 1200B + NO_INDEX | 150 | 1327 | END_HEADERS | 13477 | 15477 | 16739 | 21796 |
| fixed 1225B + NO_INDEX | 150 | 1345 | END_HEADERS | 13200 | 15231 | 17016 | 23445 |
| fixed 1250B + NO_INDEX | 150 | 1369 | END_HEADERS | 13510 | 15507 | 19321 | 39197 |

Findings:

- The correction is valid: fixed metadata + NO_INDEX keeps HEADERS large without per-RPC unique values.
- NO_INDEX-only native path becomes fast once the steady HEADERS payload reaches roughly `1230B+`.
- Best run in this sweep was fixed 1125B + NO_INDEX, steady HEADERS `1261B`, p50 `12.4ms`, p99 `16.0ms`.
- This confirms that the unique-value experiment was only a workaround for HPACK reuse; the cleaner diagnostic is fixed value + NO_INDEX.
- Still diagnostic only: adding meaningless request metadata is not a production design. The production-relevant inference is that preventing over-compression of selected large headers or using protocol padding can shift Spanner SELECT 1 into the fast range.

## 2026-05-22 official ext-grpc HPACK table state and lite deflate-table-only experiment

Question: ext-grpc 1.58 が encoder dynamic table size 0 で動いているのかを確認し、もしそうなら lite でも dynamic table size だけを 0 にして metadata による HEADERS size 拡張が効くか確認する。

### Source/code finding

- ext-grpc 1.58 Core has an encoder HPACK table and initializes `HPackEncoderTable::max_table_size_` to the HTTP/2 default `4096`.
- PHP ext-grpc passes channel args from PHP options, but no default `grpc.http2.hpack_table_size.encoder=0` setting was found in the PHP wrapper path used by this repro.
- Therefore, `SETTINGS_HEADER_TABLE_SIZE=0` / `SETTINGS_MAX_HEADER_LIST_SIZE` should not be treated as proof that request-side dynamic table is disabled. The request encoder table needs direct observation.

### Official trace result

Added an official ext-grpc diagnostic trace around `HPackCompressor::EncodeHeaders()`.

| variant | stream | raw HEADERS bytes | max usable size | before table bytes / elems | after table bytes / elems |
|---|---:|---:|---:|---:|---:|
| official ext-grpc 1.58 | 1 | 1423 | 4096 | 0 / 0 | 482 / 7 |
| official ext-grpc 1.58 | 3 | 1263 | 4096 | 482 / 7 | 614 / 9 |
| official ext-grpc 1.58 | 5 | 1191 | 4096 | 614 / 9 | 614 / 9 |
| official ext-grpc 1.58 | 7+ | 1191 | 4096 | 614 / 9 | 614 / 9 |

Conclusion: ext-grpc 1.58 is **not** using encoder dynamic table size 0 in this repro. It uses a 4096-byte usable table and inserts entries during the first RPCs. The steady `1191B` HEADERS size is not explained by “dynamic table disabled”.

### Lite isolated experiment: deflate dynamic table size only 0

Added a diagnostic-only lite knob:

- `grpc_lite.http2_experimental_hpack_deflate_table_size_zero=1`
- This only calls `nghttp2_option_set_max_deflate_dynamic_table_size(option, 0)`.
- It does not enable ext-grpc SETTINGS profile, request ordering changes, authorization NO_INDEX, HTTP/2 padding, or other wire-profile behavior.

SELECT 1, real Spanner, steady HEADERS payload from trace:

| variant | n | steady HEADERS payload | p50 ms | p99 ms | max ms |
|---|---:|---:|---:|---:|---:|
| official ext-grpc 1.58 | 40 | 1191 | 12.9 | 16.7 | 17.7 |
| ADC + hpack0 + extra header 0B | 100 | 661 | 22.3 | 27.7 | 28.0 |
| ADC + hpack0 + extra header 900B | 100 | 1467 | 12.2 | 18.3 | 21.6 |
| ADC + hpack0 + extra header 1100B | 100 | 1635 | 14.5 | 18.0 | 18.1 |
| ADC + hpack0 + extra header 1200B | 100 | 1724 | 12.9 | 16.4 | 18.3 |
| SA JSON + hpack0 + extra header 0B | 100 | 1021 | 22.9 | 31.6 | 139.1 |
| SA JSON + hpack0 + extra header 200B | 100 | 1216 | 13.8 | 17.1 | 17.2 |

Findings:

- Lite `hpack deflate table size 0` alone increases steady HEADERS size compared with default dynamic-table reuse, but it is not sufficient unless the resulting HEADERS payload reaches the fast size range.
- Metadata expansion still improves latency with only the deflate table disabled: ADC `900B+` and SA JSON `200B` extra header both move p50 into the `12–14ms` range.
- This isolates the important property more precisely: the improvement comes from maintaining a sufficiently large request HEADERS/TLS packet, not from the full ext-grpc wire profile as a bundle.
- Since official ext-grpc itself uses a 4096-byte dynamic table in this repro, copying “table size 0” is not an ext-grpc-alignment fix. It remains a diagnostic lever to control HPACK compression and HEADERS size.

## 2026-05-22 actual table SELECT with normal dynamic table reuse

Question: `SELECT 1` が固定値でSpanner側に特殊にキャッシュされている可能性を分離するため、実テーブル `BenchRows` の1行10列SELECTで確認する。`CreateSession` metadata is not modified. First SQL is stream id 3. Subsequent SQL calls reuse the same metadata and normal HPACK dynamic table behavior is left enabled.

SQL:

```sql
SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM BenchRows WHERE Id = 1
```

| variant | stream | RPC | HEADERS payload | out HEADERS -> last in ms |
|---|---:|---|---:|---:|
| official ext-grpc 1.58 | 1 | CreateSession | 1423 | 232.7 |
| official ext-grpc 1.58 | 3 | first ExecuteStreamingSql | 1260 | 33.6 |
| official ext-grpc 1.58 | 5 | second ExecuteStreamingSql | 1210 | 19.3 |
| official ext-grpc 1.58 | 7+ | steady ExecuteStreamingSql | 1191 | p50 10.8-ish in this trace family |
| lite SA native | 1 | CreateSession | 989 | 351.2 |
| lite SA native | 3 | first ExecuteStreamingSql | 774 | 39.1 |
| lite SA native | 5+ | steady ExecuteStreamingSql | 644 | p50 21.2, p90 24.0, p99 27.8 |
| lite SA + repeated `x-bench-padding` 1200B | 1 | CreateSession | 986 | 271.2 |
| lite SA + repeated `x-bench-padding` 1200B | 3 | first ExecuteStreamingSql | 1840 | 35.4 |
| lite SA + repeated `x-bench-padding` 1200B | 5+ | steady ExecuteStreamingSql | 642 | p50 23.1, p90 27.7, p99 48.0 |

Operation marker summary:

| variant | n | p50 ms | p90 ms | p99 ms | max ms |
|---|---:|---:|---:|---:|---:|
| official ext-grpc 1.58 | 60 | 12.1 | 15.1 | 24.0 | 387.6 |
| lite SA native | 60 | 23.5 | 26.5 | 30.2 | 442.6 |
| lite SA + repeated `x-bench-padding` 1200B | 60 | 25.4 | 30.1 | 125.8 | 349.3 |

Findings:

- The `SELECT 1`-only concern does not explain the observed pattern. The real table SELECT shows the same core behavior.
- First SQL on stream id 3 becomes large with repeated metadata (`1840B`) but is not clearly fast. It remains around `35ms`, close to official first SQL `33.6ms`, and not comparable to official steady `~10–12ms`.
- From stream id 5 onward, repeated metadata is compressed by HPACK dynamic table and steady HEADERS falls back to `642B`. Steady latency also remains slow (`~23ms p50`).
- Therefore, for steady-state latency, simply adding repeated metadata is not useful. Maintaining wire HEADERS size across calls requires either no-index-like behavior for the chosen metadata or another mechanism that avoids dynamic-table collapse.
- CreateSession itself differs, but it is a one-time/session path and not the steady ExecuteStreamingSql gap. It should remain a separate observation, not the primary optimization target.

## 2026-05-22 actual table SELECT with diagnostic NO_INDEX padding metadata

Question: for the experimental padding metadata, mark only `x-bench-padding` as `NGHTTP2_NV_FLAG_NO_INDEX` and keep normal dynamic table behavior for other headers. This tests whether preserving large HEADERS across stream id 5+ explains steady-state latency.

Condition:

- SQL: `SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM BenchRows WHERE Id = 1`
- `CreateSession` metadata is not modified.
- `x-bench-padding: <1200 bytes>` is added only to SQL calls.
- `grpc_lite.http2_experimental_no_index_x_bench_padding=1`
- Normal dynamic table behavior remains enabled for other headers.

| variant | stream | RPC | HEADERS payload | padding flags | out HEADERS -> last in ms |
|---|---:|---|---:|---:|---:|
| official ext-grpc 1.58 | 3 | first ExecuteStreamingSql | 1260 | n/a | 33.6 |
| official ext-grpc 1.58 | 5 | second ExecuteStreamingSql | 1210 | n/a | 19.3 |
| official ext-grpc 1.58 | 7+ | steady ExecuteStreamingSql | 1191 | n/a | marker p50 12.1 |
| lite SA native | 3 | first ExecuteStreamingSql | 774 | n/a | 39.1 |
| lite SA native | 5+ | steady ExecuteStreamingSql | 644 | n/a | stream p50 21.2, marker p50 23.5 |
| lite SA + repeated padding metadata | 3 | first ExecuteStreamingSql | 1840 | 0 | 35.4 |
| lite SA + repeated padding metadata | 5+ | steady ExecuteStreamingSql | 642 | 0 | stream p50 23.1, marker p50 25.4 |
| lite SA + NO_INDEX padding metadata | 3 | first ExecuteStreamingSql | 1843 | 1 | 49.5 |
| lite SA + NO_INDEX padding metadata | 5+ | steady ExecuteStreamingSql | 1710 | 1 | stream p50 10.4, p90 12.6, p99 14.3; marker p50 10.9 |

Findings:

- Marking only the experimental padding metadata as NO_INDEX preserves large HEADERS on stream id 5+ (`1710B` steady instead of `642B`).
- With large HEADERS preserved, steady table SELECT becomes as fast as or slightly faster than the observed official ext-grpc run (`marker p50 10.9ms` vs official `12.1ms`).
- This strongly supports the steady-state causal chain: repeated metadata is indexed by HPACK -> wire HEADERS collapses -> Spanner steady latency remains slow. Preventing indexing for the artificial padding metadata keeps the wire HEADERS large and removes the steady latency gap.
- First SQL on stream id 3 is still not the steady fast path even when large. It should be analyzed separately from stream id 5+ steady behavior.
- This remains diagnostic only. It does not justify sending meaningless production metadata. The production-relevant decision is whether any real metadata should be NO_INDEX, or whether HTTP/2 PADDED/other transport-level padding should be used for selected Google/Spanner paths.

## 2026-05-22 actual table SELECT NO_INDEX padding sweep 650B down to 500B

Condition:

- SQL: `SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM BenchRows WHERE Id = 1`
- `CreateSession` metadata is not modified.
- SQL RPCs include `x-bench-padding` with `NGHTTP2_NV_FLAG_NO_INDEX`.
- Normal dynamic table behavior remains enabled for other headers.
- n=60 per padding size.

| padding bytes | stream 1 CreateSession HEADERS | stream 1 ms | stream 3 first SQL HEADERS | stream 3 ms | stream 5+ steady HEADERS | stream 5+ p50 ms | stream 5+ p90 ms | stream 5+ p99 ms | marker p50 ms | marker p99 ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 650 | 986 | 310.8 | 1357 | 46.6 | 1226 | 11.4 | 14.0 | 16.4 | 11.8 | 25.9 |
| 640 | 988 | 109.1 | 1353 | 34.2 | 1219 | 13.3 | 15.9 | 22.9 | 15.6 | 45.7 |
| 630 | 985 | 30.0 | 1340 | 13.9 | 1208 | 10.9 | 13.5 | 18.0 | 11.7 | 19.8 |
| 620 | 986 | 121.8 | 1331 | 47.0 | 1200 | 12.0 | 13.5 | 16.5 | 13.6 | 22.3 |
| 610 | 985 | 306.5 | 1322 | 37.2 | 1190 | 13.6 | 14.8 | 16.4 | 15.9 | 19.3 |
| 600 | 983 | 139.9 | 1311 | 29.6 | 1179 | 12.0 | 13.8 | 23.8 | 13.5 | 32.3 |
| 590 | 987 | 304.8 | 1306 | 52.4 | 1175 | 12.5 | 13.6 | 17.5 | 14.7 | 22.6 |
| 580 | 987 | 117.4 | 1299 | 26.3 | 1166 | 13.0 | 14.4 | 15.3 | 15.0 | 21.1 |
| 570 | 984 | 402.6 | 1286 | 44.4 | 1154 | 13.6 | 15.1 | 16.4 | 16.1 | 19.3 |
| 560 | 985 | 300.1 | 1278 | 40.5 | 1146 | 13.1 | 14.8 | 17.0 | 15.0 | 23.0 |
| 550 | 987 | 312.3 | 1273 | 38.1 | 1140 | 13.7 | 15.3 | 33.7 | 16.0 | 37.5 |
| 540 | 982 | 320.5 | 1258 | 37.5 | 1126 | 13.4 | 14.7 | 20.8 | 15.5 | 27.1 |
| 530 | 987 | 110.8 | 1254 | 28.5 | 1122 | 11.4 | 12.5 | 14.5 | 12.8 | 19.4 |
| 520 | 988 | 29.2 | 1245 | 13.6 | 1114 | 13.4 | 15.9 | 19.0 | 15.3 | 22.6 |
| 510 | 984 | 303.3 | 1232 | 38.6 | 1102 | 14.0 | 16.2 | 19.2 | 16.3 | 26.5 |
| 500 | 990 | 113.3 | 1231 | 26.6 | 1099 | 12.2 | 13.5 | 14.5 | 14.2 | 18.7 |

Reference from the same table SELECT family:

- official ext-grpc 1.58: stream 5+ steady HEADERS `1191B`, marker p50 `12.1ms`.
- lite SA native without padding: stream 5+ steady HEADERS `644B`, marker p50 `23.5ms`.

Findings:

- The target range around official `1191B` is approximately padding `610B` to `630B` in this run (`1190B` to `1208B` steady HEADERS).
- Padding `630B` was the best in this sweep by marker p50/p99 (`11.7ms` / `19.8ms`) and stream p50 (`10.9ms`).
- Stream 1 CreateSession remains highly variable and should not be used as the primary signal for this steady-state investigation.
- Stream 3 first SQL is also variable; the stable signal remains stream 5+ steady SQL.

## 2026-05-22 actual table SELECT NO_INDEX padding sweep 500B down to 300B

Continuation of the previous sweep. Same condition: table SELECT, SQL-only `x-bench-padding`, `NGHTTP2_NV_FLAG_NO_INDEX`, normal dynamic table for other headers, n=60.

| padding bytes | stream 1 CreateSession HEADERS | stream 1 ms | stream 3 first SQL HEADERS | stream 3 ms | stream 5+ steady HEADERS | stream 5+ p50 ms | stream 5+ p90 ms | stream 5+ p99 ms | marker p50 ms | marker p99 ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 500 | 990 | 113.3 | 1231 | 26.6 | 1099 | 12.2 | 13.5 | 14.5 | 14.2 | 18.7 |
| 490 | 988 | 298.5 | 1219 | 44.8 | 1088 | 22.6 | 25.7 | 30.4 | 24.5 | 43.1 |
| 480 | 986 | 246.1 | 1209 | 27.1 | 1077 | 22.0 | 23.5 | 24.5 | 24.1 | 27.4 |
| 470 | 984 | 307.3 | 1199 | 46.3 | 1067 | 21.9 | 23.8 | 25.1 | 24.2 | 27.8 |
| 460 | 984 | 292.6 | 1189 | 35.8 | 1058 | 21.7 | 23.0 | 23.7 | 24.4 | 26.6 |
| 450 | 985 | 32.1 | 1182 | 12.1 | 1050 | 20.5 | 22.5 | 23.0 | 22.5 | 26.6 |
| 440 | 986 | 232.0 | 1174 | 20.4 | 1042 | 22.6 | 24.2 | 26.6 | 24.7 | 29.3 |
| 430 | 984 | 274.6 | 1166 | 33.9 | 1032 | 21.5 | 23.9 | 26.7 | 23.8 | 31.4 |
| 420 | 985 | 246.9 | 1157 | 21.6 | 1024 | 22.0 | 24.1 | 26.6 | 24.4 | 30.6 |
| 410 | 986 | 320.0 | 1150 | 38.8 | 1016 | 21.4 | 23.1 | 24.8 | 23.9 | 29.7 |
| 400 | 983 | 28.2 | 1136 | 13.9 | 1004 | 21.9 | 24.3 | 26.4 | 24.4 | 29.0 |
| 390 | 982 | 290.6 | 1127 | 35.9 | 995 | 21.7 | 23.2 | 28.6 | 23.9 | 33.2 |
| 380 | 982 | 269.3 | 1118 | 26.3 | 986 | 21.5 | 23.6 | 25.5 | 23.9 | 36.4 |
| 370 | 985 | 322.0 | 1113 | 40.4 | 980 | 21.1 | 23.4 | 25.5 | 22.7 | 35.3 |
| 360 | 988 | 293.2 | 1106 | 37.4 | 974 | 22.7 | 24.5 | 26.0 | 25.4 | 33.9 |
| 350 | 985 | 258.1 | 1096 | 60.4 | 963 | 22.6 | 24.2 | 26.6 | 24.6 | 29.0 |
| 340 | 984 | 292.3 | 1086 | 32.5 | 953 | 22.0 | 23.6 | 24.7 | 24.4 | 27.6 |
| 330 | 986 | 31.2 | 1079 | 21.2 | 946 | 21.6 | 24.2 | 25.5 | 24.1 | 37.8 |
| 320 | 986 | 246.8 | 1069 | 22.3 | 937 | 21.6 | 23.6 | 25.3 | 23.7 | 29.9 |
| 310 | 986 | 234.3 | 1061 | 22.7 | 929 | 21.9 | 23.5 | 24.4 | 24.0 | 29.1 |
| 300 | 986 | 294.6 | 1052 | 43.7 | 920 | 22.5 | 24.3 | 25.6 | 25.1 | 28.6 |

Findings:

- There is a sharp transition between padding `500B` and `490B` in this run.
- At padding `500B`, steady HEADERS is `1099B` and latency is still in the fast range (`stream p50 12.2ms`, marker p50 14.2ms).
- At padding `490B`, steady HEADERS is `1088B` and latency returns to the slow range (`stream p50 22.6ms`, marker p50 24.5ms).
- In this table SELECT condition, the apparent threshold is around steady HEADERS `~1090B–1100B`, not exactly official ext-grpc's `1191B`.
- Stream 1 and stream 3 remain noisy and are not good threshold signals. Stream 5+ steady behavior is the reliable signal.

## 2026-05-22 actual table SELECT NO_INDEX padding sweep repeat 400B to 600B

Repeat run for the threshold range. Same condition: table SELECT, SQL-only `x-bench-padding`, `NGHTTP2_NV_FLAG_NO_INDEX`, normal dynamic table for other headers, n=60.

| padding bytes | stream 1 CreateSession HEADERS | stream 1 ms | stream 3 first SQL HEADERS | stream 3 ms | stream 5+ steady HEADERS | stream 5+ p50 ms | stream 5+ p90 ms | stream 5+ p99 ms | marker p50 ms | marker p99 ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 400 | 987 | 316.2 | 1140 | 47.3 | 1008 | 21.6 | 23.5 | 25.8 | 23.2 | 28.6 |
| 410 | 986 | 271.0 | 1148 | 37.4 | 1016 | 21.4 | 23.7 | 25.9 | 24.0 | 28.3 |
| 420 | 985 | 33.1 | 1155 | 13.7 | 1024 | 21.8 | 23.7 | 27.9 | 23.6 | 30.2 |
| 430 | 983 | 288.3 | 1163 | 39.3 | 1031 | 21.0 | 23.0 | 30.8 | 22.2 | 36.8 |
| 440 | 985 | 31.5 | 1173 | 13.3 | 1041 | 21.7 | 23.2 | 24.6 | 23.9 | 28.2 |
| 450 | 985 | 260.6 | 1182 | 25.9 | 1050 | 21.1 | 23.5 | 26.9 | 23.6 | 30.9 |
| 460 | 988 | 279.7 | 1194 | 32.7 | 1062 | 22.3 | 24.2 | 28.7 | 24.8 | 37.7 |
| 470 | 986 | 276.4 | 1201 | 41.9 | 1069 | 20.9 | 22.7 | 24.0 | 23.2 | 28.3 |
| 480 | 987 | 204.0 | 1210 | 21.1 | 1078 | 21.9 | 24.3 | 31.6 | 24.3 | 34.7 |
| 490 | 985 | 314.1 | 1217 | 35.3 | 1085 | 21.7 | 24.0 | 27.9 | 23.8 | 33.0 |
| 500 | 983 | 286.1 | 1224 | 33.2 | 1092 | 22.4 | 23.7 | 24.6 | 24.8 | 30.3 |
| 510 | 985 | 284.8 | 1235 | 36.0 | 1103 | 11.6 | 13.3 | 14.4 | 13.0 | 16.4 |
| 520 | 982 | 28.4 | 1240 | 13.9 | 1108 | 11.5 | 13.8 | 14.7 | 13.1 | 17.5 |
| 530 | 983 | 374.7 | 1249 | 34.1 | 1118 | 13.1 | 14.4 | 15.4 | 15.1 | 18.1 |
| 540 | 984 | 311.7 | 1260 | 25.0 | 1128 | 13.2 | 16.2 | 17.1 | 15.2 | 24.0 |
| 550 | 984 | 390.1 | 1269 | 35.8 | 1137 | 12.1 | 13.6 | 15.1 | 13.7 | 27.2 |
| 560 | 987 | 301.3 | 1280 | 36.7 | 1148 | 14.2 | 15.9 | 17.9 | 16.3 | 21.5 |
| 570 | 985 | 281.4 | 1287 | 35.0 | 1155 | 14.0 | 15.7 | 16.8 | 16.4 | 21.3 |
| 580 | 985 | 30.1 | 1296 | 12.7 | 1164 | 11.0 | 14.9 | 23.1 | 11.8 | 28.4 |
| 590 | 989 | 256.0 | 1309 | 26.4 | 1177 | 13.0 | 14.5 | 18.7 | 15.2 | 22.7 |
| 600 | 986 | 241.8 | 1314 | 26.0 | 1182 | 11.8 | 13.4 | 19.7 | 13.6 | 30.4 |

Repeat findings:

- The repeat run shows the transition between padding `500B` and `510B`.
- At `500B`, steady HEADERS `1092B` remained slow (`stream p50 22.4ms`, marker p50 24.8ms).
- At `510B`, steady HEADERS `1103B` entered the fast range (`stream p50 11.6ms`, marker p50 13.0ms).
- Combining this with the previous sweep, the threshold is best described as approximately steady HEADERS `~1095B–1105B`, with run-to-run jitter near the boundary.

## 2026-05-23 unary ExecuteSql SELECT 1 with NO_INDEX padding sweep

Question: whether the same HEADERS-size threshold appears for unary `ExecuteSql` as it does for server-streaming `ExecuteStreamingSql`.

Condition:

- Direct GAPIC `Google\Cloud\Spanner\V1\Client\SpannerClient::executeSql()`.
- SQL: `SELECT 1 AS n`.
- `CreateSession` metadata is not modified.
- SQL RPCs optionally include `x-bench-padding` with `NGHTTP2_NV_FLAG_NO_INDEX`.
- Normal dynamic table behavior remains enabled for other headers.
- n=60 per padding size.

Baseline:

| variant | stream 1 CreateSession HEADERS | stream 1 ms | stream 3 first ExecuteSql HEADERS | stream 3 ms | stream 5+ steady HEADERS | stream 5+ p50 ms | stream 5+ p90 ms | stream 5+ p99 ms | marker p50 ms | marker p99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| official ext-grpc 1.58 | 1191 | 507.9 | 1060 | 140.6 | 996 | 114.5 | 128.6 | 136.1 | 118.8 | 141.2 |
| lite SA native | 808 | 558.2 | 698 | 142.5 | 573 | 112.8 | 129.3 | 141.4 | 115.2 | 145.0 |

Lite SA + SQL-only `x-bench-padding` NO_INDEX sweep:

| padding bytes | stream 1 CreateSession HEADERS | stream 1 ms | stream 3 first ExecuteSql HEADERS | stream 3 ms | stream 5+ steady HEADERS | stream 5+ p50 ms | stream 5+ p90 ms | stream 5+ p99 ms | marker p50 ms | marker p99 ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 400 | 813 | 550.7 | 1069 | 136.9 | 944 | 108.9 | 128.6 | 133.6 | 111.1 | 141.2 |
| 410 | 813 | 135.4 | 1078 | 103.0 | 953 | 111.1 | 131.1 | 141.6 | 112.2 | 143.8 |
| 420 | 813 | 121.3 | 1087 | 75.3 | 962 | 114.8 | 125.8 | 135.7 | 117.2 | 138.1 |
| 430 | 810 | 128.2 | 1094 | 87.1 | 968 | 113.5 | 127.2 | 139.9 | 115.4 | 140.9 |
| 440 | 811 | 450.7 | 1104 | 125.6 | 977 | 115.7 | 129.1 | 138.7 | 117.5 | 140.6 |
| 450 | 813 | 351.3 | 1112 | 101.8 | 988 | 116.4 | 132.6 | 140.2 | 117.6 | 143.2 |
| 460 | 810 | 390.1 | 1120 | 123.1 | 994 | 109.3 | 128.2 | 140.5 | 111.3 | 143.0 |
| 470 | 811 | 139.7 | 1129 | 120.5 | 1004 | 112.8 | 127.3 | 133.9 | 115.5 | 134.6 |
| 480 | 812 | 361.2 | 1138 | 103.4 | 1013 | 109.3 | 125.9 | 144.1 | 111.6 | 145.9 |
| 490 | 811 | 333.8 | 1147 | 100.0 | 1021 | 113.5 | 136.8 | 146.1 | 113.6 | 147.9 |
| 500 | 812 | 153.8 | 1155 | 112.7 | 1031 | 112.5 | 129.1 | 145.5 | 115.1 | 148.4 |
| 510 | 815 | 138.3 | 1167 | 65.1 | 1043 | 115.1 | 134.4 | 138.3 | 116.6 | 141.7 |
| 520 | 811 | 386.5 | 1172 | 91.0 | 1047 | 109.7 | 127.6 | 148.5 | 111.4 | 150.7 |
| 530 | 812 | 166.0 | 1182 | 83.1 | 1057 | 102.6 | 123.3 | 137.1 | 104.3 | 139.9 |
| 540 | 809 | 122.5 | 1188 | 57.8 | 1063 | 102.4 | 123.8 | 130.4 | 104.4 | 133.1 |
| 550 | 812 | 339.8 | 1200 | 77.5 | 1075 | 106.0 | 124.2 | 127.8 | 108.0 | 129.6 |
| 560 | 812 | 311.8 | 1208 | 94.1 | 1083 | 113.0 | 129.1 | 142.9 | 114.6 | 144.9 |
| 570 | 811 | 383.8 | 1216 | 72.7 | 1091 | 115.2 | 129.7 | 141.6 | 118.3 | 144.5 |
| 580 | 811 | 126.6 | 1225 | 63.6 | 1100 | 113.5 | 126.4 | 135.3 | 115.5 | 138.1 |
| 590 | 811 | 364.5 | 1234 | 73.1 | 1109 | 108.3 | 125.7 | 151.8 | 110.6 | 154.3 |
| 600 | 810 | 128.0 | 1241 | 59.7 | 1116 | 117.1 | 136.5 | 159.3 | 119.2 | 161.7 |

Findings:

- Unary `ExecuteSql` does not reproduce the sharp server-streaming threshold behavior.
- Baseline lite and official ext-grpc are already similar in unary: lite marker p50 `115.2ms`, official marker p50 `118.8ms`.
- Increasing steady HEADERS from lite baseline `573B` to roughly `944B–1116B` does not create a clear fast/slow transition. All cases stay around `~100–120ms` p50.
- This suggests the HEADERS-size effect is specific to, or much stronger in, `ExecuteStreamingSql` server-streaming behavior. For unary `ExecuteSql`, server-side execution path/response semantics dominate the observed latency.

### Unary ExecuteSql boundary re-aggregation: HEADERS out to first/last inbound frame

The previous unary table used `HEADERS out -> last inbound frame`. Re-aggregated with the same boundary split used for streaming investigation:

- `steady_first_*`: request HEADERS out -> first inbound frame on stream id 5+
- `steady_last_*`: request HEADERS out -> last inbound frame on stream id 5+

Baseline:

| variant | steady HEADERS | steady first p50 ms | steady first p99 ms | steady last p50 ms | steady last p99 ms | marker p50 ms | marker p99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| official ext-grpc 1.58 | 996 | 113.4 | 134.6 | 114.5 | 136.1 | 118.8 | 141.2 |
| lite SA native | 573 | 111.9 | 138.6 | 112.8 | 141.4 | 115.2 | 145.0 |

NO_INDEX padding sweep, selected values:

| padding bytes | steady HEADERS | steady first p50 ms | steady first p99 ms | steady last p50 ms | steady last p99 ms | marker p50 ms | marker p99 ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 400 | 944 | 108.0 | 132.8 | 108.9 | 133.6 | 111.1 | 141.2 |
| 500 | 1031 | 111.7 | 144.5 | 112.5 | 145.5 | 115.1 | 148.4 |
| 530 | 1057 | 101.9 | 132.0 | 102.6 | 137.1 | 104.3 | 139.9 |
| 540 | 1063 | 100.9 | 128.4 | 102.4 | 130.4 | 104.4 | 133.1 |
| 550 | 1075 | 105.1 | 127.3 | 106.0 | 127.8 | 108.0 | 129.6 |
| 580 | 1100 | 112.4 | 134.7 | 113.5 | 135.3 | 115.5 | 138.1 |
| 600 | 1116 | 116.0 | 158.5 | 117.1 | 159.3 | 119.2 | 161.7 |

Finding:

- Even with the same first/last inbound frame boundaries, unary `ExecuteSql` does not show the sharp streaming threshold.
- `first` and `last` are only about `1ms` apart for unary, so nearly all observed time is before the first inbound frame.
- Lite and official are similar despite very different steady HEADERS sizes (`573B` vs `996B` baseline), so HEADERS-size is not the dominant unary factor in this repro.
