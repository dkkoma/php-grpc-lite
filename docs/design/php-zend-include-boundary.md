# PHP/Zend include boundary

この文書は、C/PHP extensionとしてのPHP/Zend依存をどこまで許容し、どこから外すべきかを判断するための設計地図である。

目的は `php.h` を機械的に減らすことではない。PHP extensionである以上、Zend object、`zval`、`zend_string`、module globals、request lifecycleに近い層ではPHP/Zend依存が自然である。一方で、pure Cで表せるprotocol helperやtransport policyまで `php.h` に結合すると、C unit、fuzz、教材としての読みやすさ、将来の境界整理が弱くなる。

同時に、「PHP/Zend依存が必要なheaderは `common.h` を読む」という方針にも寄せない。`common.h` は現在 `php.h` だけでなくnghttp2、OpenSSL、socket/system headerまでまとめて読むため、PHP/Zend型だけが必要なheaderから読むには広すぎる。最終形では、`common.h` からPHP/Zend関連includeも外し、PHP/Zend型が必要なheaderは `php.h` や必要なZend headerを直接読む。`common.h` はC standard headers、project-wide constants、必要ならconfig include程度に限定する。

## Target Model

理想形は、依存を次の層に分けることである。

| Layer | Owns | Allowed dependencies | Should not depend on |
|---|---|---|---|
| Pure protocol core | gRPC wire value validation、status parse、timeout formatなどの副作用なしhelper | C standard headers | PHP/Zend、nghttp2、OpenSSL、socket |
| Transport policy core | size/window/metadata limit policy、target/path validation、authority construction | C standard headers。必要ならPHP scalarを呼び出し側でC scalarへ変換 | `php.h`、Zend allocation、nghttp2 session、OpenSSL object |
| PHP scalar boundary | `zend_long` / PHP INI / user optionsをtransport policyへ渡す変換 | PHP/Zend、transport policy header | socket/TLS I/O、nghttp2 callback |
| PHP-owned exchange state | `zend_string` / `smart_str` / queued payload / metadata listのownership | PHP/Zend、必要なsystem I/O型 | PHP object surface、module registration |
| HTTP/2 transport object | `h2_connection`、nghttp2 session、socket、TLS、active stream lifecycle | nghttp2、OpenSSL、socket/system、exchange state | PHP userland object layout、wrapper method implementation |
| PHP metadata conversion | user metadata `zval` からHTTP/2 header、response metadata listからPHP array | PHP/Zend、nghttp2 header representation | connection lifecycle policy、status taxonomy priority |
| PHP extension surface | `Grpc\Channel` / `Grpc\Call` / credentials / module globals | PHP/Zend | low-level parser internals unless orchestration needs them |
| Diagnostic / bench | bench-only observation、diagnostic result shape | PHP/Zend、対象内部header | production coreへの逆依存 |

重要なのは、PHP/Zend依存を「悪」と見ないことである。`zend_string` がpayloadやmetadataの実ownerなら、その層はPHP-awareでよい。逆に、`effective_http2_window_size()` のようなpolicy helperが `zend_long` のためだけに `php.h` を読む状態は、C/PHP境界として薄くできる。

## Current State

現状は `src/common.h` が広いprivate convenience headerとして機能している。

`src/common.h` がまとめて読んでいるもの:

- extension root: `php_grpc.h`
- PHP/Zend: `php.h`、`php_ini.h`、`ext/standard/info.h`、`ext/standard/base64.h`、`Zend/zend_exceptions.h`、`Zend/zend_smart_str.h`
- HTTP/2: `nghttp2/nghttp2.h`
- TLS: OpenSSL headers
- socket/system: `arpa/inet.h`、`fcntl.h`、`netdb.h`、`poll.h`、`sys/socket.h`、`sys/uio.h` など
- shared constants: gRPC status code、batch op constants

このため、`common.h` を読むheaderは、実際には使っていないPHP/Zend、nghttp2、OpenSSL、socket/system依存まで同時に受け取る。

したがって、PHP/Zend依存が自然なheaderでも、最終的に `common.h` を読むべきではない。

