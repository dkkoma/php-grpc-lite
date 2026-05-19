---
Status: Open
Owner: Codex
Created: 2026-05-19
GitHub-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# official ext-grpc / php-grpc-lite のSA JSON wire shape比較

## 目的

service-account JSON / JWT credential pathで、報告者提供Spanner CLI reproの1.75x差が手元でも再現した。

同じSA JSON、同じSpanner database、同じDocker repro、同じcomposer依存で、official ext-grpc 1.58.0は約24ms、php-grpc-lite 0.0.8は約42msになる。ここから、officialだけが速くなるactual request / transport shapeの差を特定する。

## 背景

`docs/issues/open/2026-05-19-compare-sa-json-adc-credential-path.md` で、lite単体のSA JSONとgcloud ADC比較を行った。

- lite内ではmetadata/header shapeは大きく違うが、RPC elapsed差は小さい。
- issue #5の差は「SA JSON/JWT条件でofficialだけが速い」ことにある。

## スコープ

- `spanner-repro:official` と `spanner-repro:lite` をSA JSONで比較する。
- GAX transport入口でのmetadataを可能な範囲で比較する。
- strace/tcpdumpでsend/recv/pollとpacket timingを比較する。
- official側の `GRPC_TRACE` がPHP ext-grpc 1.58で有効に使えるか確認する。
- lite側は `GRPC_LITE_TRACE_FILE` を使い、method/header/frame/RPC elapsedを参照する。

## 非スコープ

- ext-grpcのC-core実装を模倣すること。
- trace有効時のlatency絶対値を性能比較として扱うこと。
- credential secretやauthorization valueの記録。

## 進捗

- [x] trace上のRPC sequence比較
- [x] GAX metadata比較
- [x] strace概要比較
- [ ] tcpdump比較
- [x] GRPC_TRACE確認
- [x] 差分仮説整理

## 完了条件

- official SA JSONとlite SA JSONの差分が、取れる観測点ごとに表になっている。
- 次に実装で試す候補、または追加で必要な計測が明確になっている。

## 観測 2026-05-19

### RPC sequence

metadata shapeの差を見る前に、まず処理経路が同じかを確認した。

報告reproの処理は `Database::execute('SELECT 1')` によるwarmup後、各iterationで `Database::runTransaction()` の中から `Transaction::execute('SELECT @i')` と `Transaction::commit()` を呼ぶ。

liteのtraceでは、SA JSON / gcloud ADC ともRPC sequenceは同じ。

| phase | RPC | kind | 備考 |
|---|---|---|---|
| warmup/session | `/google.spanner.v1.Spanner/CreateSession` | unary | channel cold / session creation |
| warmup query | `/google.spanner.v1.Spanner/ExecuteStreamingSql` | server streaming | `SELECT 1` |
| transaction query | `/google.spanner.v1.Spanner/ExecuteStreamingSql` | server streaming | `SELECT @i` |
| transaction commit | `/google.spanner.v1.Spanner/Commit` | unary | transaction commit |

official ext-grpcの `GRPC_TRACE=http,api,call_error,client_channel` でも、同じSpanner RPC sequenceが見えている。したがって、現時点では「Spanner APIの処理経路がliteだけ別物」という証拠はない。

一方で、同じsequenceでもSA JSON/JWT条件ではofficialだけが大きく速い。以降の調査対象は、同じRPCを通したうえでの `CallCredentials` 後段metadata、HTTP/2 request shape、C-coreとlite transportの送受信挙動差に絞る。

event countでも、lite SA JSON / lite ADC は同じ形になっている。

| variant | `wire.frame_out` | `wire.frame_in` | `rpc.end` | 判断 |
|---|---:|---:|---:|---|
| lite SA JSON | 132 | 46 | 43 | JWT metadata追加以外は同じRPC lifecycle |
| lite ADC | 132 | 46 | 43 | 同じ |

official traceのpath sequenceも以下で、liteと同じ。

1. `/google.spanner.v1.Spanner/CreateSession`
2. `/google.spanner.v1.Spanner/ExecuteStreamingSql`
3. `/google.spanner.v1.Spanner/ExecuteStreamingSql`
4. `/google.spanner.v1.Spanner/Commit`
5. `/google.spanner.v1.Spanner/DeleteSession`

### GAX metadata entry

`GrpcTransport::startUnaryCall()` の入口で、authorization等のsensitive値を出さず、name / length / sha12 / safe valueだけをstderrへ出す一時patchを使った。

GAX入口では、officialとliteのmetadata差は小さい。代表Commit:

| header | official ext-grpc 1.58 | lite 0.0.8 | 判断 |
|---|---:|---:|---|
| `x-goog-api-client` | len 81, `grpc/1.58.0` | len 80, `grpc/0.1.0` | version文字列のみ違う |
| `User-Agent` | len 25, `gcloud-php-legacy/1.106.0` | 同じ | 同じ |
| `x-goog-request-params` | len 165 | len 165 | session値はrunごとに違う |
| `x-goog-spanner-route-to-leader` | `true` | `true` | 同じ |
| `google-cloud-resource-prefix` | len 70 | len 70 | 同じ |

重要: GAX入口ではauthorizationや `cred-type/jwt` はまだ入っていない。これらはCallCredentials plugin / binding / core側で後段追加される。

### official ext-grpc GRPC_TRACE

