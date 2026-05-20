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
- grpc-liteのactive PINGはSA JSON/ADCどちらでも改善する。これはSA JSON/JWT専用効果ではなく、grpc-lite transport/control state側の改善候補として扱う。
- official ext-grpcは `grpc.http2.bdp_probe=0` でもSA JSONで速いため、BDP probe単独はofficial SA fast pathの必要条件ではない。
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


2026-05-20に、PR #6 branchでcredential / BDP / active PING matrixを再計測した。iterationsは1000。

| impl | credential | option | mean | p50 | p90 | p99 | 判断 |
|---|---|---|---:|---:|---:|---:|---|
| official ext-grpc 1.58 | SA JSON | default | 11.723ms | 11.552ms | 13.677ms | 16.984ms | official SA fast path |
| official ext-grpc 1.58 | SA JSON | `grpc.http2.bdp_probe=0` | 12.664ms | 12.231ms | 14.732ms | 21.776ms | BDP offでも速い |
| official ext-grpc 1.58 | ADC | default | 21.160ms | 20.826ms | 23.571ms | 27.934ms | ADCではlite offと同レンジ |
| official ext-grpc 1.58 | ADC | `grpc.http2.bdp_probe=0` | 22.157ms | 21.297ms | 24.239ms | 31.725ms | BDP offでやや悪化 |
| grpc-lite source | SA JSON | active off | 22.511ms | 21.087ms | 24.409ms | 35.348ms | baseline |
| grpc-lite source | SA JSON | active `0ms` | 17.325ms | 16.704ms | 19.903ms | 25.808ms | 改善 |
| grpc-lite source | SA JSON | active `10ms` | 17.061ms | 16.090ms | 19.100ms | 28.761ms | 改善 |
| grpc-lite source | ADC | active off | 21.966ms | 21.521ms | 24.479ms | 32.360ms | baseline |
| grpc-lite source | ADC | active `0ms` | 16.839ms | 16.424ms | 19.173ms | 25.526ms | 改善 |
| grpc-lite source | ADC | active `10ms` | 15.495ms | 15.192ms | 17.620ms | 23.233ms | 改善 |

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

### SELECT 1: HPACK / grpc-timeout single-factor variants

server-visible metadata値だけでは改善しなかったため、wire表現に近い単独要素を追加で切った。

| variant | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| lite source-build baseline相当 | 50 | 21.891ms | 21.672ms | 24.590ms | 26.797ms | protobuf拡張なしtrace image |
| HPACK deflate dynamic table size 0 | 200 | 22.240ms | 21.942ms | 25.067ms | 29.120ms | 改善なし |
| `grpc-timeout: 3600S` 固定 | 200 | 22.331ms | 21.731ms | 24.819ms | 39.356ms | 改善なし |

判断:

- request headerのHPACK dynamic table利用有無だけでは、official SA JSONのp50約10〜11msには近づかない。
- `grpc-timeout` の単位表現差だけでもない。
- 残る候補は、接続先frontend/IP、connection preface/settings/control lifecycle、C-core transport scheduler、またはSpanner frontendがそれらを組み合わせて扱う部分。

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

### SELECT 1 fine-grained follow-up

Spanner固有の差分に戻し、`ExecuteStreamingSql SELECT 1` 単体で未分解候補を追加で切った。

#### 同一条件の再計測

SA JSON / `ExecuteStreamingSql SELECT 1` / 20 iterations:

| variant | mean | p50 | p90 | p99 | 備考 |
|---|---:|---:|---:|---:|---|
| official ext-grpc 1.58.0 | 11.554ms | 11.294ms | 12.842ms | 15.144ms | `spanner-repro:official-select1-strace` |
| php-grpc-lite source build | 22.044ms | 21.535ms | 24.768ms | 31.329ms | `spanner-repro:lite-local-peertrace` |

`strace -f -ttt -T -yy -e trace=read,write,network` で、request送信後のfirst response read/recvを見た。