- `surface.h` や `module.h` のようにextension全体の土台を扱うheaderも、最終的にはPHP/Zend boundary用の直接includeまたは薄いPHP専用headerへ寄せる。
- `grpc_result.h` のように `zend_string` / `zval` だけが必要なheaderは、`php.h` と必要なC standard headerを直接読む方向が望ましい。
- `wrapper_adapter.h` のように `PHP_METHOD` だけが必要なheaderも、`common.h` ではなくPHP/Zendの必要最小includeに寄せる余地がある。
- `tls_config.h` のようにPHP/Zend型を公開しないheaderは、`common.h` を読まない形を目指す。

現在の主なheader分類:

| Header | Current shape | Assessment |
|---|---|---|
| `protocol_core.h` | C standard headersのみ | 現状維持。理想形に近い |
| `status_core.h` | `grpc_call` forward + C bool | status taxonomyとしては十分薄い。`grpc_call` fieldへ依存する設計は現状許容 |
| `transport_core.h` | C standard headersのみ。PHP由来のscalarは呼び出し側から `int64_t` / `uint64_t` のC scalarとして渡す | transport policy coreとしてPHP/Zendなしを維持 |
| `tls_config.h` | `stddef.h` と OpenSSL headerを直接読む | OpenSSL helperとしてPHP/Zendなしを維持 |
| `grpc_exchange_state.h` | `common.h`、`zend_string`、`smart_str`、`struct iovec` | PHP-owned exchange stateとしてPHP/Zend依存は自然。ただし `common.h` 経由は広すぎる |
| `h2_request_headers.h` | `php.h` + nghttp2、`zval`、`zend_string` | PHP metadata to HTTP/2 header conversion boundaryとしてPHP-awareでよい |
| `grpc_result.h` | `php.h`、`stdbool.h`、`zend_string`、`zval` | PHP result bridgeなのでPHP/Zend依存が自然。`common.h` には戻さない |
| `transport.h` | transport aggregate | 移行中のaggregateとして許容。これ以上新しい責務を足さない |
| `module.h` / `surface.h` / `wrapper_adapter.h` | PHP/Zend surface | PHP/Zend依存が自然 |
| `diagnostic/*.h` | bench/PHP types。`bench_call.h` は必要なPHP/C scalar headerを直接読む | diagnostic boundaryとして許容。ただしproduction coreへ逆流させない |

## Gap Analysis

現状との差分は、次の3種類に分けて扱う。

### 1. Mechanical include narrowing

挙動、struct layout、function boundaryを変えずに、headerが読むincludeを必要最小限へ寄せる。

候補:

- `tls_config.h` を `common.h` 依存からOpenSSL + `stddef.h` 依存へ薄くする。
- `grpc_result.h` はPHP result bridgeなので `php.h` は必要だが、nghttp2/OpenSSL/socketまでは不要。
- `wrapper_adapter.h` は `PHP_METHOD` のためPHP/Zendが必要だが、`common.h` 全体は不要。
- `diagnostic/bench_call.h` はbench-only structで、`zend_long` とC scalarが必要。`common.h` 全体は不要。

この分類はruntime benchmark不要。build/static analysis/C unit/PHPTをgateにする。

### 2. Boundary type conversion

PHP/Zend型をcore helperのsignatureから外す。挙動は変えないが、呼び出し側のcastやunit testを更新する。

候補:

- `transport_core.h` の `zend_long` を `int64_t` などの内部scalarへ置き換える。
- `hash_bytes()` の戻り値を `zend_ulong` ではなく `uint64_t` または `uintptr_t` 相当へ寄せるか判断する。
- PHP INI / user optionsはsurfaceやmodule boundaryで受け取り、transport policy helperへ渡す前にC scalarへ変換する。

これはC/PHP boundaryとして重要だが、signature変更を伴う。C unitを主gateにし、PHPTでsurface互換性を確認する。hot pathの関数境界を増やさない限りbenchmarkは不要。

### 3. Domain boundary split

型やownerの意味を分ける変更。お手本としての効果は大きいが、allocation、lifetime、hot pathへ影響し得る。

候補:

- `h2_request_headers` を「HTTP/2 header storage」と「PHP metadata conversion」に分ける。
- `grpc_exchange_state.h` の `zend_string` / `smart_str` / queue payload ownershipを、field mapに沿ってsub-struct化する。
- response metadata listとPHP result map conversionを、transport parserからより明確なbridgeへ寄せる。

これは慎重に扱う。実装前に仮説、対象workload、before benchmark、採否基準をissueへ書く。改善が見えない場合は採用しない。

## What Not To Do

- `php.h` 依存を減らすために、`zend_string` ownershipを曖昧な `char *` / `size_t` pairへ置き換えない。
- `zval` を扱うheaderを無理にpure C扱いしない。
- `common.h` を削るついでにfunction body、inline化、struct layout、allocation policyを変えない。
- request metadata conversionとHTTP/2 header storageの分離を、benchmarkなしで実装しない。
- `grpc_call` のsub-struct化をinclude整理のついでに行わない。

## Proposed End State

最終的には次の形を目指す。

| Header group | Direction |
|---|---|
| Pure headers | `protocol_core.h`、可能なら `transport_core.h` をC standard headersだけで読めるようにする |
| PHP boundary headers | `surface.h`、`module.h`、`grpc_result.h`、`h2_request_headers.h` のPHP/Zend依存を明示的に許容する。ただし `common.h` 経由ではなく、必要なPHP/Zend includeを直接読む |
| Transport headers | `transport.h` はaggregateに留め、narrow headersではnghttp2/OpenSSL/socket依存を必要なものだけにする |
| Common header | PHP/Zend関連includeを持たない、C standard headersとproject-wide constants中心の共通土台にする。PHP-aware headerの標準入口にはしない |
| Diagnostic headers | production coreから独立し、bench buildだけの情報を `src/diagnostic/` に閉じ込める |

`common.h` はすぐに消す対象ではない。移行期間はprivate aggregateとして残してよい。ただし、最終ゴールではPHP/Zend includeを `common.h` から外す。新しいheader、pure寄りheader、PHP/Zend型だけが必要な小さいboundary headerは `common.h` を読まない方針にする。

## Expected PHP/Zend Boundary Files

最終的にPHP/Zendとの境界を持つ想定ファイルは、次のgroupに限定する。

| File / group | Boundary reason | Target direction |
|---|---|---|
| `php_grpc.h` | extension root、module entry、version/user-agent constant | PHP/Zend boundaryとして維持 |
| `grpc.c` | module lifecycle、INI、module globals、MINIT/MSHUTDOWN/RSHUTDOWN/MINFO | PHP/Zend boundaryとして維持 |
| `src/module.h` | module globals、`PHP_GRPC_LITE_G()` accessor | PHP/Zend boundaryとして維持。ただし `common.h` 経由ではなく必要なPHP/Zend includeへ寄せる |
| `src/surface.h` / `src/surface.c` | `Grpc\Channel` / `Grpc\Call` / credentials / `Timeval` object layoutとclass registration | PHP object surface boundaryとして維持 |
| `src/wrapper_adapter.h` / `src/wrapper_adapter.c` | official wrapper互換のPHP method implementation、`zval` batch handling | PHP userland API boundaryとして維持 |
| `src/grpc_result.h` | `zend_string` / `zval` を使うPHP-visible result shape | PHP result bridge boundaryとして維持。ただし `common.h` は読まない |
| `src/unary_call.h` / `src/unary_call.c` | PHP surfaceからtransportへ渡すorchestration。metadata `zval`、`zend_string` payload/statusを扱う | 当面PHP/Zend boundary。将来、core executionとPHP bridgeを分けられるか検討 |
| `src/server_streaming_call.h` / `src/server_streaming_call.c` | Zend resource、streaming next/cancel result、metadata/status `zval` を扱う | 当面PHP/Zend boundary。resource lifecycle部分はPHP-awareとして維持 |
| `src/h2_request_headers.h` とrequest metadata builder | PHP metadata `zval` / `zend_string` からnghttp2 header blockを作る | PHP metadata to HTTP/2 boundaryとして維持。将来storageとconversionを分ける場合もconversion側はPHP-aware |
| response metadata/result bridge helper | response metadata listをPHP arrayへ変換する | PHP result bridge boundaryへ寄せる候補。transport parserとは分けたい |
| `src/diagnostic/diagnostic.h` / `src/diagnostic/diagnostic.c` / `src/diagnostic/bench.c` | bench/diagnostic resultをPHP arrayへ出す | diagnostic PHP boundaryとして維持。production coreへ逆流させない |

