---
Status: Open
Owner: Codex
Created: 2026-05-21
Related:
  - https://github.com/dkkoma/php-grpc-lite/issues/5
  - docs/issues/open/2026-05-20-minimal-select1-four-variant-wire-trace.md
  - docs/issues/open/2026-05-20-spanner-gap-pacing-investigation.md
---

# HTTP/2 control lifecycle 実験

## 目的

real Spanner `ExecuteStreamingSql SELECT 1` でofficial ext-grpc SA JSONだけが速い差について、grpc-lite側で観測済みのHTTP/2 control lifecycle差を1因子ずつ再現し、どの差が `request DATA -> first response HEADERS` gapに効くか確認する。

## 背景

4条件traceで、official ext-grpcとgrpc-liteには次の差がある。

- officialのinitial `SETTINGS` は36Bで、`INITIAL_WINDOW_SIZE=4194304`, `MAX_FRAME_SIZE=4194304`, `MAX_HEADER_LIST_SIZE=16384`, `GRPC_ALLOW_TRUE_BINARY_METADATA=1` などを明示する。
- grpc-liteのinitial `SETTINGS` は12Bで、`ENABLE_PUSH=0`, `INITIAL_WINDOW_SIZE=8388608` のみ。
- officialは小さいresponse後にもconnection-level `WINDOW_UPDATE` をほぼ毎RPC送る。
- grpc-liteは初期connection `WINDOW_UPDATE` 以外を送っていない。

ただしofficial SA/ADCのinitial `SETTINGS` とresponse後 `WINDOW_UPDATE` lifecycleはほぼ同じなので、これら単独でofficial SA fast pathを説明するとは断定しない。ここでは「officialとliteのtransport差として効くか」を切り分ける。

## スコープ

- initial `SETTINGS` official profileの単独実験。
- initial `SETTINGS` handshake完了待ちの単独実験。
- response後の小さいconnection-level `WINDOW_UPDATE` 単独実験。
- 両方有効時の補助確認。
- real Spanner `SELECT 1` / SA JSON / ADC のtrace付き比較。

## 非スコープ

- production defaultの変更。
- gRPC Core BDP estimatorやkeepaliveの移植。
- credential値やauthorization payloadの保存。
- Spanner/GFE内部挙動の断定。

## 計画

- [x] 診断INIを追加する。
- [x] initial `SETTINGS` official profileを実装する。
- [x] initial `SETTINGS` handshake完了待ちを実装する。
- [x] response後 connection-level `WINDOW_UPDATE` を実装する。
- [x] default offでPHPT/static analysisを通す。
- [x] 4条件または必要最小条件でtrace計測する。
- [x] 結果から採用候補/棄却候補を分ける。

## 進捗

- `grpc_lite.http2_experimental_ext_grpc_158_settings_profile=1` を追加した。default off。
  - initial `SETTINGS` をofficial ext-grpc 1.58.0 traceで観測したprofileへ寄せる。
  - `ENABLE_PUSH=0`, `MAX_CONCURRENT_STREAMS=0`, `INITIAL_WINDOW_SIZE=4194304`, `MAX_FRAME_SIZE=4194304`, `MAX_HEADER_LIST_SIZE=16384`, `65027=1`。
  - initial connection-level `WINDOW_UPDATE` は `4128769` に寄せる。
- `grpc_lite.http2_experimental_data_chunk_window_update=1` を追加した。default off。
  - response `DATA` chunk受信ごとにconnection-level `WINDOW_UPDATE` を `len` 分送る。
  - これは「applicationがresponseを消費した」ではなく「transportがDATA chunkを観測した」時点の実験である。
  - stream-level `WINDOW_UPDATE` は送らない。
- `grpc_lite.http2_experimental_wait_initial_settings_ack=1` を追加した。default off。
  - connection preface / initial `SETTINGS` / initial connection-level `WINDOW_UPDATE` 送信後、peer `SETTINGS` 受信、client `SETTINGS ACK` 送信、client initial `SETTINGS` に対するpeer `SETTINGS ACK` 受信まで待ってから最初のRPC streamを送る。
  - sleepではなくHTTP/2 settings handshake完了を条件にする。

## 検証

- Build: `docker compose run --rm dev sh -lc 'make -C /workspace/ext/grpc -j2'`
- PHPT: `./tools/test/check-phpt.sh`
  - 16 passed.
- Static analysis: `./tools/test/check-c-static-analysis.sh`
  - passed.
- Raw data: `var/bench-results/select1-http2-control-experiments-20260521/`

### trace確認

SA JSON / tcpdumpあり / n=20 で4条件を確認した。