| variant | request send size | send -> next recv/read p50目安 | 観察 |
|---|---:|---:|---|
| official ext-grpc | 約1400B | 約10ms | `sendmsg` で送信、`recvmsg` で受信 |
| php-grpc-lite | 約850B | 約21ms | `SSL_write` 経由の `write`、`SSL_read` 経由の `read` |

判断:

- 差分は引き続き、request write完了後からfirst response到着までに集中している。
- PHP wrapper後段やresponse decodeではなく、Spanner frontendが応答を開始するまでのtransport/wire shape差として見る。

#### 接続先frontend / socket family

official/liteを同時期に `strace` すると、どちらも同じIPv4 frontend `142.250.23.95:443` に到達するrunがある。その状態でもp50差は残る。

追加で、liteをofficialに寄せてIPv4-mapped IPv6 socketで接続するvariantを作った。

| variant | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| lite IPv4-mapped IPv6 socket | 200 | 22.253ms | 21.679ms | 25.252ms | 31.384ms | 改善なし |

判断:

- 接続先Google frontend IP差だけではない。
- officialがIPv4-mapped IPv6 socketを使う点も主因ではない。

#### HPACK / grpc-timeout / header order

server-visible metadataやHPACK表現をさらに切った。

| variant | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| HPACK deflate dynamic table size 0 | 200 | 22.240ms | 21.942ms | 25.067ms | 29.120ms | 改善なし |
| `grpc-timeout: 3600S` 固定 | 200 | 22.331ms | 21.731ms | 24.819ms | 39.356ms | 改善なし |
| server-streaming regular header order official寄せ | 200 | 22.521ms | 22.283ms | 25.275ms | 32.920ms | 改善なし |

HPACK dynamic table size 0 variantでは、`ExecuteStreamingSql` のHEADERS frame payloadは通常の約638Bから1022Bに増えた。それでもlatencyは改善しない。

判断:

- HPACK dynamic table利用有無だけではない。
- `grpc-timeout` の単位表現差だけではない。
- header orderだけでもない。
- officialのper-RPC送信サイズが約1400Bである点は残るが、lite側でHEADERSを大きくしても改善しないため、「小さく圧縮されているから遅い」という単純な説明は成り立たない。

#### 現時点の未解決点

- officialの約1400B送信が、HTTP/2 frame列、TLS record、C-core scheduler/control frameのどの組み合わせから来ているか。
- officialとliteで、Spanner frontendがresponseを開始する条件に効くwire/control stateが何か。
- 単純なmetadata同一化、HPACK無効化、socket family同一化、BDP probe off、PING追加では説明できていない。

### SELECT 1: BDP Ping再検証

別口の調査でBDP Pingが効く可能性が出たため、`ExecuteStreamingSql SELECT 1` で改めて診断variantを作り、request/recv前後にPINGを入れる形を試した。

#### 各RPCごとにPING

最初に、request送信前後・recv前後へ各RPCごとにPINGを入れた。

| point | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| before_request | 200 | 24.466ms | 23.168ms | 30.008ms | 43.938ms | 悪化 |
| after_request | 200 | 21.997ms | 21.369ms | 25.526ms | 30.070ms | baseline相当 |
| before_recv | 200 | 50.683ms | 45.886ms | 112.442ms | 133.296ms | 悪化 |
| after_recv | 200 | 89.868ms | 46.050ms | 69.526ms | 4054.828ms | tail破壊 |
| all | - | - | - | - | - | `SSL_read failed: SSL_get_error=1` |

判断:

- 各RPCごとの単純PINGは過剰で、特にrecv側はtailを壊す。
- これはC-coreのBDP estimatorのような「outstandingを制御し、ACK完了後に次回probe時刻を調整する」動きとは違う。

#### connectionごとに各point 1回だけPING

次に、connectionごとに各point 1回だけPINGを入れた。

| point | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| all once | 200 | 23.906ms | 22.089ms | 27.604ms | 60.106ms | tail悪化 |
| before_request once | 200 | 21.263ms | 21.018ms | 23.488ms | 29.198ms | 小改善だが不安定 |
| after_request once | 200 | 21.962ms | 21.445ms | 24.192ms | 28.978ms | baseline相当 |
| before_recv once | 200 | 20.743ms | 20.480ms | 22.317ms | 27.116ms | 小改善 |
| after_recv once | 200 | 21.948ms | 21.147ms | 24.846ms | 39.131ms | baseline相当〜tail悪化 |

