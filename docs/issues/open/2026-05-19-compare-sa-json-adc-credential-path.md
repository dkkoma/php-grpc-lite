---
Status: Open
Owner: Codex
Created: 2026-05-19
GitHub-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# SA JSON / ADC credential path trace比較

## 目的

GitHub issue #5のSpanner CLI再現fixtureで、php-grpc-liteがservice-account JSON keyでは約42ms、gcloud ADCでは約46msになる一方、official ext-grpcはservice-account JSON keyで約24ms、gcloud ADCでは約46msになる差分を説明する。

まずphp-grpc-lite単体で、service-account JSON keyとADC credential pathのwire traceを比較し、metadata、CallCredentials、authorization、HTTP/2 frame shape、RPC elapsedにどの差があるかを特定する。

## 背景

`tools/diagnostics/issue5-spanner-repro/` で報告者提供reproを再現した。

- gcloud ADC mount: official/lite とも約46ms。
- service-account JSON key: official約24ms、lite約42ms。

lite側はcredential pathを変えても大きくは速くならない。official側だけがservice-account JSON keyで大きく速くなるため、まずlite側でcredential pathごとのactual request shapeを細かく比較する。

## スコープ

- `spanner-repro:lite` のみを対象にする。
- service-account JSON keyとgcloud ADC mountの2条件で `GRPC_LITE_TRACE_FILE` を取得する。
- `wire.request_header`、`wire.frame_out`、`wire.frame_in`、`rpc.end` を比較する。
- publicに貼れないcredential値は扱わず、traceのredacted/digest/lengthだけを使う。

## 非スコープ

- official ext-grpcの内部trace。まずlite内の差分を固定する。
- metadata server ADCの完全再現。手元Dockerで使えるADCはgcloud ADCであり、GCE metadata server credentialとは別物として扱う。

## 進捗

- [ ] SA JSON trace取得
- [ ] gcloud ADC trace取得
- [ ] metadata/header差分集計
- [ ] frame/RPC elapsed差分集計
- [ ] 次の調査対象決定

## 完了条件

- credential pathごとの差分が表で記録されている。
- 差分がissue #5の1.75x gapを説明し得るか、説明しないかを判断している。
- 次に見るべき対象が1〜3個に絞られている。

## 実行 2026-05-19

fixture: `tools/diagnostics/issue5-spanner-repro/`
image: `spanner-repro:lite`
iterations: `20`
trace: `GRPC_LITE_TRACE_FILE`

条件:

- SA JSON: `GOOGLE_APPLICATION_CREDENTIALS=/sa.json`
- ADC: hostのgcloud ADCを `/root/.config/gcloud` へread-only mount

注意: ADC条件はGCE metadata serverではなくgcloud ADCである。metadata server credential pathとは別物として扱う。

## latency summary

trace有効なので絶対値は比較用ではない。shape差の参考値として見る。

| condition | total mean | total p50 | total p90 | total p99 |
|---|---:|---:|---:|---:|
| SA JSON | 44.987ms | 42.724ms | 50.657ms | 82.733ms |
| gcloud ADC | 42.677ms | 40.301ms | 54.528ms | 72.465ms |

`rpc.end` method別:

| condition | method | n | mean | p50 | p90 | p99 |
|---|---|---:|---:|---:|---:|---:|
| SA JSON | Commit | 20 | 22.324ms | 21.168ms | 25.273ms | 47.047ms |
| SA JSON | ExecuteStreamingSql | 21 | 22.721ms | 20.699ms | 27.453ms | 44.565ms |
| gcloud ADC | Commit | 20 | 20.927ms | 19.008ms | 26.841ms | 44.286ms |
| gcloud ADC | ExecuteStreamingSql | 21 | 21.944ms | 20.870ms | 26.037ms | 43.982ms |

lite単体ではSA JSONとgcloud ADCのRPC elapsed差は小さい。issue #5の1.75x差そのものは、lite内のSA/ADC差では説明できない。

## request metadata差分

代表として `Spanner/Commit` を比較する。

| header | SA JSON | gcloud ADC | 判断 |
|---|---:|---:|---|
| `authorization` | len 739 | len 261 | 大きく異なる。SA JSONはJWT bearer、ADCは短いOAuth token系と見える。値は記録しない。 |
| `x-goog-api-client` | len 94, `... cred-type/jwt` | len 80, cred-typeなし | credential markerが異なる。SA JSONでは0.0.8のfold後1値。 |
| `x-goog-user-project` | なし | len 18 | ADCだけquota project metadataが付く。 |
| `x-goog-request-params` | len 165, digest A | len 165, digest B | lengthは同じだがsession名等が違うためdigestは異なる。 |
| `grpc-timeout` | `3600000m` | `3600000m` | 同じ。 |
| `user-agent` | len 40 | len 40 | 同じ。 |
| `content-type` / `te` / pseudo headers | 同じ | 同じ | 同じ。 |

CreateSessionではADCのみ `x-goog-api-client` に `cred-type/u` が付くが、Commit/ExecuteStreamingSqlでは付かない。SA JSONでは全Spanner RPCに `cred-type/jwt` が付く。

## HTTP/2 frame差分

代表として `Spanner/Commit` を比較する。

| frame | SA JSON | gcloud ADC | 判断 |
|---|---:|---:|---|
| HEADERS payload length | 630B | 249B | SA JSONのauthorization/JWTにより大きい。 |
| DATA payload length | 208B | 208B | 同じ。Commit protobuf/gRPC bodyは同じ。 |
| connection SETTINGS | `ENABLE_PUSH=0`, `INITIAL_WINDOW_SIZE=8388608` | 同じ | 同じ。 |
| connection WINDOW_UPDATE | increment `8323073` | 同じ | 同じ。 |
| PING count | 43 | 43 | 同じiteration数では同程度。 |

SA JSONはHEADERSが大きいが、lite単体ではSA JSONが大幅に遅くなるわけではない。むしろtraceなし200 iterationではSA JSON liteが42.2ms、gcloud ADC liteが46.2msだった。

## 判断

- php-grpc-lite内のSA JSON vs gcloud ADCでは、metadata/header shapeは明確に違う。
- ただし、liteのRPC elapsedは近く、issue #5の1.75x gapはlite内のcredential path差だけでは説明できない。
- 再現の本質は「SA JSON/JWT条件でofficial ext-grpcだけが約24msまで速くなるが、liteは約42msに留まる」こと。
- 次は `official SA JSON` と `lite SA JSON` のactual request/transport shape差を見る必要がある。

## 次の調査対象

1. official ext-grpc SA JSON条件で、可能な範囲のGAX metadata / syscall / tcpdump / GRPC_TRACEを取得し、lite SA JSONと比較する。
2. lite SA JSONで、authorization headerのHPACK indexing方針、header order、dynamic table warm state、HEADERS payload length推移を確認する。
3. official ext-grpcがSA JSON/JWT条件でSpannerから速い応答を得るwire shape要素を特定する。

## 進捗

- [x] SA JSON trace取得
- [x] gcloud ADC trace取得
- [x] metadata/header差分集計
- [x] frame/RPC elapsed差分集計
- [x] 次の調査対象決定
