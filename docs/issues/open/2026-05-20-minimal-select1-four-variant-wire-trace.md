---
Status: Open
Owner: Codex
Created: 2026-05-20
Related:
  - docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md
  - docs/issues/open/2026-05-20-spanner-gap-pacing-investigation.md
---

# minimal SELECT 1 四条件 wire trace 比較

## 目的

real Spanner `ExecuteStreamingSql SELECT 1` のSA JSON差について、official ext-grpc 1.58 / grpc-lite と SA JSON / ADC の4条件を同じ粒度で比較する。official ext-grpc側にもgrpc-liteの `wire.frame_*` に近いtraceを仕込み、request送信完了からfirst response到着までの差とHTTP/2 control lifecycleを確認する。

## 方針

- 対象は minimal `select1-bench.php`、gapなし、real Spanner。
- official ext-grpc 1.58はローカルbuild sourceへ診断patchを入れ、payloadやsecretを出さずにframe type、stream id、length、flags、timestampだけを出す。
- grpc-liteは既存 `GRPC_LITE_TRACE_FILE` を使う。
- raw trace / tcpdump / strace はgit管理外 `var/` に保存し、docsには集計結果だけ残す。

## スコープ

- official SA JSON
- official ADC
- grpc-lite SA JSON
- grpc-lite ADC
- request send完了からfirst response frame/readまでの比較
- SETTINGS / PING / WINDOW_UPDATE / HEADERS / DATA / trailing HEADERS の相対時刻比較

## 非スコープ

- production code変更
- credential値やauthorization payloadの保存
- Spanner/GFE内部原因の断定

## 進捗

- [x] official ext-grpc 1.58のframe trace patch箇所特定
- [x] trace付きofficial ext-grpcのビルド
- [x] 4条件のtrace取得
- [x] 比較表作成

## 計測

- 実行日時: 2026-05-21
- 対象: real Spanner `ExecuteStreamingSql SELECT 1`
- runner: `tools/diagnostics/issue5-spanner-repro/select1-bench.php`
- iteration: 30
- gap: 0ms
- trace保存先: `var/bench-results/select1-four-variant-wire-trace-20260521-012940/`
- official ext-grpc: 1.58.0 + local diagnostic patch (`GRPC_OFFICIAL_FRAME_TRACE_FILE`)
- grpc-lite: current branch image (`GRPC_LITE_TRACE_FILE`)

| variant | credential | mean | p50 | p90 | p99 | trace lines |
|---|---:|---:|---:|---:|---:|---:|
| official ext-grpc | SA JSON | 13.101ms | 12.952ms | 15.531ms | 20.180ms | 282 |
| official ext-grpc | ADC | 20.620ms | 20.165ms | 22.113ms | 31.524ms | 280 |
| grpc-lite | SA JSON | 22.237ms | 21.723ms | 25.298ms | 25.718ms | 569 |
| grpc-lite | ADC | 22.090ms | 22.060ms | 25.326ms | 26.765ms | 602 |

## tcpdump + trace 相関

4条件について、少数試行で同一コンテナ内 `tcpdump` とframe traceを同時取得した。RPCごとにmarkerを出し、marker区間内の最後のclient outbound payload/frameから最初のserver inbound payload/frameまでを比較した。

- 実行日時: 2026-05-21
- iteration: 10
- raw data: `var/bench-results/select1-four-variant-tcpdump-trace-20260521-053648/`
- capture: container内 `tcpdump -i any -nn -s 0 -w ... 'tcp port 443'`
- marker: `epoch_us` と `mono_us` をRPC開始/終了で出力
- official trace時刻: diagnostic patchの `gettimeofday()` epoch usec
- grpc-lite trace時刻: `monotonic_us()` monotonic usec

| variant | credential | elapsed p50 | tcp last outbound payload -> first inbound payload p50 | trace last outbound frame -> first inbound frame p50 |
|---|---|---:|---:|---:|
| official ext-grpc | SA JSON | 13.461ms | 9.854ms | 10.402ms |
| official ext-grpc | ADC | 20.153ms | 16.423ms | 17.249ms |
| grpc-lite | SA JSON | 22.585ms | 19.168ms | 19.688ms |
| grpc-lite | ADC | 24.547ms | 19.833ms | 20.520ms |

