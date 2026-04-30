# nghttp2 direct transport PoC 2026-04-30

libcurl 継続判断の材料として、PHP C extension から `libnghttp2` を直接呼ぶ h2c unary PoC を追加した。目的は ext-grpc に近づけることではなく、php-grpc-lite の transport 固定費が libcurl / PHP ext/curl 境界にどの程度依存しているかを観測すること。

## 実装範囲

| 項目 | 内容 |
|---|---|
| 配置 | `poc/nghttp2-client-ext/` |
| extension | `nghttp2_poc` |
| API | `nghttp2_poc_unary()` / `nghttp2_poc_unary_batch()` |
| transport | plaintext h2c prior knowledge only |
| RPC | unary only |
| reuse | `nghttp2_poc_unary_batch()` が 1 TCP connection / 1 HTTP/2 session を使い回す |
| 対象外 | TLS, mTLS, streaming, deadline, retry, full metadata compatibility |

`nghttp2_poc_unary_batch()` は同一 connection 上で逐次 unary stream を作る。PHP resource lifecycle をまだ作らず、PoC 計測専用の batch API にしている。第 7 引数 `split_grpc_frame=true` では、PHP 側から serialized protobuf payload だけを渡し、C 側の nghttp2 data provider が 5 byte gRPC header と payload を分割送信する。

## 実装上の注意

- `nghttp2_session_client_new()` だけでは client initial SETTINGS が送られないため、`nghttp2_submit_settings()` を明示的に呼ぶ必要があった。これが無いと Go gRPC server は SETTINGS 前に HEADERS を受け取り、HTTP response 前に connection を閉じる。
- `TCP_NODELAY` が無いと小さい sequential unary で Nagle / delayed ACK の影響が大きく、10 calls で約 406ms まで悪化した。gRPC transport としては無効化が必須。
- request は通常は 5 byte gRPC frame header 付き body を渡す契約にしている。`split_grpc_frame=true` の時だけ、C 側で 5 byte header を作り、protobuf payload と連結せずに送る。
- response body も gRPC frame header 付き raw DATA のまま返す。drop-in互換実装ではなく、transport観測用。

## 実行方法

```bash
docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && phpize && ./configure --enable-nghttp2-poc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so poc/nghttp2-client-ext/bench.php --iterations=1000 --response-bytes=102400
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so poc/nghttp2-client-ext/bench.php --iterations=1000 --response-bytes=100 --request-bytes=1048576 --split-grpc-frame
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so poc/nghttp2-client-ext/bench.php --iterations=1000 --response-bytes=100 --request-bytes=1048576 --split-grpc-frame --no-copy
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so poc/nghttp2-client-ext/bench.php --iterations=1000 --response-bytes=100 --request-bytes=1048576 --split-grpc-frame --poll-loop
```

## 参考計測

条件:

| 項目 | 内容 |
|---|---|
| 実行日 | 2026-04-30 |
| 対向 | Go test-server `:50051` h2c |
| RPC | `/helloworld.Greeter/BenchUnary` |
| payload | cached response payload |
| connection | 1 TCP connection / 1 HTTP/2 session reused |
| 実行入口 | `poc/nghttp2-client-ext/bench.php` |

| case | iterations | calls/s | p50 | p99 | body bytes |
|---|---:|---:|---:|---:|---:|
| response 100B | 1000 | 36374.2 | 20μs | 77μs | 107 |
| response 100KB | 1000 | 7983.9 | 68μs | 1425μs | 102409 |
| response 1MB | 300 | 1317.9 | 533μs | 3909μs | 1048585 |
| request 1MB / response 100B | 300 | 1226.4 | 435μs | 3985μs | 107 |

追加で large request の gRPC frame build を C 側に寄せるため、serialized protobuf payload と 5 byte gRPC header を連結しない `--split-grpc-frame` を測った。

| case | implementation | iterations | calls/s | p50 | p99 | request wire bytes |
|---|---|---:|---:|---:|---:|---:|
| request 1MB / response 100B | frame済みPHP string | 1000 | 1370.2 | 379μs | 3866μs | 1048587 |
| request 1MB / response 100B | C側 split gRPC frame | 1000 | 1461.2 | 372μs | 3772μs | 1048587 |

さらに `NGHTTP2_DATA_FLAG_NO_COPY` と `nghttp2_send_data_callback` を使い、nghttp2 の DATA buffer への payload copy を避ける経路を測った。send data callback は HTTP/2 frame header と payload segment を `writev()` でまとめて送る。

