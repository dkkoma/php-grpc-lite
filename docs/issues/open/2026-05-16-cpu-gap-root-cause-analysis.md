---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# ext-grpc optimizedとの差を根拠にCPU差の要因を分析する

## 目的

real Spanner + Laravel/FPM fixtureで見えている `php-grpc-lite` 通常ビルド vs `ext-grpc 1.58.0 optimized` のCPU/request差を起点に、CPU差の要因を分解し、修正候補を出し切る。

## 背景

`ext-grpc 1.58.0` を `-O3 -flto -fno-semantic-interposition` 付きでビルドし、`php-grpc-lite` は通常ビルドのまま比較すると、mixed transaction c16付近でCPU/requestが約1.3〜1.4倍になった。安定した1.5倍には届いていないが、要因分析には十分な差がある。

## スコープ

- `php-grpc-lite` 側も optimized build を作り、ビルドフラグだけでどこまで縮むか確認する。
- `callgrind` / cgroup CPU sampling / 必要に応じたtraceで、`php-grpc-lite` のCPU使用箇所を分解する。
- eBPF / perf / hardware counterは、Docker Desktop環境で利用できる範囲を確認し、使える場合だけ追加する。
- 修正候補を個別issue化し、各issueで実装、検証、レビューを行う。

## 非スコープ

- 実アプリ本体の最適化。
- Spannerサーバ側の性能改善。
- ext-grpcの実装変更。

## 計画

1. `php-grpc-lite` current tree の default / optimized so を作る。
2. `grpc-lite default` / `grpc-lite optimized` / `ext-grpc 1.58 optimized` を同じ負荷で比較する。
3. mixed transaction c16を主条件としてcallgrindを取り、CPU上位を分類する。
4. 実装上の修正候補、ビルド/配布上の修正候補、計測上の追加確認候補に分ける。
5. 修正候補を `docs/issues/open/` に個別issueとして作る。

## 進捗

- `tools/dev/build-current-grpc-lite-so.sh` を追加し、current treeの `default` / `optimized` soを作成できるようにした。
- `grpc-lite default` / `grpc-lite optimized` / `ext-grpc 1.58 optimized` のmixed transaction c16比較を実施した。
- `callgrind`、cgroup CPU sustain、CLI `strace -c` を取得した。
- `perf stat` はDocker Desktop環境の権限不足で `No permission to enable task-clock event` となり利用不可。

## 検証

### build flag比較

real Spanner / Laravel FPM / 16 workers / 4 CPU limit / `transaction_select2_update1_insert1` / concurrency 16 / 256 requests。

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms |
|---|---:|---:|---:|---:|---:|
| grpc-lite default | 12909.9 | 25.8996 | 608.9 | 615.0 | 690.3 |
| grpc-lite optimized | 13459.3 | 27.6641 | 566.2 | 564.0 | 661.2 |
| ext-grpc 1.58 optimized | 10404.6 | 26.7523 | 587.6 | 593.2 | 663.2 |

判断:

- `grpc-lite` 側へ `-O3 -flto -fno-semantic-interposition` を付けてもCPU/requestは改善せず、今回の条件ではむしろ悪化した。
- したがって、現時点の差分は単純なビルドフラグ不足ではなく、実装構造・API bridge・transport loop側の固定費として扱う。

### cgroup CPU sustain

20秒 sustain / concurrency 16 / `transaction_select2_update1_insert1`。

| variant | requests/sec | cpu_cores_avg | worker_ticks | voluntary_ctxt | nonvoluntary_ctxt |
|---|---:|---:|---:|---:|---:|
| grpc-lite default | 26.6644 | 0.392 | 717 | 5629 | 17357 |
| ext-grpc 1.58 optimized | 27.0170 | 0.298 | 548 | 5907 | 14576 |

判断:

- throughputはほぼ同等だが、`grpc-lite` の平均CPU core使用は約1.32倍。
- CPU throttlingは発生していない。
- `grpc-lite` は nonvoluntary context switch も多く、同期I/O / poll loop / requestごとの固定費がCPU差に乗っている可能性がある。