補足:

- tcpdumpとtraceは同じ区間差を見ている。各variantでtcpとtraceの差は概ね1ms以内で、計測点の違いとして説明できる範囲。
- 4条件すべて、requestはmarker開始後およそ1〜2msでclient outbound payload/frameまで進む。差の大半はその後のfirst inbound response到着までにある。
- official SA JSONのみ、serverがresponseを開始するまでのwire gapが約10msで、official ADC / grpc-lite SA / grpc-lite ADCは約16〜20ms。
- tcpdump上のclient outbound payload countはp50で各条件1個。今回のSELECT 1では、単純なTCP packet分割数の差は主因ではない。
- この計測のため、grpc-lite診断traceにnghttp2 begin-frame hookを追加し、inbound `HEADERS` / `DATA` のframe境界もpayloadなしで記録できるようにした。

## HTTP/2 frame count summary

| variant | frame | count |
|---|---|---:|
| official SA | out SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 39 / 33 / 33 / 36 |
| official SA | in SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 1 / 66 / 33 / 36 |
| official ADC | out SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 37 / 33 / 33 / 36 |
| official ADC | in SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 1 / 66 / 33 / 36 |
| grpc-lite SA | out SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 1 / 33 / 33 / 33 |
| grpc-lite SA | in SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 1 / 0 / 0 / 33 |
| grpc-lite ADC | out SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 1 / 33 / 33 / 33 |
| grpc-lite ADC | in SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING | 2 / 1 / 0 / 0 / 33 |

注: grpc-liteの現行traceはnghttp2 `on_frame_recv` ベースで、inbound HEADERS/DATAのframe境界が出ていない。official ext-grpc側はC-core parserでinbound HEADERS/DATA/trailersまで確認できる。

## 観測

- official ext-grpcではSA JSONだけが明確に速く、ADCはgrpc-liteと同じ20ms台になる。
- grpc-liteではSA JSONとADCに有意差がない。credential方式だけを変えてもfast pathに入っていない。
- official ext-grpcとgrpc-liteはどちらも1接続をreuseし、stream idは奇数で増加している。毎RPC新規TLS connectionではない。
- official ext-grpcはresponseごとにserver PINGを受け、PING ACKを返している点はgrpc-liteと同じ。
- official ext-grpcだけ追加のclient PINGを少数回送っているが、毎RPCではない。今回のSA JSON fast pathを直接説明する主要差分には見えない。
- official ext-grpcはper-RPCで多数のoutbound `WINDOW_UPDATE` を出している。grpc-liteは初期connection `WINDOW_UPDATE` 以外を出していない。
- official ext-grpcのinitial `SETTINGS` payloadは36 bytes、grpc-liteは12 bytes。`SETTINGS`差とその後の`WINDOW_UPDATE` lifecycleは次の確認対象。
- official SAの `out HEADERS -> first inbound stream frame` p50は約10.5ms、official ADCは約18.5ms。差はrequest frame送信後からresponse frame到着までに集中している。
- tcpdump同時取得でも同じ傾向を確認した。クライアント内traceだけの見かけではなく、wire上のfirst inbound payload到着時刻として差が出ている。

## 次の確認対象

1. official ext-grpcのinitial `SETTINGS` payload内容を特定し、grpc-liteとの差を表にする。
2. official ext-grpcのper-RPC `WINDOW_UPDATE` がstream/connectionどちらで、どのタイミング・incrementで出ているか確認する。
3. `SETTINGS` / `WINDOW_UPDATE` 差をgrpc-liteに小さく反映した実験branchを作り、SA JSON gapが縮むか確認する。

## 判断ログ

- active PINGは今回の主要差分として扱わない。公式側にも少数のclient PINGはあるが、SA JSONとADCの差は同じPING lifecycle上でも発生している。
- credentialそのもののwire header差は主因候補から下げる。official SA JSONとlite SA JSONの差が残り、official ADCはliteと近い。
- 今回のraw traceはpayloadやcredential値を保存していないため、git管理外 `var/` に保持する。

## 完了条件

- 4条件で同じRPC境界の `request sent -> first response` が比較できる。
- official SA JSONだけが速い場合、wire/control lifecycle上の差分候補を列挙できる。
