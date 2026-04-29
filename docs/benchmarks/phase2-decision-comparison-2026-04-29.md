# Phase 2 decision comparison 2026-04-29

Phase 2 の最適化判断に使う長めの比較 run。`bench/phase2/preset.sh decision` で取得した。

## 条件

| 項目 | 内容 |
|---|---|
| 実行日 | 2026-04-29 |
| 実行入口 | `bench/phase2/preset.sh decision` |
| 保存 tag | `phase2-decision-20260429` |
| 対向 | Go test-server |
| 比較対象 | `php-grpc-lite` / 公式 `ext-grpc` |
| 保存先 | `var/bench-results/phase2-*-phase2-decision-20260429-*.json` |

生成 JSON は Phase 2 result contract で検証済み。

## Unary

| suite | php-grpc-lite | ext-grpc | 観察 |
|---|---:|---:|---|
| `throughput-unary` | 25905.2 calls/s | 17237.7 calls/s | 軽量 unary は php-grpc-lite が高 throughput |
| `payload-unary 100KB` | 5442.4 calls/s | 6043.2 calls/s | 100KB は ext-grpc が throughput 優位 |
| `payload-unary 100KB p99` | 2310.4μs | 1341.9μs | 100KB tail は php-grpc-lite が悪い |

100KB 以外の payload では php-grpc-lite の p50/p99 と throughput は良好。大きい response body だけ tail が残る。

## Payload diagnostics

`payload-breakdown` はネットワークを外した hot path。100KB でも slice/decode は μs 台だった。

| operation | p99 |
|---|---:|
| 100KB payload slice | 2.0μs |
| 100KB decode only | 6.2μs |
| 100KB slice + decode | 6.2μs |
| 100KB deserialize apply | 5.8μs |

`payload-unary-diagnostic` は実 RPC 内の opt-in diagnostic。

| metric | p50 | p99 |
|---|---:|---:|
| total unary latency | 94.1μs | 2226.2μs |
| `curl_exec` | 84.2μs | 2187.4μs |
| curl total time | 82.0μs | 2186.0μs |
| curl pretransfer time | - | 108.0μs |
| curl starttransfer time | - | 1894.0μs |
| body append total | - | 9.0μs |
| payload slice | - | 4.6μs |
| deserialize | - | 6.2μs |

100KB tail は `curl_exec` / curl total time 内に集中している。body append、frame parse、payload slice、protobuf deserialize は tail の主因ではない。

追加で `x-bench-server-timing: 1` を送った場合のみ、Go test-server が handler duration と payload allocation duration を trailers に載せるようにした。これにより `curl starttransfer` の内訳として、server handler 側の tail も観測できる。

```bash
BENCH_TAG=phase2-server-timing-20260429 ./bench/phase2/run.sh payload-unary-diagnostic --duration=3 --payload-sizes=102400 --warmup-calls=10
```

| metric | p50 | p99 |
|---|---:|---:|
| total unary latency | 93.0μs | 2028.1μs |
| `curl_exec` | 83.0μs | 1960.5μs |
| curl starttransfer | 37.0μs | 1684.0μs |
| server handler | 2.4μs | 585.3μs |
| server payload allocation | 0.9μs | 563.6μs |
| body append total | - | 9.4μs |
| payload slice | - | 5.0μs |
| deserialize | - | 7.5μs |

この run では `curl starttransfer` p99 の一部は Go test-server 側の 100KB payload allocation tail で説明できる。ただし `starttransfer` p99 1684μs に対して server handler p99 585μs なので、残りは gRPC-Go marshal / HTTP/2 write、libcurl receive、または Docker scheduler の範囲に残る。

server payload allocation を外すため、`x-bench-server-cached-payload: 1` で test-server の事前生成 payload を返す診断も追加した。

```bash
BENCH_TAG=phase2-server-cached-20260429 ./bench/phase2/run.sh payload-unary-diagnostic --duration=3 --payload-sizes=102400 --warmup-calls=10
BENCH_TAG=phase2-server-cached-20260429 ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=3 --payload-sizes=102400 --warmup-calls=10
```