### callgrind

FPM worker path / `transaction_select2_update1_insert1` / 4 requests / concurrency 1。

| variant | representative worker Ir |
|---|---:|
| grpc-lite default | 137M〜140M、最大worker 930M |
| ext-grpc 1.58 optimized | 120M〜123M、最大worker 913M |

判断:

- callgrind上も通常worker単位で `grpc-lite` が約1.1〜1.2倍重い傾向。
- ただしPHP/FPM起動、autoload、Zend実行、dl lookupが大きく、C拡張内部関数単位の帰属は十分に分解できなかった。
- callgrindは「差がある」確認には使えるが、C hot path特定の主資料にはしにくい。

### strace

CLI profile path / `transaction_select2_update1_insert1` / 8 iterations。

| variant | elapsed_ms | notable syscall shape |
|---|---:|---|
| grpc-lite default | 3052.7 | `read` 1438, `write` 178, `ppoll` 85 |
| ext-grpc 1.58 optimized | 1475.9 | `futex`/`epoll_pwait` が目立つ。background thread待ちが混ざる |

判断:

- `strace -c` の時間値はext-grpc側でbackground thread / futex waitが混ざるためCPU比較には使いにくい。
- syscall countとしては、`grpc-lite` は同期read/write/ppoll型、ext-grpcはthreaded event loop + epoll型という構造差が見える。

### コードリーディングからの強い候補

1. `grpc_lite_channel_key()` がRPCごとにchannel key文字列を作り、credential PEMを毎回hashしている。
   - `vendor/grpc/grpc/etc/roots.pem` は 264,440 bytes。
   - GAX/BaseStubは `ChannelCredentials::setDefaultRootsPem()` によりdefault rootsを入れる。
   - `ChannelCredentials::createSsl()` はdefault rootsをcredentialsに持ち、`grpc_lite_channel_key()` は `root_certs` / `cert_chain` / `private_key` を毎回hashする。
   - mixed transactionは複数RPCを含むため、この固定費がリクエスト内で繰り返される。
2. unary response pathが gRPC 5B frame込みの `smart_str body` を作り、`typed_result->body` にコピーし、bridge側でさらにpayloadだけを `zend_string_init()` している。
   - small unaryでも1RPCごとに余分なallocation/copyが発生する。
   - server streaming側にはdirect payload assemblyがあるが、unary側はまだbody経由。
3. request metadata / user-agent pathがPHP metadata zval配列に毎回 `user-agent` 配列を作ってからHTTP/2 headersへ変換している。
   - user-agentはwire headerであり、PHP metadata配列を経由する必要は薄い。
   - metadataが多い実ワークロードでは既に効くことが分かっているが、small mixedでも固定費として残る。
4. `startBatch()` result constructionは、空metadataでもobject propertyとarrayを毎回作る。
   - ext-grpc API互換上、返すshapeは必要だが、空metadata/status/messageの構築順とコピー回数を減らす余地がある。
5. transport loopは同期 `SSL_read` / `SSL_write` + `poll` で、ext-grpcのevent engineとはscheduler構造が違う。
   - ただし現時点の差はwall waitではなくCPU/request差として見えているため、まずは上記のC/PHP bridge固定費を潰すのが先。

## 判断ログ

- CPU差の観測線は `transaction_select2_update1_insert1` / FPM 16 workers / 4 CPU limit / concurrency 16 を主条件にする。
- まずはGCC 14のまま、ビルドフラグ差とCPU構造差を見る。
- Docker Desktop環境ではperf/eBPF hardware counterは使えないため、localではcgroup CPU、callgrind、strace、コード読解を一次資料にする。
- 最初に直す候補は、RPCごとのchannel key/credential hash、unary response double-copy、user-agent/metadata header固定費。

## 完了条件

- CPU差の主要因候補が、計測根拠付きで列挙されている。
- 修正候補が個別issue化されている。
- 少なくともビルドフラグ差、callgrind上位、cgroup CPU挙動の3観点で説明できる。
