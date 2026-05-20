---
Status: Open
Owner: Codex
Created: 2026-05-20
Related:
  - https://github.com/dkkoma/php-grpc-lite/pull/6
  - https://github.com/dkkoma/php-grpc-lite/issues/5
  - docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md
  - docs/reviews/issues/2026-05-20-pr6-http2-specialist-review.md
  - docs/reviews/issues/2026-05-20-pr6-grpc-specialist-review.md
  - docs/reviews/issues/2026-05-20-pr6-spanner-specialist-review.md
---

# Spanner SELECT 1 gap / pacing 調査

## 目的

GitHub issue #5 の `ExecuteStreamingSql SELECT 1` で、grpc-lite が official ext-grpc 1.58.0 より遅い原因を、clientから観測できるrequest / response / HTTP/2 connection lifecycleで切り分ける。

このブランチでは、client-origin PINGを送る診断機能は削除する。今後の調査軸は、PING有無ではなく、同一HTTP/2 connection上で連続RPCを投げたときのinter-RPC gap、wire timing、trace timing、peer側pacing / schedulingの差に置く。

## 背景

real Cloud Spanner `ExecuteStreamingSql SELECT 1` + service-account JSON 条件では、grpc-liteの連続RPCでresponse到着前の待ち時間がofficial ext-grpcより長い。

その後のtrace + tcpdump確認で、10ms程度のinter-RPC gapを入れるだけでgrpc-liteのp50が大きく縮み、official ext-grpcに近づくことがわかった。gap有無でrequest payloadやconnection churnは変わらないため、現時点ではclient request生成の差よりも、Google Frontend / Spanner側のconnection schedulingまたはpacing差を疑う。

## スコープ

- grpc-lite gap 0ms / 10ms のtrace + tcpdump相関を残す。
- official ext-grpc 1.58.0 とgrpc-liteの同条件比較を残す。
- 同一connection reuseが維持されているかを確認する。
- request送信完了からfirst response frame / first inbound response packetまでの差を主要観測点にする。
- future implementation candidateは、gapによる観測結果から別issueとして切る。

## 非スコープ

- client-origin PING診断機能の維持。
- gRPC Core BDP estimatorの移植。
- keepalive PINGの実装。
- Google内部のofficial service-account fast pathの根因追跡。
- trace有効時のlatency絶対値を性能比較として扱うこと。

## 進捗

- [x] mainのpersistent preflight drainをPRブランチへ取り込む。
- [x] tcpdump + grpc-lite traceでgap 0ms / 10msを比較する。
- [x] official ext-grpc 1.58.0とのgap比較を残す。
- [x] client-origin PING診断機能を削除する。
- [x] PHPT / static analysisで削除後のtransportを検証する。
- [x] PR説明をgap / pacing調査として更新する。

## 主要観測

### gap sweep

marker-only grpc-liteで、RPC間gapだけを変えた結果。

| variant | gap | elapsed p50 | 判断 |
|---|---:|---:|---|
| grpc-lite | 0ms | 21.254ms | gapなし連続RPCで遅い |
| grpc-lite | 10ms | 12.187ms | official ext-grpcに近い |
| grpc-lite | 50ms | 13.063ms | 10msと同レンジ |

同条件のofficial ext-grpc 1.58.0。

| variant | gap | elapsed p50 | 判断 |
|---|---:|---:|---|
| official ext-grpc 1.58.0 | 0ms | 10.881ms | gapなしでも速い |
| official ext-grpc 1.58.0 | 10ms | 12.703ms | grpc-lite gap 10msと近い |
| official ext-grpc 1.58.0 | 50ms | 13.267ms | grpc-lite gap 50msと近い |

### trace + tcpdump相関

raw dataはgit管理外の `var/issue5-bdp-matrix/tcpdump-gap-dual-marker-20260520-2132/` に保存している。

| variant | SYN | gap control frames | elapsed p50 | trace last request frame -> first response frame | tcp last outbound payload -> first inbound payload |
|---|---:|---:|---:|---:|---:|
| grpc-lite gap 0ms | 1 | 0 | 25.034ms | 22.105ms | 21.465ms |
| grpc-lite gap 10ms | 1 | 0 | 15.186ms | 10.959ms | 9.883ms |

判断:

- gap 0ms / 10msともTCP connectionは1本で、connection churnではない。
- gap中にHTTP/2 control frameは流れていない。
- gap 10msではrequest送信開始・完了はむしろ少し遅い。client send timingの短縮では説明できない。
- 差はrequest送信完了後、first response packet/frame到着前に集中している。
- wire shape / packet countは大きく変わらない。

## 現時点の仮説

- grpc-liteのgapなし連続RPCは、Google Frontend / Spanner側のconnection-level schedulingまたはfairness windowに当たり、次RPCのresponse schedulingが遅れる可能性がある。
- inter-RPC gapは、そのpeer側状態を自然に緩和する観測手段になっている。
- official ext-grpcはgapなしでも同じ遅延に入りにくい。clientから観測できる差として、HTTP/2 control lifecycle、poll/read timing、connection idle handling、flow-control update timingを比較対象にする。

## 次に調べること

1. official ext-grpc 1.58.0、grpc-lite SA JSON、grpc-lite ADCで、gap 0msのwire/control lifecycleを同じ粒度で比較する。
2. grpc-lite gap 0msで、request送信完了直後からfirst responseまでにclient側で観測できるread/poll状態を追加確認する。
3. flow-control SETTINGS / WINDOW_UPDATE / connection-level frame処理の差がresponse timingに影響し得るかを、実装とtcpdumpの両方で確認する。
4. 実装候補は、観測できた差分ごとに別issueへ分離し、bench / trace / reviewを通して採否を判断する。

## 完了条件

- client-origin PING診断機能がC実装、INI、PHPTから消えている。
- 今後の調査docsがPING有無を状態軸にしない。
- gap 0ms / 10ms のtrace + tcpdump根拠から、次に調べるtransport差分が明確になっている。
