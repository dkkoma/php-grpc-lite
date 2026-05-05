# コードリーディングガイド

このガイドは、現行の `php-grpc-lite` を読むための入口です。現在の実装でユーザーコードから1本のRPCが送信され、レスポンスとstatusが返るまでの流れだけを扱います。

## 全体像

```text
generated-like client
  -> Grpc\BaseStub
  -> Grpc\UnaryCall / Grpc\ServerStreamingCall
  -> Grpc\AbstractCall
  -> Grpc\Internal\NativeTransport
  -> ext/grpc/grpc.c
  -> nghttp2 + socket / OpenSSL
```

現在のruntime transportは native nghttp2 の1系統です。

読む順序は次の通りです。

1. `tests/Integration/Fixtures/GreeterClient.php`
2. `src/Grpc/BaseStub.php`
3. `src/Grpc/UnaryCall.php`
4. `src/Grpc/ServerStreamingCall.php`
5. `src/Grpc/AbstractCall.php`
6. `src/Grpc/Internal/NativeTransport.php`
7. `ext/grpc/grpc.c`

## 1. 生成クライアント相当

`tests/Integration/Fixtures/GreeterClient.php` は、`protoc-gen-php-grpc` が生成する `*GrpcClient` 相当のfixtureです。

重要なのは、public methodが `BaseStub` のprotected helperへ委譲する形です。

```php
return $this->_simpleRequest(
    '/helloworld.Greeter/BenchUnary',
    $argument,
    [BenchReply::class, 'decode'],
    $metadata,
    $options,
);
```

引数の意味:

| 引数 | 役割 |
|---|---|
| method path | HTTP/2 `:path` になるgRPC method名 |
| request object | `serializeToString()` でprotobuf wire bytesへ変換される |
| deserialize spec | ext-grpc互換の `[ClassName::class, 'decode']` 形式 |
| metadata | user request metadata |
| options | timeout、call credentials、diagnosticsなどのper-call option |

## 2. Stub dispatch

`src/Grpc/BaseStub.php` は、generated client surfaceとcall objectをつなぎます。

### Unary

`_simpleRequest()` は `UnaryCall` を作り、すぐ `start()` します。実I/Oはこの時点では走らず、ユーザーが `UnaryCall::wait()` を呼んだ時に実行されます。

```text
BaseStub::_simpleRequest()
  -> new UnaryCall(...)
  -> UnaryCall::start($argument)
  -> return UnaryCall
```

### Server streaming

`_serverStreamRequest()` は `ServerStreamingCall` を作り、すぐ `start()` します。実I/Oは `responses()` のGeneratorをiterateした時に始まります。

```text
BaseStub::_serverStreamRequest()
  -> new ServerStreamingCall(...)
  -> ServerStreamingCall::start($argument)
  -> return ServerStreamingCall
```

### Interceptor

`InterceptorChannel` が渡された場合だけ、`buildUnaryChain()` / `buildServerStreamChain()` でinterceptor chainを組みます。I/O層はinterceptorを意識せず、最終的には素の `Channel` に対してcall objectを作ります。

## 3. Channel と credentials

`src/Grpc/Channel.php` はtarget、channel options、credentialsを保持します。

現行実装ではPHP userlandにsocketやHTTP/2 sessionを保持しません。HTTP/2 session/socketのpersistent cacheはC extension側にあります。そのため `Channel::close()` はuserland resource cleanupを持ちません。

`src/Grpc/ChannelCredentials.php` は3種類のcredentialsを表します。

| factory | 用途 |
|---|---|
| `createInsecure()` | h2c |
| `createSsl()` | TLS / mTLS |
| `createDefault()` | default TLS credentials |

TLS/mTLS用のroot cert、client cert、private keyは `NativeTransport` 経由でC extensionへ渡され、OpenSSLの `SSL_CTX` に設定されます。

## 4. 共通call処理

`src/Grpc/AbstractCall.php` は、unary/server streaming共通のrequest metadata構築を担当します。

主な責務:

- `getPeer()` の提供
- request metadataの正規化
- library-owned metadataのfiltering
- call credentialsのmetadata統合
- `grpc-timeout` の8桁制限に収まる単位変換
- `user-agent` の構築
- binary metadataのbase64 wire encoding

### Request metadata

metadataはtransportへ渡す前に正規化されます。

```text
user metadata
  -> channel update_metadata callback
  -> call_credentials_callback / CallCredentials
  -> key/value validation
  -> library-owned metadata filtering
  -> native request headers
```