| metric | normal p99 | cached p99 |
|---|---:|---:|
| calls/sec | 5413.0 | 6877.7 |
| total unary latency | 2157.9μs | 1605.4μs |
| `curl_exec` | 2132.3μs | 1497.6μs |
| curl starttransfer | 1882.0μs | 1106.0μs |
| server handler | 827.2μs | 13.7μs |
| server payload allocation | 682.9μs | 2.2μs |
| body append total | 7.5μs | 9.5μs |
| deserialize | 6.5μs | 7.9μs |

cached payload では throughput が上がり、p99 tail も大きく下がる。100KB unary の悪化は client decode/copy ではなく、ベンチ server が毎回 100KB payload を allocate する条件に強く影響されていた。cached 条件でも `curl starttransfer` p99 は 1ms 程度残るため、残りは gRPC-Go marshal / HTTP/2 write / Docker scheduler / libcurl receive の複合として扱う。

さらに client 側で取れる範囲を分解するため、curl transfer stats と `total - starttransfer` を追加した。

```bash
BENCH_TAG=phase2-transfer-breakdown-20260429 ./bench/phase2/run.sh payload-unary-diagnostic --duration=3 --payload-sizes=102400 --warmup-calls=10
BENCH_TAG=phase2-transfer-breakdown-20260429 ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=3 --payload-sizes=102400 --warmup-calls=10
```

| metric | normal p99 | cached p99 |
|---|---:|---:|
| calls/sec | 5254.8 | 7200.8 |
| total unary latency | 2269.8μs | 1337.8μs |
| `curl_exec` | 2126.1μs | 1233.1μs |
| curl starttransfer | 1812.0μs | 1057.0μs |
| curl download after starttransfer | 440.0μs | 88.0μs |
| server handler | 793.3μs | 10.9μs |
| server payload allocation | 666.3μs | 1.9μs |
| downloaded bytes | 102409B | 102409B |
| num connects | 0 | 0 |
| body chunks | 13 | 13 |
| largest chunk | 16375B | 16375B |
| body append total | 12.2μs | 9.5μs |
| deserialize | 6.6μs | 8.0μs |

クライアント側で分解できる範囲では、connection reuse は維持され、100KB download 後半は cached 条件で p99 88μsまで下がる。残る cached p99 の大半は `starttransfer`、つまり first byte 到着前にある。server handler も小さいため、残りは gRPC-Go marshal / HTTP/2 write / Docker scheduler / libcurl が first byte を受けるまでの区間であり、PHP userland の改善対象ではない。

server 側をさらに分解するため、Go test-server に opt-in の grpc-go `stats.Handler` を追加した。`payload-unary-diagnostic*` は `x-bench-server-stats: 1` を送り、handler entry/exit、`InPayload`、`OutHeader`、`OutPayload`、response wire bytes を trailers 経由で保存する。`End` は trailer 送出後のため、この用途の metric には含めない。

```bash
BENCH_TAG=phase2-server-stats-20260429 ./bench/phase2/run.sh payload-unary-diagnostic --duration=3 --payload-sizes=102400 --warmup-calls=10
BENCH_TAG=phase2-server-stats-20260429 ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=3 --payload-sizes=102400 --warmup-calls=10
```

| metric | normal p99 | cached p99 |
|---|---:|---:|
| calls/sec | 4967.9 | 6450.1 |
| total unary latency | 2187.4μs | 1541.4μs |
| `curl_exec` | 2110.5μs | 1412.1μs |
| curl starttransfer | 1772.0μs | 926.0μs |
| curl download after starttransfer | 593.0μs | 131.0μs |
| server handler | 580.0μs | 5.6μs |
| server payload allocation | 546.5μs | 1.0μs |
| server stats handler end | 654.2μs | 8.2μs |
| server stats out header | 1343.3μs | 354.5μs |
| server stats out payload | 1343.5μs | 354.8μs |
| response wire bytes | 102409B | 102409B |

