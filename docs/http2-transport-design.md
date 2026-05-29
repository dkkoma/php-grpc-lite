# HTTP/2 Transport Design

## Goal

公式 `ext-grpc` のdrop-in surfaceを保ちながら、HTTP/2 transportをC extension内で直接制御する。

目的はext-grpcの完全再実装ではなく、探索ベンチで確認した性能上の本筋改善をproduction extensionへ移すこと。

## Architecture

```text
Grpc\BaseStub
  -> UnaryCall / ServerStreamingCall
    -> Grpc\Call bridge
      -> grpc_channel
        -> h2_connection
          -> socket / TLS
          -> nghttp2_session
          -> active stream table
            -> grpc_call per stream
```

HTTP/2 transport は `Grpc\Channel` に紐づく persistent `h2_connection` として実装する。`grpc_channel` はgRPC channel identityとchannel optionを保持し、`h2_connection` はHTTP/2 connection resource(TCP/TLS socket、nghttp2 session、GOAWAY/draining/dead state)とactive stream tableを保持する。各HTTP/2 streamはnghttp2 stream user dataで `grpc_call` に対応し、DATA / HEADERS / RST_STREAM / stream close callbackはstream idから対象callへdispatchする。

## Request Path

```text
protobuf PHP string
  -> grpc header slice
  -> payload slice
  -> nghttp2 data provider
  -> NGHTTP2_DATA_FLAG_NO_COPY
  -> partial write state
  -> poll loop
```

Design requirements:

- request全体のlarge concatを避ける。
- gRPC 5B headerとpayloadを別sliceとして扱う。
- `EAGAIN` / partial write時は現在のDATA frame iovecとremaining bytesをstream stateに保持する。
- upload complete時刻を計測可能にする。

## Response Path

### Frame parser

DATA chunkを受けたらC側でgRPC frameをparseする。

```text
DATA
  -> 5B gRPC frame header
  -> compressed flag validation
  -> payload length
  -> payload assembly
  -> PHP message delivery
```

圧縮flagが1の場合、圧縮実装が入るまでは `STATUS_UNIMPLEMENTED` とする。

### compact/ring buffer

many-small / long stream向け。

```text
DATA
  -> ring/compact buffer
  -> complete frame検出
  -> payload slice/string
  -> consumed bytes解放
```

Requirements:

- append-only bufferは禁止。
- consumed bytesをstream完了まで保持しない。
- buffer上限とcompact thresholdを持つ。
- long streamで最大bufferがmessage size近辺に収まること。

### direct payload assembly

large payload向け。

```text
DATA
  -> frame headerでpayload length確定
  -> final payload buffer allocate
  -> DATAからfinal payloadへcopy
  -> complete後にPHPへ渡す
```

Requirements:

- `DATA -> body buffer -> payload` の二重copyを避ける。
- large single messageで中間body bufferを持たない。
- final payload buffer lifetimeはPHP delivery完了後に解放する。

## Stream State

各HTTP/2 streamは少なくとも以下を持つ。

- stream id
- method path
- request buffer/slices
- upload offset / pending write iovec
- response parse state
- response buffer or direct payload state
- initial metadata
- trailing metadata
- grpc status
- completion/error state
- diagnostics counters

現在のC実装では、1 RPC over 1 HTTP/2 streamの状態を `grpc_call` にまとめて保持する。`grpc_call` は純粋なgRPC semantic stateだけではなく、HTTP/2 stream id、request offset、response frame parser、metadata/status、diagnosticsを含む combined state である。connectionはdispatch用の `active_stream_count` / active stream listと、PHP call/resource lifetime用の `stream_owner_count` を分けて持つ。stream idごとのdispatchは `nghttp2_session_get_stream_user_data()` を使い、stream close後もPHP resourceがconnection pointerを持つ間はowner countでconnection破棄を遅延する。

## Channel Lifetime