`GRPC_TRACE=http,api,call_error,client_channel` / `GRPC_VERBOSITY=DEBUG` でofficial ext-grpc 1.58.0のfinal-ish metadataを確認できた。ただしtraceにはauthorization値やproject/db名が出るため、公開共有不可のローカル資料として扱う。

代表Commitでofficialがtransportに渡しているmetadata:

- `user-agent: gcloud-php-legacy/1.106.0 grpc-php/1.58.0 grpc-c/35.0.0 (linux; chttp2)`
- `:authority: spanner.googleapis.com:443`
- `:path: /google.spanner.v1.Spanner/Commit`
- `grpc-timeout: @3602344ms` のようなinternal deadline表記
- `te: trailers`
- `content-type: application/grpc`
- `:scheme: https`
- `:method: POST`
- `grpc-accept-encoding: identity, deflate, gzip`
- `x-goog-api-client: ... grpc/1.58.0 ... pb/+n`
- `x-goog-request-params: session=...`
- `x-goog-spanner-route-to-leader: true`
- `google-cloud-resource-prefix: ...`
- `authorization: Bearer ...`（値は記録しない）
- `x-goog-api-client: cred-type/jwt`

### lite 0.0.8 traceとの差分

同じSA JSON条件のlite 0.0.8では、代表Commitのactual request headerは以下。

- `user-agent: gcloud-php-legacy/1.106.0 grpc-php/0.1.0`
- `:authority`, `:path`, `:scheme`, `:method`
- `grpc-timeout: 3600000m`
- `te`, `content-type`
- `x-goog-api-client: ... grpc/0.1.0 ... pb/+n cred-type/jwt`（0.0.8でfold済み）
- `x-goog-request-params`
- `x-goog-spanner-route-to-leader`
- `google-cloud-resource-prefix`
- `authorization` len 739

主な差分:

| item | official | lite 0.0.8 | 備考 |
|---|---|---|---|
| `user-agent` | grpc-php/1.58.0 + grpc-c/35.0.0 + `(linux; chttp2)` | grpc-php/0.1.0 | server visible差分 |
| `grpc-accept-encoding` | `identity, deflate, gzip` | なし | server visible差分。liteはresponse compression未対応のため安易に送れない |
| `x-goog-api-client` cred marker | duplicate header `cred-type/jwt` | folded single value | 0.0.7ではduplicateだったが遅いまま |
| `grpc-timeout` | trace上 `@...ms` | wire trace上 `3600000m` | official traceの`@`はC-core internal表記の可能性があり、wire valueとは断定しない |
| header order | official C-core order | lite nghttp2 submit order | HPACK/wire差分候補 |
| HEADERS HPACK length | official traceでは未取得 | Commit 630B | officialは未確定 |

### 0.0.7 duplicate確認

`x-goog-api-client` duplicateが主因かを切り分けるため、同じrepro/SA JSONで `php-grpc-lite 0.0.7` も測定した。

| variant | iter | mean | p50 | p90 | p99 |
|---|---:|---:|---:|---:|---:|
| lite 0.0.7 | 200 | 43.121ms | 41.939ms | 46.797ms | 85.161ms |
| lite 0.0.8 | 200 | 42.242ms | 41.791ms | 45.180ms | 49.984ms |
| official 1.58 | 200 | 24.072ms | 23.470ms | 27.772ms | 43.954ms |

0.0.7と0.0.8は同レンジなので、`x-goog-api-client` duplicate/fold単独では1.75x差を説明しない。

## 現時点の判断

- GAX入口のmetadataはofficial/liteでほぼ同じ。差は後段のbinding/core/transportで作られる。
- official final metadataには、liteにない `grpc-accept-encoding` と、より詳細な `user-agent` がある。
- officialは `x-goog-api-client: cred-type/jwt` をduplicate headerとして送っているが、lite 0.0.7もduplicateで遅かったため、これ単独は主因ではない。
- 次の有力候補は、server visible metadata shapeのうち `grpc-accept-encoding`、`user-agent`、HPACK/header order/dynamic table、またはofficial C-coreのHTTP/2 connection settings/control behavior。

### minimal SELECT 1 repro

transaction/Commit固有の要因を外すため、`tools/diagnostics/issue5-spanner-repro/select1-bench.php` を追加した。

このreproは以下だけを実行する。

1. `SpannerClient` からdatabaseを作る。
2. warmupとして `SELECT 1` を1回実行する。
3. 計測ループで `Database::execute('SELECT 1')->rows()->current()` だけを実行する。

つまり、実測対象は `ExecuteStreamingSql SELECT 1` のserver streaming RPC単独に近い。transaction、BeginTransaction、Commit、mutation、DMLは含まない。

SA JSON条件では、この最小reproでも差が再現した。

| condition | variant | iter | mean | p50 | p90 | p99 |
|---|---|---:|---:|---:|---:|---:|
| SA JSON | official ext-grpc 1.58 | 200 | 11.1ms | 10.4ms | 13.5ms | 21.8ms |
| SA JSON | grpc-lite 0.0.8 | 200 | 22.0ms | 21.3ms | 25.3ms | 38.2ms |
| gcloud ADC | official ext-grpc 1.58 | 200 | 21.3ms | 21.0ms | 24.0ms | 29.2ms |
| gcloud ADC | grpc-lite 0.0.8 | 200 | 21.1ms | 20.7ms | 23.4ms | 33.4ms |

