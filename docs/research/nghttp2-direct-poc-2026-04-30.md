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

最後に、client内の upload 完了時刻と server stats trailer を同一 call の結果として返すようにした。client時刻は call start からの相対μs、server stats は Go `stats.Handler` の `TagRPC` からの相対nsであり、時計同期した絶対比較ではない。同一 call の中で、client upload が終わる前に遅いのか、serverがrequestを受けるまでに遅いのか、response headerまでに遅いのかを読むためのもの。

| case | implementation | iterations | calls/s | total p50 | total p99 | client upload done p99 | client response header p99 | server InPayload p99 | server OutHeader p99 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| request 1MB / response 100B | split + copy + server stats | 1000 | 1374.8 | 429μs | 3494μs | 745μs | 3491μs | 3257μs | 3264μs |
| request 1MB / response 100B | split + no-copy + server stats | 1000 | 1510.3 | 375μs | 3187μs | 573μs | 3186μs | 3004μs | 3010μs |

同一 call sample でも、遅い call は `client_upload_complete_us` より `server_stats_in_payload_ns` / `client_response_header_us` 側が大きい。つまり PoC の no-copy 後に残る p99 は、client が最後の DATA を socket に書き終えるまでというより、server が request payload を受け終えるまで、またはその後 response header が返るまでの区間に寄っている。

この傾向が payload size に対して安定しているかを見るため、`split + no-copy + server stats` で 100KB / 512KB / 1MB / 2MB を各 3 run 測った。各 run は 1000 calls。表は中央値で、p99 range は 3 run の最小〜最大。

| request | calls/s median | p50 median | p99 median | p99 range | upload done p99 median | response header p99 median | server InPayload p99 median | WINDOW_UPDATE median | DATA frames median | min remote window |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100KB | 5577.1 | 61μs | 1920μs | 1829-2139μs | 66μs | 1916μs | 1389μs | 3029 | 7000 | 12274 |
| 512KB | 2100.7 | 185μs | 3449μs | 3040-3556μs | 231μs | 3447μs | 2756μs | 3048 | 33001 | 30 |
| 1MB | 1433.7 | 352μs | 4176μs | 4044-4743μs | 592μs | 4175μs | 3767μs | 3041 | 65001 | 16342 |
| 2MB | 966.2 | 759μs | 4269μs | 4241-4711μs | 1709μs | 4267μs | 4033μs | 5311 | 129056 | 2 |

100KB でも total p99 は upload done p99 より response header / server InPayload p99 に寄る。512KB 以上では upload done p99 も増えるが、total p99 の主成分は引き続き response header / server InPayload 側にある。2MB では upload done p99 が 1.7ms まで伸びるため client送信側の比率も上がるが、それでも response header p99 4.27msとの差が残る。

同じ Go test-server / server stats 条件で ext-grpc も 100KB / 512KB / 1MB / 2MB を各 3 run 測った。実行入口は `bench/phase2/run.sh request-unary-diagnostic`、各 run は warmup 10 calls 後に最大 1000 calls。

| request | calls/s median | p50 median | p99 median | p99 range | server InPayload p99 median | server OutHeader p99 median |
|---:|---:|---:|---:|---:|---:|---:|
| 100KB | 4379.4 | 125μs | 2043μs | 1504-2087μs | 1182μs | 1204μs |
| 512KB | 2088.1 | 246μs | 2908μs | 2734-2937μs | 2490μs | 2504μs |
| 1MB | 1455.7 | 436μs | 3433μs | 2806-3886μs | 3165μs | 3175μs |
| 2MB | 907.9 | 908μs | 3602μs | 3158-3783μs | 3111μs | 3117μs |

PoC の request size sweep と並べると、512KB 以上では ext-grpc の server InPayload p99 が PoC より低い。つまり PoC / ext-grpc の p99 差は、server が request payload を受け終わった後だけで発生しているわけではなく、server InPayload までの upload / wire / flow-control / scheduler を含む区間にも既に出ている。

| request | PoC p99 | ext-grpc p99 | PoC server InPayload p99 | ext-grpc server InPayload p99 | PoC response header p99 | ext-grpc server OutHeader p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 100KB | 1920μs | 2043μs | 1389μs | 1182μs | 1916μs | 1204μs |
| 512KB | 3449μs | 2908μs | 2756μs | 2490μs | 3447μs | 2504μs |
| 1MB | 4176μs | 3433μs | 3767μs | 3165μs | 4175μs | 3175μs |
| 2MB | 4269μs | 3602μs | 4033μs | 3111μs | 4267μs | 3117μs |

