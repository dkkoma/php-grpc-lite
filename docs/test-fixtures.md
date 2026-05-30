# Test fixtures

この文書は、local test-server と検証用 metadata control protocol の読み方をまとめる。

`poc/test-server/main.go` は、通常のgRPC serverだけでなく、HTTP/2 / gRPC lifecycleの異常系を再現するfixtureも同時に起動する。PHPT runnerは `test-server:50051` から `test-server:50060` までをpreflightする。

## Ports

| Port | Mode | Purpose | Representative tests |
|---:|---|---|---|
| `50051` | h2c gRPC | 通常のunary / server streaming、metadata、deadline、resource limit、interceptor、Spanner以外の主経路 | `tests/phpt/010-unary.phpt`, `tests/phpt/011-server-streaming.phpt`, `tests/phpt/020-request-metadata-control.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| `50052` | h2 over TLS | TLS channel credentials、ALPN h2、bad root rejection | `tests/phpt/030-tls.phpt`, `tests/Integration/TlsTest.php` |
| `50053` | h2 over mTLS | client certificate必須のmTLS、missing client cert rejection | `tests/phpt/030-tls.phpt`, `tests/Integration/MtlsTest.php` |
| `50054` | h2c HTTP server, non-gRPC by default | invalid content-type、HTTP status fallback、compressed flag、unsupported encoding、partial frameなどのprotocol validation | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php`, `tests/Integration/HttpValidationTest.php` |
| `50055` | raw h2c lifecycle fixture | successful gRPC response中にGOAWAYを送る。既存streamは成功し、connectionはdraining扱いになることを見る | `tests/phpt/024-control-semantics.phpt` |
| `50056` | raw h2c lifecycle fixture | 奇数回の接続で即EOF、偶数回で正常応答。connection failure後に再接続できることを見る | `tests/phpt/024-control-semantics.phpt` |
| `50057` | raw h2c mid-stream failure fixture | gRPC response frameを途中まで送ってcloseする。malformed frame / unavailable系の扱いを見る | `tests/phpt/024-control-semantics.phpt` |
| `50058` | raw h2c server RST_STREAM fixture | 同一connectionの最初のunary streamを `REFUSED_STREAM`、次streamをOKにする | `tests/phpt/024-control-semantics.phpt` |
| `50059` | raw h2c server RST_STREAM fixture | 同一connectionの最初のserver streaming streamを `REFUSED_STREAM`、次streamをOKにする | `tests/phpt/024-control-semantics.phpt` |
| `50060` | raw h2c GOAWAY refused fixture | request streamを受けた直後に `GOAWAY(last_stream_id=0)` を送る。stream refused by GOAWAYを検証する | `tests/phpt/024-control-semantics.phpt` |

## Service methods

| Method | Kind | Fixture behavior |
|---|---|---|
| `/helloworld.Greeter/SayHello` | unary | `Hello, <name>` を返す。`delay_ms` でhandler delayを入れられる |
| `/helloworld.Greeter/SayManyHellos` | server streaming | 5件の `Hello #n, <name>` を返す |
| `/helloworld.Greeter/BenchUnary` | unary | payload size、server delay、error status、metadata echo、server timingをmetadata/requestで制御する |
| `/helloworld.Greeter/BenchServerStream` | server streaming | message count、payload size、server delay、error status、metadata echoをmetadata/requestで制御する |

## Metadata controls on gRPC fixtures

これらは主に `50051` / `50052` / `50053` のgRPC serverで使う。

| Request metadata | Effect | Used by |
|---|---|---|
| `x-bench-error-code` | 指定したgRPC status codeでhandler errorを返す | `tests/Integration/ErrorSemanticsTest.php` |
| `x-bench-error-message` | `x-bench-error-code` と一緒に返すdetails | `tests/Integration/ErrorSemanticsTest.php` |
| `x-bench-response-metadata-count` | initial/trailing metadataを指定個数生成する | `tests/phpt/025-resource-limits.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-response-metadata-value-bytes` | 生成metadata valueのbyte長を指定する | `tests/phpt/025-resource-limits.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-echo-bin` | request binary metadataを `x-bench-initial-bin` / `x-bench-trailing-bin` として返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-echo-ascii` | request ASCII metadataを `x-bench-initial-ascii` / `x-bench-trailing-ascii` として返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-response-duplicate` | duplicate ASCII valuesをinitial/trailing metadataへ返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-response-duplicate-bin` | duplicate binary valuesをinitial/trailing metadataへ返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-observe-metadata-key` | 指定keyのrequest metadataを `x-bench-seen-*` metadataとして返す | `tests/phpt/020-request-metadata-control.phpt` |
| `x-bench-server-timing` | server handler timingをtrailing metadataへ返す | benchmark helpers |
| `x-bench-server-stats` | gRPC stats handler timingをtrailing metadataへ返す | benchmark helpers |
| `x-bench-server-cached-payload` | fixture server側でcached payloadを使う | benchmark helpers |

## Metadata controls on non-gRPC h2c fixture

これらは `50054` のHTTP/2 serverで使う。通常のgRPC serverではなく、client側のHTTP/gRPC validationを確認するためのfixtureである。

| Request metadata | Effect | Used by |
|---|---|---|
| `x-bench-http-status` | HTTP statusを指定する。`grpc-status` がないHTTP responseのfallback mappingを見る | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-content-type` | response `content-type` を指定する | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/HttpValidationTest.php` |
| `x-bench-grpc-status` | trailersの `grpc-status` を指定する | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/HttpValidationTest.php` |
| `x-bench-grpc-response=compressed-flag` | compressed flag=1のgRPC frameを返す | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php` |
| `x-bench-grpc-response=partial-frame` | incomplete gRPC frameを返す | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-grpc-encoding` | response `grpc-encoding` を指定する | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-observe-authority=1` | observed authorityを `x-bench-authority` として返す | authority / TLS identity diagnostics |

## Fixture ownership

- `poc/test-server/main.go` がfixture behaviorの一次ソース。
- `tests/phpt/helpers.inc` はPHPTからtest-server到達性を確認する。
- `tools/test/check-phpt.sh` と `tools/test/check-c-coverage.sh` は `50051`-`50060` を必須preflightとして扱う。
- fixture behaviorを変える場合は、該当PHPT/PHPUnitだけでなく `docs/verification-matrix.md` も更新する。
