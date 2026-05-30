# HTTP/2 / gRPC Domain Model Review Guide

HTTP/2 transport / gRPC client implementationを、プロトコル仕様のドメインモデルとしてレビューする専用ロール向けガイド。

このレビューの目的は、コードが「動くか」「Cとして安全か」「ext-grpcと表面互換か」だけでなく、HTTP/2 と gRPC の概念、責務、命名、状態遷移が実装構造に正しく写像されているかを確認すること。性能レビュー、メモリ安全レビュー、API surface互換レビューとは別枠で実施する。

## 1. レビューの主眼

このロールは、特定のcontrol frameだけを見るのではなく、次を横断的に見る。

- HTTP/2 / gRPC の仕様語彙が、変数名・関数名・構造体名に正しく反映されているか。
- gRPC Channel、HTTP/2 Connection、HTTP/2 Stream、gRPC Call、PHP resource の責務が混ざっていないか。
- transport層、gRPC protocol層、PHP wrapper adapter層の副作用境界が明確か。
- flow-control、metadata、status、deadline、cancel、TLS、persistent lifecycleがそれぞれ正しいscopeに置かれているか。
- unary / server streaming の違いが call orchestration の違いとして表現され、HTTP/2 stream modelを壊していないか。
- 仕様上の状態遷移に対するテストがあるか。

## 2. レビュー対象

主対象:

- `src/internal.h`
- `src/transport.c`
- `src/wrapper_adapter.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- HTTP/2 / gRPC のfixtureを持つ `poc/test-server/main.go`
- HTTP/2 / gRPC 制御・互換性の統合テスト

対象外ではないが、このレビューの主目的ではないもの:

- PHP APIの通常互換性だけを見るレビュー
- protobuf serialize / deserialize性能
- release packaging / install手順
- ASAN/Valgrind/cppcheckで検出できる一般的なC品質問題
- ベンチ数値の優劣判断

## 3. まず作るドメインモデル

コードを読む前に、変更対象を次のdomain objectへ分類する。

| domain object | 意味 | 所有してよいstate | 所有してはいけないstate |
|---|---|---|---|
| gRPC Channel | logical target / credentials / authority / channel options | target, credentials, authority, max message/metadata option, persistent identity key | socket fd, nghttp2 stream progress, response body |
| HTTP/2 Connection | TCP/TLS socket + nghttp2 session | fd, ssl, session, ALPN, GOAWAY/draining, dead, receive window | PHP method, protobuf payload, per-call status |
| HTTP/2 Stream | 1 RPCが載るHTTP/2 stream | stream id, half-close, RST_STREAM, per-stream flow-control, stream-local error | channel identity, persistent cache ownership |
| gRPC Call | gRPC RPCのsemantic state | method path, deadline, request metadata, response metadata, status, message limit | raw socket lifecycle, global channel cache |
| PHP Call Object | official wrapperから見える低レベルobject | `Grpc\Call` state, batch op progress, PHP-visible response/status | nghttp2 session ownership |
| Server Streaming Resource | PHP Generator pullに対応するresource | queued messages, cancel/completed, active stream ownership | connection identity, unrelated stream state |
| Transport Cache | process/thread-local persistent resource table | connection entries keyed by channel identity | per-call response/status |

レビューでは、struct / function / variable がどのdomain objectに属するかを必ず説明する。複数domainをまたぐ関数は orchestration 関数として扱い、どの副作用を委譲しているかを見る。

## 4. 命名レビュー

命名は単なる読みやすさではなく、仕様モデルが崩れていないかの主要な検査対象。

確認すること:

- `channel` は gRPC Channel identity / options を指しているか。
- `connection` は HTTP/2 session/socket を指しているか。
- `stream` は HTTP/2 stream か、server streaming call か、名前で区別できているか。
- `call` は gRPC RPC semantic state か、PHP `Grpc\Call` objectか、混同していないか。
- `transport` はHTTP/2接続制御の層を指しているか。
- `metadata` はrequest / initial / trailing / status metadataのどれかが名前で分かるか。
- `deadline`, `timeout`, `grpc-timeout` がPHP option、wire metadata、transport deadlineのどれか分かるか。
- `native`, `h2`, `grpc`, `lite` などprefixが実装都合で残っていないか。

危険な兆候:

- `h2_connection` がgRPC Channel identityを直接所有する。
- `channel` 変数がHTTP/2 connection pointerを指す。
- `stream` がHTTP/2 stream idとserver streaming resourceの両方を曖昧に指す。
- `transport` 関数がgRPC status objectを直接組み立てる。
- wrapper adapter関数がconnection cacheを直接壊す。
- `grpc_*` prefixがHTTP/2だけの処理にも付いている、またはその逆。

## 5. 責務分離レビュー

層ごとに持つべき責務を確認する。

| layer | 持つ責務 | 持たない責務 |
|---|---|---|
| PHP surface | class registration, object lifecycle, option parsing | socket I/O, HTTP/2 frame parsing |
| Bridge | official wrapper batch opをgRPC callへ写像、status/metadataをPHP shapeへ変換 | HTTP/2 connection lifecycleの判断 |
| gRPC protocol | gRPC 5B frame、metadata semantics、status/trailer semantics、compression policy | TCP/TLS/nghttp2 session ownership |
| HTTP/2 transport | socket/TLS、nghttp2 session、HEADERS/DATA/RST/GOAWAY/WINDOW_UPDATE、connection cache | protobuf decode、PHP response object construction |
| Unary orchestration | unary request/response lifecycle | server streaming queue ownership |
| Server streaming orchestration | pull-based response delivery、cancel、queue/backpressure | unrelated unary status handling |

レビュー質問:

- gRPC protocol parserがconnection cacheを直接操作していないか。
- Bridgeがprotocol failure一般をconnection discardへ畳んでいないか。
- TransportがPHP wrapperの都合を知りすぎていないか。
- Unary / server streaming の違いがHTTP/2 connection構造体に漏れていないか。
- persistent cacheのkeyはgRPC Channel identityだけから構成されているか。

## 6. Scope別stateレビュー

stateは最小scopeに置く。

| state | 正しいscope |
|---|---|
| target / authority / TLS verify name / credentials hash | gRPC Channel / persistent identity |
| fd / SSL / nghttp2 session / negotiated protocol | HTTP/2 Connection |
| GOAWAY last stream id / connection dead / draining | HTTP/2 Connection |
| stream id / stream reset code / half close | HTTP/2 Stream / gRPC Call |
| grpc-status / grpc-message / response metadata | gRPC Call |
| queued payloads / slow consumer counters | Server Streaming Resource |
| max receive message / max metadata | Channel option copied into Call |
| stream window / connection window | HTTP/2 Connection setup / nghttp2 settings |
| deadline absolute time | Call, applied by transport I/O |

危険な兆候:

- connection globalに1つだけactive stream stateがあり、将来のmultiplexを構造的に阻害している。
- call stateにsocket close ownershipがある。
- server streaming resource destructorがconnection closeの責務まで持つ。
- metadata limitがglobal constantだけでchannel optionへ写像されていない。

## 7. Lifecycle / Error Taxonomy

すべてのerror/control pathを分類する。

| 分類 | connection扱い | 例 |
|---|---|---|
| stream-local failure | streamを閉じる。connectionは原則再利用可能 | server/client RST_STREAM, message too large, metadata too large, unsupported compression, malformed gRPC frame |
| connection failure | connectionをdead/drainingにし、cacheから外す | TCP EOF, TLS error, nghttp2 connection error, GOAWAY, ALPN failure |
| call semantic failure | gRPC statusとして返す。transportは正常 | application `grpc-status != 0`, trailers-only error |
| API misuse | PHP例外または事前validation | invalid method path, invalid metadata key, invalid channel option |

レビュー質問:

- stream-local failureをconnection failureとして扱っていないか。
- connection failureをcall semantic failureへ畳んで隠していないか。
- GOAWAY後に新規streamを同connectionへ載せていないか。
- EOF/TLS/socket error後にconnectionをcacheへ残していないか。
- stream-local failure後にstream user data / active stream tableから解除されるか。
- transport層で同じRPCを暗黙retryしていないか。

## 8. HTTP/2 modelレビュー

HTTP/2 transportとして最低限以下を確認する。

- clientが各RPCでHTTP/2 streamを作り、HEADERS + DATA + END_STREAMを送る。
- unary / server streamingでもclient request sideは単一messageで基本的に早期half-closeする。
- server response HEADERS / DATA / trailers HEADERSを正しく区別する。
- `RST_STREAM` はstream-localとして扱う。RSTにRSTで応答しない。
- `GOAWAY` はconnection drainingとして扱い、新規RPCには使わない。
- `SETTINGS_INITIAL_WINDOW_SIZE` はstream receive windowである。
- connection receive windowはconnection-level `WINDOW_UPDATE` で別途扱う。
- receive buffer sizeとflow-control windowを混同しない。
- pending control frame、ACK、WINDOW_UPDATEを必要なタイミングでflushする。
- nghttp2 callback user_dataのlifetimeがactive call/resourceのlifetimeを超えない。

## 9. gRPC modelレビュー

gRPC over HTTP/2として最低限以下を確認する。

- requestは `POST`、`:path`、`:authority`、`content-type: application/grpc`、`te: trailers` を持つ。
- request/response messageは `1 byte compressed-flag + 4 byte big-endian length + payload`。
- `grpc-timeout` はcall deadlineから生成され、user metadataからは上書きさせない。
- `user-agent`、`grpc-status`、`grpc-message`、`grpc-status-details-bin` などlibrary-owned metadataをuser metadataから送らない。
- response `content-type` を検証する。
- `grpc-status` / `grpc-message` / `grpc-status-details-bin` はtrailers semanticsで扱う。
- trailers-only errorを扱う。
- compression未対応時は明示statusに変換する。
- duplicate metadata values / binary metadataのshapeがSPECと一致している。
- response message size / metadata size limitはgRPC statusへ変換される。

## 10. Call type modelレビュー

unary / server streaming の差を、gRPC call orchestrationとして見る。

| call type | 共通点 | 固有点 |
|---|---|---|
| Unary | 1 HTTP/2 stream、1 request message、gRPC status trailers | response messageは最大1つ。完了時にpayload/statusをPHPへ返す |
| Server streaming | 1 HTTP/2 stream、1 request message、gRPC status trailers | response messageを複数回pull delivery。cancel/destructor/backpressureがある |

レビュー質問:

- server streamingを「複数HTTP/2 stream」と誤解していないか。
- unaryの「response最大1message」制約がgRPC frame parserに反映されているか。
- server streamingでmessage単位のdeliveryとtrailing status取得が両立しているか。
- slow consumer時に無制限にread/queueしないか。
- cancel時にstreamだけを閉じ、connectionを過剰に捨てていないか。

## 11. 必須テスト観点

ドメインモデルの境界はhappy pathとは別に固定する。

| 項目 | unary | server streaming |
|---|---:|---:|
| normal response + trailers | 必須 | 必須 |
| trailers-only error | 必須 | 必須 |
| server-sent RST_STREAM | 必須 | 必須 |
| client cancel / client-sent RST_STREAM | 必須 | 必須 |
| GOAWAY / draining | 必須 | 必須 |
| EOF / socket close | 必須 | 必須 |
| malformed gRPC frame | 必須 | 必須 |
| message too large | 必須 | 必須 |
| metadata too large | 必須 | 必須 |
| unsupported compression | 必須 | 必須 |
| invalid content-type | 必須 | 必須 |
| deadline during connect/send/recv | 必須 | 必須 |
| duplicate/binary metadata | 必須 | 必須 |
| slow consumer memory bound | 対象外 | 必須 |

各テストは可能なら次を確認する。

- expected status code / details
- initial/trailing/status metadata shape
- response/message count
- stream/resource completed
- connection dead/draining有無
- follow-up RPC成功
- persistent reuseの有無

## 12. レビュー出力フォーマット

レビュー結果は次の形式で出す。

```text
Blocker
- [domain/scope] 指摘内容
  - 仕様/設計上の根拠:
  - 現コードの問題:
  - 必要な修正:
  - 必要なテスト:

High
- ...

Medium
- ...

Design Decision / Accept
- 見送る判断:
  - 理由:
  - 再検討条件:
```

重要: 「ext-grpcと違う」だけでは指摘にしない。HTTP/2/gRPC仕様、gRPC Coreの設計思想、またはこのrepositoryのdesign docに照らして、どのdomain modelが壊れているかを説明する。

## 13. レビュー前チェックリスト

- [ ] 変更対象をdomain objectへ分類した。
- [ ] struct / function / variableの名前がHTTP/2/gRPC語彙と矛盾しないことを確認した。
- [ ] stateが最小scopeに置かれていることを確認した。
- [ ] transport / gRPC protocol / PHP wrapper adapter の責務境界を確認した。
- [ ] stream-local failure / connection failure / call semantic failure / API misuseを分類した。
- [ ] flow-control windowとbuffer sizeを分けて確認した。
- [ ] unaryとserver streamingの違いがHTTP/2 stream modelを壊していないことを確認した。
- [ ] control frameだけでなくmetadata/status/deadline/TLS/persistent lifecycleも見た。
- [ ] 対応する統合テストまたはfixtureがあることを確認した。
