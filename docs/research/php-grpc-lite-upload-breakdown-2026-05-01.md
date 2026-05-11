# php-grpc-lite upload breakdown 2026-05-01

large request / small response で、現行 `php-grpc-lite` が upload で詰まっているのか、`libcurl` / `nghttp2` 内部またはserver到達後で詰まっているのかを分けるため、unary upload path の診断を追加した。

## 追加した計測

| 指標 | 意味 |
|---|---|
| `curl_posttransfer_time_ns` | `curl_exec()` 開始から request body upload 完了まで。`CURLINFO_POSTTRANSFER_TIME_T` 由来 |
| `curl_wait_after_upload_before_first_byte_ns` | upload完了から first response byte まで |
| `curl_after_upload_until_done_ns` | upload完了から `curl_exec()` 完了まで |
| `upload_read_callback_*` | opt-in `CURLOPT_READFUNCTION` 経路で、libcurl がPHP callbackからrequest bodyを読む回数、bytes、完了時刻 |
| curl debug trace | 少数call限定で `DATA_OUT` と response header / DATA arrival の順序を見る |

通常実装は引き続き `CURLOPT_POSTFIELDS`。`CURLOPT_READFUNCTION` は分解用の diagnostic で、通常実装への採用判断ではない。

## 実行条件

| 項目 | 内容 |
|---|---|
| 実行日 | 2026-05-01 |
| 対向 | Go test-server `:50051` h2c |
| RPC | `/helloworld.Greeter/BenchUnary` |
| response | 0B payload / gRPC frame 5B |
| server | `docker compose up -d --force-recreate test-server` 後に同一containerで連続測定 |
| 通常経路 | `BENCH_TAG=upload-breakdown-postfields-20260501 ./bench/run.sh request-unary-diagnostic --duration=30 --max-calls=1000 --request-payload-sizes=102400,524288,1048576,2097152 --warmup-calls=10` |
| read callback | 上記に `--upload-read-callback` を追加 |

## POSTFIELDS 経路

現行実装の `CURLOPT_POSTFIELDS` 経路。`posttransfer` は libcurl がrequest bodyを送り終えた時刻、`starttransfer` は response first byte 到達時刻。

| request | calls/s | p50 | p99 | serialize p99 | frame build p99 | posttransfer p99 | starttransfer p99 | upload→first byte p99 | server InPayload p99 | server OutHeader p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100KB | 4215 | 123μs | 2111μs | 10μs | 3μs | 163μs | 2055μs | 2025μs | 1695μs | 1751μs |
| 512KB | 1596 | 369μs | 3596μs | 29μs | 14μs | 1026μs | 3521μs | 3039μs | 3015μs | 3155μs |
| 1MB | 868 | 898μs | 4494μs | 920μs | 196μs | 1418μs | 4232μs | 3783μs | 3943μs | 3950μs |
| 2MB | 534 | 1568μs | 5384μs | 433μs | 277μs | 3363μs | 4969μs | 4308μs | 4697μs | 4704μs |

p50ではpayload sizeに応じて `posttransfer` が増え、upload自体のcostが見える。1MB p50は total 898μs のうち `posttransfer` 299μs、serialize + frame build 126μs、server InPayload 509μs。

p99では `posttransfer` も伸びるが、total p99は `starttransfer` / server InPayload に近い。特に 1MB は posttransfer p99 1418μsに対して starttransfer p99 4232μs、server InPayload p99 3943μsで、tailの大部分は upload完了後からfirst byte / server InPayload側にある。ただし p99同士は同一callとは限らないため、差分は区間の上限目安として読む。

## READFUNCTION 診断経路

`CURLOPT_READFUNCTION` 経路では、PHP側の `5B header . payload` full concat を避け、libcurlがPHP callbackから最大 64KB 単位でrequest bodyを読む。これは採用判断ではなく、POSTFIELDS の frame copy と libcurl upload path を分けるための観測線。

| request | calls/s | p50 | p99 | serialize p99 | frame build p99 | posttransfer p99 | starttransfer p99 | upload→first byte p99 | server InPayload p99 | server OutHeader p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100KB | 3808 | 131μs | 3047μs | 9μs | 0μs | 347μs | 3011μs | 2669μs | 2303μs | 2308μs |
| 512KB | 1566 | 388μs | 3685μs | 29μs | 0μs | 1244μs | 3590μs | 3188μs | 2987μs | 2992μs |
| 1MB | 947 | 775μs | 3731μs | 311μs | 0μs | 2494μs | 3512μs | 3037μs | 3276μs | 3282μs |
| 2MB | 551 | 1538μs | 5221μs | 238μs | 0μs | 3641μs | 5038μs | 3992μs | 4845μs | 4852μs |

