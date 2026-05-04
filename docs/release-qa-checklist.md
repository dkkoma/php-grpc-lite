# Release QA checklist

release default を native にするための判定表。

`php-grpc-lite` の target default は native のまま進める。ただし release artifact は native extension を同梱・必須化し、extension 未導入環境で暗黙に curl fallback しない。curl は `php_grpc_lite.transport=curl` の明示 stable route として残す。

## Blocker gates

| gate | status | 判定 | evidence / next |
|---|---|---|---|
| native extension packaging | partial | release artifact として `grpc` extension をbuild/install/loadできること | module名とbuild targetは `grpc.so` 化済み。install artifact整理は継続 |
| extension-required default native | partial | default nativeを維持するなら、install/load段階でnative extension必須を保証すること | `extension_loaded('grpc')` は満たした。package install時の必須化は継続 |
| native memory checker | open | ASAN/UBSANまたはValgrindでnative lifecycle fixtureがcleanであること | `VALGRIND=1 ./bench/phase2/check-native-lifecycle-stress.sh` 入口は追加済み |
| long lifecycle stress | open | repeated open/close, break, cancel, timeout, raw resource unsetでFD/RSSがiteration比例増加しないこと | 100 iterationは済み。release前に10k+または時間固定stressへ拡張 |
| FPM / worker lifecycle | open | FPM worker process / FrankenPHP workerでrequestまたぎpersistent channelが安全に再利用・破棄されること | CLI stressとは別に実行環境fixtureが必要 |
| large response decision | open | `100x100KiB` などlarge response例外形状をrelease defaultの対象に含めるか明示すること | small Spanner主用途と分離。large response tuningまたはknown limitation化 |

## High-priority compatibility gates

| gate | status | 判定 | evidence / next |
|---|---|---|---|
| `Grpc\CallCredentials::createFromPlugin()` | done | callback wrapperとしてper-call metadataへ統合 | `src/Grpc/CallCredentials.php` |
| `grpc-timeout` 8桁制限 | done | microseconds固定ではなく `u/m/S/M/H` に単位変換 | `src/Grpc/AbstractCall.php` |
| malformed unary frame | done | incomplete / multiple frameを `STATUS_INTERNAL` | `src/Grpc/UnaryCall.php` / `src/Grpc/Internal/NativeTransport.php` |
| cancel observable status | partial | cancel-before-wait / cancel-before-iteration は `STATUS_CANCELLED` | mid-flight curl cancelとnative stream cancelの追加faultは継続 |
| metadata control | done | invalid key/value reject、library-owned metadata filter | `docs/research/metadata-control-compat-2026-05-04.md` |
| duplicate metadata policy | accepted diff | gRPC仕様準拠で複数values保持。ext-grpc PHP surfaceとの差分はrelease note対象 | `docs/SPEC.md` |

## Decision-process gates

| gate | status | 判定 | next |
|---|---|---|---|
| benchmark run-order bias | open | fixed orderの単発比較をrelease判断の唯一根拠にしない | ABBA / randomized order / repeat median を追加 |
| release SLO under load | open | sequential CLIだけでrelease SLOを判断しない | 100/500/1000/2000 RPS相当、saturation p95/p99を取得 |
| memory upper bound | partial | representative slow consumerは済み。長時間・多streamは未完 | RSS/FD時系列stressへ拡張 |
| HTTP/2 fault automation | partial | GOAWAY/EOF/RST代表はあり。idle close/server restart等は追加余地 | h2 fault fixtureをQA runner化 |

## Current judgment

- target default を native にする設計判断は維持する。
- release default native はまだ不可。
- blocker を通す条件は、defaultをcurlへ戻すことではなく、native extension packagingとmemory/lifecycle QAをrelease artifact側で満たすこと。