この追加分解で、normal 条件では payload allocation 後にも server 側の marshal / send path に p99 約690μsの tail が乗ることが見える。cached 条件では handler end が p99 8.2μs、`OutPayload` が p99 354.8μsなので、server 側 gRPC transport までで約350μs程度の tail が残る。client の `curl starttransfer` p99 926μsとの差分は、client request が server `Begin` に届くまでの時間、server `OutPayload` 後に実際の first byte が client に届くまでの HTTP/2 / Docker scheduling / libcurl receive の複合であり、PHP userland の body parse / protobuf decode ではない。

client 側の libcurl event を見るため、`payload-unary-diagnostic*` に少数 call 限定の debug trace 出力も追加した。これは latency 計測値を歪めるため、通常比較には使わず、event 順序の確認だけに使う。

```bash
BENCH_TAG=phase2-curl-trace-smoke-20260429 ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=0.2 --payload-sizes=102400 --warmup-calls=3 --curl-trace-output=var/bench-results/phase2-curl-trace-smoke-20260429.log --curl-trace-calls=2
```

cached 100KB の trace では、2 call とも `Re-using existing http: connection` が出ており、connection reuse は libcurl trace 上でも確認できる。代表 call では HTTP/2 stream open、request headers、9B request body upload、response headers、100KB response DATA、trailers の順序で到着している。

| event | call 2 elapsed |
|---|---:|
| reuse existing connection | 14.2μs |
| HTTP/2 stream open | 23.1μs |
| request headers out | 67.7μs |
| request body out | 77.0μs |
| upload complete | 80.1μs |
| response `HTTP/2 200` | 140.5μs |
| first response DATA | 155.4μs |
| trailers start | 228.6μs |
| connection kept alive | 287.4μs |

この trace は tail sample ではないが、残区間を読む上で重要な確認点を与える。first byte 前に余分な connection setup はなく、100KB body は複数の `DATA_IN` callback に分割される。server stats trailer には `OutPayload` も含まれており、client trace と server stats を同じ call のイベント列として読める。ただし libcurl debug callback 自体の overhead があるため、p99 の定量判断は通常の `payload-unary-diagnostic*` JSON を使う。

client 実装の効率化判断に使うため、cached payload の size sweep を取り直した。ここでは Go test-server 側の payload allocation を外し、php-grpc-lite が制御できる userland work と、libcurl 境界に残る work を分ける。1MB payload まで含めるため、test-server の cached payload に 1MB を追加し、runner は `--max-calls=1000` で sample 数を固定した。

```bash
BENCH_TAG=phase2-client-breakdown-20260429 ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=5 --max-calls=1000 --payload-sizes=0,100,1024,10240,102400,1048576 --warmup-calls=10
```

| payload | total p99 | curl exec p99 | starttransfer p99 | download after starttransfer p99 | server OutPayload p99 | body chunks p99 | max chunk p50 | connects p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0B | 404.9μs | 395.1μs | 345.0μs | 36.0μs | 17.6μs | 1 | 5B | 0 |
| 100B | 285.5μs | 272.9μs | 187.0μs | 33.0μs | 15.2μs | 1 | 107B | 0 |
| 1KB | 307.2μs | 296.8μs | 159.0μs | 37.0μs | 15.8μs | 1 | 1032B | 0 |
| 10KB | 341.4μs | 332.0μs | 305.0μs | 33.0μs | 24.2μs | 1 | 10248B | 0 |
| 100KB | 1175.8μs | 1162.1μs | 836.0μs | 521.0μs | 364.5μs | 13 | 16375B | 0 |
| 1MB | 3659.2μs | 3605.7μs | 2748.0μs | 1574.0μs | 2402.2μs | 130 | 16375B | 0 |