この結果は重要。

- Commit固有ではない。
- transaction/write path固有でもない。
- `ExecuteStreamingSql SELECT 1` の軽いserver streaming RPCだけで、SA JSON条件ではofficialだけが速い。
- ADC条件ではofficial/liteがほぼ同等になるため、credential pathが差の発現条件である可能性は高い。
- 一方で、liteのSA JSONとADCはほぼ同レンジなので、credential処理のPHP CPUだけでは説明しない。

以降は、この最小reproを主対象にする。Commitは実アプリ再現確認として残すが、原因切り分けの主対象からは外す。

### credential path から transport state へつながる仮説

`ADC vs SA JSON` は最初の再現条件だが、コード上はcredential providerが直接HTTP/2 transportを選ぶわけではない。

ただし、SA JSON/JWT経路では以下の差が発生する。

1. `Google\ApiCore\CredentialsWrapper::getAuthorizationHeaderCallback()` は、tokenがexpired扱いのとき `UpdateMetadataInterface::updateMetadata()` を呼ぶ。
2. `Google\Auth\Credentials\ServiceAccountCredentials::updateMetadata()` は、self-signed JWTを使える場合に `ServiceAccountJwtAccessCredentials` へ委譲する。
3. `Google\Auth\MetricsTrait::applyServiceApiUsageMetrics()` は `x-goog-api-client` に `cred-type/jwt` を付与する。
4. 結果としてSA JSON/JWTでは、Commit requestの `authorization` が大きくなり、`x-goog-api-client` もJWT markerを含む。

一方、ADC/gcloud user credentialでは `authorization` が短く、CreateSession以外のCommit/ExecuteStreamingSqlではJWT markerも付かない。このため、credential pathは少なくとも以下を変える。

- request HEADERS payload size
- HPACK dynamic table state
- Spanner frontendから見える credential type marker
- 同一HTTP/2 connection上で、Commit直前までに通過したstreamのheader/control state

ここまでの実測では、lite単体のSA JSON/ADC差は小さいため、credential path単独では説明しない。問題は「SA JSON/JWT条件でofficialだけが速い」ことであり、仮説は次の形に絞る。

> SA JSON/JWTによってrequest/header stateは変わるが、official C-coreはDATA受信後のBDP probe/control schedulingを持つため、そのconnection stateでCommitを開始する。liteは同じJWT requestを送っても、その前段のcontrol/BDP stateがofficialと異なるため、Spanner frontendのCommit応答タイミングが遅い。

この仮説は、credential差を無視してPINGへ飛ぶものではない。credential pathが変えるmetadata/header stateを入口として、official C-coreだけが持つHTTP/2 connection state更新が効いている可能性を見るものとして扱う。

### strace summary

前提: syscall / tcpdump / markerによる切り分けは、先方がissue #5で先に詳細に実施している。この節は新発見ではなく、同じreproをこちらの環境で動かして、以降のlocal診断の基準点を作るための確認である。

同じSA JSON、同じDocker repro、20 iterationsで `strace -f -c` を取得した。strace自体のoverheadが大きいためlatency絶対値ではなく、syscall shapeの比較として扱う。

| item | official ext-grpc 1.58 | lite 0.0.8 | 判断 |
|---|---:|---:|---|
| latency mean | 27.4ms | 43.8ms | strace下でも差は残る |
| total syscalls | 3354 | 2879 | total数だけでは説明不可 |
| wait primitive | `epoll_pwait` 55, `futex` 120 | `ppoll` 51, `futex` 29 | officialはC-core pollset/thread寄り、liteは同期ppoll寄り |
| read path | `recvmsg` 56 + `read` 358 | `read` 547, error 89 | liteはOpenSSL BIO/file/read系に寄っている |
| write path | `sendmsg` 93 + `sendmmsg` 1 | `write` 91 + `sendmmsg` 1 | officialはsendmsg中心、liteはwrite中心 |

これは「内部syscall差がない」という結果ではない。先方報告どおり、officialとliteではtransport scheduler / socket I/O primitiveの形が違う。

ただし、現時点の `strace -c` は集計であり、どのRPCのどの区間で差が出たかまでは説明しない。先方報告では `Commit` の送信後から最初の応答までのwire RTT差が主対象になっているため、こちらでも同じ粒度で確認する。

### Commit marker + strace

`Google\ApiCore\Transport\GrpcTransport::startUnaryCall()` に一時markerを入れ、`Spanner/Commit` の `BEGIN` / `_simpleRequest` 後 / `wait()` 開始 / `wait()` 終了をstraceと相関した。20 iterations、SA JSON、同じrepro。

初回Commitはclass loadや揺れを含むため除外して平均した。

| item | official ext-grpc 1.58 | lite 0.0.8 | 判断 |
|---|---:|---:|---|
| `BEGIN -> WAIT_END` | 12.3ms | 20.5ms | こちらでもCommit gapは再現 |
| `BEGIN -> WAIT_BEGIN` | 0.37ms | 0.20ms | PHP/GAX前段では説明不可 |
| wait区間 | 12.0ms | 20.3ms | gapの大半 |
| first request send | +0.25ms | +0.38ms | send開始差では説明不可 |
| first successful response read | +11.94ms | +20ms級 | response arrival待ちが差分 |
| request TLS write | `sendmsg` 約1476B + post-response 52B | `write` 約877B + post-response 39B | wire shape差は残る |

