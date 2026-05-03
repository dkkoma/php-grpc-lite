# Native compatibility gates: TLS/mTLS, metadata, status, compression (2026-05-04)

## 目的

native nghttp2 transportをrelease default候補へ近づけるため、libcurl経路で既に持っている互換性・制御系のうち、native surfaceで不足していた部分を埋める。

対象:

- TLS / mTLS unary
- TLS / mTLS server streaming
- trailers-only gRPC error
- `grpc-message`
- HTTP status mapping
- invalid `content-type`
- binary metadata round-trip
- unsupported compressionの明示エラー

## 実装

- `poc/nghttp2-client-ext/nghttp2_poc.c`
  - native persistent channel / stream resourceでTLS credentialを使う。
  - TLS channelではHTTP/2 `:scheme` を `https` にする。
  - HTTP/2 response header/trailerを汎用metadataとして収集する。
  - `grpc-status` / `grpc-message` をnative resultへ返す。
  - compressed message flagを検出し、payloadとしてdeliveryしない。
- `Grpc\Internal\NativeTransport`
  - C側raw metadataを `Grpc\` surface向けに正規化する。
  - `*-bin` metadataはbase64 decodeしてraw PHP stringとして返す。
  - HTTP status mapping / content-type validation / unsupported `grpc-encoding` / compressed flagをstatusへ変換する。
- `UnaryCall` / `ServerStreamingCall`
  - native経路でもinitial metadataとtrailing metadataを保持する。

## 検証

```bash
docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && make -j2'
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so vendor/bin/phpunit tests/Integration/NativeTransportControlTest.php
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so vendor/bin/phpunit
```

結果:

- `NativeTransportControlTest`: 24 tests / 74 assertions / 1 skipped
- extension loaded full PHPUnit: 58 tests / 194 assertions / 1 skipped

## 確認済み

| gate | 状態 |
|---|---|
| native TLS unary | OK |
| native TLS server streaming | OK |
| native mTLS unary | OK |
| native mTLS server streaming | OK |
| invalid root cert | `STATUS_UNAVAILABLE`、curl fallbackなし |
| mTLS client certなし | `STATUS_UNAVAILABLE`、curl fallbackなし |
| unary trailers-only error | `STATUS_INVALID_ARGUMENT` + `grpc-message` |
| server streaming trailers-only error | `STATUS_INVALID_ARGUMENT` + `grpc-message` |
| binary metadata | raw PHP value round-trip |
| HTTP 503 without grpc-status | `STATUS_UNAVAILABLE` |
| invalid content-type | `STATUS_UNKNOWN` |
| compressed flag | `STATUS_UNIMPLEMENTED` |
| unsupported grpc-encoding | `STATUS_UNIMPLEMENTED` |

## 判断

native transportの主要compatibility gateは、Phase 2 MVP surface上では通った。

残りはrelease defaultへ進めるための実装品質ゲートであり、主に以下:

- native extension packaging
- 長時間stressとmemory upper bound
- large response `100x100KiB` p99 tuning
- duplicate metadata valuesやmetadata sizeなど細部互換の追加照合
- production C extensionとしてのエラー文言・resource lifecycle整理
