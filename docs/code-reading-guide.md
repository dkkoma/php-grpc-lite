# コードリーディングガイド

このガイドは、現行の `php-grpc-lite` を読むための入口です。過去の純PHP/libcurl経路は前提にせず、現在の公式 `grpc/grpc` wrapper + このrepositoryの `ext/grpc` 実装だけを扱います。

## 全体像

```text
generated client / gax
  -> grpc/grpc Composer wrapper
     -> Grpc\BaseStub
     -> Grpc\UnaryCall / Grpc\ServerStreamingCall
  -> Grpc\Call
  -> ext/grpc/main.c
     -> surface.c
     -> transport.c
     -> unary_call.c / server_streaming_call.c
     -> bridge.c
     -> nghttp2 + socket / OpenSSL
```

このrepositoryは高レベルの `Grpc\*` PHP wrapperを実装しません。`Grpc\BaseStub`、`Grpc\UnaryCall`、`Grpc\ServerStreamingCall`、`Grpc\Interceptor` などは Composer package `grpc/grpc` が提供します。このrepositoryの拡張は、公式 `ext-grpc` と同じ低レベルsurfaceを登録します。

## 読む順序

HTTP/2/gRPCのドメインモデル、命名、責務境界、状態機械をレビューする場合は、このガイドと併せて `docs/protocol-model-review-guide.md` を使います。

1. `tests/Integration/Fixtures/GreeterClient.php`
2. `vendor/grpc/grpc/src/lib/BaseStub.php`
3. `vendor/grpc/grpc/src/lib/UnaryCall.php`
4. `vendor/grpc/grpc/src/lib/ServerStreamingCall.php`
5. `ext/grpc/main.c`
6. `ext/grpc/surface.c`
7. `ext/grpc/bridge.c`
8. `ext/grpc/unary_call.c`
9. `ext/grpc/server_streaming_call.c`
10. `ext/grpc/transport.c`
11. `ext/grpc/internal.h`

## 1. 生成クライアント相当

`tests/Integration/Fixtures/GreeterClient.php` は `protoc-gen-php-grpc` が生成する `*GrpcClient` 相当のfixtureです。public methodは `BaseStub` のprotected helperへ委譲します。

```php
return $this->_simpleRequest(
    '/helloworld.Greeter/BenchUnary',
    $argument,
    [BenchReply::class, 'decode'],
    $metadata,
    $options,
);
```

| 引数 | 役割 |
|---|---|
| method path | HTTP/2 `:path` になるgRPC method名 |
| request object | `serializeToString()` でprotobuf wire bytesへ変換される |
| deserialize spec | official wrapperがresponse payloadをPHP objectへ戻すための指定 |
| metadata | user request metadata |
| options | timeout、call credentials、channel/call option |

## 2. official wrapper

`grpc/grpc` の wrapper は ext-grpc 互換の呼び出し形を作ります。

### Unary

```text
BaseStub::_simpleRequest()
  -> DefaultCallInvoker::UnaryCall()
  -> new Grpc\UnaryCall(...)
  -> UnaryCall::start()
     -> Grpc\Call::startBatch(SEND_INITIAL_METADATA, SEND_MESSAGE, SEND_CLOSE)
  -> user calls UnaryCall::wait()
     -> Grpc\Call::startBatch(RECV_INITIAL_METADATA, RECV_MESSAGE, RECV_STATUS)
```

`UnaryCall::start()` でrequestはprotobuf bytesへserializeされます。gRPC 5B frame headerの付与、metadata validation、HTTP/2送受信は拡張側で行います。

### Server streaming

```text
BaseStub::_serverStreamRequest()
  -> DefaultCallInvoker::ServerStreamingCall()
  -> new Grpc\ServerStreamingCall(...)
  -> ServerStreamingCall::start()
     -> Grpc\Call::startBatch(SEND_INITIAL_METADATA, SEND_MESSAGE, SEND_CLOSE)
  -> user iterates ServerStreamingCall::responses()
     -> Grpc\Call::startBatch(RECV_INITIAL_METADATA, RECV_MESSAGE)
     -> Grpc\Call::startBatch(RECV_MESSAGE) ...
  -> user calls getStatus()
     -> Grpc\Call::startBatch(RECV_STATUS)
```

messageごとに `responses()` がpullするため、現在の実装はbatch drainではありません。

## 3. 拡張が登録するPHP surface

`ext/grpc/main.c` の `PHP_MINIT_FUNCTION(grpc_lite)` が次を登録します。class/object実装は `ext/grpc/surface.c`、official wrapperから呼ばれる `Grpc\Call` bridgeは `ext/grpc/bridge.c` にあります。

| surface | 用途 |
|---|---|
| `Grpc\Channel` | target、credentials、channel optionsを保持する低レベルchannel |
| `Grpc\Call` | official wrapperから呼ばれるbatch API |
| `Grpc\ChannelCredentials` | insecure / TLS / mTLS credentials |
| `Grpc\CallCredentials` | per-call metadata callback wrapper |
| `Grpc\Timeval` | deadline計算用 |
| `Grpc\STATUS_*` | gRPC status constants |
| `Grpc\OP_*` / `CALL_*` / `CHANNEL_*` | official wrapper互換constants |

production buildでは低レベル診断entrypointをPHP関数として公開しません。`--enable-grpc-bench` でbuildした開発用extensionだけが `grpc_lite_unary()` などのbench/diagnostic関数を登録します。