交互run:

| run | point | mean | p50 | p90 | p99 |
|---:|---|---:|---:|---:|---:|
| 1 | baseline | 20.367ms | 20.008ms | 22.185ms | 26.141ms |
| 1 | before_recv once | 20.339ms | 20.033ms | 22.089ms | 26.001ms |
| 1 | before_request once | 21.704ms | 20.939ms | 24.559ms | 36.783ms |
| 2 | baseline | 19.888ms | 19.667ms | 21.758ms | 26.161ms |
| 2 | before_recv once | 21.357ms | 20.839ms | 24.183ms | 30.637ms |
| 2 | before_request once | 21.959ms | 21.299ms | 25.530ms | 32.502ms |
| 3 | baseline | 21.118ms | 20.501ms | 23.723ms | 45.306ms |
| 3 | before_recv once | 20.539ms | 20.124ms | 23.616ms | 30.182ms |
| 3 | before_request once | 21.316ms | 20.587ms | 23.668ms | 39.047ms |

判断:

- `before_recv once` は一部runで小さく良いが、baselineとの差はノイズ域に近い。
- `before_request once` は安定改善ではない。

#### after_recvでPING ACKまで待つ

BDP estimatorの意味はPING送信そのものではなく、ACK完了後にconnection stateを更新する点にあるため、`after_recv` でPINGを送りACK受信まで待つvariantを試した。

| point | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| baseline | 200 | 22.521ms | 21.781ms | 24.176ms | 47.190ms | 同一imageのno ping |
| after_recv_wait once | 200 | 21.283ms | 20.888ms | 24.013ms | 28.651ms | tail改善気味 |
| before_recv_wait once | 200 | 22.185ms | 21.315ms | 25.505ms | 40.057ms | baseline相当 |

交互run:

| run | point | mean | p50 | p90 | p99 |
|---:|---|---:|---:|---:|---:|
| 1 | baseline | 20.849ms | 20.571ms | 23.749ms | 28.793ms |
| 1 | after_recv_wait once | 21.422ms | 21.035ms | 24.004ms | 30.909ms |
| 2 | baseline | 23.437ms | 22.833ms | 27.158ms | 38.394ms |
| 2 | after_recv_wait once | 21.139ms | 20.678ms | 23.718ms | 30.187ms |
| 3 | baseline | 20.969ms | 20.669ms | 24.251ms | 31.648ms |
| 3 | after_recv_wait once | 23.582ms | 23.281ms | 26.436ms | 36.378ms |

判断:

- `after_recv_wait once` はtailが良いrunもあるが、p50改善は安定しない。
- BDP Pingが関係している可能性は残る。ただし「任意の場所にPINGを1回入れる」だけでは、official ext-grpcの約10〜11ms p50には近づかない。
- 次に見るなら、単純PINGではなく、C-coreに近い `incoming bytes accumulator`、`one outstanding ping`、`ACK completion`、`next probe delay`、`flow-control target update` を分けて実装したBDP estimatorとして検証する。

### SELECT 1: reporter BDP Ping差分の再現確認

GitHub issue #5 の報告者から、`Spanner/ExecuteStreamingSql SELECT 1` でBDP-style active PINGにより `grpc-lite 0.0.8` のmean/p50が大きく改善するという追加報告があった。

前節のこちらの検証は、reporter差分を正確に再現していなかった。特に以下が違う。

- reporter差分の主効果は `on_data_chunk_recv_callback()` 内でresponse DATA chunk受信ごとに `nghttp2_submit_ping()` をqueueする点。
- 前節の検証はrecv loop外側のbefore/afterでPINGを入れており、DATA callback境界ではない。
- 前節の一部variantはPING送信後に即flush/waitする形で、reporter差分の「nghttp2 queueへ積み、既存のsend cycleに乗せる」形と違う。