この結果は、先方の「gapはPHP CPUやsend syscall完了後のkernel handoffではなく、client outbound後からserver first responseまでに見えている」という報告と整合する。

重要: 39B/52Bのsmall writeは measured request HEADERS ではなく、response後のcontrol frameである。HEADERS/DATA splitを主因として扱わない。

### Diagnostic variants

server-visible metadataやHPACK/windowが主因かを切るため、ローカルソースから診断variantを作って同じSA JSON reproで100 iterations測定した。

| variant | mean | p50 | p90 | 判断 |
|---|---:|---:|---:|---|
| official ext-grpc 1.58 | 23.2ms | 22.3ms | 27.4ms | 比較対象 |
| lite 0.0.8 pie | 46.1ms | 44.6ms | 52.6ms | gap再現 |
| local source base | 42.6ms | 41.5ms | 47.3ms | local buildでもgap残存 |
| add `grpc-accept-encoding` | 43.0ms | 42.3ms | 47.2ms | 効果なし |
| official-like `user-agent` | 43.2ms | 41.6ms | 47.3ms | 効果なし |
| both | 47.4ms | 42.3ms | 48.3ms | 改善なし |
| sensitive headers `NO_INDEX` | 42.9ms | 42.2ms | 46.5ms | 効果なし |
| all headers `NO_INDEX` | 46.0ms | 45.1ms | 53.2ms | 改善なし |
| server-visible metadata official寄せ | 43.4ms | 42.4ms | 49.4ms | 改善なし |
| HTTP/2 window 65KiB | 47.0ms | 46.0ms | 53.5ms | 悪化 |

ここから、少なくとも以下は主因として弱い。

- `x-goog-api-client` duplicate/fold
- `grpc-accept-encoding` の有無
- `user-agent` 文字列
- `x-goog-api-client` の `grpc/0.1.0` / `grpc/1.58.0`
- sensitive metadata / all metadata のHPACK indexing
- 8MiB receive window設定

残る有力候補は、server-visible metadataそのものではなく、HTTP/2 connection / control scheduling、TLS record / frame packing、またはgRPC C-coreとnghttp2直接利用の間にある送受信lifecycle差。

### BDP PING診断variant

official ext-grpc 1.58.0 の `GRPC_TRACE=http` には、SA JSON条件でも `BDP_PING` の開始/完了が出ている。gRPC Core側の実装でも、DATA frame受信時に `BdpEstimator` へincoming bytesを加算し、必要に応じて `schedule_bdp_ping_locked()` でclient-origin PINGを送る。

ただし、診断variantの結果は「BDP PINGを入れれば解決」とまでは言えない。

| variant | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| base | 200 | 41.8ms | 41.1ms | 45.6ms | 53.9ms | 比較基準 |
| server-streaming DATA後だけPING | 200 | 40.3ms | 38.9ms | 43.3ms | 84.6ms | p50は小改善、tail悪化 |
| 全DATA後PING | 200 | 42.9ms | 33.8ms | 40.7ms | 377.9ms | p50は改善するが過剰PINGでtailが壊れる |
| official ext-grpc 1.58 | 200 | 27.1ms | 25.7ms | 32.1ms | 49.7ms | まだ大きな差あり |

追加の100 iteration反復でも同傾向。

| round | variant | mean | p50 | p90 | p99 |
|---:|---|---:|---:|---:|---:|
| 1 | base | 44.6ms | 44.1ms | 48.8ms | 79.3ms |
| 1 | server-streaming DATA後だけPING | 43.4ms | 40.0ms | 47.9ms | 166.6ms |
| 1 | 全DATA後PING | 35.5ms | 34.3ms | 39.1ms | 88.4ms |
| 1 | official ext-grpc 1.58 | 25.7ms | 24.7ms | 30.6ms | 66.2ms |
| 2 | base | 43.0ms | 42.4ms | 47.0ms | 55.5ms |
| 2 | server-streaming DATA後だけPING | 42.0ms | 39.7ms | 49.2ms | 165.9ms |
| 2 | 全DATA後PING | 42.5ms | 38.4ms | 49.9ms | 206.6ms |
| 2 | official ext-grpc 1.58 | 30.6ms | 27.7ms | 38.5ms | 105.9ms |

この結果から、次のように扱う。

- ext-grpcがBDP PINGを送ること自体は事実。
- grpc-liteにBDP/control scheduling系がないことも事実。
- しかし、単純なPING追加はtailを悪化させるため、そのまま修正候補にしない。
- `server-streaming response DATA後だけ` では改善が小さいため、Commit直前の1要素だけでなく、C-coreのtransport scheduler / flow-control periodic update / ping outstanding管理を含むconnection lifecycle差として見る。
- credential pathはこのtransport差と独立ではない。SA JSON/JWTはHEADERS size、HPACK state、`cred-type/jwt`、authorization token shapeを変えるため、その状態でC-core transportだけが速い応答を得ている可能性を調べる。

## 次の作業

1. SSL key logまたはnghttp2 frame callback traceで、liteのCommit request/response/control frame sequenceをpayload levelで確定する。
2. official側は `GRPC_TRACE=http` で得られるC-core frame/control sequenceと、straceのTLS write/read sizeを対応付ける。
3. まだ差分が残る場合、lite側でC-coreに近いcontrol scheduling variantを作る。

