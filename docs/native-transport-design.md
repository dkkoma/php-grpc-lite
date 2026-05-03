# Native Transport Design

## Goal

公式 `ext-grpc` のdrop-in surfaceを保ちながら、transport内部をlibcurlからnative nghttp2へ置き換える。

MVPの目的はext-grpcの完全再実装ではなく、Phase 2で確認した性能上の本筋改善をproduction extensionへ移すこと。

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

MVPではまずinsecure h2cのcontrolled benchmark pathを実装する。その後TLS/mTLS、deadline、metadata互換、error semanticsを追加する。

## Request Path

### Current libcurl path

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

MVPでは1 session内1 active streamでもよい。ただし構造は後でmultiple active streamsへ拡張できるように分離する。

## Channel Lifetime

MVP直後:

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

native transportをdefaultにする。ただし、libcurl経路はfallbackではなく明示的な安定経路として残す。

```php
new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'native', // default
]);

new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'curl',   // explicit stable route
]);
```

`php_grpc_lite.native_transport=true` はPoC期の互換optionとして当面残す。テストやベンチで経路を固定する場合は `PHP_GRPC_LITE_TRANSPORT=native|curl` も使える。user optionが指定された場合は環境変数より優先する。

自動fallbackはしない。native未対応機能、native extension未ロード、transport errorは黙ってlibcurlへ落とさず、選択された経路の失敗として扱う。

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

互換性・制御系は `docs/compatibility-control-checklist.md` に沿って追加する。

## Benchmark Work

MVP comparison runner:

```bash
BENCH_TAG=20260503-native-mvp-vs-libcurl-ext bench/phase2/compare-native-mvp-vs-libcurl-ext.sh
```

比較対象:

- current libcurl transport (`php_grpc_lite.transport=curl`)
- native MVP upload/direct/compact
- official ext-grpc

判定軸:

- large request unary p50/p99/calls/s
- server streaming many-small
- server streaming large payload
- server streaming long stream
- memory upper bound
- server timing trailerとの残差

## Non-goals for MVP

- client streaming / bidi streaming
- full ext-grpc channel args互換
- shared event loop / multiplex scheduler
- PHP-FPM worker persistent pool
- compression implementation
- production TLS/mTLS path

これらはnative transport MVPの後に、compatibilityとworkload優先度に応じて扱う。