- Channel identityからC側persistent connection cache keyを作る。
- HTTP/2 session/socketはC extensionのprocess-local / thread-local cacheに保持する。
- PHP userlandではchannel resource/socket/sessionを保持しない。
- socket/TLS/nghttp2 session errorやEOFなどのconnection failureはC側persistent connectionをdead扱いにし、次RPCで新規接続する。
- message size超過、metadata size超過、unsupported compression、malformed gRPC frame、invalid content-type、明示cancelなどのstream-local failureは該当streamを閉じ、connectionがusableなら次RPCで再利用する。
- server streamingはC stream resourceをPHP Generatorがpullし、messageごとにyieldする。
- client receive stream / connection windowは8MiBをdefaultにし、large responseでHTTP/2 flow-controlによる送信停止とWINDOW_UPDATE往復を減らす。どちらも `grpc_lite.http2_stream_window_size` / `grpc_lite.http2_connection_window_size` INIで調整可能にする。
- 初期HTTP/2 SETTINGSでは、`SETTINGS_MAX_FRAME_SIZE` をHTTP/2 defaultの16KiB、`SETTINGS_MAX_HEADER_LIST_SIZE` を64KiBとして明示する。どちらも `grpc_lite.http2_max_frame_size` / `grpc_lite.http2_max_header_list_size` INIで検証時に調整可能にする。
- slow consumer時はPHPが次messageを要求するまで追加readを進めない。別streamのI/Oで同じconnectionを読む場合は、server streaming payload queueをdefault 32 messages / 8MiBに制限し、超過時は対象streamを `RST_STREAM(CANCEL)` で閉じる。上限は `grpc_lite.server_streaming_read_ahead_max_messages` / `grpc_lite.server_streaming_read_ahead_max_bytes` INIで調整できる。
- `cancel()` は対象streamへ `RST_STREAM(CANCEL)` を送る。

Deferred:

- shared event loop / multiplex scheduler。

## PHP Surface