ただし `client_upload_complete_us` は client が最後の DATA を socket buffer に書き終えた時刻であり、server が受信完了した時刻ではない。PoC の upload done p99 が小さくても server InPayload p99 が大きいのは矛盾ではなく、kernel buffer 以降の送出、HTTP/2 flow control、server側受信処理、Docker scheduler を含む区間が残っていることを示す。

flow-control / scheduler 側をさらに見るため、PoC に call 単位の `WINDOW_UPDATE`、remote window、flow-control pause、write syscall 最大時間、tail sample を追加した。再実行用の入口は `bench/phase2/nghttp2-poc-flow-sweep.sh`。下表は `split + no-copy + server stats`、各 1000 calls の単発 run。

| request | p50 | p99 | upload p99 | response header p99 | server InPayload p99 | first WINDOW_UPDATE p99 | last WINDOW_UPDATE p99 | WINDOW_UPDATE p99 | flow pause p99 | max write syscall p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100KB | 65μs | 1986μs | 80μs | 1880μs | 1628μs | 1182μs | 1590μs | 3 | 0 | 35μs |
| 512KB | 192μs | 3636μs | 389μs | 3551μs | 3367μs | 1643μs | 3427μs | 4 | 0 | 179μs |
| 1MB | 372μs | 4046μs | 1355μs | 4044μs | 3617μs | 1658μs | 3081μs | 8 | 0 | 778μs |
| 2MB | 740μs | 4638μs | 1517μs | 4636μs | 4174μs | 1979μs | 4129μs | 6 | 0 | 994μs |

この run では `nghttp2_data_source_read_length_callback()` が `allowed == 0` で返す明示的な flow-control pause は p99 でも 0 だった。一方で、tail sample では `client_upload_complete_us` が数十〜数百μsで終わっている call でも、`server_stats_in_payload_ns` または `server_stats_out_header_ns` が数msまで伸びる。例えば 1MB request の最遅 call は upload complete 95μs、last WINDOW_UPDATE 222μs、server InPayload 6153μs、response header 6440μsだった。2MB request では InPayload 1675μsに対して OutHeader 6818μsまで伸びる sample もあり、server handler後のGo runtime / scheduler / transport write側のtailも混ざる。

したがって、現時点で見えている問題を「client側でremote windowが0になり、DATA readがPAUSEしている」とは言い切れない。より正確には、kernel buffer へ早く書き終わった後、server側HTTP/2 read loopがrequest全体をgRPC payloadとして引き上げるまで、またはhandler後にresponse headerを返すまでの区間でtailが出ている。HTTP/2 flow-controlは関与しているが、nghttp2 client API上の単純なwindow枯渇待ちではなく、gRPC-Go transport、Docker network、OS scheduler、Go scheduler を含む境界として扱うべき。

## 観察

- direct nghttp2 は 100KB response の p50 で libcurl 通常経路より明確に低い。直近の libcurl 通常経路は 100KB response p50 120.1μs / p99 1976.8μs だったため、transport 境界に改善余地がある。
- 1MB response は p50 533μs / p99 3909μs で、直近の libcurl 通常経路 582.8μs / 4289.3μs より少し良い程度。large response の主因は transport API だけではなく、memory copy / server pacing / HTTP/2 flow control も混ざる。
- 1MB request / small response でも p50 435μs / p99 3985μs なので、upload 側でも direct nghttp2 は観測線として有効。
- C 側 split gRPC frame は 1MB request で throughput 1370.2 → 1461.2 calls/s、p50 379 → 372μs、p99 3866 → 3772μs と小さく改善した。PHP 側の `5B header . payload` 連結を外す効果はあるが、p99 の主因を単独で消すほどではない。
- no-copy 送信は 1MB request で throughput 1390.2 → 1499.3 calls/s、p50 378 → 342μs、p99 3779 → 3623μs に改善した。nghttp2 の内部 DATA buffer copy を避ける効果はある。
- 8KB DATA cap は DATA frame / syscall 数をほぼ倍増させ、throughput と latency を悪化させた。peer `remote_max_frame_size` は 16KB なので、PoC側で小さく切る意味はない。
- min remote connection window は 2B まで落ちており、1MB upload は connection-level flow control を踏んでいる。no-copy でも p99 が ext-grpc に届かない残差は、copyよりも flow-control / send-recv loop / scheduler 側に残る可能性が高い。
- poll loop は copy 経路では p50 がほぼ同等、throughput が少し上がった一方で p99 は 4162μs に悪化した。`recv wouldblock` と `poll calls` が増えており、この単純な poll 駆動は tail 改善にならない。
- 同一 call 時刻対応では、1MB no-copy の client upload done p99 は 573μs、client response header p99 は 3186μs、server InPayload p99 は 3004μsだった。残る tail は client内のpayload copyや最後のDATA write完了前ではなく、wire / server受信 / flow-control / schedulerを含む区間に寄っている。
- request size sweepでも同じ傾向だった。100KB〜2MBの全サイズで、total p99はclient upload done p99ではなくresponse header / server InPayload p99に近い。2MBではclient upload done p99も大きくなるが、まだ全体tailの主因ではない。
- ext-grpc の server stats size sweep でも、p99 は server InPayload / OutHeader に寄る。PoCとの差は512KB以上で server InPayload までに既に出ており、response parse や post-handler response path だけを詰めても large request p99 差は消えない。
- flow-control追加指標では、明示的な `allowed == 0` pause は観測されなかった。tail sampleは `WINDOW_UPDATE` 受信時刻、server InPayload、server OutHeader のいずれかに寄るため、client側nghttp2の単純なPAUSEではなく、server受信、gRPC-Go transport、Docker/OS/Go schedulerの複合tailとして追う。
- ただしこの PoC は互換実装ではない。metadata / trailer / error semantics / deadline / TLS を満たすにはかなり追加実装が必要。

