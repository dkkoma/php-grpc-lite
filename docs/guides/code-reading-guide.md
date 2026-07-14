# コードリーディングガイド

このガイドは、現行の `php-grpc-lite` を読むための入口です。過去の純PHP/libcurl経路は前提にせず、現在の公式 `grpc/grpc` wrapper + repository rootに置かれた `grpc` extension実装だけを扱います。

C extensionの構造に関する設計制約は [SPEC.md](../SPEC.md) の「C extension architecture policy」を基準にします。このガイドは、その前提で現在のファイル配置と読む順序を示します。

## 全体像

```text
generated client / gax
  -> grpc/grpc Composer wrapper
     -> Grpc\BaseStub
     -> Grpc\UnaryCall / Grpc\ServerStreamingCall
  -> Grpc\Call
  -> grpc.c
     -> src/surface.c
     -> src/transport.c
     -> src/unary_call.c / src/server_streaming_call.c
     -> src/wrapper_adapter.c
     -> nghttp2 + socket / OpenSSL
```

このrepositoryは高レベルの `Grpc\*` PHP wrapperを実装しません。`Grpc\BaseStub`、`Grpc\UnaryCall`、`Grpc\ServerStreamingCall`、`Grpc\Interceptor` などは Composer package `grpc/grpc` が提供します。このrepositoryの拡張は、公式 `ext-grpc` と同じ低レベルsurfaceを登録します。

## 読む順序

HTTP/2/gRPCのドメインモデル、命名、責務境界、状態機械をレビューする場合は、このガイドと併せて `docs/verification/protocol-model-review-guide.md` を使います。

### 初学者向け

PHP拡張やCのtranslation unitに慣れていない場合は、まず「PHPから見えるsurface」と「それを守るテスト」だけを読む。

1. `README.md`
2. `docs/SPEC.md` の目的、スコープ、C extension architecture policy
3. `grpc.c`
4. `src/surface.c`
5. `tests/phpt/001-load.phpt`
6. `tests/phpt/003-timeval.phpt`
7. `tests/phpt/010-unary.phpt`
8. `tests/phpt/011-server-streaming.phpt`
9. `docs/verification/native-test-framework.md`

この順序では、socket / nghttp2 / OpenSSLの詳細には入らず、PHP extensionのmodule lifecycle、class registration、object destructor、PHPTの基本を確認する。

### 中級者向け

Cのヘッダ境界、Zend object ownership、official wrapperとの接続を読む場合は、次を追加する。

1. `config.m4`
2. `php_grpc.h`
3. `src/surface.h`
4. `src/wrapper_adapter.c`
5. `src/unary_call.c`
6. `src/server_streaming_call.c`
7. `src/grpc_result.h`
8. `tests/unit/*.c`
9. `docs/verification/test-fixtures.md`
10. `docs/verification/verification-matrix.md`

この順序では、`.c` を直接includeしない構造、internal header、`zend_string` / `zval` の所有権、`Grpc\Call::startBatch()` からproduction RPC helperへ流れる境界を確認する。

### 上級者向け

HTTP/2 / gRPC transport、persistent connection、deadline、metadata/status、RST_STREAM / GOAWAY / EOF lifecycleを読む場合は、次を読む。

1. `docs/verification/protocol-model-review-guide.md`
2. `docs/design/grpc-call-exchange-state.md`
3. `src/grpc_exchange_state.h`
4. `src/transport.h`
5. `src/transport.c`
6. `src/transport_core.c`
7. `src/status_core.c`
8. `src/protocol_core.c`
9. `tests/phpt/020-request-metadata-control.phpt`
10. `tests/phpt/022-error-and-http-validation.phpt`
11. `tests/phpt/024-control-semantics.phpt`
12. `tests/Integration/MetadataCompatibilityTest.php`
13. `tests/Integration/ControlSemanticsTest.php`