そのため、前節の「単純PINGでは近づかない」という判断は、reporter方式の否定としては扱わない。

#### 実装した診断variant

同一imageで切り替えられるように、以下のenv flag付き診断variantを一時実装した。

- `GRPC_LITE_ISSUE5_BDP_DATA_PING=1`: `on_data_chunk_recv_callback()` でresponse DATA chunk受信時にtimestamp opaqueのPINGをqueueする。
- `GRPC_LITE_ISSUE5_BDP_SETUP_PING=1`: connection setup時、SETTINGS / WINDOW_UPDATE submit後、初回 `send_pending_h2_frames()` 前に固定opaqueのPINGをqueueする。
- `GRPC_LITE_ISSUE5_BDP_PRE_REQUEST_PING=1`: `nghttp2_submit_request()` 直前にtimestamp opaqueのPINGをqueueする。`ExecuteStreamingSql` はserver streaming経路なので `server_streaming_call.c` 側にも入れた。

いずれも `nghttp2_submit_ping()` だけを行い、その場で追加flush/waitはしない。

#### 計測結果

fixture:

- script: `tools/diagnostics/issue5-spanner-repro/select1-bench.php`
- image: `spanner-repro:lite-local-issue5-bdp`
- target: real Cloud Spanner `bench/laravel-bench-db`
- credentials: service account JSON via `GOOGLE_APPLICATION_CREDENTIALS=/sa.json`
- iterations: 500

| variant | iter | mean | p50 | p90 | p99 | 備考 |
|---|---:|---:|---:|---:|---:|---|
| official ext-grpc 1.58 | 500 | 11.453ms | 10.749ms | 14.257ms | 24.158ms | 既存 `spanner-repro:official-select1` |
| lite local, no BDP Ping | 500 | 28.673ms | 23.240ms | 30.923ms | 112.108ms | 同一image、envなし |
| lite local, DATA Ping | 500 | 17.170ms | 16.001ms | 18.885ms | 27.898ms | 主効果あり |
| lite local, setup + DATA + pre-request Ping | 500 | 16.857ms | 16.173ms | 19.030ms | 26.421ms | DATA単独からの追加効果は小さい |

200 iterationの初回確認でも、DATA Ping単独は `mean=16.210ms / p50=15.557ms` で、no BDP Pingの `mean=20.479ms / p50=20.222ms` から明確に改善した。

#### 判断

- issue #5 reporterの「BDP-style active PINGでgapの大半が縮む」という報告は、こちらのreal Spanner環境でも再現した。
- とくに `on_data_chunk_recv_callback()` でresponse DATA受信時にPINGをqueueする経路が主効果。
- setup PING / pre-request PINGは、この環境ではDATA Ping単独に比べた追加効果が小さい。tailを悪化させるrunもあり、production実装にそのまま入れる根拠は弱い。
- official ext-grpc 1.58とはまだp50で約5.4ms差が残るため、BDP Pingだけで完全説明ではない。

production候補:

- 無制限のper-DATA Pingは採用しない。
- これは完全なBDP estimatorではなく、まずは `active BDP probe PING` として扱う。目的は、response DATA受信後にclient-origin active PINGを出すconnection behaviorがSpanner frontend応答差に効くかをproduction-safeに再現すること。
- `BDP probe outstanding`、`BDP probe opaque`、`BDP probe sent_at` をconnection stateに持つ。
- response DATA受信でprobe eligibleにし、未outstandingかつre-arm条件を満たす場合だけPINGをqueueする。
- ACK受信でoutstandingを解除する。ただし、解除するのはclient-origin probeの8-byte opaqueと一致するACKだけ。server-origin PINGや無関係なPING ACKはBDP probe stateを変更しない。
- PINGはDATA callback内で `nghttp2_submit_ping()` するが、ACKをinline waitしない。queued control frameは既存のnonblocking send pathで速やかにflushできるようにし、PING submit時刻、wire write時刻、ACK時刻をtraceで確認する。
- 初期re-arm policyは保守的にする。候補は「connection generationごとに1回」または「最小intervalあり」。継続streamingでRTTごとに投げ続ける設計にはしない。
- まずはwindow size自動変更までは入れず、fixed 8MiB windowのままactive PING有無によるSpanner frontend応答差の再現を目的にする。adaptive flow-controlは別issueで扱う。

