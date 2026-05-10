# instrumented nghttp2 upload path 計測

## 目的

libcurl 側の instrumentation で socket write block は観測されず、残りは `nghttp2_session_send()` 内部に絞られた。そこで nghttp2 1.64.0 を instrumented build に差し替え、DATA frame 分割、data provider、flow-control window、send callback の進行を確認した。

## 差し替え方法

- upstream `nghttp2` `v1.64.0` を `_research/nghttp2` に clone。
- `lib/nghttp2_session.c` に `NGHTTP2INST=1` gated の stderr instrumentation を追加。
- その nghttp2 を `var/instrumented-nghttp2` に install。
- curl 8.14.1 を `PKG_CONFIG_PATH=/workspace/var/instrumented-nghttp2/lib/pkgconfig` で再 build し、`var/instrumented-curl-nghttp2` に install。
- PHP 実行時は `LD_LIBRARY_PATH=/workspace/var/instrumented-curl-nghttp2/lib:/workspace/var/instrumented-nghttp2/lib` で両方を差し替えた。

再現用入口:

```bash
./bench/build-instrumented-nghttp2-libcurl.sh
./bench/run-instrumented-nghttp2-upload.sh
```

## 計測条件

- server: Go test-server
- client: php-grpc-lite
- RPC: request-unary diagnostic
- request payload: 1MiB
- response payload: 0B
- calls: 3
- warmup: 0
- trace: curl `CURLOPT_DEBUGFUNCTION` + libcurl `[CURLINST]` + nghttp2 `[NGHTTP2INST]`

`[NGHTTP2INST]` は stderr へ大量に出すため、wall-time は通常実行より悪化する。この計測は絶対性能ではなく送信構造の確認として扱う。

## 結果

| case | p50 total | p99 total | p50 POSTTRANSFER | p50 STARTTRANSFER | p50 server in-payload |
| --- | ---: | ---: | ---: | ---: | ---: |
| POSTFIELDS | 7414.2us | 14935.7us | 6489.0us | 6615.0us | 6358.0us |
| READFUNCTION | 8141.6us | 10670.7us | 7547.0us | 7567.0us | 7178.7us |

nghttp2 summary:

| case | stream | DATA frames | payload bytes | user deferred | flow-control deferred | stream WINDOW_UPDATE | min conn window | min stream window |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| POSTFIELDS | 1 | 68 | 1048585 | 16 | 2 | 1 | 0 | 0 |
| POSTFIELDS | 3 | 65 | 1048585 | 16 | 0 | 1 | 49150 | 65773 |
| POSTFIELDS | 5 | 65 | 1048585 | 16 | 0 | 1 | 49150 | 65773 |
| READFUNCTION | 1 | 66 | 1048585 | 17 | 1 | 1 | 0 | 0 |
| READFUNCTION | 3 | 66 | 1048585 | 17 | 0 | 1 | 49150 | 65748 |
| READFUNCTION | 5 | 65 | 1048585 | 17 | 0 | 1 | 49150 | 65748 |

## 観測

- libcurl の 1 回の `cf_h2_body_send()` は概ね 64KiB を `stream->sendbuf` に積む。
- nghttp2 はその 64KiB を `max_frame_size=16384` に従って 16KiB DATA frame へ分割する。
- 1MiB request は nghttp2 内部では約 65 個の DATA frame になる。curl debug trace の `DATA_OUT` 17 個は socket write 側で複数 DATA frame がまとまった単位だった。
- 各 64KiB buffer を消費したところで curl の `req_body_read_callback()` が `NGHTTP2_ERR_DEFERRED` を返し、nghttp2 側では `defer_user` として記録される。これは payload 全体で 16〜17 回発生する。
- 先頭 stream では初期 HTTP/2 window 65,535 bytes を使い切り、`defer_flow_control` が発生した。その後 server から stream `WINDOW_UPDATE +1048580` が届く。
- 後続 stream では stream window が拡張済みで、flow-control deferred は出ていない。したがって warm call の主要な進行単位は flow-control stall ではなく、curl send buffer 供給単位と nghttp2 DATA frame 分割である。
- `send_callback_wouldblock` は出ていない。libcurl 側で見た通り、socket write block は主因ではない。

## 判断

未解明だった `nghttp2_session_send()` 内部は、少なくともこの条件では以下の構造だった。

1. PHP/libcurl 側から request body が約 64KiB 単位で curl HTTP/2 stream buffer に供給される。
2. nghttp2 が各 buffer を 16KiB DATA frame に分割する。
3. buffer を読み切るたびに data provider が `NGHTTP2_ERR_DEFERRED` を返し、curl が次の約 64KiB を積むまで nghttp2 の DATA item が user-deferred になる。
4. cold stream だけ初期 flow-control window による停止があるが、warm stream では flow-control deferred は出ていない。
5. socket write block は観測されない。

したがって、1MiB warm request の主な構造的コストは「flow-control待ち」ではなく、libcurl が request body を 64KiB 前後で供給し、nghttp2 が 16KiB frame に分割し、provider deferred/resume を繰り返す送信ループにある。

この結果から、PHP userland で `POSTFIELDS` を `READFUNCTION` に置き換えるだけでは根本的な改善にはなりにくい。大きな改善を狙うなら、libcurl 経由の provider/resume 進行単位を受け入れるか、HTTP/2 transport / C extension 側で request body buffer と nghttp2 send loop をより直接制御する必要がある。