この順序では、1 RPC over 1 HTTP/2 streamの交換状態、connection cache、nghttp2 callbacks、gRPC status taxonomy、metadata shape、stream-local failureとconnection failureの切り分けを確認する。

### 実装全体の読み順

1. `tests/Integration/Fixtures/GreeterClient.php`
2. `vendor/grpc/grpc/src/lib/BaseStub.php`
3. `vendor/grpc/grpc/src/lib/UnaryCall.php`
4. `vendor/grpc/grpc/src/lib/ServerStreamingCall.php`
5. `grpc.c`
6. `src/surface.c`
7. `src/wrapper_adapter.c`
8. `src/unary_call.c`
9. `src/server_streaming_call.c`
10. `src/transport.c`
11. `src/grpc_exchange_state.h` / `src/grpc_result.h`
12. `docs/design/grpc-call-exchange-state.md`
13. `src/internal.h`

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

`grpc.c` の `PHP_MINIT_FUNCTION(grpc_lite)` はmodule lifecycleとINI / constants登録を担当し、PHP class登録は `src/surface.c` の `grpc_lite_register_surface_classes()` に委譲します。class/object実装、method table、`Grpc\Call` の単純なlifecycle/status参照methodは `src/surface.c`、official wrapperから呼ばれる `Grpc\Call::startBatch()` adapterは `src/wrapper_adapter.c` にあります。`src/surface.c` は `src/wrapper_adapter.h` を通じてそのmethod implementationを参照します。

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
| `grpc_lite.http2_max_frame_size` | `16384` | HTTP/2 `SETTINGS_MAX_FRAME_SIZE` としてpeerへ通知する最大DATA frame payload size |
| `grpc_lite.http2_max_header_list_size` | `65536` | HTTP/2 `SETTINGS_MAX_HEADER_LIST_SIZE` としてpeerへ通知するheader list size上限 |
| `grpc_lite.server_streaming_read_ahead_max_messages` | `32` | 別streamのI/Oで先読みされたserver streaming payload queueのmessage上限 |
| `grpc_lite.server_streaming_read_ahead_max_bytes` | `8388608` | 別streamのI/Oで先読みされたserver streaming payload queueのbyte上限 |

window size は `65535` 未満ならHTTP/2 defaultへ丸め、HTTP/2上限を超える値は上限へ丸めます。read-ahead上限は0以下ならdefaultへ戻します。

INI / channel optionとは別の固定上限が `src/transport_core.h` にあります。user request metadataは256 values(`GRPC_LITE_MAX_REQUEST_METADATA_VALUES`、超過は例外)、response metadataは128 entries(`GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES`、超過は `RESOURCE_EXHAUSTED`)、persistent connection cacheは128 connections(`GRPC_LITE_MAX_PERSISTENT_CONNECTIONS`)です。

診断用に、環境変数 `GRPC_LITE_TRACE_FILE` を指定するとRPC完了recordを指定fileへ追記します(`tests/phpt/029-trace-file.phpt`)。なお `wire.frame_out` はnghttp2がchunkを渡した時点(write coalesceバッファ投入時)で記録されるため、対応する `wire.tls_write` / `wire.socket_write` より先に並びます(TLS・平文共通)。また送信DATA frameは no-copy 経路(`h2_send_data_callback`)が frame header のみで記録するため、DATA の `wire.frame_out` は常に `chunk_len: 9` になります(`frame_payload_len` は正しい payload 長)。`transport.connection_destroy` は wire event ではなく local lifecycle event で、`destroy_h2_connection()` の入口(TLS/fd/nghttp2 session の解放より前)で発火します。preface 送出前に setup が失敗した connection でも発火するため、`wire.connection_preface` と1対1に対応するとは限りません(fatal 経路で消費された connection が実際に destroy されることを固定する oracle 用途、`tests/phpt/041`)。

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

