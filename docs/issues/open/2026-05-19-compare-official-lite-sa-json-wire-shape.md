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