| payload | request serialize p99 | frame build p99 | init curl p99 | request header build p99 | curl setopt p99 | after-curl userland p99 | body append total p99 | payload slice p99 | protobuf deserialize p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0B | 0.3μs | 0.3μs | 0.4μs | 1.1μs | 2.4μs | 8.9μs | 0.3μs | 0.1μs | 2.1μs |
| 100B | 0.2μs | 0.2μs | 0.2μs | 0.7μs | 1.3μs | 5.2μs | 0.2μs | 0.1μs | 1.0μs |
| 1KB | 0.2μs | 0.2μs | 0.2μs | 0.7μs | 1.4μs | 6.6μs | 0.2μs | 0.1μs | 1.4μs |
| 10KB | 0.2μs | 0.2μs | 0.2μs | 0.6μs | 1.3μs | 8.3μs | 0.2μs | 0.5μs | 1.5μs |
| 100KB | 0.2μs | 0.2μs | 0.2μs | 0.6μs | 1.5μs | 11.5μs | 7.3μs | 2.6μs | 6.5μs |
| 1MB | 0.4μs | 0.3μs | 0.3μs | 1.0μs | 2.1μs | 55.7μs | 48.0μs | 24.2μs | 28.3μs |

この sweep では、クライアントが直接改善できる PHP userland work は 1MB でも p99 数十μsに収まる。payload size に比例するのは body append、payload slice、protobuf deserialize だが、1MB でも合計は概ね 100μs未満で、total p99 3.66ms の主因ではない。100KB / 1MB の tail は `curl_exec` 内、特に first byte 前と download 後半に残る。connection reuse は全 payload で `num_connects=0` なので、次の効率化候補は payload copy の C 化ではなく、metadata path の局所最適化、body buffer のコピー削減が小さい改善として成立するか、または libcurl / HTTP/2 境界を変更する大きな設計判断になる。

同じ server stats を ext-grpc 側にも送れるようにし、1MB cached payload で比較した。ext-grpc では php-grpc-lite 固有の curl / userland diagnostics は取れないが、total latency と server `OutPayload` は同じ trailers から比較できる。

```bash
BENCH_TAG=phase2-ext-client-boundary-20260429 BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=5 --max-calls=1000 --payload-sizes=1048576 --warmup-calls=10
BENCH_TAG=phase2-ext-client-boundary-20260429 BENCH_IMPLEMENTATION=ext-grpc ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=5 --max-calls=1000 --payload-sizes=1048576 --warmup-calls=10
```

| metric | php-grpc-lite | ext-grpc |
|---|---:|---:|
| calls/sec | 1261.8 | 1760.1 |
| total p50 | 590.3μs | 401.2μs |
| total p99 | 3907.0μs | 2488.1μs |
| server handler end p99 | 44.5μs | 32.6μs |
| server OutPayload p99 | 2282.1μs | 2047.9μs |
| server payload allocation p99 | 2.1μs | 1.8μs |
| response wire bytes p50 | 1048585B | 1048585B |

この比較では、server 側の cached payload / `OutPayload` は両実装で近く、total p99 の差は約1.42msある。p99 同士の差分なので同一 request の厳密な内訳ではないが、1MB payload では server-side だけで php-grpc-lite の遅さを説明するのは弱い。ext-grpc が同じ server / 同じ payload で低い total p99 を出しているため、差分は client stack、つまり libcurl/nghttp2 receive、PHP callback 境界、buffering、または ext-grpc C-core の受信経路との差として扱うのが妥当。

client 側で制御できる小さい改善として、unary response body の受信を `body .= $chunk` から chunk list + parse 前 `implode()` に変更して試した。これにより write callback 内の累積コピーを避け、parse 前に一度だけ連結する。

```bash
BENCH_TAG=phase2-chunk-list-20260429 BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=5 --max-calls=2000 --payload-sizes=102400,1048576 --warmup-calls=10
BENCH_TAG=phase2-chunk-list-1mb-1000-20260429 BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=5 --max-calls=1000 --payload-sizes=1048576 --warmup-calls=10
```