Public `Grpc\` APIは維持する。

- `UnaryCall::wait()`
- `ServerStreamingCall::responses()`
- `getMetadata()`
- `getTrailingMetadata()`
- `getStatus()`
- `cancel()`

Runtime transportはnghttp2のHTTP/2実装1系統とする。libcurl fallback、transport選択option、FrankenGrpc delegationは持たない。

```php
new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);
```

HTTP/2 transport未対応機能、source-built grpc extension未ロード、transport errorは黙って別経路へ落とさず、HTTP/2 transportの失敗として扱う。

## Channel Lifecycle

HTTP/2 transportはChannel lifetimeに対応するHTTP/2 sessionをC側のpersistent connection cacheに保持する。通常のRPCは同じsession上に新しいstreamを作る。connectionは複数active streamを表現できるが、public PHP surfaceは同期pull型なので、shared event loopによる積極的なconcurrent schedulingは別段階で扱う。active streamが残るconnectionのreuse時はidle preflightを行わず、次のsend/recvでsocket/TLS failureを検出する。

FPMではworker process内でrequestをまたいでpersistent connectionを再利用する。ZTS / FrankenPHP workerではthread-local module globals上のcacheとして扱い、threadをまたいでsocket/sessionを共有しない。PHP userlandではchannelを保持しない。

connectionが壊れた場合、transport層では同じRPCを自動retryしない。send/recv errorやEOFはconnectionをdead扱いにし、そのRPCはエラーとして返す。GOAWAYを受けたconnectionはdraining扱いにし、新規RPCには使わない。次のRPC開始時に新しいHTTP/2 connectionを作る。

stream-local failureはconnection failureと分ける。message size超過、metadata size超過、unsupported compression、malformed gRPC frame、invalid content-typeなどは該当streamへ `RST_STREAM` を送り、RPC statusへ変換する。nghttp2 session / socket がconnection errorになっていなければpersistent connectionは再利用可能として扱う。

retry policyやidempotency判断はtransport lifecycleとは分離し、将来の明示機能として扱う。

HTTP/2 resourceの所有権はC extension側に閉じる。`grpc_call` per-call stateが持つbody buffer、queued payload、metadata、`grpc-message` はcall完了・failure・resource destructorのいずれでも同じcleanup pathを通す。明示cancel時と未完了server streaming resource destructorでは、対象streamへ `RST_STREAM(CANCEL)` を送る。stream cancelはconnection failureではないため、socket/TLS/nghttp2 sessionがusableであればpersistent connectionは再利用する。

`timeout` / `grpc-timeout` はconnect、TLS handshake、RPC send/recvに適用する。ただしDNS解決はlibc `getaddrinfo()` の同期呼び出しであり、C extension内では途中中断できない。DNS解決が長時間blockする環境では、OS resolver設定、host cache、または将来のasync resolver/c-ares導入で扱う。

response message sizeは `grpc.max_receive_message_length` で制御する。channel optionを基本とし、call optionで上書きできる。未指定時は64MiBを上限にし、`-1` は上限なしとして扱う。上限超過は `STATUS_RESOURCE_EXHAUSTED` として返し、unary pathではbody集約前にgRPC frame headerを検査して過大messageを捨てる。

response metadata sizeは `grpc.max_metadata_size` / `grpc.absolute_max_metadata_size` channel optionで制御する。未指定時のhard limitは64KiB。超過時は `STATUS_RESOURCE_EXHAUSTED` とし、該当streamをcancelする。

response metadataは、公式ext-grpc PHP surfaceとの互換性を優先し、`content-type`、`grpc-status`、`grpc-message` などのprotocol-owned response headers/trailersもPHP metadata mapへ残す。request側は別方針で、library-owned headersをuser metadataから送信させない。

## Multiplex

HTTP/2 session自体は複数streamを同時にin-flightにできる。production pathはactive stream tableを持ち、server streamingを開いたまま同一HTTP/2 connection上に別unary / server streaming streamをsubmitできる。現行のpublic PHP `Grpc\` surfaceは `wait()` / `responses()` が同期blockingなので、transport専用threadやshared event loopによる自動read-ahead schedulingは持たない。connection readは呼び出し中RPCが駆動するため、別server streaming streamへ届いたpayloadはbounded queueへ積み、上限超過時はそのstreamをcancelしてmemory boundを守る。

## Compatibility Work

HTTP/2 transportで通す必要があるもの:

- unary / server streaming OK
- trailers-only error
- HTTP status to gRPC status mapping
- content-type validation
- `grpc-message` percent decode
- binary metadata request/initial/trailing
- duplicate metadata values are preserved as `array<string, list<string>>`
- client-side deadline
- cancel / early stream termination
- TLS / mTLS
- TLS verify / ALPN / mTLS handshake error detail
- max receive message length
- compression unsupported explicit error

互換性・制御系は `docs/compatibility-control-checklist.md` に沿って追加する。HTTP/2 surfaceではTLS/mTLS、metadata/status/compression/error semanticsの代表条件を検証済み。実装進捗や検証途中の制限は `docs/research/` に残し、このdesign docには最終設計として満たすべき形だけを書く。

## Benchmark Requirements

HTTP/2 transportの採用・調整では、以下をactual `Grpc\` API surfaceで測る。

- small SELECT相当のserver streaming p50/p99/messages/s
- Spanner DML相当のsmall unary flow p50/p99/calls/s
- large request unary p50/p99/calls/s
- server streaming many-small
- server streaming large payload
- server streaming long stream
- memory upper bound
- server timing trailerとの残差

transport内部の診断runnerは候補比較に使う。採用判断はpublic `Grpc\` API surfaceの測定を優先する。

## Non-goals

- client streaming / bidi streaming
- full ext-grpc channel args互換
- shared event loop / multiplex scheduler
- compression implementation

これらはHTTP/2 unary / server streaming transportの後に、compatibilityとworkload優先度に応じて扱う。