## 判断

- C extension PoC は transport decision の材料として有効。特に small / medium unary では libcurl + PHP ext/curl の固定費を外す余地が見える。
- large request は C 側で gRPC frame を分割送信する余地がある。改善幅は限定的だが、現行 libcurl の `POSTFIELDS` copy / PHP frame concat を避ける方向性としては筋が良い。
- large request upload は no-copy DATA 送信まで入れると ext-grpc の throughput / p50 に到達する。ただし p99 は残る。
- copy 経路の単純な nonblocking poll loop は p99 を改善しなかった。次に tail を追うなら、no-copy 経路でpartial writeを安全に扱う状態機械を作るか、client側だけでなく server stats と同一 call 対応した upload完了時刻を取る必要がある。
- 同一 call 時刻対応後の優先度は、client側copy削減よりも、server InPayload 到達までの tail をpayload size sweep / repeated runで確認すること。ここが安定して残るなら、client C拡張だけでext-grpc p99へ完全に寄せるのは難しく、server側gRPC-Go / Docker scheduler / HTTP/2 flow-controlの影響として扱う。
- ext-grpc の server stats size sweep と比較すると、large request p99差は「server InPayload後〜client response header」だけではなく「server InPayloadまで」にも出る。no-copyでp50/throughputは十分詰まっているため、次に詰めるなら client側copy削減より、送信駆動、HTTP/2 flow-control、server受信完了までのtailを分ける必要がある。
- flow-control / scheduler 追加計測後の判断として、まずは client C拡張内の `allowed == 0` pause を主因にしない。次に進むなら、test-server の `TEST_SERVER_GOMAXPROCS`、`TEST_SERVER_GRPC_INITIAL_WINDOW_SIZE`、`TEST_SERVER_GRPC_INITIAL_CONN_WINDOW_SIZE` を振って、server側windowとGo scheduler条件で p99 / server InPayload / OutHeader がどう動くかを見る。
- ただし現時点で php-grpc-lite 本体を C extension 前提に切り替える判断材料としては不足。drop-in replacement の価値は互換性が主で、C transport は別フェーズの候補に留める。
- 次に進めるなら、C extension を本体化する前に `php-grpc-lite` の PHP userland 側で削れる固定費を先に潰し、その後に「native transport を入れるならどの surface を C に落とすか」を決める。

## 候補一覧と期待値

PoC で見た候補、まだ見ていない候補、期待できる改善を分ける。

### 実施済み