| payload | implementation | total p50 | total p99 | curl exec p50 | curl exec p99 | body append p50 | body append p99 | assemble p50 | assemble p99 | after-curl userland p50 | after-curl userland p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100KB | append string | 111.6μs | 1606.5μs | 99.0μs | 1530.4μs | 3.7μs | 7.8μs | - | - | 5.8μs | 12.9μs |
| 100KB | chunk list | 112.1μs | 1597.4μs | 96.7μs | 1560.7μs | 1.1μs | 2.1μs | 2.1μs | 8.1μs | 7.8μs | 20.1μs |
| 1MB | append string | 548.4μs | 3659.2μs | 501.3μs | 3605.7μs | 30.6μs | 48.0μs | - | - | 38.0μs | 55.7μs |
| 1MB | chunk list | 540.1μs | 3541.1μs | 471.1μs | 3476.1μs | 9.3μs | 13.4μs | 19.9μs | 33.1μs | 57.6μs | 86.7μs |

100KB では total はほぼ横ばいで、callback 内 append は軽くなるが `implode()` 分が後段に移るだけに近い。1MB では body append p99 が 48.0μs → 13.4μs に下がり、assemble p99 33.1μs を足しても同程度からやや改善、total p99 も 3659.2μs → 3541.1μs と小さく下がった。改善幅は限定的だが、低リスクで payload が大きい場合の累積コピーを避けられるため、採用する価値はある。

同じ観点で server streaming の receive buffer も確認した。`bufferOffset` が buffer 全体を消費した場合は `substr($buffer, $bufferOffset)` せず空文字へ戻すことで、全消費済み buffer の不要コピーを避ける。metadata header parse の `explode` → `strpos/substr` 化も試したが、metadata-heavy 診断で改善が見えなかったため採用しない。

```bash
BENCH_TAG=phase2-stream-buffer-20260429 ./bench/phase2/run.sh throughput-streaming --duration=2 --message-count=1000 --payload-bytes=100 --warmup-streams=2
```

| scenario | msg/s | p50 stream | p99 stream |
|---|---:|---:|---:|
| throughput-streaming 1000x100B | 705312.9 | 1300.9μs | 3179.7μs |

## Large request / small response

Spanner insert / commit のような upload 側を確認するため、`BenchRequest.request_payload` を追加し、response payload を 0B に固定した `request-unary-diagnostic` を追加した。server は request payload length を読むため、bytes は protobuf decode 済み request の一部として扱われる。

```bash
BENCH_TAG=phase2-large-request-20260429 BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh request-unary-diagnostic --duration=5 --max-calls=1000 --request-payload-sizes=102400,1048576 --warmup-calls=10
BENCH_TAG=phase2-large-request-20260429 BENCH_IMPLEMENTATION=ext-grpc ./bench/phase2/run.sh request-unary-diagnostic --duration=5 --max-calls=1000 --request-payload-sizes=102400,1048576 --warmup-calls=10
```

| request payload | metric | php-grpc-lite | ext-grpc |
|---:|---|---:|---:|
| 100KB | calls/sec | 4254.2 | 4351.0 |
| 100KB | total p50 | 126.3μs | 129.7μs |
| 100KB | total p99 | 2151.1μs | 1505.1μs |
| 100KB | server InPayload p50 | 22.6μs | 17.7μs |
| 100KB | server InPayload p99 | 1317.6μs | 1141.2μs |
| 1MB | calls/sec | 833.7 | 1387.0 |
| 1MB | total p50 | 912.7μs | 456.8μs |
| 1MB | total p99 | 4318.7μs | 3073.9μs |
| 1MB | server InPayload p50 | 529.5μs | 157.7μs |
| 1MB | server InPayload p99 | 3844.0μs | 2637.3μs |

php-grpc-lite 固有の client diagnostics では、1MB request の request serialize p50/p99 が 73.2μs / 700.6μs、frame build p50/p99 が 58.8μs / 195.6μs、curl upload bytes は 1048585B だった。100KB は p50 では ext-grpc とほぼ同等だが p99 は php-grpc-lite が重い。1MB では p50/p99 と throughput の差が明確で、server `InPayload` に到達する時刻からも client upload path の差が見える。large request は Spanner insert/commit に近い軸なので、1MB 級 request を重視するなら response payload とは別に client upload path の最適化判断が必要。