| variant | outbound SETTINGS | initial conn WINDOW_UPDATE | response後 conn WINDOW_UPDATE |
|---|---:|---:|---:|
| baseline | 2 entries, `INITIAL_WINDOW_SIZE=8388608` | `8323073` | なし |
| settings | 6 entries, official profile | `4128769` | なし |
| response-wu | 2 entries, `INITIAL_WINDOW_SIZE=8388608` | `8323073` | `179`, `26` repeated, tail `5` |
| both | 6 entries, official profile | `4128769` | `179`, `26` repeated, tail `5` |

意図した2因子はtrace上で独立して切り替わっている。

### latency確認

tcpdumpなし / traceあり / n=100 の `ExecuteStreamingSql SELECT 1`。値はPHP markerのelapsed。

| credential | variant | p50 us | p90 us | p99 us |
|---|---|---:|---:|---:|
| SA JSON | baseline | 22525 | 25846 | 27807 |
| SA JSON | settings | 22603 | 24437 | 26872 |
| SA JSON | response-wu | 21290 | 22867 | 24808 |
| SA JSON | both | 19571 | 22320 | 36473 |
| ADC | baseline | 22614 | 25261 | 30524 |
| ADC | settings | 21514 | 23618 | 26842 |
| ADC | response-wu | 19981 | 22022 | 23953 |
| ADC | both | 20332 | 21957 | 25842 |

## 判断ログ

- `ext_grpc_158_settings_profile` はSA JSON/ADCともp90/p99に改善傾向があるが、p50の効果は小さい。単独採用を判断できるほど強い結果ではない。
- `data_chunk_window_update` はSA JSON/ADCともp50/p90/p99に改善傾向がある。現時点の2因子ではこちらが有力な追加調査候補。
- 両方有効時はp50/p90改善傾向があるが、SA JSON p99に大きな外れ値が出た。組み合わせ採用はまだ判断しない。
- `ext_grpc_158_settings_profile` の `ext_grpc_158` は比較対象imageで観測したprofile名であり、HTTP/2/gRPC仕様上のofficial semanticsではない。
- 次は「official profileへ寄せるべき仕様差」と「Spanner/GFEへの経験的チューニング」を分離して、採用候補を絞る必要がある。

### initial SETTINGS ACK待ち確認

tcpdumpなし / traceあり / n=100 の `ExecuteStreamingSql SELECT 1`。値はPHP markerのelapsed。

| credential | variant | p50 us | p90 us | p99 us |
|---|---|---:|---:|---:|
| SA JSON | baseline | 22404 | 25394 | 48267 |
| SA JSON | wait-ack | 21891 | 23651 | 26197 |
| SA JSON | settings | 22710 | 25417 | 27008 |
| SA JSON | settings + wait-ack | 22680 | 24677 | 27745 |
| ADC | baseline | 22518 | 24363 | 30383 |
| ADC | wait-ack | 22504 | 24737 | 32005 |
| ADC | settings | 22081 | 24381 | 26441 |
| ADC | settings + wait-ack | 21570 | 23623 | 25771 |

trace上のframe順序:

- baseline:
  - `OUT SETTINGS`
  - `OUT WINDOW_UPDATE`
  - `OUT HEADERS/DATA stream=1 CreateSession`
  - `IN SETTINGS`
  - `OUT SETTINGS ACK`
  - `IN SETTINGS ACK`
- wait-ack:
  - `OUT SETTINGS`
  - `OUT WINDOW_UPDATE`
  - `IN SETTINGS`
  - `OUT SETTINGS ACK`
  - `IN SETTINGS ACK`
  - `OUT HEADERS/DATA stream=1 CreateSession`

判断:

- ACK待ちはSA JSONでp90/p99改善が見える。特にbaseline p99の外れ値を抑えている。
- ADCではACK待ち単独の改善は見えないが、`ext_grpc_158_settings_profile` と組み合わせるとp50/p90/p99が改善する。
- この効果は測定対象SELECT 1の直前にsleepを入れる話とは異なり、connection作成時の最初のRPC前にHTTP/2 settings handshakeを完了させる話である。測定対象SELECT 1自体はwarmup後の同一connection上で実行している。
- ただし、実験対象はreal Spannerであり揺れがあるため、これだけでproduction default化は判断しない。次に、`wait-ack` と `data_chunk_window_update` のどちらが実ワークロード差に効くかを同条件で比較する。

## 完了条件

- 2因子を別々に測った結果が記録されている。
- trace上で意図したSETTINGS/WINDOW_UPDATE差が出ている。
- 次に本実装へ進めるべき候補が明確になっている。