### minimal SELECT 1 follow-up: ADC strace and single-factor checks

`ExecuteStreamingSql SELECT 1` の最小reproで、SA JSONだけでなくADC条件も同じ `strace -f -ttt -T -yy` 粒度で取得した。20 iterationsのため絶対値は参考だが、4条件の相対形状は安定している。

| condition | variant | mean | p50 | repeated request TLS write | request write -> first response read |
|---|---|---:|---:|---:|---:|
| SA JSON | official ext-grpc 1.58 | 11.98ms | 12.32ms | `sendmsg` 1400B | mean 10.26ms / p50 11.23ms |
| SA JSON | grpc-lite 0.0.8 | 20.67ms | 20.59ms | `write` 852B | mean 19.35ms / p50 19.12ms |
| ADC | official ext-grpc 1.58 | 21.05ms | 21.37ms | `sendmsg` 929B | mean 19.42ms / p50 20.26ms |
| ADC | grpc-lite 0.0.8 | 19.88ms | 20.06ms | `write` 467B | mean 18.75ms / p50 18.76ms |

ここで見えている事実は次の通り。

- 差分は `SELECT 1` のrequest write後からfirst response readまでに出ている。
- official SA JSONだけが速い。official ADCはliteと同レンジ。
- liteはSA JSON/ADCでほぼ同レンジ。つまり、credential処理のPHP CPUやtoken生成だけでは説明しない。
- repeated request TLS write sizeはofficial/liteで違うが、official ADCは大きいwriteでも速くないため、write size単独でも説明しない。

追加で、単独差分を小さく潰した。

| check | result | 判断 |
|---|---:|---|
| official ext-grpc `grpc.http2.bdp_probe=0` | mean 11.9ms / p50 11.1ms | BDP probe offでもofficial SA JSONは速い。BDP単独ではない |
| lite `grpc.primary_user_agent` official寄せ | mean 20.9ms / p50 20.7ms | user-agent文字列差ではない |
| lite `grpc-accept-encoding: identity, deflate, gzip` 診断追加 | mean 22.0ms / p50 21.4ms | accept-encoding有無ではない |
| lite user-agent + accept-encoding | mean 21.2ms / p50 20.6ms | 複合でも改善なし |
| lite authorization `NO_INDEX` | mean 20.5ms / p50 19.9ms under strace; normal runは揺れ大 | HPACK indexing単独ではない |
| lite `x-goog-api-client` duplicate化 | mean 20.8ms / p50 20.8ms | duplicate/foldではない |
| lite `phpversion('grpc') = 1.58.0` | mean 20.8ms / p50 20.6ms | `x-goog-api-client`のgrpc version差ではない |
| lite HTTP/2 receive window 1MiB | mean 22.1ms / p50 21.8ms | 8MiB window設定ではない |
| lite HTTP/2 receive window 64KiB | mean 22.3ms / p50 21.7ms | window縮小は改善しない |

この段階で、次の候補は主因から外す。

- `x-goog-api-client` duplicate/fold
- `x-goog-api-client` 内の `grpc/0.1.0` vs `grpc/1.58.0`
- `grpc-accept-encoding`
- `user-agent`
- authorization headerのHPACK no-index指定
- liteの8MiB receive window
- BDP probe単独

残る調査対象は、個別metadata値ではなく、official C-core と lite nghttp2 direct transport の wire/control lifecycle 差分。
具体的には以下を次に見る。

1. official SA JSONとlite SA JSONのHTTP/2 frame sequenceを、request HEADERS/DATA、server PING、client PING ACK、response HEADERS/DATA/TRAILERSの順序と時刻で比較する。
2. official側は `GRPC_TRACE=http` とstrace TLS write/readを相関する。raw traceにはauthorization tokenが含まれるため、共有する場合は必ずredactする。
3. lite側は `GRPC_LITE_TRACE_FILE` のframe traceを一次ソースにする。
4. 必要ならSSL key log + decrypted HTTP/2 captureで、HPACK後のwire frameを直接比較する。

### lite inbound frame trace extension

lite側の `GRPC_LITE_TRACE_FILE` はこれまで inbound stream frames を出しておらず、`PING` / `SETTINGS` などcontrol frameだけを見ていた。`on_frame_recv_callback` のtrace対象を全frameへ広げ、payloadを出さずに `HEADERS` / `DATA` / trailing `HEADERS` のframe type、flags、payload length、timestampだけを記録できるようにした。

SA JSON / `ExecuteStreamingSql SELECT 1` / 20 iterationsのlite traceでは、request HEADERS/DATA送信後、response initial HEADERSが概ね +18ms〜+22msで到着している。

代表形状:

| stream | request HEADERS out | request DATA out | response HEADERS in | response DATA in | trailing HEADERS in | server PING in |
|---:|---:|---:|---:|---:|---:|---:|
| 5 | +0.000ms | +0.293ms | +20.920ms | +21.204ms | +21.571ms | +21.811ms |
| 7 | +0.000ms | +0.149ms | +18.142ms | +18.483ms | +18.798ms | +18.924ms |
| 9 | +0.000ms | +0.226ms | +19.378ms | +19.883ms | +20.390ms | +20.744ms |
| 11 | +0.000ms | +0.176ms | +20.480ms | +24.532ms | +25.398ms | +26.503ms |
| 19 | +0.000ms | +0.059ms | +12.768ms | +12.978ms | +13.795ms | +13.889ms |

