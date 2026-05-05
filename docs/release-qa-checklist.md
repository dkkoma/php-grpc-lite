# Release QA checklist

target default を native にした状態をrelease可能にするための判定表。

`php-grpc-lite` の transport は native nghttp2 の1系統にする。ただし現時点は reviewable state であり、release-ready ではない。native extension は `ext/grpc/` をsource buildしてloadする。extension 未導入環境で暗黙に別transportへfallbackしない。

## Blocker gates

| gate | status | 判定 | evidence / next |
|---|---|---|---|
| native extension packaging | partial | source build前提で `grpc` extension をbuild/install/loadできること | sourceは `ext/grpc/` に配置。install手順は `docs/install-native-extension.md`。CI matrixは継続 |
| extension-required default native | partial | default nativeを維持するなら、install/load段階でnative extension必須を保証すること | `extension_loaded('grpc')` と `function_exists('grpc_lite_unary')` で確認する。Composer installだけでは満たさない |
| native memory checker | partial | ASAN/UBSANまたはValgrindでnative lifecycle fixtureがcleanであること | Docker dev imageのValgrind smokeはclean。release artifact / CI matrixでの再実行は継続 |
| long lifecycle stress | partial | repeated open/close, break, cancel, timeout, raw resource unsetでFD/RSSがiteration比例増加しないこと | Docker短縮stressはclean。release前に `LONG_ITERATIONS=10000` または時間固定stressを実行 |
| FPM / worker lifecycle | partial | FPM worker process / FrankenPHP workerでrequestまたぎpersistent channelが安全に再利用・破棄されること | single-worker PHP-FPM fixtureはrequestまたぎ再利用を確認済み。FrankenPHP workerは別fixtureとして継続 |
| large response decision | done | `100x100KiB` などlarge response例外形状をrelease defaultの対象に含めるか明示すること | release blockerではなくtransport selection guide対象。large bulk streaming閾値は `>=64KiB/message` かつ `>=8MiB/stream` またはlarge payload `>=50 messages` |

## High-priority compatibility gates

| gate | status | 判定 | evidence / next |
|---|---|---|---|
| `Grpc\CallCredentials::createFromPlugin()` | done | callback wrapperとしてper-call metadataへ統合 | `src/Grpc/CallCredentials.php` |
| `grpc-timeout` 8桁制限 | done | microseconds固定ではなく `u/m/S/M/H` に単位変換 | `src/Grpc/AbstractCall.php` |
| malformed unary frame | done | incomplete / multiple frameを `STATUS_INTERNAL` | `src/Grpc/UnaryCall.php` / `src/Grpc/Internal/NativeTransport.php` |
| cancel observable status | partial | cancel-before-wait / cancel-before-iteration は `STATUS_CANCELLED` | native stream cancelの追加faultは継続 |
| metadata control | done | invalid key/value reject、library-owned metadata filter | `docs/research/metadata-control-compat-2026-05-04.md` |
| duplicate metadata policy | accepted diff | gRPC仕様準拠で複数values保持。ext-grpc PHP surfaceとの差分はrelease note対象 | `docs/SPEC.md` |
| client/bidi streaming | scoped out | 現時点のdrop-in対象はunary/server streaming。client streaming / bidi streamingは未実装 | `docs/api-surface.md` / `docs/install-native-extension.md` |

## Decision-process gates

| gate | status | 判定 | next |
|---|---|---|---|
| benchmark run-order bias | open | fixed orderの単発比較をrelease判断の唯一根拠にしない | ABBA / randomized order / repeat median を追加 |
| native benchmark gate | open | shipped transportと同じnative経路を通常gateでも検証する | `bench/run.sh` / baseline運用はnative実装を直接測る |
| release SLO under load | open | sequential CLIだけでrelease SLOを判断しない | 100/500/1000/2000 RPS相当、saturation p95/p99を取得 |
| memory upper bound | partial | representative slow consumerは済み。長時間・多streamは未完 | lifecycle stressにFD/RSS閾値を追加。多stream時系列stressは継続 |
| HTTP/2 fault automation | partial | GOAWAY/EOF/RST代表はあり。idle close/server restart等は追加余地 | h2 fault fixtureをQA runner化 |

## Current judgment

- target default を native にする設計判断は維持する。
- reviewには出せるが、正式リリースでnative defaultを有効化する条件はまだ満たしていない。
- blocker を通す条件は、`./bench/phase2/check-native-release-hardening.sh` をrelease artifact相当のimageで通し、CI matrixへ載せること。
