# instrumented libcurl upload path 計測

## 目的

php-grpc-lite の large request / small response 経路について、PHP userland から見える `CURLINFO_POSTTRANSFER_TIME_T` より内側の HTTP/2 upload path を確認する。

公式 ext-curl を差し替えるのではなく、PHP バイナリが動的リンクしている `libcurl.so.4` を `LD_LIBRARY_PATH` で研究用ビルドに差し替えた。

## 差し替え方法

- dev image の PHP は `curl.so` をロードしておらず、`/usr/local/bin/php` が `libcurl.so.4` を直接 dynamic link している。
- `var/instrumented-curl/lib/libcurl.so.4` を置き、`LD_LIBRARY_PATH=/workspace/var/instrumented-curl/lib php ...` で差し替えた。
- CMake build の libcurl は symbol version 情報を持たないため、PHP 起動時に `no version information available` warning が出る。計測用としては動作する。
- 研究用ビルドは HTTP/2/nghttp2 を有効にしているが、distro libcurl と完全同一 feature set ではない。TLS/mTLS や通常ベースラインには使わない。

再現用入口:

```bash
./bench/build-instrumented-libcurl.sh
./bench/run-instrumented-libcurl-upload.sh
```

## 計測条件

- server: Go test-server
- client: php-grpc-lite
- RPC: request-unary diagnostic
- request payload: 1MiB
- response payload: 0B
- calls: 3
- warmup: 0
- trace: `CURLOPT_DEBUGFUNCTION` + libcurl `infof("[CURLINST] ...")`

## 結果

| case | p50 total | p99 total | p50 POSTTRANSFER | p99 POSTTRANSFER | p50 STARTTRANSFER | p50 server in-payload |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| POSTFIELDS | 3078.4us | 7282.3us | 2273.0us | 3869.0us | 2355.0us | 2033.1us |
| READFUNCTION | 3884.7us | 5954.3us | 3023.0us | 5352.0us | 2914.0us | 1770.0us |

trace summary:

| case | call | first DATA_OUT | last DATA_OUT | first HEADER_IN | WINDOW_UPDATE | body_send | req_body_read result=81 | send blocked |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| POSTFIELDS | 1 | 3605.8us | 6394.0us | 6426.8us | 1 | 17 | 16 | 0 |
| POSTFIELDS | 2 | 985.7us | 2893.7us | 2907.5us | 1 | 17 | 16 | 0 |
| POSTFIELDS | 3 | 759.0us | 2644.0us | 2812.5us | 1 | 17 | 16 | 0 |
| READFUNCTION | 1 | 1583.1us | 3591.8us | 3600.9us | 1 | 17 | 17 | 0 |
| READFUNCTION | 2 | 2254.6us | 5308.6us | 5377.7us | 1 | 17 | 17 | 0 |
| READFUNCTION | 3 | 521.9us | 2115.9us | 2124.0us | 1 | 17 | 17 | 0 |

## 観測

- 1MiB request は概ね 17 個の `DATA_OUT` に分割される。各 chunk は約 64KiB。
- `h2_send_callback blocked` は出ていない。少なくともこの localhost/Docker 条件では socket write の `CURLE_AGAIN` が主因ではない。
- `req_body_read result=81` は nghttp2 の data provider が「今は追加 body なし」を返す箇所で、送信中に繰り返し発生する。
- `WINDOW_UPDATE` は各 call で 1 回見えている。flow-control は関与しているが、今回の trace では明示的な send block としては現れていない。
- `first HEADER_IN` は `last DATA_OUT` の直後に来ている。POSTTRANSFER -> STARTTRANSFER の待ちは小さく、1MiB request の主な client observable 時間は upload 完了前に寄っている。

## 判断

この条件では「upload で詰まっているか / libcurl-nghttp2 内部か」の切り分けについて、公開 curl info より一段内側まで確認できた。

結論は、large request の 1MiB では libcurl/nghttp2 の HTTP/2 DATA frame 生成・送信ループに wall time が乗っている。ただし socket write block は観測されていないため、単純な OS write 待ちではなく、nghttp2 data provider / flow-control window / curl send loop の進行単位に時間が出ていると見るのが妥当。

次に php-grpc-lite 側で試す価値があるのは、採用前提ではなく検証目的としての upload path variants である。`POSTFIELDS` と `READFUNCTION` の差は今回安定して優位とは言えないため、実装採用には追加計測が必要。
