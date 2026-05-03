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

### libcurl stable route

```text
protobuf PHP string
  -> "\0" + length + payload のPHP string
  -> CURLOPT_POSTFIELDS / READFUNCTION
  -> libcurl HTTP/2 upload path
```

### Native path

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

- Channelにnative transport resourceを持たせる。
- 同一PHP request内ではHTTP/2 session/socketを再利用する。
- error/cancel/途中終了時はsessionを破棄する。

Deferred:

- PHP-FPM worker lifetimeにまたがるpersistent native channel pool。
- shared event loop / multiplex scheduler。

## PHP Surface

Public `Grpc\` APIは維持する。

- `UnaryCall::wait()`
- `ServerStreamingCall::responses()`
- `getMetadata()`
- `getTrailingMetadata()`
- `getStatus()`
- `cancel()`

native transportをdefault経路とする。libcurl経路はfallbackではなく、ユーザーが明示選択できる安定経路として残す。

```php
new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'native',
]);

new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'curl',   // explicit stable route
]);
```

テストやベンチで経路を固定する場合は `PHP_GRPC_LITE_TRANSPORT=native|curl` も使える。user optionが指定された場合は環境変数より優先する。

自動fallbackはしない。native未対応機能、native extension未ロード、transport errorは黙ってlibcurlへ落とさず、選択された経路の失敗として扱う。

## Channel Lifecycle

native transportはChannel lifetimeに対応するHTTP/2 sessionをC resourceとして保持する。通常のRPCは同じsession上に新しいstreamを作る。

connectionが壊れた場合、transport層では同じRPCを自動retryしない。send/recv errorやEOFはchannelをdead扱いにし、そのRPCはエラーとして返す。GOAWAYを受けたchannelはdraining扱いにし、新規RPCには使わない。次のRPC開始時に新しいchannel resourceを作る。

retry policyやidempotency判断はtransport lifecycleとは分離し、将来の明示機能として扱う。

## Multiplex PoC

HTTP/2 session自体は複数streamを同時にin-flightにできる。PoC extensionでは `nghttp2_poc_multiplex_unary()` で同一session上に複数unary streamをsubmitし、全streamが独立に完了することを検証した。

現行のpublic PHP `Grpc\` surfaceは `wait()` / `responses()` が同期blockingなので、FPM / thread-local FrankenPHP の主用途では共有event loop / multiplex schedulerをrelease default gateにしない。async runtimeや同一実行コンテキスト内の並列RPCを扱う段階で、channel resource上のstream tableと共有event loop / poll loopを追加する。

## Compatibility Work

libcurl pathと同等に通す必要があるもの:

- unary / server streaming OK
- trailers-only error
- HTTP status to gRPC status mapping
- content-type validation
- `grpc-message` percent decode
- binary metadata request/initial/trailing
- duplicate metadata value behavior
- client-side deadline
- cancel / early stream termination
- TLS / mTLS
- compression unsupported explicit error

互換性・制御系は `docs/compatibility-control-checklist.md` に沿って追加する。実装進捗や検証途中の制限は `docs/research/` に残し、このdesign docには最終設計として満たすべき形だけを書く。

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
- PHP-FPM worker persistent pool
- compression implementation

これらはnative unary / server streaming transportの後に、compatibilityとworkload優先度に応じて扱う。