unaryは `RECV_STATUS` を含むbatchで `grpc_lite_unary_call_perform_on_connection()` が走ります。server streamingは最初の `RECV_MESSAGE` でC stream resourceを開き、以後C helperで1 messageずつ返します。

`src/wrapper_adapter.c` は official wrapper の `Grpc\Call` batch API を受け、`src/unary_call.c` と `src/server_streaming_call.c` の production RPC helperへ委譲します。bench build限定のdiagnostic PHP関数は `src/diagnostic/bench.c` に閉じ込め、通常のwrapper経路は `Grpc\Call::startBatch()` 経由でC helperを直接呼びます。

## 6. HTTP/2 transport

`src/transport.c` はC拡張内のprivate implementationです。主な責務は次です。

- TCP connect、TLS/mTLS handshake、ALPN h2確認
- nghttp2 session/callback設定
- request header validation/filtering
- `*-bin` request metadataのbase64 wire encoding
- response `*-bin` metadataのbase64 decode
- response header blockのsemantic phase分類と1xx informational fieldの隔離
- gRPC 5B frame parse/build
- deadlineをconnect、TLS handshake、read/write poll loopへ適用
- client receive stream / connection windowを8MiBに広げ、large responseでWINDOW_UPDATE待ちを減らす
- response size / metadata size上限
- GOAWAY / EOF / RST_STREAM / protocol failure時のHTTP/2 connection lifecycle管理

protocol failure、compression unsupported、invalid content-type、invalid grpc-status、message size exceedなどはstream-local failureとしてstatusへ変換し、該当streamへ `RST_STREAM` を送ります。connection自体がdead/drainingでなければpersistent cacheには残します。

1 RPC over 1 HTTP/2 stream の交換状態は `src/grpc_exchange_state.h` の `grpc_call` にまとまっています。fieldの責務、lifetime、hot/cold性は `docs/design/grpc-call-exchange-state.md` に整理しています。wrapper / orchestrationが返す小さな結果DTOは `src/grpc_result.h`、bench build専用の観測field群は `src/diagnostic/bench_call.h` です。

`src/transport.h` は現時点ではHTTP/2 transportのprivate aggregate headerです。connection、persistent cache、nghttp2 callback、request header builder、response parser、server streaming resource stateなどのboundaryと将来の分割順は `docs/design/transport-header-boundaries.md` に整理しています。

### Response HEADERS blockの読み方

nghttp2の `NGHTTP2_HCAT_RESPONSE` / `NGHTTP2_HCAT_HEADERS` は、そのままgRPCのinitial / trailing metadata ownershipを表しません。最初のresponse HEADERSが1xxの場合、後続の1xxとfinal response HEADERSはいずれも `HCAT_HEADERS` で届きます。

transportは1 RPCごとのheader-block phaseを次の順序で確定します。

1. `on_begin_headers_callback()` はshared pure helperで、final response未観測なら `GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS`、観測済みなら `GRPC_RESPONSE_HEADER_BLOCK_TRAILING` を `response_header_phase.block_phase` に設定し、blockのEND_STREAM validityも記録する。
2. `AWAITING_STATUS` blockの先頭の`:status`が100–199なら `GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL` とし、block内の全fieldを無視してfinal responseを待つ。
3. 非1xxの`:status`なら `GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL` とし、`final_response_headers_seen` を設定する。同じblockのcustom metadataをinitial metadataへ保存してinitial response validationを適用する。これは1xx後に `HCAT_HEADERS` で届いたfinal responseにも同じく適用する。
4. final response確定後のheader blockは `TRAILING` とし、custom metadataとgRPC statusをtrailing側で処理する。