PHP/Zend boundaryから外したいfile/groupは次の通り。

| File / group | Desired state |
|---|---|
| `src/protocol_core.h` / `src/protocol_core.c` | pure Cを維持 |
| `src/status_core.h` / `src/status_core.c` | PHP/Zendなし。`grpc_call`依存を続けるかDTO化するかは別判断 |
| `src/transport_core.h` / `src/transport_core.c` | `zend_long` / `zend_ulong` を外し、C scalarのtransport policy helperへ寄せる |
| `src/tls_config.h` / `src/tls_config.c` | OpenSSL helperとしてPHP/Zendなしへ寄せる |
| `h2_connection` / socket / TLS / nghttp2 callback declarations | PHP/Zend object surfaceに依存しないtransport boundaryへ寄せる |
| `src/common.h` | PHP/Zendなし。C standard headers、config、project-wide constants中心へ縮小する |

まだ完全に固定していない境界は、`unary_call.*`、`server_streaming_call.*`、`h2_request_headers.*`、response metadata/result bridgeである。これらはPHP object/resource orchestrationとtransport executionの両方に触れているため、単純なinclude整理だけで理想形へは行かない。分ける場合は、関数境界、ownership、allocation、hot pathへの影響を子issueで検証してから採否を決める。

## `common.h` policy

`common.h` は移行期間のprivate aggregateとして残すが、新しいheaderの標準入口にはしない。新しいnarrow headerは、C standard / PHP/Zend / nghttp2 / OpenSSL / socket/system headerのうち実際に使うものを直接読む。

`common.h` に新しいdomain-specific struct、transport policy constant、diagnostic-only symbol、nghttp2 callbackやTLS helper専用のincludeを追加しない。PHP/Zend型だけが必要なheaderも、`common.h` ではなく `php.h` や必要なZend headerを直接読む。

gRPC status codeとbatch op constantsは `src/grpc_constants.h` に分ける。これは `common.h` のPHP/Zend依存を外す前段の足場であり、これだけで `status_core.c` やexchange stateがpure Cになるわけではない。

## Approach Order

1. Design and issue split
   - この文書で理想形と現状差分を固定する。
   - 影響がないinclude整理、signatureだけの境界整理、performance-sensitiveなdomain splitを分ける。

2. Mechanical include narrowing
   - `tls_config.h`、`grpc_result.h`、`wrapper_adapter.h`、`diagnostic/bench_call.h` などから始める。
   - runtime挙動を変えない。

3. `transport_core.h` をpure Cへ寄せる
   - `zend_long` / `zend_ulong` 依存を外すか、外さない理由を明確化する。
   - PHP scalarからcore scalarへの変換位置を決める。

4. `common.h` policy enforcement
   - `common.h` に残すものを明文化し、PHP/Zend includeと狭いdomainのincludeを外す。
   - 必要なら `grpc_constants.h` のようなpure constants headerを作る。

5. Performance-sensitive split
   - `h2_request_headers` 分割や `grpc_exchange_state` 分割は別issueでbefore/afterを取る。
   - benchmark結果とdomain model reviewで採否を決める。

## Verification Policy

Mechanical include narrowing:

- `git diff --check`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`

Signature-only core boundary change:

- 上記に加えて、該当C unitの期待値を更新または追加する。
- PHP surfaceが同じであることをPHPTで確認する。

Performance-sensitive split:

- 実装前にbefore benchmarkを取る。
- request header boundaryなら `metadata-header` と `spanner-shape`。
- exchange state / parserなら `cpu-micro`、`payload-streaming`、`large-streaming`。
- HTTP/2/gRPC lifecycleに触れるならdomain model reviewを必須にする。