PHP userland で frame string の追加コピーを避けるため、`CURLOPT_POSTFIELDS` を使わず `CURLOPT_READFUNCTION` で 5B gRPC header と serialized protobuf body を分割 upload する案も試した。PHP/curl には `CURLOPT_POSTFIELDSIZE` が無かったため、POST のまま `CURLOPT_INFILESIZE` と read callback を使った。

```bash
BENCH_TAG=phase2-upload-callback-20260429 BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh request-unary-diagnostic --duration=5 --max-calls=1000 --request-payload-sizes=102400,1048576 --warmup-calls=10
BENCH_TAG=phase2-upload-callback-repeat-20260429 BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh request-unary-diagnostic --duration=5 --max-calls=1000 --request-payload-sizes=1048576 --warmup-calls=10
```

| request payload | implementation | calls/sec | total p50 | total p99 | frame build p99 | read callback p99 | curl exec p99 | server InPayload p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 100KB | `POSTFIELDS` | 4254.2 | 126.3μs | 2151.1μs | 7.5μs | - | 2132.1μs | 1317.6μs |
| 100KB | read callback | 3676.1 | 131.4μs | 2578.4μs | 0.4μs | 9.1μs | 2558.7μs | 2096.0μs |
| 1MB | `POSTFIELDS` | 833.7 | 912.7μs | 4318.7μs | 195.6μs | - | 4118.0μs | 3844.0μs |
| 1MB | read callback | 976.3 | 787.5μs | 4436.0μs | 0.4μs | 80.0μs | 4386.2μs | 3957.8μs |
| 1MB | read callback repeat | 964.0 | 738.0μs | 4530.7μs | - | - | - | - |

read callback 化は frame build の大きい string copy を消し、1MB の p50 / throughput は改善した。一方で 100KB は悪化し、1MB も p99 が再現して悪化した。tail latency を重視するため、この案は採用しない。PHP userland だけで upload path を大きく改善する余地は限定的で、large request の p99 改善には libcurl/nghttp2 upload path か ext-grpc C-core 相当の送信経路差を扱う必要がある。

## RTT

| scenario | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 |
|---|---:|---:|---:|---:|
| warm direct | 39.5μs | 87.0μs | 66.2μs | 274.1μs |
| cold direct | 205.7μs | 754.5μs | 72.6μs | 582.1μs |
| warm 1ms | 1978.2μs | 2196.4μs | 1954.4μs | 2977.2μs |
| cold 1ms | 2007.8μs | 3965.0μs | 1968.6μs | 2083.9μs |
| warm 3ms | 4782.1μs | 5331.8μs | 4693.9μs | 5615.4μs |
| cold 3ms | 6225.2μs | 8793.5μs | 4939.2μs | 5650.9μs |
| warm 5ms | 6934.6μs | 7978.3μs | 7082.4μs | 7844.1μs |
| cold 5ms | 7824.2μs | 10505.7μs | 7011.7μs | 7612.2μs |

direct warm は php-grpc-lite が軽い。cold と RTT ありでは php-grpc-lite の p99 が悪化しやすく、connection reuse / cold path の観測価値が高い。

追加で `rtt-unary-diagnostic` を実行し、warm / cold の curl timing を確認した。

```bash
BENCH_TAG=phase2-rtt-diagnostic-20260429 ./bench/phase2/run.sh rtt-unary-diagnostic --calls=10 --warmup-calls=2
```

| scenario | latency p99 | connect p99 | pretransfer p99 | starttransfer p99 | curl total p99 |
|---|---:|---:|---:|---:|---:|
| warm direct | 127.0μs | 0.0μs | 83.0μs | 91.0μs | 107.0μs |
| cold direct | 405.1μs | 254.0μs | 348.0μs | 355.0μs | 373.0μs |
| warm 1ms | 2236.4μs | 0.0μs | 92.0μs | 2136.0μs | 2198.0μs |
| cold 1ms | 2576.6μs | 476.0μs | 578.0μs | 2505.0μs | 2542.0μs |