phaseは各fieldをmetadata / validation / status stateへ反映する前に確定します。`INFORMATIONAL` blockはPHP-visible metadata、`http_status`、`content-type`、`grpc-status`、`grpc-message`、`grpc-encoding`を更新しませんが、pseudo-headerを含む全fieldは `wire_response_header_*` budgetに計上します。`grpc_response_header_phase_allows_status_fields()` はFINAL_INITIAL / TRAILINGのEND_STREAM付きblockだけstatus commitを認め、`grpc_response_header_phase_metadata_is_trailing()` はstatus parse成否ではなくblock roleでownershipを揃えます。nghttp2のinvalid-frame / protocol-RST callbackで拒否を観測した場合は `response_header_protocol_error` が先行statusやHTTP fallbackより優先します。phase遷移、commit/ownership truth table、call resetは `tests/unit/test_response_header_phase.c`、wire semanticsはPHPTで固定します。

## 7. persistent connection

connection cacheはprocess-localです。FPMでは同一worker process内のrequestをまたいで再利用されます。processをまたぐ共有はしません。

`Grpc\Channel` はtarget、credentials、channel optionsから再利用keyを作るidentity元です。cache entryはこのidentityと `h2_connection` を束ね、`h2_connection` はsocket/TLS/nghttp2 sessionなどwire transport状態だけを持ちます。

再利用keyにはhost、port、authority、TLS verify name、credentials種別と証明書情報が入ります。connectionがdead/draining、またはcache entryのidentity mismatchの場合はcacheから外し、次RPCで新規接続します。cache entry数は128(`GRPC_LITE_MAX_PERSISTENT_CONNECTIONS`)が上限で、満杯時に新規接続が必要なRPCはエラーになります。active streamが残っていても、HTTP/2 stream idごとにdispatchできるため同じconnection上へ新しいstreamを作れます。

## 8. 主要テスト

| テスト | 見るもの |
|---|---|
| `tests/unit/*.c` | I/Oに依存しないgRPC protocol helperとstatus taxonomyのC unit gate |
| `tests/phpt/*.phpt` | C拡張surface、INI、object lifecycle、basic unary/server streaming、deadline status、TLS/mTLS baseline、protocol error、metadata/call credentials、transport control semantics、resource limitsのPHPT gate |
| `tests/Integration/DeadlineTest.php` | deadlineのelapsed/count/immediate timeoutなど、PHPT baselineより細かいclient-side挙動 |
| `tests/Integration/CompressionTest.php` | server streaming compression、grpc-status併用、stream-local failure後のrecovery |
| `tests/Integration/HttpValidationTest.php` | PHPT baselineにないcontent-type / grpc-status validation variants |
| `tests/Integration/ErrorSemanticsTest.php` | trailers-only error statusとmetadata propagation |
| `tests/Integration/MetadataCompatibilityTest.php` | duplicate binary metadata、large/many metadata、response metadata limit、server streaming duplicate metadata |
| `tests/Integration/ControlSemanticsTest.php` | cancel、GCでdropされたstream、channel close後のstatus保持などPHP object lifecycleをまたぐ制御 |
| `tests/Integration/InterceptorTest.php` | PHP wrapper interceptor chain |
| `tests/Integration/TlsTest.php` / `MtlsTest.php` | TLS server streaming、mTLS client certificate failure |
| `tests/Integration/Spanner/*` | gax / Spanner emulator compatibility |

標準検証コマンド:

```bash
docker compose run --rm dev sh -lc 'make -j$(nproc)'
./tools/test/check-c-unit.sh
./tools/test/check-phpt.sh
./tools/test/check-c-coverage.sh
docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist
```

`check-c-unit.sh` は pure protocol helperとstatus taxonomyだけを直接検証します。`check-phpt.sh` は `vendor/autoload.php` と Go test-server `:50051`〜`:50054`、raw HTTP/2 fixture `:50055`〜`:50071` をpreflightで必須にします。PHPT単体にはskip条件を残していますが、標準runnerでは必要serviceが欠ける場合は失敗として扱います。`check-c-coverage.sh` はC unitとPHPT gateをgcov/lcov付きで実行し、`var/coverage/c-lcov/` にtraceとHTMLを出力します。
