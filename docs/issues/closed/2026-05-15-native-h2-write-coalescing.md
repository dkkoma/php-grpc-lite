---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# native HTTP/2 write coalescing検証

## 目的

Cloud Spanner small SELECT系の高負荷CPUで、HTTP/2/TLS送信側の小さいwrite分割がCPU・context switch・tail latencyへ影響しているかを検証し、改善できるならproduction経路へ取り込む。

## 背景

`setsockopt` hot path削減後、Cloud Spanner / Laravel / native / c32 sustainedは大きく改善した。一方、targeted straceではsmall SELECT 3 iterationsでnetwork fdに対して複数の小さいTLS `write()` が発生している。

production経路は `nghttp2_session_send()` + `send_callback()` で、nghttp2が出したchunkごとに `SSL_write()` / `send()` している。small unary/server streaming requestではHEADERS/DATA/WINDOW_UPDATE/ACKなどが小さいwriteに分かれやすい。

## スコープ

- productionのHTTP/2 send flush経路を確認する。
- `nghttp2_session_mem_send()` または送信bufferで、小さいchunkを1回のtransport writeへまとめられるか検証する。
- unary / server streaming / TLS / h2c / deadline / RST_STREAM / GOAWAYの挙動を維持する。
- 大きいrequestで余分なcopyが悪化しないよう、large chunkは直接送る、またはbuffer thresholdを明示する。
- c32 sustainedとcontrolled CPU microで改善/悪化を判断する。

## 非スコープ

- PHP/Laravel/google-cloud-spanner/laravel-spanner側のfile/session/bootstrap最適化。
- ext-grpc比較を主目的にすること。
- client streaming / bidi streaming。

## 現在の観測

- `var/bench-results/native-strace-select50-after-timeout-20260515/strace-summary.txt`: syscall totalは29ms/50RPCで、elapsed 2.08sの主因ではない。
- `var/bench-results/native-strace-fcntl-after-timeout-20260515/fcntl.log`: 3 iterationsでnetwork fdの小さいTLS writeが複数見える。
- `bench/run.sh cpu-micro`: controlled Go serverではsmall unary / small streaming固定費は10〜18µs/callで、transport単体は大きくない。高負荷FPMでは小さい差分がCPU quota下で増幅する可能性を検証する。

## 計画

1. 現行send pathのwrite回数をproduction-equivalentに近い形で記録する。
2. write coalescing PoCを実装する。
3. PHPT / static analysisを通す。
4. `cpu-micro` とCloud Spanner c32 sustainedで比較する。
5. HTTP/2 / gRPC domain model reviewを実施する。
6. 悪化ケースがあれば見送る。採用する場合はこのissueに判断を記録する。

## 完了条件

- 小さいwrite分割がCPU問題の有意な要因か判断できている。
- 採用/見送りの理由が、数値とプロトコル責務の両面で説明できる。

## 検証結果

### controlled CPU micro

比較対象は同一ブランチ上のcoalescing実装前後。Go test-server / native only。

| suite | measurement | before cpu_us/call | after cpu_us/call | 判断 |
| --- | ---: | ---: | ---: | --- |
| cpu-micro | small_unary_100b | 13.3 | 15.0 | 悪化 |
| cpu-micro | small_streaming_1x100b | 13.0 | 14.3 | 悪化 |
| cpu-micro | small_streaming_100x100b | 61.2 | 63.5 | 悪化 |
| cpu-concurrent | small_unary_100b | 17.6 | 29.5 | 大きく悪化 |
| cpu-concurrent | small_streaming_1x100b | 18.1 | 27.5 | 大きく悪化 |
| cpu-concurrent | small_streaming_100x100b | 78.2 | 94.0 | 悪化 |

`nghttp2_session_mem_send2()` + stack buffer coalescingは、小さいwrite回数を減らせる可能性はあるが、この実装ではcopyとloopの固定費が上回った。Cloud Spanner c32 sustainedへ進む前にcontrolled CPUで悪化が明確なため、採用しない。

## 判断

- Status: Closed
- 修正コミット: なし。PoC実装は戻した。
- 判断: 見送り。現行の `nghttp2_session_send()` + send callback経路を維持する。
- 理由: native transport単体の固定費が悪化し、特にconcurrent microでCPU悪化が大きい。小さいwrite分割は観測されたが、現時点の優先改善候補ではない。