library-owned metadataはユーザー指定からは送信されません。代表例は `content-type`、`te`、`user-agent`、`grpc-status`、`grpc-timeout`、`grpc-encoding` です。

### Timeout

per-call optionの `timeout` はmicrosecondsです。`AbstractCall::encodeGrpcTimeout()` が `u` / `m` / `S` / `M` / `H` のいずれかに変換し、`grpc-timeout` headerとして送ります。

C extension側でも同じdeadlineをconnect、TLS handshake、read/write poll loopの上限として使います。

## 5. Unary call

`src/Grpc/UnaryCall.php` は1 request / 1 responseの同期RPCです。

### start()

`start()` はrequest objectをprotobuf bytesへserializeし、diagnostics用の値を記録します。

```text
UnaryCall::start()
  -> merge metadata/options
  -> request->serializeToString()
  -> serializedRequestを保持
```

ここではgRPC 5B headerをまだ付けません。framingは `NativeTransport::unarySimple()` がC extensionへ渡す直前に行います。

### wait()

`wait()` が実I/Oの入口です。

```text
UnaryCall::wait()
  -> buildNativeRequestHeaders()
  -> NativeTransport::unarySimple()
  -> payloadをDeserialize::apply()
  -> stdClass statusを返す
```

戻り値はext-grpc互換の `[$response, $status]` です。`$status` は `stdClass` で、少なくとも `code`、`details`、`metadata` を持ちます。

transport例外は次のように扱います。

| 条件 | status |
|---|---|
| deadline exceeded | `STATUS_DEADLINE_EXCEEDED` |
| その他native transport error | `STATUS_UNAVAILABLE` |
| `cancel()` 済み | `STATUS_CANCELLED` |

## 6. Server streaming call

`src/Grpc/ServerStreamingCall.php` は1 request / N responseの同期Generator APIです。

### start()

`start()` はmetadata/optionsをmergeし、requestをserializeして保持します。実I/Oはまだ行いません。

### responses()

`responses()` をiterateするとnative stream resourceを開きます。

```text
ServerStreamingCall::responses()
  -> NativeTransport::streamOpen()
  -> loop:
       NativeTransport::streamNext()
       done=false: payloadをdeserializeしてyield
       done=true : final status / metadataを保存してreturn
```

messageごとに `yield` するため、server streamingはbatch drainではありません。consumerが次のmessageを要求するまで次の `streamNext()` は呼ばれません。

### cancel()

`cancel()` はC stream resourceに `grpc_lite_stream_cancel()` を呼びます。C側ではactive streamに `RST_STREAM(CANCEL)` を送信し、channel busy状態を解除します。

## 7. NativeTransport wrapper

`src/Grpc/Internal/NativeTransport.php` はPHP userlandとC extensionの境界です。

public wrapper:

| method | C function | 用途 |
|---|---|---|
| `unarySimple()` | `grpc_lite_unary()` | unary RPC |
| `streamOpen()` | `grpc_lite_stream_open()` | server streaming開始 |
| `streamNext()` | `grpc_lite_stream_next()` | 次messageまたはfinal status取得 |
| `streamCancel()` | `grpc_lite_stream_cancel()` | active stream cancel |

`NativeTransport` の重要な仕事:

- `target` をhost/portへ分解する
- credentialsをchannel keyへ反映する
- unary requestにgRPC 5B headerを付ける
- C extensionのraw resultをPHP API用のpayload/status/metadataへ正規化する
- HTTP status、content-type、HTTP/2 reset、compressed response、missing trailersをgRPC statusへ変換する
- `*-bin` metadataをbase64 wire valueからraw binary valueへ戻す

## 8. C extension

`ext/grpc/grpc.c` がproduction native transportです。`ext/grpc/grpc_bench.c` はbench/diagnostic entrypointで、通常の `Grpc\` APIからは通りません。

### 主要構造体

| 構造体 | 役割 |
|---|---|
| `h2_channel` | socket/TLS/nghttp2 session/persistent state |
| `grpc_call` | 1 RPCのrequest/response parser state |
| `grpc_stream_resource` | PHP stream resourceとactive call/channelの紐付け |

### Channel lifecycle

```text
NativeTransport::channelKey()
  -> grpc_lite_unary()/grpc_lite_stream_open()
  -> get_persistent_channel()
  -> HashTable persistent_channels
  -> h2_channel reuse or create_h2_channel()