### SELECT 1: active PING probe 実装後の比較

上記方針に沿って、production経路にopt-inの `active BDP probe PING` を小さく実装した。

注意: この実装はgRPC CoreのBDP estimator parityではない。DATA受信後にclient-origin PINGをqueueし、ACKを照合するconnection-level active PING実験である。したがって、同一RPCのfirst responseを直接速くするものではなく、効果がある場合は同一HTTP/2 connection上の後続RPCに対するconnection state / peer schedulingの変化として解釈する。

実装:

- `grpc_lite.active_bdp_probe=0` を既定値として追加。Cloud Spanner issue #5向けの診断・opt-in機能として扱う。
- `grpc_lite.active_bdp_probe_min_interval_ms=100` を既定値として追加。gRPC Core v1.58.0のBDP estimatorは初期inter-ping delay 100msから始めるため、opt-in既定値はこれに寄せる。
- `grpc_lite.active_bdp_probe_min_interval_ms=0` はmin intervalなし。ただしconnection単位でoutstanding PINGは1つまで。これはissue #5の再現確認用overrideとして残す。
- response DATA chunk受信時に `maybe_submit_active_bdp_probe()` を呼ぶ。
- `active_bdp_probe_outstanding`、`active_bdp_probe_opaque`、`active_bdp_probe_sent_at_us` をHTTP/2 connection stateに保持する。
- inbound PING ACKのopaqueが現在のclient-origin probe opaqueと一致した場合だけoutstandingを解除する。
- DATA callback内では `nghttp2_submit_ping()` だけ行い、inline waitしない。既存の `nghttp2_session_want_write()` / `send_pending_h2_frames()` に乗せてflushする。
- window size自動変更は入れない。

検証:

- PHPT: `./tools/test/check-phpt.sh` pass
- C static analysis: `./tools/test/check-c-static-analysis.sh` pass
- trace: `GRPC_LITE_TRACE_FILE` で outbound PING、server-origin PING、PING ACKの往復を確認

#### 計測結果

fixture:

- script: `tools/diagnostics/issue5-spanner-repro/select1-bench.php`
- image: `spanner-repro:lite-local-active-bdp-probe`
- target: real Cloud Spanner `bench/laravel-bench-db`
- credentials: service account JSON via `GOOGLE_APPLICATION_CREDENTIALS=/sa.json`
- iterations: 500

| variant | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| official ext-grpc 1.58 | 500 | 11.047ms | 10.387ms | 13.671ms | 19.268ms | 同時期基準 |
| lite active BDP probe off | 500 | 22.974ms | 21.485ms | 28.462ms | 39.604ms | 同一imageでINI無効 |
| lite active BDP probe on, min interval 0ms | 500 | 15.173ms | 14.706ms | 17.308ms | 22.592ms | 大きく改善 |
| lite active BDP probe on, min interval 1000ms | 500 | 24.238ms | 21.845ms | 31.322ms | 64.248ms | 改善なし |

2026-05-20に、PR #6のsource-built `grpc.so` で100ms intervalも追加計測した。

| variant | iter | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---:|---|
| official ext-grpc 1.58 default | 500 | 10.691ms | 10.398ms | 12.162ms | 15.728ms | 比較基準 |
| official ext-grpc 1.58 `grpc.http2.bdp_probe=0` | 500 | 11.372ms | 11.244ms | 12.827ms | 15.812ms | BDP offでも速い |
| lite source active off | 500 | 18.954ms | 18.576ms | 20.128ms | 23.715ms | baseline |
| lite source active on, `0ms` | 500 | 15.506ms | 15.221ms | 17.316ms | 24.072ms | 明確に改善 |
| lite source active on, `10ms` | 500 | 14.716ms | 14.574ms | 16.075ms | 19.511ms | 0msに近い改善 |
| lite source active on, `100ms` | 500 | 20.178ms | 20.450ms | 22.547ms | 27.274ms | 改善なし |