このtraceから、lite内でresponseを読んだ後の処理が主因ではないことはさらに強くなった。遅延はrequest DATA送信後からresponse initial HEADERS到着までにあり、つまりclientから見える範囲ではserver/frontend response arrival待ちとして観測される。

server PINGは多くのstreamでresponse/trailingと同時または直後に来ており、liteは即座にACKしている。したがって、このreproでは「clientがserver PINGに遅くACKするためresponseが遅れる」という形ではない。

次はofficial側も同じ粒度で、response HEADERS/DATA/TRAILERS到着時刻とserver PINGの相対順序を比較する。officialは `GRPC_TRACE=http` でraw authorizationを含むため、解析結果だけをredactして記録する。

### official/lite socket option and per-RPC I/O shape

`strace -f -ttt -T -yy` で `setsockopt` / `getsockopt` / `fcntl` とTCP 443向けI/Oを確認した。

socket option差分:

| variant | socket shape | observed socket options | 判断 |
|---|---|---|---|
| official ext-grpc 1.58 | `TCPv6` v4-mapped socket | `TCP_NODELAY=1`, `TCP_INQ=1`, `SO_REUSEADDR=1`, `IPV6_V6ONLY=0` | C-core固有のsocket設定差はある |
| grpc-lite | `TCP` IPv4 socket | `TCP_NODELAY=1` | 最小設定 |

`TCP_INQ` はofficialとの差分として見えたため、liteへ条件付き追加したvariantをsource buildして `SELECT 1` / SA JSON / 200 iterationsで測った。

| variant | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---|
| lite + `TCP_INQ` | 23.1ms | 22.0ms | 25.6ms | 32.2ms | 改善なし。採用しない |

この結果から、`TCP_INQ` 自体は今回の10ms級差分の主因から外す。`SO_REUSEADDR` とv4-mapped socketも、request write後からfirst response readまでの差を説明する候補としては弱いが、未検証なので「主因候補からは低優先」として残す。

per-RPC I/O shapeでは、official/liteともwarm後は1RPCにつき小さいcontrol writeとrequest writeが見える。

| variant | repeated request write shape | response read shape | 判断 |
|---|---|---|---|
| official ext-grpc 1.58 | `sendmsg` 52B control write → `sendmsg` 約1400B request write | `recvmsg` 約196B前後 | C-coreはcontrol/write schedulerを持つ |
| grpc-lite | `write` 39B control write → `write` 約847〜855B request write | `read` 約196B前後 | nghttp2/OpenSSL直接経路 |

この差は「wire上の分割/HPACK/TLS recordが違う」ことを示すが、単独で遅延原因とはまだ言えない。official ADCはrequest writeがlite ADCより大きいにもかかわらず速くないため、write size単独では説明できない。

official `GRPC_TRACE=http` では、serverからのresponse DATA後にserver PINGが来て、C-coreがPING ACKを返す形が見える。liteのinbound frame traceでも、server PINGはresponse/trailingと同時または直後に来て即ACKされる。したがって、この最小reproでは「server PING ACKが遅いせいでresponseが止まる」という形ではない。

raw official traceにはauthorization tokenが含まれる。今後もdocs/GitHubへ貼るのはredact済みの要約のみとする。

現時点で残る未解明点:

1. official SA JSONだけが速く、official ADCとlite SA/ADCが同レンジになる理由。
2. 同じPHP package構成でも、official C-core transportとlite nghttp2 direct transportでSpanner frontendの応答開始が変わる理由。
3. TLS record / HPACK dynamic table / HTTP/2 scheduler / C-core call lifecycleのどれが、SA JSON/JWT条件と相互作用しているか。

次に見るべき候補:

1. `SELECT 1` のrequest HEADERS/DATAを可能な範囲で完全に同一化し、server-visible metadata差をまとめて潰す。
2. lite側でdiagnostic限定のTLS write/read traceを追加し、nghttp2 frame outと実際のOpenSSL write boundaryを対応付ける。
3. 必要ならdecrypted HTTP/2 captureを検討する。ただしcredential漏えいリスクが高いため、実施する場合はtoken redaction手順を先に決める。

### lite TLS I/O boundary trace

`GRPC_LITE_TRACE_FILE` にdiagnostic限定の `wire.tls_write` / `wire.tls_read` / retry eventを追加し、nghttp2 frame outとOpenSSL I/O境界を対応付けた。payloadやtokenは出さず、requested/result length、stream id、RPC methodだけを記録する。

PHPT:

- `./tools/test/check-phpt.sh`: 16/16 PASS
- `./tools/test/check-c-static-analysis.sh`: PASS

最初のsource-build trace imageには `pecl protobuf` が入っており、公式/lite release repro imageと `x-goog-api-client` の `pb/...` が変わっていた。そのため、protobuf拡張なしのsource-build imageを作り直して、release repro環境と同じ `pb/+n` 条件で測り直した。

`SELECT 1` / SA JSON / protobuf拡張なし / 50 iterationsのtraceでは、`ExecuteStreamingSql` のrequest HEADERS/DATAは1回のTLS writeにcoalesceされている。つまり、liteがHEADERSとDATAを別TLS writeで送っているわけではない。

