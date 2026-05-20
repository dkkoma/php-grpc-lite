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

## control payload detail 再検証

official ext-grpc診断patchを拡張し、`SETTINGS` payload、`WINDOW_UPDATE` increment、`PING` opaqueをtraceへ出すようにして4条件を再取得した。grpc-lite側はcurrent local `grpc.so` を明示mountし、inbound `HEADERS` / `DATA` frame境界も取れる状態に揃えた。

- 実行日時: 2026-05-21
- iteration: 10
- raw data: `var/bench-results/select1-four-variant-control-detail-20260521-062836/`
- capture: container内 `tcpdump -l -tttt -nn -i any -s 0 'tcp port 443'`
- official SO: `var/official-ext-grpc-so/1.58.0-frame-trace/grpc.so`
- grpc-lite SO: `ext/grpc/modules/grpc.so`

### latency summary

| variant | credential | elapsed p50 | trace last request frame -> first response frame p50 | tcp last outbound payload -> first inbound payload p50 |
|---|---|---:|---:|---:|
| official ext-grpc | SA JSON | 12.019ms | 9.732ms | 9.295ms |
| official ext-grpc | ADC | 19.890ms | 17.609ms | 17.202ms |
| grpc-lite | SA JSON | 18.946ms | 17.120ms | 16.841ms |
| grpc-lite | ADC | 22.380ms | 19.124ms | 18.368ms |

tcpdumpはlive text出力に切り替えたため、official SAはmarker区間とpacket timestampが合ったsampleが6/10だった。他の3条件は10/10。trace側は全条件10/10で集計できている。

### initial SETTINGS / WINDOW_UPDATE

| variant | outbound SETTINGS | outbound connection WINDOW_UPDATE | inbound peer SETTINGS | inbound peer WINDOW_UPDATE |
|---|---|---:|---|---:|
| official ext-grpc | `ENABLE_PUSH=0`, `MAX_CONCURRENT_STREAMS=0`, `INITIAL_WINDOW_SIZE=4194304`, `MAX_FRAME_SIZE=4194304`, `MAX_HEADER_LIST_SIZE=16384`, `GRPC_ALLOW_TRUE_BINARY_METADATA=1` | `4128769` | `MAX_CONCURRENT_STREAMS=100`, `INITIAL_WINDOW_SIZE=1048576`, `MAX_HEADER_LIST_SIZE=65536` | `983041` |
| grpc-lite | `ENABLE_PUSH=0`, `INITIAL_WINDOW_SIZE=8388608` | `8323073` | `MAX_CONCURRENT_STREAMS=100`, `INITIAL_WINDOW_SIZE=1048576`, `MAX_HEADER_LIST_SIZE=65536` | `983041` |

officialとgrpc-liteは、peerから受ける初期SETTINGS/WINDOW_UPDATEは同じ。一方、clientから送るSETTINGSはかなり違う。officialはreceive stream window / max frame / max header / true binary metadataを明示し、grpc-liteはreceive stream windowだけを8MiBにしている。

### response後 WINDOW_UPDATE / PING lifecycle

official ext-grpcは小さい`SELECT 1` response後にも、ほぼ毎RPCでconnection-level `WINDOW_UPDATE`を送っている。incrementはresponse payload相当の小さい値で、代表値は`26`、trailingや一部streamでは`21`、`31`、`5`も出る。stream-level `WINDOW_UPDATE`も一部で出る。

| variant | measured RPC後のoutbound connection WINDOW_UPDATE | stream-level WINDOW_UPDATE | client-origin PING |
|---|---|---|---|
| official ext-grpc SA | ほぼ毎RPC、first responseから約0.4〜0.9ms後、increment `21`〜`31`中心 | 一部あり。例: stream5 `inc=31`, stream17 `inc=5` | warmup後など少数あり。opaqueは`0000000000000000` |
| official ext-grpc ADC | ほぼ毎RPC、first responseから約0.3〜0.6ms後、increment `21`〜`26`中心 | 一部あり。例: stream11 `inc=5` | warmup後など少数あり。opaqueは`0000000000000000` |
| grpc-lite SA | 初期connection `WINDOW_UPDATE`のみ | なし | なし。server PING ACKのみ |
| grpc-lite ADC | 初期connection `WINDOW_UPDATE`のみ | なし | なし。server PING ACKのみ |

server PINGへのACK latencyは、official/liteともsub-msで、今回の差の直接要因ではない。officialのclient-origin PING RTTは約5〜7msだが、毎RPCではなく、measured RPCのrequest直後からfirst responseまでに常に挟まるわけでもない。

### request送信後からfirst responseまでのconnection-level frame

measured RPCの `last request DATA -> first response HEADERS` の間にconnection-level frameが入る回数は、多くのRPCで0だった。officialでも一部RPCで `WINDOW_UPDATE` / `PING` が間に入るが、SA JSON fast path全体を説明するほど一貫していない。

- official SA: `conn_between` は `[0, 0, 0, 0, 0, 0, 2, 1, 0, 0]`
- official ADC: `conn_between` は `[0, 0, 0, 2, 1, 0, 0, 0, 0, 0]`
- grpc-lite SA/ADC: 全RPCで0

したがって、現時点の強い仮説は「RPC中にPING/WINDOW_UPDATEが挟まるから速い」ではなく、「connection開始〜warmup streamの間に形成されたHTTP/2 control/flow-control stateが、その後のSpanner frontendのresponse schedulingに影響している」。

### first RPC前の差分

official ADCは、peer SETTINGS/WINDOW_UPDATE受信後からstream1 request送信までに約120msの空きがある。official SAは約3ms、grpc-liteは約0ms〜1msでstream1を送る。この差はcredential/token準備側の挙動と見られるが、measured stream 5以降の差を単独で説明しない。

また、official SAとofficial ADCはHTTP/2 control lifecycleはほぼ同じだが、measured RPCのfirst response gapはSAが約10ms、ADCが約18msで差が残る。credential/header shape由来のserver-side handling差と、official C-coreのconnection control state差の両方を見る必要がある。

## 現時点の次アクション候補

1. grpc-liteでofficial相当のinitial `SETTINGS`を送る実験を行う。特に `MAX_FRAME_SIZE=4194304`, `MAX_HEADER_LIST_SIZE=16384`, `GRPC_ALLOW_TRUE_BINARY_METADATA=1`, `INITIAL_WINDOW_SIZE=4194304`。
2. grpc-liteでresponse DATA消費後に小さいconnection-level `WINDOW_UPDATE`を送る実験を行う。ただしreceive windowを既に8MiBに広げているため、仕様上必要な更新ではなく、Spanner/GFE側のheuristicに効くかを見る診断として扱う。
3. official SA / ADC とlite SA / ADCで、request HEADERSのHPACK block長・metadata shapeを同じ表に戻す。今回のtraceではpayload値は保存していないため、credential値そのものはrawに残さない。

## 判断ログ

- active PINGは今回の主要差分として扱わない。公式側にも少数のclient PINGはあるが、SA JSONとADCの差は同じPING lifecycle上でも発生している。
- credentialそのもののwire header差は主因候補から下げる。official SA JSONとlite SA JSONの差が残り、official ADCはliteと近い。
- 今回のraw traceはpayloadやcredential値を保存していないため、git管理外 `var/` に保持する。

## 完了条件

- 4条件で同じRPC境界の `request sent -> first response` が比較できる。
- official SA JSONだけが速い場合、wire/control lifecycle上の差分候補を列挙できる。
