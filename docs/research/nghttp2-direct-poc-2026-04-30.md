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

`nghttp2_poc_unary_batch()` は同一 connection 上で逐次 unary stream を作る。PHP resource lifecycle をまだ作らず、PoC 計測専用の batch API にしている。

## 実装上の注意

- `nghttp2_session_client_new()` だけでは client initial SETTINGS が送られないため、`nghttp2_submit_settings()` を明示的に呼ぶ必要があった。これが無いと Go gRPC server は SETTINGS 前に HEADERS を受け取り、HTTP response 前に connection を閉じる。
- `TCP_NODELAY` が無いと小さい sequential unary で Nagle / delayed ACK の影響が大きく、10 calls で約 406ms まで悪化した。gRPC transport としては無効化が必須。
- request は protobuf payload ではなく、5 byte gRPC frame header 付き body を渡す契約にしている。
- response body も gRPC frame header 付き raw DATA のまま返す。drop-in互換実装ではなく、transport観測用。

## 実行方法

```bash
docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && phpize && ./configure --enable-nghttp2-poc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so poc/nghttp2-client-ext/bench.php --iterations=1000 --response-bytes=102400
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

## 観察

- direct nghttp2 は 100KB response の p50 で libcurl 通常経路より明確に低い。直近の libcurl 通常経路は 100KB response p50 120.1μs / p99 1976.8μs だったため、transport 境界に改善余地がある。
- 1MB response は p50 533μs / p99 3909μs で、直近の libcurl 通常経路 582.8μs / 4289.3μs より少し良い程度。large response の主因は transport API だけではなく、memory copy / server pacing / HTTP/2 flow control も混ざる。
- 1MB request / small response でも p50 435μs / p99 3985μs なので、upload 側でも direct nghttp2 は観測線として有効。
- ただしこの PoC は互換実装ではない。metadata / trailer / error semantics / deadline / TLS を満たすにはかなり追加実装が必要。

## 判断

- C extension PoC は transport decision の材料として有効。特に small / medium unary では libcurl + PHP ext/curl の固定費を外す余地が見える。
- ただし現時点で php-grpc-lite 本体を C extension 前提に切り替える判断材料としては不足。drop-in replacement の価値は互換性が主で、C transport は別フェーズの候補に留める。
- 次に進めるなら、C extension を本体化する前に `php-grpc-lite` の PHP userland 側で削れる固定費を先に潰し、その後に「native transport を入れるならどの surface を C に落とすか」を決める。