| request | read callback calls p50 | callback CPU p50 | callback CPU p99 | upload read complete p50 | upload read complete p99 | max callback bytes p50 |
|---:|---:|---:|---:|---:|---:|---:|
| 100KB | 3 | 3μs | 8μs | 29μs | 225μs | 65322B |
| 512KB | 10 | 13μs | 88μs | 147μs | 1240μs | 65536B |
| 1MB | 18 | 24μs | 179μs | 308μs | 2452μs | 65536B |
| 2MB | 34 | 52μs | 135μs | 727μs | 3625μs | 65536B |

read callback自体のPHP CPUは小さい。1MB p50で callback合計 24μs、p99 179μs。したがって READFUNCTION 経路の主costはPHP callback処理ではなく、libcurl/nghttp2 がcallbackで得たbytesをHTTP/2 DATAとして送る区間、またはその後のserver到達側にある。

READFUNCTION は 1MB p50 / p99 では今回のrunで POSTFIELDS より良いが、100KBでは悪化した。過去runでもp99安定性に課題があったため、現時点では通常実装へ採用しない。

## curl debug trace

1MBの少数call traceでは、libcurl は 64KB前後の `DATA_OUT` を複数回出す。POSTFIELDSでもREADFUNCTIONでも、HTTP/2 DATA_OUTは最終的に同じようなchunk列になる。

代表例:

| mode | observation |
|---|---|
| POSTFIELDS | warm callで `DATA_OUT` が約65KB単位で並び、`upload completely sent off: 1048585 bytes` が response header の直前に出る |
| READFUNCTION | 同じく約65KB単位で `DATA_OUT`。一部callではresponse到達により `abort upload after having sent 1048585 bytes` と表示される |

traceは `CURLOPT_DEBUGFUNCTION` のoverheadが大きいため、p50/p99の定量判断には使わない。event順序とchunking確認だけに使う。

## 判断

- 現行 `POSTFIELDS` 経路では、1MB以上のp50に upload cost が明確にある。1MB p50で `posttransfer` は299μs、2MB p50で672μs。
- ただし p99 の主成分は upload完了前だけではない。1MB POSTFIELDS p99では `posttransfer` 1418μsに対して `starttransfer` 4232μs、server InPayload 3943μsで、upload完了後〜server InPayload / first byte側が大きい。
- `request_frame_build_ns` は large request で実costだが、単独の主因ではない。1MB p50で55μs、p99で196μs、2MB p50で122μs、p99で277μs。
- `serializeToString()` も無視はできないが、ext-grpcでも同じprotobuf serializeは通るため、transport差の主因としては frame build / libcurl upload / HTTP/2送信側を見る。
- READFUNCTION は frame concatを消すが、libcurl/nghttp2 upload pathは残る。callback CPUは小さいため、改善・悪化の差はPHP callbackそのものではなく、libcurlのupload駆動、HTTP/2 DATA送信、server到達側の揺れに出る。
- PHP/libcurl公開APIで分解できる境界は、`posttransfer`、`starttransfer`、server InPayload、curl trace DATA_OUT まで。これ以上、libcurl内のnghttp2 flow-control、socket write、WINDOW_UPDATE待ちを定量分解するには、libcurl/nghttp2をinstrumentしたビルド、またはC拡張/HTTP/2 transport側の計測が必要。

## 次の打ち手

1. php-grpc-lite本体の改善候補としては、large request の `frame = header . payload` を避ける方向は有効。ただしREADFUNCTION単純採用ではなく、p50/p99/throughputの安定性を同一server条件で再確認する。
2. p99の原因追跡は、php-grpc-lite単体ではなく ext-grpc / nghttp2 PoC と同じserver条件で比較する。p99だけを単独目標にせず、まず1MB以上のp50とthroughputでclient upload path改善を評価する。
3. これ以上 `libcurl/nghttp2内部` を掘るなら、PHP userlandではなく `_research` のlibcurl/nghttp2 source readingか、instrumented libcurl build / HTTP/2 transport PoCで見る。