代表形状:

| event | timing |
|---|---|
| `wire.frame_out` HEADERS | request header block生成 |
| `wire.frame_out` DATA | gRPC 5B + protobuf request |
| `wire.tls_write` | HEADERS + DATA を1 writeで送信 |
| `wire.tls_read_retry` | `SSL_ERROR_WANT_READ` でpoll待ち |
| `wire.tls_read` | response initial HEADERSを含むTLS record到着 |
| `wire.frame_in` HEADERS/DATA/trailing HEADERS | nghttp2がresponseを処理 |
| `wire.frame_in` PING → `wire.tls_write` 17B | server PING ACK |

protobuf拡張なしのlite traceから算出した `request TLS write -> first TLS read`:

| source | n | min | p50 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| lite `GRPC_LITE_TRACE_FILE` | 51 | 12.4ms | 18.8ms | 20.7ms | 22.5ms | 34.1ms |

同じ見方を既存straceへ適用した結果:

| source | n | min | p50 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| official ext-grpc 1.58 strace | 23 | 0.5ms | 11.0ms | 12.4ms | 12.8ms | 36.9ms |
| grpc-lite strace | 24 | 7.2ms | 19.1ms | 21.6ms | 38.8ms | 42.3ms |

この追加traceで確定したこと:

- liteはrequest HEADERS/DATAを1回のTLS writeにまとめている。
- trace-onlyのTLS read/write eventには、receive側も `current_io_call` を設定してstream id / RPC methodを相関できるようにした。
- liteの主な待ちは `SSL_ERROR_WANT_READ` 後のpoll待ちであり、first TLS readが来るまでの時間として観測される。
- responseが到着した後のnghttp2処理、protobuf decode、PHP deliveryは今回の差分の主因ではない。
- server PING ACKはresponse後に即座に出ており、response開始前のブロッカーではない。

追加で、lite source-buildに一時的な公式寄せvariantを入れて測った。内容は `grpc-accept-encoding: identity, deflate, gzip` の追加、`user-agent: grpc-php/1.58.0`、`x-goog-api-client` のfold無効化による `cred-type/jwt` 分離である。これは診断用の一時差分であり、本実装には残さない。

`SELECT 1` / SA JSON / protobuf拡張なし / 200 iterations:

| variant | mean | p50 | p90 | p99 | min | max |
|---|---:|---:|---:|---:|---:|---:|
| lite official-ish metadata | 21.142ms | 20.494ms | 23.636ms | 37.580ms | 17.400ms | 37.961ms |

同traceの `request TLS write -> first TLS read`:

| source | n | min | p50 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| lite official-ish metadata | 201 | 15.334ms | 18.220ms | 21.065ms | 35.635ms | 35.873ms |

このvariantでもofficial ext-grpc SA JSONのp50約11msには近づかなかった。`Grpc\VERSION` 変更は `BaseStub` の `user-agent` には効くが、GAXの `x-goog-api-client` 内 `grpc/...` は `phpversion('grpc')` 由来であり、この診断variantでは `grpc/0.1.0` のままだった。ただし、過去の単独検証で `x-goog-api-client` の `grpc/1.58.0` 相当化は改善を示していないため、個別metadata値だけが主因である可能性は低い。

まだ説明できていないこと:

- official SA JSONでは同じ `request write -> first read` がp50約11msまで短くなる理由。
- liteのwireは1 TLS writeにcoalesce済みなので、単純な「write分割」ではない。
- official C-coreのHTTP/2 scheduler / HPACK state / TLS record / stream lifecycleのどれがSpanner frontendの応答開始に効いているか。

### Pub/Sub ListTopics cross-check

Spanner固有の現象か、Google API gRPC全般で起きる現象かを切り分けるため、同じSA JSONでPub/Sub `ListTopics` の最小reproを追加した。

追加fixture:

- `tools/diagnostics/issue5-spanner-repro/list-topics-bench.php`
- Docker build arg: `BENCH_SCRIPT=list-topics-bench.php`
- composer dependency: `google/cloud-pubsub` `v1.51.0`

計測条件:

- project: `vast-falcon-165704`
- credential: 同じSA JSON
- iterations: 200
- official: `pecl grpc-1.58.0`
- lite: `dkkoma/php-grpc-lite:0.0.8`

結果:

| case | ext.grpc | mean | p50 | p90 | p99 | min | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| Pub/Sub `ListTopics` official | 1.58.0 | 511.489ms | 429.626ms | 1011.703ms | 1437.054ms | 149.333ms | 1454.317ms |
| Pub/Sub `ListTopics` lite | 0.1.0 | 513.301ms | 405.557ms | 993.418ms | 1469.382ms | 148.918ms | 1539.694ms |

判断:

- Pub/Sub `ListTopics` ではofficial/liteの差は再現しない。
- したがって、issue #5の `SELECT 1` 差分は「Google API gRPC全般でliteが常に遅い」ではなく、Spanner `ExecuteStreamingSql` / Spanner frontend / Spanner向けmetadata・session条件との相互作用として扱うのが妥当。
- Pub/Sub自体の絶対値は大きく揺れているため、ここでは性能評価ではなく、差分再現有無のcross-checkとしてのみ扱う。

### Pub/Sub GetTopic cross-check