同じsource-built `grpc.so` で、順序を入れ替えながら1000 iterations x 3 roundを追加計測した。

| variant | rounds | mean avg | p50 avg | p90 avg | p99 avg | 判断 |
|---|---:|---:|---:|---:|---:|---|
| lite source active off | 3 | 19.660ms | 19.279ms | 20.730ms | 25.015ms | baseline |
| lite source active on, `0ms` | 3 | 14.696ms | 14.120ms | 15.529ms | 21.641ms | 明確に改善 |
| lite source active on, `10ms` | 3 | 14.537ms | 14.263ms | 15.769ms | 20.339ms | 明確に改善、0msと同レンジ |
| lite source active on, `100ms` | 3 | 18.832ms | 19.000ms | 20.566ms | 25.293ms | offと同レンジ |

参考: 最初に試した「connectionごとに1回だけprobe」は `mean=19.953ms / p50=19.439ms / p90=21.988ms / p99=29.105ms` で、改善はあるが効果が弱かった。

判断:

- `one outstanding` 制約だけで重複PINGは避けつつ、ACK後に再armする形がSpanner SELECT 1では最も効く。
- 1回/connectionや1000ms intervalでは、reporterが見た改善幅に近づかない。
- 現時点では、`active_bdp_probe_min_interval_ms=0` または `10` はissue #5の診断overrideとして価値がある。今回の反復平均では10msがわずかに良いが、0ms/10msは同レンジであり優劣は未確定として扱う。100msはCore初期値に近いが、単純active PING実装ではSpanner `SELECT 1` の改善を示さなかった。ただしdefault-onにする根拠はなく、production defaultはoffに戻す。
- official ext-grpcとの差はまだp50で約4.3ms残るため、BDP probeだけで完全解決ではない。

### 主要ベンチ再計測: default on の副作用

`active BDP probe` を既定有効にした状態で主要ベンチを再計測した。

run ids:

- `active-bdp-major-20260519`: `spanner-shape`
- `active-bdp-real-client-20260519`: `spanner-real-client`
- `active-bdp-rtt-20260519`: `rtt-unary`
- `active-bdp-streaming-20260519`: `throughput-streaming`
- `active-bdp-unary-20260519`: `throughput-unary`
- `active-bdp-tls-spanner-shape-20260519`: `tls-spanner-shape`

#### spanner-shape

| measurement | native p50 | native p99 | ext-grpc p50 | ext-grpc p99 | 判断 |
|---|---:|---:|---:|---:|---|
| begin_txn_unary | 59.6µs | 940.2µs | 162.9µs | 665.5µs | native p50優位、p99劣後 |
| commit_txn_unary | 54.1µs | 984.5µs | 116.5µs | 468.3µs | native p50優位、p99劣後 |
| select_1row_10col_streaming | 63.0µs | 938.7µs | 148.1µs | 506.0µs | native p50優位、p99劣後 |
| dml_insert_10col_streaming | 62.5µs | 780.9µs | 156.6µs | 407.5µs | native p50優位、p99劣後 |
| dml_update_10col_streaming | 62.3µs | 1013.3µs | 88.6µs | 544.1µs | native p50優位、p99劣後 |
| dml_delete_10col_streaming | 58.4µs | 872.3µs | 165.1µs | 402.3µs | native p50優位、p99劣後 |

同一コードで `grpc_lite.active_bdp_probe=0` にした切り分け:

| measurement | native off p50 | native off p99 | default on p50 | default on p99 |
|---|---:|---:|---:|---:|
| begin_txn_unary | 68.1µs | 747.8µs | 59.6µs | 940.2µs |
| commit_txn_unary | 56.3µs | 492.9µs | 54.1µs | 984.5µs |
| select_1row_10col_streaming | 51.3µs | 417.6µs | 63.0µs | 938.7µs |
| dml_insert_10col_streaming | 48.9µs | 327.8µs | 62.5µs | 780.9µs |
| dml_update_10col_streaming | 47.4µs | 251.3µs | 62.3µs | 1013.3µs |
| dml_delete_10col_streaming | 47.1µs | 256.8µs | 58.4µs | 872.3µs |