warm direct / warm 1ms では `connect` が 0 で、Channel 内の handle / connection reuse は効いている。cold は new Channel / new curl handle のため connect + pretransfer が乗る。したがって次の焦点は「reuse を実装する」ではなく、php-fpm request 境界で cold をどれだけ受け入れるか、または純 PHP で request 内 reuse をどの API surface で自然に促すか。

## Streaming

| suite | php-grpc-lite | ext-grpc | 観察 |
|---|---:|---:|---|
| `throughput-streaming` | 725744.2 msg/s | 383759.0 msg/s | decision 条件では php-grpc-lite が高 throughput |
| `large-streaming 10K` | 790147.2 msg/s | 344685.2 msg/s | php-grpc-lite 優位 |
| `large-streaming 100K` | 1366440.5 msg/s | 425908.1 msg/s | php-grpc-lite 優位 |
| `payload-streaming 10KB` | 72076.9 msg/s | 76898.9 msg/s | 10KB streaming は ext-grpc が僅かに優位 |

streaming は全体として php-grpc-lite が強い。payload が大きい streaming は差が縮む。

## Metadata

| scenario | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 |
|---|---:|---:|---:|---:|
| req 0 / resp 0 | 44.5μs | 135.5μs | 61.2μs | 144.3μs |
| req 50 / resp 0 | 118.0μs | 464.9μs | 94.7μs | 465.4μs |
| req 50 / resp 50 | 287.1μs | 736.3μs | 227.7μs | 667.6μs |

metadata が多いケースでは ext-grpc の p50 が優位。header parse / metadata append は将来の改善候補だが、100KB payload tail より優先度は低い。

追加で `metadata-header-diagnostic` を実行し、request header build と response header callback の内訳を確認した。

```bash
BENCH_TAG=phase2-metadata-diagnostic-20260429 ./bench/phase2/run.sh metadata-header-diagnostic --calls=100
```

| scenario | latency p50 | latency p99 | request header build p50 | request header build p99 | header callback p50 | header callback p99 | header lines |
|---|---:|---:|---:|---:|---:|---:|---:|
| req 0 / resp 0 | 47.0μs | 151.3μs | 0.3μs | 2.4μs | 1.4μs | 8.0μs | 5 |
| req 50 / resp 0 | 120.7μs | 1230.9μs | 3.3μs | 10.7μs | 1.2μs | 2.3μs | 5 |
| req 50 / resp 50 | 277.2μs | 809.3μs | 3.5μs | 5.4μs | 26.4μs | 37.8μs | 105 |

request metadata 50 keys の header build は p99 でも 10μs 程度で、php-grpc-lite 側の request header construction は主因ではない。request metadata が多い時の tail は `curl starttransfer` 側に出ており、server / gRPC-Go 側の request metadata 処理や scheduler の影響を含む。

response metadata 50 initial + 50 trailing では header callback が p50 26.4μs / p99 37.8μs まで増える。全体 p50 277.2μsに対して支配的ではないが、php-grpc-lite 内で明確に増える固定費としてはここが見える。metadata path を改善するなら response header parse / metadata append が候補になる。

## 判断

| 対象 | 判断 |
|---|---|
| curl handle / connection reuse | Channel 内 reuse は効いている。cold / RTT は request 境界のコストとして扱う |
| payload decode / copy | 現状の主犯ではない。C 化候補としての優先度は下げる |
| Go test-server payload allocation | 100KB tail の大きな部分を説明する。client 実装改善対象ではなく、ベンチ解釈上の注意点 |
| 100KB transfer after first byte | cached 条件では p99 88μs。PHP body append / decode も小さく、主犯ではない |
| streaming hot path | 現状は大きな弱点ではない。改善より回帰監視を優先 |
| metadata path | response metadata で header callback 固定費が見える。改善候補は response header parse / metadata append |

次は通常の `payload-unary` と diagnostic の解釈を分ける。client 側の改善候補としては 100KB unary tail より、response metadata parse / append の局所改善、または php-fpm request 境界での cold コストの説明を優先する。