| 候補 | 狙い | 期待した効果 | 結果 | 判断 |
|---|---|---|---|---|
| client initial SETTINGS 明示送信 | h2c prior knowledge として正しく Go gRPC server に接続する | HTTP response 前 close の解消 | `nghttp2_submit_settings()` で unary が通るようになった | 必須 |
| `TCP_NODELAY` | small sequential unary の Nagle / delayed ACK を避ける | 小さい RPC の数十ms tail を消す | 10 calls が約406msから約0.8msに改善 | 必須 |
| C側 split gRPC frame | PHP 側の `5B header . payload` large concat を避ける | large request p50 / throughput 改善 | 1MB request で throughput 1370.2 → 1461.2 calls/s、p99 3866 → 3772μs | 有効だが決定打ではない |
| DATA no-copy + `writev()` | nghttp2 内部 DATA buffer への payload copy を避ける | large request p50 / throughput と一部 p99 改善 | 1MB request で throughput 1390.2 → 1499.3 calls/s、p99 3779 → 3623μs | large request では有効 |
| DATA frame size cap 8KB | 小さい DATA frame で flow-control / scheduler tail が減るか確認 | p99 改善の可能性 | DATA frame / syscall が倍増し、throughput 1255.0 calls/s、p99 3979μsに悪化 | 不採用 |
| copy経路の nonblocking `poll()` loop | blocking `send()` / `recv()` の詰まりを避ける | p99改善、flow-control待ちの平準化 | throughput は少し上がるが p99 4162μsに悪化 | 単純実装は不採用 |

### 未実施または追加検証候補

| 候補 | 期待できること | 見るべき指標 | リスク / 注意 |
|---|---|---|---|
| no-copy 経路のpartial write対応状態機械 | no-copy のcopy削減を維持しつつ、nonblockingでsocket backpressureを安全に扱う | p99、send wouldblock、partial write回数、DATA frame破損なし | `send_data_callback` は complete DATA frame 送信契約。途中まで書いた後の再開状態を自前で持つ必要があり、PoCとしては重い |
| client/server同一call時刻対応 | p99残差が client upload、server read/decode、server response のどこにあるか分ける | client first/last DATA sent、server `InPayload`、server `OutPayload`、client response header | 現在のbatch集計だけではp99同士の比較で、同一callの因果が弱い |
| upload完了時刻のclient内分解 | p99が「最後のDATA送信前」か「送信後のresponse待ち」か切る | first DATA sent、last DATA sent、first response header、stream close | 計測点を増やすだけでは改善しないが、次の判断材料として重要 |
| remote window / WINDOW_UPDATE時刻の時系列化 | connection-level flow control 枯渇がtailに効いているか見る | window 0/near-zero回数、WINDOW_UPDATE間隔、last DATAまでのstall時間 | aggregateのmin windowだけではtail sampleとの対応がない |
| initial window / connection window tuning | 1MB uploadでwindow枯渇を減らせるか確認する | DATA frame数、WINDOW_UPDATE数、p99、server互換性 | clientから増やせるのは主に受信window。送信側はserver設定に依存するため効かない可能性がある |
| `nghttp2_session_mem_send2` 駆動 | libnghttp2に送信bufferを作らせ、アプリ側でwrite順序を制御する | write syscall数、p50/p99、WOULDBLOCK処理 | no-copy DATAとの組み合わせでは結局partial frame状態管理が必要 |
| response body discard | large request比較でresponse 100Bの受信処理ノイズを消す | p50/p99、body append回数 | 100B responseなので効果は小さいはず。確認用 |
| request size sweep | 100KB〜2MBでflow-control起因の段差を探す | size別 p50/p99、WINDOW_UPDATE数、min window | 実装改善ではなく、適用領域の特定 |
| repeated run / confidence | Docker scheduler等の揺れと真の改善を分ける | 複数runの中央値、p99分散 | 時間がかかるが、p99判断には必要 |
| TLS/mTLS版native transport | 実運用に近いtransport overheadを見る | TLS handshake除外のwarm p50/p99、large request | PoC実装量が増える。Phase 2 transport判断では後回しでよい |
| C側 response assembler | large responseでchunkごとのPHP callback/appendを消す | 100KB/1MB response p50/p99、copy回数 | 互換実装にはtrailer/metadata/error handlingが必要 |
| persistent PHP resource API | batchではなく実利用形に近いconnection reuseを測る | per-call p50/p99、request間reuse、cleanup安全性 | 実装量が増え、PoCから本体設計に近づく |

### 優先順位

| 優先 | 候補 | 理由 |
|---:|---|---|
| 1 | client/server同一call時刻対応 | p99残差の場所がまだ曖昧。次の実装判断に必要 |
| 2 | upload完了時刻のclient内分解 | C側だけで簡単に足せて、last DATA送信後の待ちを分けられる |
| 3 | request size sweep + repeated run | no-copy改善がどのpayload領域で安定するか確認できる |
| 4 | no-copy partial write状態機械 | 効く可能性はあるが実装が重い。時刻分解後でよい |
| 5 | C側 response assembler | large response側の本命だが、互換性論点が大きい |
| 6 | persistent resource API | 本体化判断が近づいた段階で必要 |
