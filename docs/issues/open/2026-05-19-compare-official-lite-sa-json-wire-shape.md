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

### strace summary

同じSA JSON、同じDocker repro、20 iterationsで `strace -f -c` を取得した。strace自体のoverheadが大きいためlatency絶対値ではなく、syscall shapeの比較として扱う。

| item | official ext-grpc 1.58 | lite 0.0.8 | 判断 |
|---|---:|---:|---|
| latency mean | 27.4ms | 43.8ms | strace下でも差は残る |
| total syscalls | 3354 | 2879 | total数だけでは説明不可 |
| wait primitive | `epoll_pwait` 55, `futex` 120 | `ppoll` 51, `futex` 29 | officialはC-core pollset/thread寄り、liteは同期ppoll寄り |
| read path | `recvmsg` 56 + `read` 358 | `read` 547, error 89 | liteはOpenSSL BIO/file/read系に寄っている |
| write path | `sendmsg` 93 + `sendmmsg` 1 | `write` 91 + `sendmmsg` 1 | officialはsendmsg中心、liteはwrite中心 |

これは「内部syscall差がない」という結果ではない。むしろ、officialとliteではtransport scheduler / socket I/O primitiveの形が明確に違う。

ただし、現時点の `strace -c` は集計であり、どのRPCのどの区間で差が出たかまでは説明しない。次は `Commit` / `ExecuteStreamingSql` の単位で、send completionからresponse first byte / trailersまでを時系列で比較する必要がある。

## 次の作業

1. `Commit` / `ExecuteStreamingSql` のsyscall時系列を取り、send完了、poll wait、read/recv完了の差を見る。
2. tcpdumpまたはSSL key logが使える範囲で、wire上のrequest completion / response arrivalを確認する。
3. transport差で説明できない場合に限り、`grpc-accept-encoding`、`user-agent`、HPACK/header order/dynamic tableを診断variantとして試す。
