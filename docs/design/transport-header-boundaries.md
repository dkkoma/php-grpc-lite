# transport header boundary

`src/transport.h` はHTTP/2 transport用のprivate aggregate headerである。installされるpublic C APIではないが、現状では複数のinternal domainをまとめて露出している。

- connection / session state。
- persistent connection cache entry。
- server streaming resource state。
- nghttp2 callback。
- TCP/TLS I/O helper。
- request header construction。
- response data / metadata parsing。
- status / result bridge helper。
- call cleanup helper。

この文書は、宣言を移動する前に意図するboundaryを記録する。後続のheader refactorを機械的かつ挙動変更なしで進めるための地図である。

## Current consumers

| consumer | transport宣言を必要とする理由 |
|---|---|
| `src/transport.c` | 具体的なtransport実装のownerであり、現状では `transport.h` の全宣言を定義している |
| `src/unary_call.h` / `src/unary_call.c` | `h2_connection`、`h2_request_headers`、request writer callback、response metadata/status helper、cleanup、deadline helperを使う |
| `src/server_streaming_call.h` / `src/server_streaming_call.c` | `server_streaming_call_state`、`h2_connection`、streaming queue helper、cancel/ownership helper、response metadata/status helperを使う |
| `src/surface.c` | channel/call surfaceからpersistent connection entryのlookup/destructionとconnection create/reuse helperを使う |
| `src/diagnostic/bench.c` | bench専用codeが低レベルhelper、callback形状のhelper、status/metadata conversionを再利用する |

`src/common.h` はprivate convenience includeとして残す。ただし、新しいheaderから `common.h` へ依存をさらに寄せない。新しいheaderでは、可能な限り必要なstandard/PHP includeだけを直接includeする。

## Target header groups

| target header | ownerになるもの | notes |
|---|---|---|
| `h2_connection.h` | `struct _h2_connection`、lifecycle helper、active stream registration、connection usability/draining/dead state | 主要transport objectとそのinvariantをaggregate headerから外へ出す |
| `persistent_connection_cache.h` | `struct persistent_connection_entry`、cache key/build/match/create/get/discard/remove helper | process-local cache semanticsのprivate helperとして残す。public pool APIにはしない |
| `server_streaming_state.h` | `struct server_streaming_call_state`、resource dtor、cancel/destroy/ownership clear helper | Zend resource lifetimeを見える場所に置き、request header/protocol helperとは混ぜない |
| `h2_transport_io.h` | TCP connect、nonblocking mode、poll/deadline helper、TLS setup、`connection_send()`、`connection_recv()` | 宣言移動は機械的にできるが、function boundaryの変更はheader refactorには含めない |
| `h2_callbacks.h` | nghttp2 callback registrationとcallback関数 | callback signatureはnghttp2に対してはpublicだが、このextension内ではprivate |
| `h2_request_headers.h` | `h2_request_headers` とrequest metadata/header builder helper | 明確なdata objectとcleanup pairがあるため、最初の機械的分割候補として扱いやすい |
| `grpc_response_processing.h` | response DATA parser、response metadata list helper、queued payload helper、response metadata map conversion | hot path。宣言移動とparser変更は必ず分ける |
| `grpc_call_status_bridge.h` | `resolve_grpc_call_status()`、`add_status_result_to_return()`、`cleanup_grpc_call()` | exchange stateをresult/return shapeへ橋渡しする。pure helperのstatus taxonomyとは混ぜない |

call siteを移行する間、`src/transport.h` はbackward-compatibleなaggregate headerとして残せる。最終形は、各 `.c` が必要な最小のinternal headerをincludeし、移行期間だけ `transport.h` がそれらを束ねる形である。

## `common.h` include policy

`src/common.h` はrepository全体で共有するprivate基盤だけに限定する。現時点では移行期間のaggregateとしてPHP/Zendやtransport周辺headerも含むが、新しいheaderの標準入口にはしない。

- build configやextension rootなど、既存のaggregate利用で必要なinclude。
- すでに事実上globalになっているstandard library / system header。ただし新しいnarrow headerでは直接includeを優先する。
- surface、wrapper、transportで共有するgRPC status / operation constant。これはpure constants headerへ分け、`common.h` は互換のために読む。
- domain固有ではない、project-wideなcapacity constant。

狭いdomainに属するsymbolは `common.h` へ追加しない。

- PHP/Zend base includeは、最終的には `common.h` から外す。PHP/Zend型が必要なheaderは、直接 `php.h` や必要なZend headerを読むか、PHP専用の薄いboundary headerを使う。
- HTTP/2 window / metadata / cache limit policyは `transport_core.h` に置く。
- gRPC wire parsing helperは `protocol_core.h` に置く。
- call exchange stateは `grpc_exchange_state.h` に置く。
- transport object / resource宣言は上記のtransport header groupに置く。
- diagnostic専用field/helperは `src/diagnostic/` 配下に置く。

## Safe first split

低リスクな実装順は次の通り。

1. `transport.h` をaggregate includeとして残したまま、narrow headerを追加する。
2. 最初に `h2_request_headers` とその宣言blockを移す。
3. 次にpersistent cache宣言を移す。cache挙動は変えない。
4. server streaming state宣言は、Zend resource dtorとcancel pathのincludeを確認してから移す。
5. callbackとresponse parser宣言は最後に移す。transportの中でもhotで、bench buildにも再利用されるため。

各stepは独立したissue/commitにする。function body、inline boundary、struct layout、call pathを変えない宣言移動だけならbefore/after性能計測は不要。runtimeに影響する変更が入る場合は、計測付きのperformance issueへ分ける。

## Do not mix into a header-only refactor

- `struct _h2_connection`、`grpc_call`、`server_streaming_call_state` のfield order変更。
- 関数の `static inline` 化やmacro化。
- nghttp2 callback dispatchの変更。
- request header filtering/encodingの変更。
- response parser state transitionの変更。
- persistent connection cache eviction/reuse policyの変更。
- bench専用の観測field/helperをproduction headerへ移すこと。

## Verification for mechanical declaration moves

declaration moveごとのcommitでは次を実行する。

- `git diff --check`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`

callback、parser、connection ownership、status resolutionのboundaryを動かす場合は、HTTP/2/gRPC domain model reviewも実行する。runtime code changeを含める場合は、実装前に該当PHPT/PHPUnit scenarioとbenchmark requirementをissueへ記録する。