```

`persistent_channels` はmodule globals上のC-side cacheです。FPMではworker process内、ZTS/worker runtimeではthread-localに閉じます。PHP userlandの `Channel` objectがsocketを持つわけではありません。

channelがdead/drainingになった場合、同じRPCをtransport層で自動retryしません。そのRPCはerrorとして返し、次RPC開始時に新しいchannelを作ります。

### Connection setup

`create_h2_channel()` がTCP接続、TLS設定、nghttp2 session初期化を行います。

```text
connect_tcp()
  -> configure_tls_channel() when TLS
  -> nghttp2_session_client_new()
  -> nghttp2_submit_settings()
```

TLSではOpenSSLを直接使い、ALPNで `h2` がnegotiatedされたことを確認します。certificate verification error、ALPN mismatch、mTLS handshake failureはchannel error detailに残し、PHP側では `STATUS_UNAVAILABLE` のdetailsとして返します。

response message sizeは `grpc.max_receive_message_length` で制御します。channel optionを基本とし、call optionで上書きできます。未指定時は64MiB、`-1` は上限なしです。上限超過時はC側でbody集約・payload allocationを止め、PHP側で `STATUS_RESOURCE_EXHAUSTED` に正規化します。

### Unary path

`perform_h2_channel_unary()` がunary RPCの中心です。

```text
append pseudo headers / request headers
  -> nghttp2_submit_request()
  -> nghttp2_session_send()
  -> recv loop
  -> nghttp2_session_mem_recv()
  -> response body / metadata / statusをreturn arrayへ詰める
```

同一 `h2_channel` 上では同時active streamを1本に制限しています。busyなchannelへ新しいstreamを載せる場合はエラーになります。

### Server streaming path

server streamingはPHP resourceとしてstream stateを保持します。

```text
grpc_lite_stream_open()
  -> h2_channel取得
  -> request submit
  -> grpc_stream_resourceを返す

grpc_lite_stream_next()
  -> poll/read/writeを進める
  -> complete payloadがあれば done=false で返す
  -> stream完了なら done=true とstatus/metadataを返す
```

resource destructorは未完了streamに `RST_STREAM(CANCEL)` を送り、channel busy状態を解除します。

## 9. Deserialize

`src/Grpc/Internal/Deserialize.php` はprotobuf payloadをPHP objectへ変換します。

ext-grpc互換上重要なのは、`[ClassName::class, 'decode']` を実メソッド呼び出しとして扱わないことです。google/protobufの生成クラスはstatic `decode()` を持たないため、`new ClassName()` して `mergeFromString($payload)` します。

## 10. 互換性を見るテスト

| 観点 | テスト |
|---|---|
| unary basic / metadata / channel semantics | `tests/Integration/UnaryTest.php` |
| server streaming | `tests/Integration/ServerStreamingTest.php` |
| deadline | `tests/Integration/DeadlineTest.php` |
| cancel / early termination | `tests/Integration/ControlSemanticsTest.php` |
| HTTP/gRPC error semantics | `tests/Integration/ErrorSemanticsTest.php` / `tests/Integration/HttpValidationTest.php` |
| compression unsupported error | `tests/Integration/CompressionTest.php` |
| binary metadata | `tests/Integration/BinaryMetadataTest.php` |
| duplicate metadata / metadata size | `tests/Integration/MetadataCompatibilityTest.php` |
| metadata validation / filtering | `tests/Integration/MetadataControlTest.php` / `tests/Integration/NativeRequestHeadersTest.php` |
| TLS / mTLS | `tests/Integration/TlsTest.php` / `tests/Integration/MtlsTest.php` |
| Spanner emulator | `tests/Integration/Spanner/` |
| native lifecycle | `tests/Integration/NativeTransportControlTest.php` |

通常の統合テストはnative extensionをloadして実行します。

```bash
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit
```

## 11. 読むときの確認ポイント

- PHP userlandのpublic surfaceは `Grpc\` API互換を維持しているか。
- request metadataはtransportへ渡す前に正規化・filterされているか。
- library-owned metadataをユーザー入力で上書きできないか。
- deadlineがheaderだけでなくnative I/O上限にも効いているか。
- status objectが `code` / `details` / `metadata` を持つか。
- binary metadataがPHP API上raw binary、wire上base64として扱われているか。
- C extensionのpersistent channel lifecycleがrequestをまたいで安全か。
- stream resourceのcancel/destructor pathでchannel busy状態が残らないか。

## 関連ドキュメント

- `docs/SPEC.md`
- `docs/api-surface.md`
- `docs/native-transport-design.md`
- `docs/native-transport-decision.md`
- `docs/release-qa-checklist.md`
- `docs/compatibility-control-checklist.md`