判断:

- Go test-serverの低RTT synthetic Spanner shapeでは、active BDP probe default onは特にstreaming系p99を悪化させる。
- この悪化は同一コードでINI無効にすると大きく戻るため、probe由来と見てよい。

#### tls-spanner-shape

| measurement | native p50 | native p99 | ext-grpc p50 | ext-grpc p99 | 判断 |
|---|---:|---:|---:|---:|---|
| begin_txn_unary | 64.7µs | 3845.1µs | 131.8µs | 485.8µs | native p50優位、p99大幅劣後 |
| commit_txn_unary | 53.0µs | 2837.4µs | 108.4µs | 490.0µs | native p50優位、p99大幅劣後 |
| select_1row_10col_streaming | 65.1µs | 2932.6µs | 164.0µs | 497.2µs | native p50優位、p99大幅劣後 |
| dml_insert_10col_streaming | 72.3µs | 3145.6µs | 133.3µs | 455.5µs | native p50優位、p99大幅劣後 |
| dml_update_10col_streaming | 48.3µs | 2730.8µs | 154.5µs | 529.6µs | native p50優位、p99大幅劣後 |
| dml_delete_10col_streaming | 55.1µs | 2903.0µs | 136.0µs | 559.5µs | native p50優位、p99大幅劣後 |

#### spanner-real-client

| measurement | native p50 | native p99 | ext-grpc p50 | ext-grpc p99 | 判断 |
|---|---:|---:|---:|---:|---|
| small_select_1row_10col | 2167.6µs | 3462.5µs | 1977.6µs | 3553.1µs | p50 native劣後、p99同等 |
| dml_insert_10col | 2342.5µs | 6951.7µs | 1873.1µs | 3194.9µs | native劣後 |
| dml_update_10col | 2550.4µs | 4849.6µs | 2259.2µs | 3160.6µs | native劣後 |
| dml_delete_10col | 2848.8µs | 4888.4µs | 2588.7µs | 3552.2µs | native劣後 |

同一コードで `grpc_lite.active_bdp_probe=0` にした切り分け:

| measurement | native off p50 | native off p99 | default on p50 | default on p99 |
|---|---:|---:|---:|---:|
| small_select_1row_10col | 1698.9µs | 4512.9µs | 2167.6µs | 3462.5µs |
| dml_insert_10col | 1725.1µs | 2885.5µs | 2342.5µs | 6951.7µs |
| dml_update_10col | 2051.9µs | 2584.3µs | 2550.4µs | 4849.6µs |
| dml_delete_10col | 2356.9µs | 3737.0µs | 2848.8µs | 4888.4µs |

判断:

- emulator高レベル実経路でもdefault onはp50を悪化させる。
- Real Cloud Spanner `SELECT 1` では大きく改善する一方、local/emulatorとGo test-serverでは副作用が大きい。

#### throughput / rtt

| suite | measurement | native p50 | native p99 | ext-grpc p50 | ext-grpc p99 | 判断 |
|---|---|---:|---:|---:|---:|---|
| throughput-unary | payload=100 | 50.4µs | 835.7µs | 112.4µs | 469.2µs | native p50優位、p99劣後 |
| throughput-streaming | payload=100 | 1605.6µs | 4906.7µs | 4489.3µs | 7379.3µs | native優位 |
| rtt-unary | warm direct | 160.0µs | 1220.0µs | 182.5µs | 265.0µs | p50同等、native p99劣後 |

#### 方針判断

`active BDP probe` はReal Cloud Spannerでは有効だが、低RTT synthetic / emulatorでは明確な副作用がある。したがって、現時点でdefault onのまま進めるのは危険。

次の候補:

1. `grpc_lite.active_bdp_probe` の既定値を `0` に戻し、Cloud Spanner向けのopt-in機能にする。
2. default onを維持するなら、少なくとも低RTT/localhost/emulatorで自動抑制するre-arm policyが必要。
3. provider/domain-specific auto enableは責務が重いため、まずはopt-inが安全。