`ListTopics` はlist RPCで揺れが大きいため、既存topic `test` に対する `GetTopic` も追加した。

追加fixture:

- `tools/diagnostics/issue5-spanner-repro/get-topic-bench.php`
- Docker build arg: `BENCH_SCRIPT=get-topic-bench.php`
- env: `PUBSUB_TOPIC=test`

200 iterationsの結果:

| case | ext.grpc | mean | p50 | p90 | p99 | min | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| Pub/Sub `GetTopic` official | 1.58.0 | 481.927ms | 596.090ms | 870.910ms | 1046.642ms | 151.900ms | 1083.099ms |
| Pub/Sub `GetTopic` lite | 0.1.0 | 447.513ms | 190.436ms | 849.231ms | 1016.056ms | 150.942ms | 1022.965ms |

50 iterations x 3の交互run:

| run | variant | mean | p50 | p90 | p99 | min | max |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | official | 449.921ms | 208.974ms | 906.025ms | 1004.303ms | 156.276ms | 1004.303ms |
| 1 | lite | 507.028ms | 590.114ms | 975.139ms | 1369.713ms | 166.240ms | 1369.713ms |
| 2 | official | 496.348ms | 607.681ms | 928.644ms | 1051.526ms | 167.451ms | 1051.526ms |
| 2 | lite | 272.590ms | 190.215ms | 699.243ms | 909.266ms | 151.342ms | 909.266ms |
| 3 | official | 300.614ms | 188.040ms | 896.211ms | 927.736ms | 156.582ms | 927.736ms |
| 3 | lite | 275.995ms | 186.858ms | 742.096ms | 973.494ms | 163.960ms | 973.494ms |

lite trace上の `GetTopic` `rpc.end` は、200 iterationsで `min=150.556ms / p50=190.064ms / p90=844.632ms / p99=1008.032ms / mean=448.220ms` だった。アプリ側の値とほぼ同じで、PHP wrapper後段ではなくRPC自体の応答分布が大きい。

判断:

- `GetTopic` は `ListTopics` より軽い単一topic RPCだが、今回の環境では150ms台と800ms〜1s台の二峰性があり、Spannerで見えている +8〜11ms級の差を検出する用途には弱い。
- official/liteの一貫した劣後は見えない。runごとのp50はofficial/liteどちらにも振れる。
- Pub/Subは、少なくともこのproject/topic条件では「Google API gRPC全般でliteだけ遅い」という仮説を支持しない。ただし、揺れが大きいため小さい差の不存在証明には使わない。

### Secret Manager / Resource Manager fixed-resource cross-check attempt

Pub/Sub以外で固定resourceを読む軽いunary RPCとして、Secret Manager `GetSecret` と Resource Manager `GetProject` のfixtureを追加した。

追加fixture:

- `tools/diagnostics/issue5-spanner-repro/get-secret-bench.php`
- `tools/diagnostics/issue5-spanner-repro/get-project-bench.php`
- Docker build arg: `BENCH_SCRIPT=get-secret-bench.php` / `BENCH_SCRIPT=get-project-bench.php`

結果:

| API | RPC | 結果 |
|---|---|---|
| Secret Manager | `GetSecret` | API有効化、SA権限付与、`test` secret作成後に計測可能 |
| Resource Manager | `GetProject` | `cloudresourcemanager.googleapis.com` がconsumer project `205742274492` で無効 |

Secret Managerについては `ListSecrets page_size=1` が通るため、空list RPCとして参考計測した。

| case | variant | iter | mean | p50 | p90 | p99 | min | max |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Secret Manager `ListSecrets` | official | 100 | 185.047ms | 150.957ms | 180.008ms | 4269.127ms | 88.091ms | 4269.127ms |
| Secret Manager `ListSecrets` | lite | 100 | 142.460ms | 143.057ms | 169.691ms | 507.891ms | 86.841ms | 507.891ms |

`test` secret作成後、固定resource unaryとして `GetSecret` を計測した。

| case | variant | iter | mean | p50 | p90 | p99 | min | max |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Secret Manager `GetSecret` | official | 200 | 161.856ms | 164.227ms | 170.481ms | 198.631ms | 85.370ms | 259.841ms |
| Secret Manager `GetSecret` | lite | 200 | 163.350ms | 164.576ms | 170.772ms | 199.320ms | 87.338ms | 206.081ms |

lite trace上の `GetSecret` `rpc.end` は、`n=202 / min=78.477ms / p50=164.132ms / p90=170.427ms / p99=198.724ms / mean=162.755ms` だった。アプリ側の値とほぼ同じで、PHP wrapper後段ではなくRPC自体の応答分布として観測される。

判断:

- Secret Manager `ListSecrets` はPub/Subよりは軽いが、officialに4.2s級outlierがあり、+8〜11ms級の差を見る用途にはまだ弱い。
- Secret Manager `GetSecret` はPub/Subより安定しており、official/liteはほぼ同等だった。
- したがって、SA JSON + Google public API gRPC + 軽量unary一般でliteが一貫して遅い、という仮説は支持しない。
- Spanner `ExecuteStreamingSql` / `Commit` で見えている差は、Spanner data plane / session / Spanner向けmetadata / frontend schedulingとの相互作用として扱うのが妥当。
- Resource ManagerはAPI無効のため、現状態では候補から外す。