## 4. `Grpc\Channel`

`Channel::__construct(string $target, array $opts)` は次を保持します。

| option | 扱い |
|---|---|
| `credentials` | 必須。`ChannelCredentials` object |
| `grpc.default_authority` | HTTP/2 `:authority` override |
| `grpc.ssl_target_name_override` | TLS verify name override |
| `grpc.primary_user_agent` | official wrapperが組み立てた user-agent |
| `grpc.max_receive_message_length` | response message size上限。`-1` は無制限 |
| `grpc.max_metadata_size` | response metadata soft limit。`grpc.absolute_max_metadata_size` 未指定時は hard limit 算出に使う |
| `grpc.absolute_max_metadata_size` | response metadata hard limit。超過時は `RESOURCE_EXHAUSTED` |

PHP object自体はsocketを持ちません。HTTP/2 session/socketのpersistent cacheは拡張のprocess-local globalにあります。

## 4.1 INI

| INI | default | 扱い |
|---|---:|---|
| `grpc_lite.http2_stream_window_size` | `8388608` | HTTP/2 `SETTINGS_INITIAL_WINDOW_SIZE` として送るstream receive window |
| `grpc_lite.http2_connection_window_size` | `8388608` | 接続直後のconnection receive window。初期値との差分を `WINDOW_UPDATE` で広げる |

window size は `65535` 未満ならHTTP/2 defaultへ丸め、HTTP/2上限を超える値は上限へ丸めます。

## 5. `Grpc\Call::startBatch()`

official wrapperは ext-grpc と同じbatch operationで拡張を呼びます。

| operation | 現在の扱い |
|---|---|
| `OP_SEND_INITIAL_METADATA` | request metadataとして保持 |
| `OP_SEND_MESSAGE` | serialized protobuf bytesとして保持 |
| `OP_SEND_CLOSE_FROM_CLIENT` | unary/server streaming scopeでは送信完了を表す |
| `OP_RECV_INITIAL_METADATA` | response initial metadataをeventへ設定 |
| `OP_RECV_MESSAGE` | unary payloadまたはserver streamingの次messageを返す |
| `OP_RECV_STATUS_ON_CLIENT` | final status objectを返す |

unaryは `RECV_STATUS` を含むbatchで `grpc_lite_unary_call_perform_on_channel()` が走ります。server streamingは最初の `RECV_MESSAGE` でC stream resourceを開き、以後C helperで1 messageずつ返します。

`ext/grpc/bridge.c` は official wrapper の `Grpc\Call` batch API を受け、`ext/grpc/unary_call.c` と `ext/grpc/server_streaming_call.c` の production RPC helperへ委譲します。bench build限定のdiagnostic PHP関数は `ext/grpc/bench.c` に閉じ込め、通常のwrapper経路は `Grpc\Call::startBatch()` 経由でC helperを直接呼びます。

## 6. HTTP/2 transport

`ext/grpc/transport.c` はC拡張内にincludeされるprivate implementationです。主な責務は次です。

- TCP connect、TLS/mTLS handshake、ALPN h2確認
- nghttp2 session/callback設定
- request header validation/filtering
- `*-bin` request metadataのbase64 wire encoding
- response `*-bin` metadataのbase64 decode
- gRPC 5B frame parse/build
- deadlineをconnect、TLS handshake、read/write poll loopへ適用
- client receive stream / connection windowを8MiBに広げ、large responseでWINDOW_UPDATE待ちを減らす
- response size / metadata size上限
- GOAWAY / EOF / RST_STREAM / protocol failure時のHTTP/2 connection lifecycle管理

protocol failure、compression unsupported、invalid content-type、invalid grpc-status、message size exceedなどはstream-local failureとしてstatusへ変換し、該当streamへ `RST_STREAM` を送ります。connection自体がdead/drainingでなければpersistent cacheには残します。

## 7. persistent connection

connection cacheはprocess-localです。FPMでは同一worker process内のrequestをまたいで再利用されます。processをまたぐ共有はしません。

`Grpc\Channel` はtarget、credentials、channel optionsから再利用keyを作るidentity元です。cache entryはこのidentityと `h2_connection` を束ね、`h2_connection` はsocket/TLS/nghttp2 sessionなどwire transport状態だけを持ちます。

再利用keyにはhost、port、authority、TLS verify name、credentials種別と証明書情報が入ります。connectionがdead/draining/busy、またはcache entryのidentity mismatchの場合はcacheから外し、次RPCで新規接続します。

## 8. 主要テスト

| テスト | 見るもの |
|---|---|
| `tests/Integration/UnaryTest.php` | unary success、metadata/trailer、sequential reuse |
| `tests/Integration/ServerStreamingTest.php` | server streaming yield |
| `tests/Integration/DeadlineTest.php` | client-side deadline |
| `tests/Integration/MetadataControlTest.php` | request metadata validation/filtering |
| `tests/Integration/MetadataCompatibilityTest.php` | duplicate/binary metadata |
| `tests/Integration/CompressionTest.php` | compression unsupported status |
| `tests/Integration/HttpValidationTest.php` | content-type / grpc-status / malformed frame |
| `tests/Integration/TlsTest.php` / `MtlsTest.php` | TLS/mTLS |
| `tests/Integration/Spanner/*` | gax / Spanner emulator compatibility |

標準検証コマンド:

```bash
docker compose run --rm dev sh -lc 'cd ext/grpc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit
```
