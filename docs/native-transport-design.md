# Native Transport Design

## Goal

公式 `ext-grpc` のdrop-in surfaceを保ちながら、transport内部をlibcurlからnative nghttp2へ置き換える。

目的はext-grpcの完全再実装ではなく、Phase 2で確認した性能上の本筋改善をproduction extensionへ移すこと。

## Architecture

```text
Grpc\BaseStub
  -> UnaryCall / ServerStreamingCall
    -> NativeChannelTransport
      -> NativeHttp2Session
        -> nghttp2_session
        -> socket / TLS stream
        -> stream state map
```

native transport は `Grpc\Channel` に紐づく HTTP/2 transport resource として実装する。`UnaryCall` / `ServerStreamingCall` は transport backend を直接意識せず、既存 `Grpc\` API surface を維持する。

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

最初のproduction実装では1 session内1 active streamでもよい。ただし構造は後でmultiple active streamsへ拡張できるように分離する。

## Channel Lifetime

- Channel identityからC側persistent channel keyを作る。
- HTTP/2 session/socketはC extensionのprocess-local / thread-local cacheに保持する。
- PHP userlandではchannel resource/socket/sessionを保持しない。
- error/cancel/途中終了時はC側persistent channelをdead扱いにし、次RPCで新規接続する。
- server streamingはC stream resourceをPHP Generatorがpullし、messageごとにyieldする。
- slow consumer時はPHPが次messageを要求するまで追加readとWINDOW_UPDATE送信を進めず、transport側でbackpressureをかける。
- `cancel()` はactive streamへ `RST_STREAM(CANCEL)` を送る。

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

transportはnative nghttp2の1系統とする。libcurl fallback / transport selection option / environment based switching は持たない。

```php
new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);
```

native未対応機能、native extension未ロード、transport errorは黙って別経路へ落とさず、native transportの失敗として扱う。

## Channel Lifecycle

native transportはChannel lifetimeに対応するHTTP/2 sessionをC側のpersistent channelとして保持する。通常のRPCは同じsession上に新しいstreamを作る。Phase 2 MVPでは1 session内1 active streamを前提にし、concurrent stream schedulingはshared event loop段階へ残す。

FPMではworker process内でrequestをまたいでpersistent channelを再利用する。ZTS / FrankenPHP workerではthread-local module globals上のcacheとして扱い、threadをまたいでsocket/sessionを共有しない。PHP userlandではchannelを保持しない。

connectionが壊れた場合、transport層では同じRPCを自動retryしない。send/recv errorやEOFはchannelをdead扱いにし、そのRPCはエラーとして返す。GOAWAYを受けたchannelはdraining扱いにし、新規RPCには使わない。次のRPC開始時に新しいchannel resourceを作る。

retry policyやidempotency判断はtransport lifecycleとは分離し、将来の明示機能として扱う。

native resourceの所有権はC extension側に閉じる。`grpc_call` per-call stateが持つbody buffer、queued payload、metadata、`grpc-message` はcall完了・failure・resource destructorのいずれでも同じcleanup pathを通す。stream resource destructorは未完了streamへ `RST_STREAM(CANCEL)` を送り、channel busy状態を解除する。

response message sizeは `grpc.max_receive_message_length` で制御する。channel optionを基本とし、call optionで上書きできる。未指定時は64MiBを上限にし、`-1` は上限なしとして扱う。上限超過は `STATUS_RESOURCE_EXHAUSTED` として返し、unary pathではbody集約前にgRPC frame headerを検査して過大messageを捨てる。

## Multiplex PoC

HTTP/2 session自体は複数streamを同時にin-flightにできる。native extensionのbench/diagnostic entrypointである `grpc_native_multiplex_unary()` で同一session上に複数unary streamをsubmitし、全streamが独立に完了することを検証した。

2026-05-05のnative mux spikeでは、production pathを `h2_channel` owner + stream table dispatch に寄せれば、server streamingを開いたまま同一channel上に別server streaming / unaryをsubmitできることを確認した。ただしmain比でsmall unary / small streamingに退行が出たため、現時点ではmainへ採用しない。

現行のpublic PHP `Grpc\` surfaceは `wait()` / `responses()` が同期blockingなので、FPM / thread-local FrankenPHP の主用途では共有event loop / multiplex schedulerをrelease default gateにしない。async runtime、同一実行コンテキスト内の並列RPC、またはtransport専用threadを扱う段階で、単一active stream fast pathを維持できることを条件に再検討する。

## Compatibility Work

native transportで通す必要があるもの:

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

互換性・制御系は `docs/compatibility-control-checklist.md` に沿って追加する。Phase 2ではnative surfaceでTLS/mTLS、metadata/status/compression/error semanticsの代表条件を検証済み。実装進捗や検証途中の制限は `docs/research/` に残し、このdesign docには最終設計として満たすべき形だけを書く。

## Benchmark Requirements

native transportの採用・調整では、以下をactual `Grpc\` API surfaceで測る。

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

これらはnative unary / server streaming transportの後に、compatibilityとworkload優先度に応じて扱う。