| case | implementation | iterations | calls/s | p50 | p99 | DATA frames | WINDOW_UPDATE recv | write syscalls |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| request 1MB / response 100B | split + copy | 1000 | 1390.2 | 378μs | 3779μs | 65003 | 3070 | 67030 |
| request 1MB / response 100B | split + no-copy | 1000 | 1499.3 | 342μs | 3623μs | 65002 | 5034 | 67025 |
| request 1MB / response 100B | split + 8KB DATA cap | 1000 | 1255.0 | 460μs | 3979μs | 129000 | 3061 | 131020 |

この run の ext-grpc 既存値は 1387.0 calls/s、p50 456.8μs、p99 3073.9μs。no-copy PoC は throughput と p50 では ext-grpc を超えたが、p99 はまだ約550μs遅い。

blocking `send()` / `recv()` の単純ループを外すため、copy 経路で socket を nonblocking にし、`nghttp2_session_want_read/write` と `poll()` で駆動する `--poll-loop` も試した。`NGHTTP2_DATA_FLAG_NO_COPY` の send data callback は complete DATA frame 送信が契約で、partial write 後の `EAGAIN` を安全に扱えないため、この PoC では poll loop と no-copy は併用しない。

| case | implementation | iterations | calls/s | p50 | p99 | poll calls | send wouldblock | recv wouldblock |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| request 1MB / response 100B | split + copy | 1000 | 1390.2 | 378μs | 3779μs | 0 | 0 | 0 |
| request 1MB / response 100B | split + poll loop | 1000 | 1432.8 | 375μs | 4162μs | 2420 | 1 | 3420 |
| request 1MB / response 100B | split + no-copy | 1000 | 1475.7 | 348μs | 3681μs | 0 | 0 | 0 |

## 観察

- direct nghttp2 は 100KB response の p50 で libcurl 通常経路より明確に低い。直近の libcurl 通常経路は 100KB response p50 120.1μs / p99 1976.8μs だったため、transport 境界に改善余地がある。
- 1MB response は p50 533μs / p99 3909μs で、直近の libcurl 通常経路 582.8μs / 4289.3μs より少し良い程度。large response の主因は transport API だけではなく、memory copy / server pacing / HTTP/2 flow control も混ざる。
- 1MB request / small response でも p50 435μs / p99 3985μs なので、upload 側でも direct nghttp2 は観測線として有効。
- C 側 split gRPC frame は 1MB request で throughput 1370.2 → 1461.2 calls/s、p50 379 → 372μs、p99 3866 → 3772μs と小さく改善した。PHP 側の `5B header . payload` 連結を外す効果はあるが、p99 の主因を単独で消すほどではない。
- no-copy 送信は 1MB request で throughput 1390.2 → 1499.3 calls/s、p50 378 → 342μs、p99 3779 → 3623μs に改善した。nghttp2 の内部 DATA buffer copy を避ける効果はある。
- 8KB DATA cap は DATA frame / syscall 数をほぼ倍増させ、throughput と latency を悪化させた。peer `remote_max_frame_size` は 16KB なので、PoC側で小さく切る意味はない。
- min remote connection window は 2B まで落ちており、1MB upload は connection-level flow control を踏んでいる。no-copy でも p99 が ext-grpc に届かない残差は、copyよりも flow-control / send-recv loop / scheduler 側に残る可能性が高い。
- poll loop は copy 経路では p50 がほぼ同等、throughput が少し上がった一方で p99 は 4162μs に悪化した。`recv wouldblock` と `poll calls` が増えており、この単純な poll 駆動は tail 改善にならない。
- ただしこの PoC は互換実装ではない。metadata / trailer / error semantics / deadline / TLS を満たすにはかなり追加実装が必要。

## 判断

- C extension PoC は transport decision の材料として有効。特に small / medium unary では libcurl + PHP ext/curl の固定費を外す余地が見える。
- large request は C 側で gRPC frame を分割送信する余地がある。改善幅は限定的だが、現行 libcurl の `POSTFIELDS` copy / PHP frame concat を避ける方向性としては筋が良い。
- large request upload は no-copy DATA 送信まで入れると ext-grpc の throughput / p50 に到達する。ただし p99 は残る。
- copy 経路の単純な nonblocking poll loop は p99 を改善しなかった。次に tail を追うなら、no-copy 経路でpartial writeを安全に扱う状態機械を作るか、client側だけでなく server stats と同一 call 対応した upload完了時刻を取る必要がある。
- ただし現時点で php-grpc-lite 本体を C extension 前提に切り替える判断材料としては不足。drop-in replacement の価値は互換性が主で、C transport は別フェーズの候補に留める。
- 次に進めるなら、C extension を本体化する前に `php-grpc-lite` の PHP userland 側で削れる固定費を先に潰し、その後に「native transport を入れるならどの surface を C に落とすか」を決める。
