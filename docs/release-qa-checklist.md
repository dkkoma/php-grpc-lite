# Release QA checklist

target default を HTTP/2 にした状態をrelease可能にするための判定表。

`php-grpc-lite` の transport は nghttp2 の1系統にする。ただし現時点は reviewable state であり、release-ready ではない。source-built grpc extension は `ext/grpc/` をsource buildしてloadする。extension 未導入環境で暗黙に別transportへfallbackしない。

## Blocker gates

| gate | status | 判定 | evidence / next |
|---|---|---|---|
| source-built grpc extension packaging | partial | source build前提で `grpc` extension をbuild/install/loadできること | sourceは `ext/grpc/` に配置。install手順は `docs/install-native-extension.md`。CI matrixは継続 |
| extension-required default nghttp2 | partial | default nghttp2を維持するなら、install/load段階でsource-built grpc extension必須を保証すること | `extension_loaded('grpc')` と `defined('Grpc\\VERSION')` で確認する。Composer installだけでは満たさない |
| C extension memory checker | partial | ASAN/UBSANまたはValgrindでHTTP/2 lifecycle fixtureがcleanであること | Docker dev imageのValgrind smokeはclean。release artifact / CI matrixでの再実行は継続 |
| long lifecycle stress | partial | repeated open/close, break, cancel, timeout, raw resource unsetでFD/RSSがiteration比例増加しないこと | Docker短縮stressはclean。release前に `LONG_ITERATIONS=10000` または時間固定stressを実行 |
| FPM / worker lifecycle | partial | FPM worker process / FrankenPHP workerでrequestまたぎpersistent channelが安全に再利用・破棄されること | single-worker PHP-FPM fixtureはrequestまたぎ再利用を確認済み。FrankenPHP workerは別fixtureとして継続 |
| large response decision | done | `100x100KiB` などlarge response例外形状をrelease defaultの対象に含めるか明示すること | release blockerではなくlarge-streaming guidance対象。large bulk streaming閾値は `>=64KiB/message` かつ `>=8MiB/stream` またはlarge payload `>=50 messages` |
| protocol model review | partial | HTTP/2/gRPC状態機械として connection / stream / call / channel scope、flow-control、control frame lifecycleが妥当であること | `docs/protocol-model-review-guide.md` に沿ってレビューする。window/RST_STREAM代表は修正済み、今後のtransport変更では必須gate |

## High-priority compatibility gates

| gate | status | 判定 | evidence / next |
|---|---|---|---|
| `Grpc\CallCredentials::createFromPlugin()` | done | official wrapperの `call_credentials_callback` から `Grpc\Call::setCredentials()` 経由でper-call metadataへ統合 | `ext/grpc/bridge.c` / `tests/Integration/CallCredentialsTest.php` |
| `grpc-timeout` 8桁制限 | done | official wrapperのdeadlineをC側で `grpc-timeout` metadataとtransport deadlineへ反映 | `ext/grpc/bridge.c` / `tests/Integration/DeadlineTest.php` |
| malformed unary frame | done | incomplete / multiple frameを `STATUS_INTERNAL` | `ext/grpc/bridge.c` / `ext/grpc/transport.c` / `tests/Integration/HttpValidationTest.php` |
| cancel observable status | done | cancel-before-wait / cancel-before-iteration は `STATUS_CANCELLED` | `ext/grpc/bridge.c` / `tests/Integration/ControlSemanticsTest.php` |
| metadata control | done | invalid key/value reject、library-owned metadata filter | `docs/research/metadata-control-compat-2026-05-04.md` |
| duplicate metadata policy | accepted diff | gRPC仕様準拠で複数values保持。ext-grpc PHP surfaceとの差分はrelease note対象 | `docs/SPEC.md` |
| client/bidi streaming | scoped out | 現時点のdrop-in対象はunary/server streaming。client streaming / bidi streamingは未実装 | `docs/api-surface.md` / `docs/install-native-extension.md` |

## Decision-process gates

| gate | status | 判定 | next |
|---|---|---|---|
| benchmark run-order bias | open | fixed orderの単発比較をrelease判断の唯一根拠にしない | ABBA / randomized order / repeat median を追加 |
| HTTP/2 benchmark gate | open | shipped transportと同じHTTP/2経路を通常gateでも検証する | `bench/run.sh` / baseline運用はHTTP/2実装を直接測る |
| release SLO under load | open | sequential CLIだけでrelease SLOを判断しない | 100/500/1000/2000 RPS相当、saturation p95/p99を取得 |
| memory upper bound | partial | representative slow consumerは済み。長時間・多streamは未完 | lifecycle stressにFD/RSS閾値を追加。多stream時系列stressは継続 |
| HTTP/2 fault automation | partial | GOAWAY/EOF/RST代表はあり。idle close/server restart等は追加余地 | h2 fault fixtureをQA runner化 |
| DNS resolver deadline | accepted limitation | libc `getaddrinfo()` は同期blockでC extension内からdeadline中断できない | OS resolver設定/host cacheで運用。必要になった段階でasync resolver/c-aresを設計する |

## Current judgment

- target default を HTTP/2 にする設計判断は維持する。
- reviewには出せるが、正式リリースでHTTP/2 defaultを有効化する条件はまだ満たしていない。
- blocker を通す条件は、`./bench/phase2/check-native-release-hardening.sh` をrelease artifact相当のimageで通し、CI matrixへ載せること。
