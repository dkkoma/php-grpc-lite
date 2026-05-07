# Protocol Model Review Guide

HTTP/2 transport / gRPC protocol実装をレビューする専用ロール向けガイド。

このレビューの目的は、コードが「動くか」や「Cとして安全か」だけでなく、HTTP/2 と gRPC の状態機械・責務境界を正しくモデル化しているかを確認すること。性能、メモリ安全、API surface互換レビューとは別枠で実施する。

## 1. レビュー対象

主対象:

- `ext/grpc/transport.c`
- `ext/grpc/unary_call.c`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/bridge.c`
- `ext/grpc/internal.h`
- HTTP/2 / gRPC 制御系に触る test-server fixture と統合テスト

対象外ではないが、このレビューの主目的ではないもの:

- PHP object API の通常互換性
- protobuf serialize / deserialize性能
- packaging / install手順
- C memory checkerで検出できる一般的なメモリ問題

## 2. 最初に作るべきモデル

コードを読む前に、現在の変更が次のどのscopeに触るかを明示する。

| scope | 意味 | 代表state |
|---|---|---|
| gRPC Channel | logical target / credentials / authority / options identity | target, credentials, max message/metadata option |
| HTTP/2 Connection | TCP/TLS socket + nghttp2 session | fd, ssl, session, GOAWAY/draining, dead |
| HTTP/2 Stream | 1 RPCが載るHTTP/2 stream | stream id, half-close, RST_STREAM, flow-control |
| gRPC Call | PHP wrapperから見える1 call | method, deadline, metadata, request, response/status |
| Server Streaming Resource | PHP Generator pullに対応するstream state | queue, cancel, completed, owner/busy |

レビューでは、各struct / function / variableがこのscopeのどれに属するかを確認する。複数scopeをまたぐ場合は、所有権と副作用を明示できている必要がある。

## 3. 命名レビュー

命名は読みやすさではなく、仕様モデルの検証対象として扱う。

確認すること:

- `channel` は gRPC Channel identity を指しているか。
- `connection` は HTTP/2 session/socket を指しているか。
- `stream` は HTTP/2 streamまたはserver streaming固有概念として一貫しているか。
- `call` は gRPC/PHP call stateとして使われているか。
- `transport` はHTTP/2接続制御の層を指しているか。
- `native`, `h2`, `grpc` など実装詳細/プロトコル名/prefixが混在していないか。

危険な兆候:

- HTTP/2 connection構造体がgRPC Channel identityを直接持つ。
- connectionに単一server streaming stateが埋め込まれている。
- `channel` 変数がHTTP/2 connection pointerを指している。
- `stream` という名前がserver streaming call resourceとHTTP/2 stream idの両方を曖昧に指す。
- 実装都合の名前が仕様概念より優先されている。

## 4. Lifecycle分類

すべてのerror/control pathを、必ず次のどちらかへ分類する。

| 分類 | connection扱い | 例 |
|---|---|---|
| stream-local failure | streamを閉じる。connectionは原則再利用可能 | RST_STREAM, message too large, metadata too large, unsupported compression, malformed gRPC frame |
| connection failure | connectionをdead/drainingにし、cacheから外す | TCP EOF, TLS error, nghttp2 connection error, GOAWAY, ALPN failure |

レビュー質問:

- このfailureはHTTP/2 stream errorかconnection errorか。
- `RST_STREAM` を受けた/送っただけでconnectionをdeadにしていないか。
- GOAWAYを受けた後、新規streamを同connectionに載せていないか。
- EOF/TLS/socket error後にconnectionをcacheへ残していないか。
- stream-local failure後にowner/busyが解除されるか。
- 同じRPCをtransport層で暗黙retryしていないか。

## 5. RST_STREAMレビュー

`RST_STREAM` はstream単位の制御frameであり、connection closeではない。

分けて確認すること:

| direction | 意味 | 必要な処理 |
|---|---|---|
| client-sent RST_STREAM | clientがそのRPCを中止する | stream cancel、status化、connectionはdeadにしない |
| server-sent RST_STREAM | server/proxyがそのRPCを中止する | status mapping、stream close、busy解除、connectionはdeadにしない |

必須確認:

- server-sent `RST_STREAM(REFUSED_STREAM)` は `UNAVAILABLE` へ変換する。
- server-sent `RST_STREAM(CANCEL)` は `CANCELLED` へ変換する。
- `RST_STREAM` 受信後にclientから不要な追加RSTを返していない。
- unary / server streaming 両方でfollow-up RPCが成功する。
- diagnostic可能ならfollow-up RPCでpersistent connection reuseを確認する。

## 6. Flow-control / Windowレビュー

HTTP/2 windowとbufferを混同しない。

| 項目 | 意味 | レビュー観点 |
|---|---|---|
| stream receive window | peerが特定streamに送れる未消費DATA量 | `SETTINGS_INITIAL_WINDOW_SIZE` で制御されているか |
| connection receive window | peerがconnection全体に送れる未消費DATA量 | connection-level `WINDOW_UPDATE` で制御されているか |
| receive buffer size | socketから1回に読むbufferサイズ | windowとは別。大きくしてもflow-controlは広がらない |
| DATA frame size | HTTP/2 frame分割単位 | `SETTINGS_MAX_FRAME_SIZE` / peer設定に依存 |

レビュー質問:

- stream window と connection window の両方を意識しているか。
- 65,535 bytes defaultを前提にしてlarge responseの挙動を説明できるか。
- window sizeが設定可能な場合、HTTP/2範囲へ丸めているか。
- `WINDOW_UPDATE` / ACK を必要なタイミングでflushしているか。
- slow consumer時に無制限read-aheadでmemoryが増えないか。
- receive buffer size変更をflow-control改善と誤認していないか。

## 7. gRPC protocolレビュー

gRPC over HTTP/2として必要な処理をHTTP/2 transport処理と混ぜて見落とさない。

確認すること:

- requestは `POST`、`:path`、`:authority`、`content-type: application/grpc`、`te: trailers` を持つ。
- request bodyは `1 byte compressed-flag + 4 byte big-endian length + payload`。
- response `content-type` を検証する。
- response DATAのgRPC 5B frameをmessage単位で検証する。
- `grpc-status` / `grpc-message` / `grpc-status-details-bin` はtrailers semanticsで扱う。
- trailers-only errorを扱う。
- compression未対応時は明示statusに変換する。
- response message size / metadata size limitはstatusへ変換し、stream-local failureとして扱う。
- request metadataはpseudo headerやlibrary-owned headerをuser指定から除外する。

## 8. Cross-layer ownership

protocol層とtransport層の副作用を分けて確認する。

望ましい形:

- protocol層は「status」「metadata」「stream cancelが必要」という判断を返す。
- transport層は `nghttp2_submit_rst_stream()`、`nghttp2_session_send()`、connection dead/draining/cache操作を行う。
- PHP bridge層はresultを `Grpc\Status` / response objectへ変換する。

危険な形:

- protocol parserがconnection cacheを直接操作する。
- bridge層がprotocol failure一般をconnection discardへ畳む。
- transport層がgRPC status semanticsを知らないままconnection lifecycleを決める。
- server streaming resource destructorがblocking I/Oに依存する。

## 9. 必須テスト観点

HTTP/2/gRPC制御系はhappy pathとは別に固定する。

| 項目 | unary | server streaming |
|---|---:|---:|
| GOAWAY後に新規RPCで新connection | 必須 | 必須 |
| EOF / socket close | 必須 | 必須 |
| server-sent RST_STREAM | 必須 | 必須 |
| client cancel / client-sent RST_STREAM | 必須 | 必須 |
| message too large | 必須 | 必須 |
| metadata too large | 必須 | 必須 |
| malformed gRPC frame | 必須 | 必須 |
| unsupported compression | 必須 | 必須 |
| deadline during connect/send/recv | 必須 | 必須 |
| slow consumer memory bound | 対象外 | 必須 |

各テストは可能なら次を確認する。

- expected status code
- response/message count
- stream/resource completed
- connection dead/draining有無
- follow-up RPC成功
- persistent reuseの有無

## 10. レビュー出力フォーマット

レビュー結果は次の形式で出す。

```text
Blocker
- [scope] 指摘内容
  - 仕様上の根拠:
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

重要: 「ext-grpcと違う」だけでは指摘にしない。HTTP/2/gRPC仕様、gRPC Coreの設計思想、またはこのrepositoryのdesign docに照らして、どのstate modelが壊れているかを説明する。

## 11. レビュー前チェックリスト

- [ ] connection / stream / call / channelの責務表を作った。
- [ ] 変更対象のstructと関数をscope分類した。
- [ ] stream-local failureとconnection failureを分類した。
- [ ] flow-control windowとbuffer sizeを分けて確認した。
- [ ] client-sent RSTとserver-sent RSTを分けて確認した。
- [ ] GOAWAY / EOF / TLS errorのconnection lifecycleを確認した。
- [ ] unaryとserver streamingの両方で制御系テストを確認した。
- [ ] namingがHTTP/2/gRPC仕様語彙と矛盾していないことを確認した。
