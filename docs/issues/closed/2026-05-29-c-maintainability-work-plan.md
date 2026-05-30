---
Status: Closed
Owner: Codex
Created: 2026-05-29
Branch: c-practice-work-plan
Parent: docs/issues/open/2026-05-29-c-practice.md
---

# C実装保守性改善の作業計画

## 目的

`docs/issues/open/2026-05-29-c-practice.md` のCプロジェクト方針を、repository-root `grpc` extension 実装へ安全に適用する。

主目的は、HTTP/2 / gRPC transport の互換性を維持したまま、翻訳単位、ヘッダ境界、internal API、単体テスト境界を明確にし、今後の保守・レビュー・静的解析をしやすくすることである。

## 背景

Phase 0開始前の実装は `main.c` が `surface.c`、`protocol_core.c`、`status_core.c`、`transport.c`、`unary_call.c`、`server_streaming_call.c`、`bridge.c`、bench buildでは `diagnostic.c` / `bench.c` も直接includeする構造になっている。

さらに `transport.c` は `transport_core.c` をincludeし、C unit / fuzz testも `protocol_core.c`、`status_core.c`、`transport_core.c` を直接includeしている。これは現状の小さい拡張では動いているが、次の問題を持つ。

- `.c` includeにより翻訳単位の境界が実装構造と一致しない。
- `internal.h` がPHP surface、transport、protocol、bench診断、module globalsの大半を一括で公開している。
- core helperの単体テストが「公開されたinternal API」ではなく `.c` の直接展開に依存している。
- `static` 関数のスコープとモジュール間internal APIの区別が曖昧になりやすい。
- `config.m4` / 静的解析 / coverage が `main.c` 単一翻訳単位前提になっている。

加えて、現行コードには `grpc_lite.backend` INI / channel option、`GRPC_LITE_BACKEND_FRANKEN_GO`、`FrankenGrpc\*` への委譲経路、`franken-go` 専用PHPTが残っている。現在の `docs/SPEC.md` と AGENTS 方針ではruntime transportは nghttp2 + socket/TLS の1系統であり、transport選択optionやfallbackを持たないため、この残存経路はC分割以前に設計方針との不整合として扱う。

一方で、現行のHTTP/2 transportはunary、server streaming、TLS、mTLS、deadline、metadata、persistent connection lifecycleまで実機検証済みであり、分割作業そのものがbehavior changeにならないよう段階を細かく分ける必要がある。

## スコープ

- repository rootの `.c` includeを段階的に廃止する。
- SPECと衝突する `franken-go` backend / backend selectionを整理する。
- `config.m4` でコンパイル対象 `.c` を明示する。
- `internal.h` を責務別ヘッダへ分割する。
- repository root直下に集中しているC実装を、責務が分かるディレクトリ構造へ段階的に整理する。
- protocol / status / transport core helperを、`.c` includeなしでC unit / fuzz testから使えるinternal APIにする。
- PHP extension surface、bridge、transport、call orchestration、diagnostic / benchの依存方向を文書化し、必要なヘッダ境界に反映する。
- 分割後も通常buildと `--enable-grpc-bench` buildの両方を維持する。
- 変更後にHTTP/2/gRPCドメインモデルレビューを実施する。

## 非スコープ

- gRPC API互換性の変更。
- runtime transportの追加、libcurl fallback、transport選択optionの追加。
- client streaming / bidi streamingの実装。
- performance改善を目的にしたhotpath最適化。
- root化後のC source/tests/support code以外の大規模ディレクトリ再編。
- public install headerの提供。このrepositoryのC APIは当面すべてextension内部APIとして扱う。
- FrankenPHP / FrankenGrpc backendの再導入。必要になった場合は、runtime transport 1系統方針を先にSPECで変更する。
- docs / bench / tools の意味変更を伴う大規模再編。Phase 0ではpath移行に限定する。

## 現状棚卸し

- `.c` include:
  - `main.c`: `surface.c`, `protocol_core.c`, `status_core.c`, `transport.c`, `diagnostic.c`, `unary_call.c`, `server_streaming_call.c`, `bridge.c`, `bench.c`
  - `transport.c`: `transport_core.c`
  - `tests/unit/test_protocol_core.c`: `../../protocol_core.c`
  - `tests/unit/test_status_core.c`: `../../status_core.c`
  - `tests/unit/test_transport_core.c`: `../../transport_core.c`
  - `tests/fuzz/fuzz_protocol_core.c`: `../../protocol_core.c`
- ビルド定義:
  - `config.m4` は `PHP_NEW_EXTENSION(grpc, main.c, $ext_shared)` のみ。
- ヘッダ:
  - 実質的なinternal headerは `internal.h` 1つ。
  - include guardはあるが、PHP / Zend、nghttp2、OpenSSL、socket、protocol helper、bench用構造体まで広く含む。
- backend selection残存:
  - `main.c`: `grpc_lite.backend` INI
  - `internal.h`: `GRPC_LITE_BACKEND_FRANKEN_GO`, `franken_channel`, `franken_server_streaming_call`
  - `surface.c`: `grpc_lite_resolve_backend()`, `grpc_lite_construct_franken_channel()`
  - `bridge.c`: FrankenGrpc unary / server streaming delegation
  - `tests/phpt/026-franken-go-backend.phpt`: `franken-go` backend behavior test
- 主要な大きい翻訳対象:
  - `transport.c`: 約3000行
  - `bench.c`: 約2200行
  - `bridge.c`: 約1100行
  - `surface.c`: 約900行

## 計画

1. extension rootをrepository rootへ移すか判断し、採用する場合は最初に移行する。
   - Xdebug / MongoDB PHP Driver のようにrepository rootを `phpize` 用のextension rootにする。
   - 採用する場合、`composer.json` の `php-ext.build-path` は `.` に変更する。
   - `config.m4`、extension entrypoint、C sources、PHPTをroot側へ移し、`cd ext/grpc && phpize` 前提を `phpize` 前提へ更新する。
   - 既存 `src/` は PHP autoload用の `GrpcLite\OpenTelemetry\*` であり、C source用 `src/` と衝突するため先に退避または廃止方針を決める。
   - root化する場合、このphaseではパス移動と参照更新だけを行い、backend削除や `.c` include廃止は混ぜない。

2. SPEC不整合のあるbackend selectionを解消する。
   - `grpc_lite.backend` INIとchannel optionを削除する。
   - `GRPC_LITE_BACKEND_FRANKEN_GO` と `GRPC_LITE_BACKEND_HTTP2` のruntime分岐を削除し、HTTP/2 transportを唯一のruntime pathにする。
   - `franken_channel` / `franken_server_streaming_call` と FrankenGrpc委譲helperを削除する。
   - `026-franken-go-backend.phpt` を削除または現方針に合うテストへ置き換える。
   - SPEC / code-reading-guide / PHPT期待値からbackend selectionの残存記述を消す。

3. 現状の依存関係を固定する。
   - `main.c` include順、各 `.c` が参照する型・関数・static globalを棚卸しする。
   - `internal.h` の宣言を、PHP surface / protocol core / status core / transport core / transport / call orchestration / diagnosticに分類する。
   - 先にドキュメントだけで dependency map を残す。

4. pure core helperから `.c` includeをやめる。
   - `protocol_core.h`、`status_core.h`、`transport_core.h` を追加する。
   - C unit / fuzz testは `.c` ではなく `.h` をincludeする。
   - `config.m4` とC unit runnerで `protocol_core.c`、`status_core.c`、`transport_core.c` を明示的にコンパイル・リンクする。
   - ここではtransport本体やPHP surfaceには触れない。

5. extension本体の複数翻訳単位buildへ移行する。
   - `config.m4` の `PHP_NEW_EXTENSION` にproduction `.c` を列挙する。
   - `main.c` からproduction `.c` includeを外す。
   - `surface.h`、`bridge.h`、`transport.h`、`unary_call.h`、`server_streaming_call.h` など最小限のinternal headerを追加する。
   - module globals、class entry、object handlers、resource idの所有元を明確にする。
   - `static` globalのうち翻訳単位をまたいで共有が必要なものは、所有する `.c` と `extern` 宣言を明示する。

6. bench / diagnostic buildを分離する。
   - `--enable-grpc-bench` 時だけ `bench.c` / `diagnostic.c` をコンパイル対象に含める。
   - production buildではdiagnostic PHP関数とbench構造体が公開・リンクされないことを維持する。
   - bench用の内部APIがproduction pathへ漏れていないか確認する。

7. `internal.h` を薄くする。
   - commonな定数・前方宣言・module globalsだけを残す。
   - PHP/Zend依存、nghttp2/OpenSSL依存、pure C helper依存を可能な範囲でヘッダ別に分ける。
   - opaqueにできるstructはヘッダから実体を隠す。ただしZend object structなど、object handlersやfetch macroに必要なものは無理に隠さない。

8. ディレクトリ構造を段階的に整理する。
   - multi translation unit化が済んでから、ファイル移動だけのコミットを作る。
   - `config.m4`、C unit runner、coverage、static analysis、fuzz buildの参照パスを同時に更新する。
   - 移動コミットでは挙動変更を入れない。
   - 既存レビューやdocsのリンク切れを更新する。

9. 検証とレビューを実施する。
   - 通常build、bench build、C unit、PHPT、PHPUnit、C coverage、C static analysisをDocker内で実行する。
   - HTTP/2 transport / gRPC protocolに触るため、ドメインモデルレビューを実施し、レビュー指摘があれば `docs/reviews/issues/` に残す。

## Root化判断

repository rootをextension rootにする案は採用可能だが、独立した移行として扱う。

採用時の目標構造:

```text
repo root/
  config.m4
  php_grpc.h
  grpc.c
  composer.json

  src/
    surface.c
    surface.h
    bridge.c
    bridge.h
    ...

  tests/
    phpt/
    unit/
    fuzz/

  docs/
  bench/
  tools/
```

既存の `src/OpenTelemetry/*.php` はこの構造と衝突する。対応候補は次のいずれか。

- `support/php/GrpcLite/OpenTelemetry/` などへ移し、Composer autoloadを `GrpcLite\\` => `support/php/GrpcLite/` に変更する。
- `lib/GrpcLite/OpenTelemetry/` などへ移し、root `src/` をC専用にする。
- このrepository packageはComposer runtime codeを提供しないというSPECへ完全に戻し、OpenTelemetry補助PHP classを別packageまたはdocs snippetへ切り出す。

現時点では、root化するなら `src/` はC source用に予約し、PHP補助コードは `support/php/` へ退避する案を第一候補にする。理由は、`src/` がCとPHPで混在すると、PHP拡張単体repoとしてもCプロジェクトとしても意味が曖昧になるため。

root化の完了条件:

- repository rootで `phpize && ./configure --enable-grpc && make` が通る。
- repository rootで `phpize && ./configure --enable-grpc --enable-grpc-bench && make` が通る。
- `composer.json` の `php-ext.build-path` が `.` になっている。
- `modules/grpc.so` 参照が `modules/grpc.so` へ移行されている。
- `cd ext/grpc && phpize` 前提がrunnerから消えている。
- `src/` がC source用で、PHP autoload用コードが別ディレクトリへ移っている、または削除されている。
- `rg 'ext/grpc|/workspace/ext/grpc|cd ext/grpc'` の残存が、historical docs / research / review recordsなど意図したものだけになっている。

## Phase管理

### Phase 0: extension rootのrepository root化

Status: Closed

開始: 2026-05-29

終了: 2026-05-29

目的:

- repository rootを `phpize` 用のextension rootにする。
- 既存のPHP autoload用 `src/` を退避し、root `src/` を今後のC source用に予約する。
- path移行だけを行い、HTTP/2 transportやPHP surfaceのロジックは変更しない。

完了条件:

- rootで通常buildとbench buildが通る。
- 主要runnerが `modules/grpc.so` と root `tests/phpt` を参照する。
- `composer.json` の `php-ext.build-path` が `.` である。
- OpenTelemetry補助PHP classは `support/php/` へ移動し、autoloadが通る。
- issueに検証結果と残存パス判断を記録する。

実施内容:

- `config.m4` とC extension sourceを `ext/grpc/` からrepository rootへ移動した。
- PHPTを `tests/phpt/`、C unitを `tests/unit/`、fuzz harness / corpusを `tests/fuzz/` へ移動した。
- `src/OpenTelemetry/` のPHP補助classを `support/php/GrpcLite/OpenTelemetry/` へ移動し、Composer autoloadを `GrpcLite\\` => `support/php/GrpcLite/` に変更した。
- `composer.json` の `php-ext.build-path` を `.` に変更した。
- runner、Dockerfile、compose、coverage、static analysis、README / SPEC / install docs の現行パスを root extension 前提へ更新した。

検証:

- `docker compose run --rm dev sh -lc 'cd /workspace && make clean >/tmp/grpc-root-clean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep && phpize >/tmp/grpc-root-phpize.log && ./configure --enable-grpc >/tmp/grpc-root-configure.log && make -j$(nproc) >/tmp/grpc-root-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") ? 0 : 1);"'`: PASS
- `docker compose run --rm dev sh -lc 'cd /workspace && make clean >/tmp/grpc-root-bench-clean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep && phpize >/tmp/grpc-root-bench-phpize.log && ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-root-bench-configure.log && make -j$(nproc) >/tmp/grpc-root-bench-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS, 16/16
- `docker compose run --rm dev sh -lc 'cd /workspace && composer dump-autoload >/tmp/composer-dump-autoload.log && php -r "require \"vendor/autoload.php\"; exit(class_exists(\"GrpcLite\\\\OpenTelemetry\\\\TraceContextMetadata\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-coverage.sh`: PASS, lines 76.5%, functions 95.2%
- `docker compose restart spanner-emulator && docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`: PASS, 30 tests / 109 assertions
- `FUZZ_RUNS=100 ./tools/test/check-c-fuzz.sh`: PASS

補足:

- PHPUnitの初回実行ではSpanner emulatorに既存instanceが残っていたため `ListInstancesTest` が1件失敗した。emulatorを再起動した再実行では全件PASS。
- `rg 'ext/grpc|/workspace/ext/grpc|cd ext/grpc'` の残存は、過去issue / benchmark docs / backward-compatible tag build helper / Docker公式extension install先など、履歴または意図した互換用途に限定されている。

### Phase 1: backend selection / franken-go runtime path削除

Status: Closed

開始: 2026-05-29

終了: 2026-05-30 00:02 JST

目的:

- SPECのruntime transport 1系統方針に合わせ、`grpc_lite.backend` と `FrankenGrpc\*` delegationをproduction pathから削除する。
- C分割前にChannel / Call lifecycleをnghttp2 HTTP/2 transportだけへ単純化する。
- franken-go backend用のビルド、PHPT、runner、current design docを削除する。

実施内容:

- `grpc_lite.backend` INI、module global、phpinfo出力を削除した。
- `GRPC_LITE_BACKEND_*`、`franken_channel`、`franken_server_streaming_call`、FrankenGrpc class lookup / method call helper、unary / server streaming delegation branchを削除した。
- `Channel::__construct()` は常にHTTP/2 connection keyを作り、`Channel::close()` / `Call::cancel()` はnative resourceだけを扱う形にした。
- `026-franken-go-backend.phpt`、`check-franken-go-backend.sh`、`tools/frankenphp-grpc-lite-run.sh`、`Dockerfile.franken-grpc-go`、`dev-franken-grpc-go` compose serviceを削除した。
- benchmark runnerから `grpc_lite.backend=franken-go` 注入を削除し、計測ラベルとしての `benchmark.transport` だけを残した。
- README、HTTP/2 transport design、OTEL instrumentation、benchmark README、AGENTSから現行transportとしてのfranken-go説明を削除した。
- `docs/frankenphp-go-backend-design.md` と `docs/frankenphp-grpc-go-client-change-request.md` はcurrent design docとしては廃止した。
- bench diagnostic recordの `backend` fieldは、現方針に合わせて `transport` fieldへ改名した。

検証:

- `docker compose run --rm dev sh -lc 'cd /workspace && make clean >/tmp/grpc-phase1-clean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep && phpize >/tmp/grpc-phase1-phpize.log && ./configure --enable-grpc >/tmp/grpc-phase1-configure.log && make -j$(nproc) >/tmp/grpc-phase1-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") ? 0 : 1);"'`: PASS
- `docker compose run --rm dev sh -lc 'cd /workspace && make clean >/tmp/grpc-phase1-bench-clean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep && phpize >/tmp/grpc-phase1-bench-phpize.log && ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-phase1-bench-configure.log && make -j$(nproc) >/tmp/grpc-phase1-bench-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-coverage.sh`: PASS, lines 76.7%, functions 94.5%
- `docker compose restart spanner-emulator && docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`: PASS, 30 tests / 109 assertions
- `FUZZ_RUNS=100 ./tools/test/check-c-fuzz.sh`: PASS

補足:

- composeは削除済みservice `dev-franken-grpc-go` の既存containerをorphanとして警告した。検証結果には影響しない。
- `rg 'franken|FrankenGrpc|grpc_lite\.backend|GRPC_LITE_BACKEND|backend selection'` の現行残存は、FrankenPHP ZTS native benchmark、open/closed issue、historical review / benchmark / research docsに限定されている。

### Phase 2: pure core helperの `.c` include廃止

Status: Closed

開始: 2026-05-30 00:04 JST

終了: 2026-05-30 05:28 JST

目的:

- `protocol_core.c`、`status_core.c`、`transport_core.c` を直接includeするC unit / fuzz構造をやめる。
- pure core helperを内部ヘッダ宣言 + 明示リンク対象にし、後続の複数翻訳単位化の前段を作る。
- production extension buildでも pure core 3ファイルを独立したコンパイル対象にする。

実施内容:

- `protocol_core.h`、`status_core.h`、`transport_core.h` を追加した。
- pure core helperのテスト対象symbolから `static` を外し、各 `.c` が対応headerを持つ形へ変更した。
- `main.c` / `transport.c` から pure core `.c` includeを削除し、`config.m4` に `protocol_core.c status_core.c transport_core.c` を追加した。
- C unit / fuzz / coverage runnerは、test sourceと対象core sourceを別ファイルとしてコンパイル・リンクする形へ変更した。
- `transport_core.c` 単独リンク時にPHPの `snprintf` macroへ寄らないよう、core source内で `snprintf` macroを解除した。

検証:

- `docker version --format '{{.Server.Version}}'`: PASS, 29.4.0
- `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-phase2-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-phase2-phpize.log && ./configure --enable-grpc >/tmp/grpc-phase2-configure.log && make -j$(nproc) >/tmp/grpc-phase2-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") ? 0 : 1);"'`: PASS
- `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-phase2-bench-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-phase2-bench-phpize.log && ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-phase2-bench-configure.log && make -j$(nproc) >/tmp/grpc-phase2-bench-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-coverage.sh`: PASS, lines 76.3%, functions 94.5%
- `FUZZ_RUNS=100 ./tools/test/check-c-fuzz.sh`: PASS
- `docker compose restart spanner-emulator && docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`: PASS, 30 tests / 109 assertions

レビュー:

- HTTP/2/gRPC domain review: `docs/reviews/issues/2026-05-30-phase2-core-helper-boundary-domain-review.md`
- Result: Blocker/High/Medium/Low none

### Phase 3: extension本体の複数翻訳単位build化

Status: Closed

開始: 2026-05-30 05:30 JST

終了: 2026-05-30 05:54 JST

目的:

- `main.c` にproduction `.c` を直接includeする構造を廃止する。
- `config.m4` でproduction / benchそれぞれのコンパイル対象を明示する。
- Zend class entry、object handlers、resource id、bench diagnostic entrypointの所有元を明確にし、後続のheader分割とディレクトリ移動の前提を作る。

実施内容:

- `config.m4` は通常buildで `main.c protocol_core.c status_core.c transport_core.c surface.c transport.c unary_call.c server_streaming_call.c bridge.c` をコンパイルする形へ変更した。
- `--enable-grpc-bench` 時だけ `diagnostic.c bench.c` を追加コンパイルする形へ変更した。
- `main.c` からproduction `.c` includeを外し、module entry / MINIT / globals所有へ寄せた。
- 複数翻訳単位から参照するclass entry、object handlers、resource id、method table、call orchestration関数を `internal.h` のextern宣言へ切り替えた。
- `internal.h` で `config.h` を読み、`PHP_GRPC_LITE_ENABLE_BENCH` などのconfigure定義が全翻訳単位で見えるようにした。
- `tools/test/check-c-static-analysis.sh` は、unity build前提の `main.c` だけではなく、production / benchの実コンパイル対象 `.c` を全てcppcheckする形へ変更した。
- multi-TU化後のcppcheckで見つかった `zend_long` / `%ld` portability警告は、port値を `(long)` へ明示castして解消した。

性能確認:

- before: `33500e5` (`Phase 2: pure core helperの直接includeを廃止`)
- after: Phase 3 working tree
- before command: `BENCH_TAG=phase3-before-20260530-spanner-shape BENCH_IMPLEMENTATION_LABEL=phase2-before ./bench/run.sh spanner-shape --calls=300 --warmup-calls=20`
- after command: `BENCH_TAG=phase3-after-20260530-spanner-shape BENCH_IMPLEMENTATION_LABEL=phase3-after ./bench/run.sh spanner-shape --calls=300 --warmup-calls=20`

| shape | before p50/p99 us | after p50/p99 us | 判断 |
|---|---:|---:|---|
| begin_txn_unary | 30.5 / 266.5 | 32.7 / 145.5 | p50 +2.2us、p99改善側 |
| commit_txn_unary | 32.6 / 228.3 | 30.8 / 248.2 | p50改善側、p99 +19.9us |
| dml_delete_10col_streaming | 25.9 / 116.7 | 27.8 / 89.3 | p50 +1.9us、p99改善側 |
| dml_insert_10col_streaming | 31.1 / 194.0 | 33.5 / 200.3 | p50 +2.4us、p99 +6.3us |
| dml_update_10col_streaming | 31.0 / 94.3 | 29.7 / 71.9 | 改善側 |
| select_1row_10col_streaming | 32.4 / 209.9 | 32.9 / 89.0 | p50ほぼ同等、p99改善側 |

判断:

- 300 calls / shape のspot checkでは、multi-TU化による明確な性能悪化は観測しなかった。
- p50は一部で +2us台の増加があるが、同時に改善側へ振れたshapeもあるため、このphaseでは揺れの範囲として扱う。
- p99は `commit_txn_unary` と `dml_insert_10col_streaming` で小幅増、他は改善側。継続的な性能判断は、次の性能目的issueで同条件の複数runまたは長めのrunを取る。

検証:

- `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-after-bench-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-after-bench-phpize.log && ./configure --enable-grpc >/tmp/grpc-after-bench-configure.log && make -j$(nproc) >/tmp/grpc-after-bench-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") ? 0 : 1);"'`: PASS
- `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-phase3-final-bench-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-phase3-final-bench-phpize.log && ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-phase3-final-bench-configure.log && make -j$(nproc) >/tmp/grpc-phase3-final-bench-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-c-coverage.sh`: PASS, PHPT 15/15, lines 76.5%, functions 94.5%
- `FUZZ_RUNS=100 ./tools/test/check-c-fuzz.sh`: PASS
- `docker compose restart spanner-emulator && docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`: PASS, 30 tests / 109 assertions

レビュー:

- HTTP/2/gRPC domain review: `docs/reviews/issues/2026-05-30-phase3-multi-tu-domain-review.md`
- Result: Blocker/High/Medium/Low none

### Phase 4: internal header境界の分割

Status: Closed

開始: 2026-05-30 05:55 JST

終了: 2026-05-30 06:08 JST

目的:

- `internal.h` に集中しているcommon include、module globals、PHP surface object、call/transport state、call orchestration、bench diagnostic宣言を責務別ヘッダへ分ける。
- `internal.h` は互換用の薄い集約ヘッダにし、後続のディレクトリ移動で各 `.c` が責務別ヘッダを直接includeできる前提を作る。
- public install headerは作らず、追加ヘッダはすべてextension内部APIとして扱う。

実施内容:

- `common.h`、`module.h`、`surface.h`、`call.h`、`transport.h`、`unary_call.h`、`server_streaming_call.h`、`diagnostic.h`、`bridge.h` を追加した。
- `internal.h` は互換用のprivate aggregate headerとして薄くし、責務別ヘッダを読むだけにした。
- production / bench `.c` とC unitは、`internal.h` ではなく責務別ヘッダを直接includeする形へ変更した。
- public install headerは追加していない。

性能確認:

- 実行なし。
- このphaseは宣言移動とinclude先変更のみで、関数実体、linkage、compile source list、hot pathロジックを変えないため、代表ベンチbefore/after対象外と判断した。

検証:

- `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-phase4-normal2-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-phase4-normal2-phpize.log && ./configure --enable-grpc >/tmp/grpc-phase4-normal2-configure.log && make -j$(nproc) >/tmp/grpc-phase4-normal2-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") ? 0 : 1);"'`: PASS
- `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-phase4-bench2-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-phase4-bench2-phpize.log && ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-phase4-bench2-configure.log && make -j$(nproc) >/tmp/grpc-phase4-bench2-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-c-coverage.sh`: PASS, PHPT 15/15, lines 77.3%, functions 95.0%
- `FUZZ_RUNS=100 ./tools/test/check-c-fuzz.sh`: PASS
- `docker compose restart spanner-emulator && docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`: PASS, 30 tests / 109 assertions

レビュー:

- HTTP/2/gRPC domain review: `docs/reviews/issues/2026-05-30-phase4-header-boundary-domain-review.md`
- Result: Blocker/High/Medium/Low none

### Phase 5: C source / internal headerの `src/` 移動

Status: Closed

開始: 2026-05-30 06:09 JST

終了: 2026-05-30 06:23 JST

目的:

- `config.m4` と extension entrypoint sourceはrepository rootに残し、production / bench C実装とinternal headerを `src/` 配下へ移す。
- ファイル移動と参照更新に限定し、HTTP/2 transport / gRPC protocol / PHP surfaceのロジックは変更しない。
- runner、coverage、static analysis、fuzz、docsの現行パスを `src/` 前提へ更新する。

検証予定:

- 通常build / extension load
- bench build / benchmark entrypoint load
- C unit
- C static analysis
- PHPT
- C coverage
- C fuzz smoke
- PHPUnit

実施内容:

- production C source / internal headerを `src/` 配下へ移動した。
- bench / diagnostic sourceを `src/diagnostic/` 配下へ移動した。
- `config.m4`、unit / fuzz / coverage / static analysis / sanitizer runner、GitHub workflow path、codecov path、現行docsの参照を `src/` 前提へ更新した。
- extension entrypoint sourceはmodule entrypointとしてrepository rootに残し、責務別headerを `src/` からincludeする形にした。
- `include/` は作らず、追加済みheaderはすべてextension内部APIとして `src/` 配下に置いた。

性能確認:

- 実行なし。
- このphaseはファイル移動、include path、runner / docs参照更新、bench-onlyコメント修正に限定し、関数実体、compile source listのproduction/bench境界、hot pathロジックを変えないため、代表ベンチbefore/after対象外と判断した。

検証:

- `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-phase5-normal-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep src/*.lo src/*.o src/*.dep src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-phase5-normal-phpize.log && ./configure --enable-grpc >/tmp/grpc-phase5-normal-configure.log && make -j$(nproc)'`: PASS
- `docker compose run --rm dev sh -lc 'cd /workspace && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") ? 0 : 1);" && make distclean >/tmp/grpc-phase5-bench-distclean.log 2>&1 || true && rm -rf .libs modules *.lo *.o *.dep src/*.lo src/*.o src/*.dep src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep Makefile config.h config.log config.status autom4te.cache include && phpize >/tmp/grpc-phase5-bench-phpize.log && ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-phase5-bench-configure.log && make -j$(nproc) >/tmp/grpc-phase5-bench-make.log && php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-c-coverage.sh`: PASS, PHPT 15/15, lines 76.9%, functions 94.5%
- `FUZZ_RUNS=100 ./tools/test/check-c-fuzz.sh`: PASS
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`: PASS, 30 tests / 109 assertions
- `git diff --check`: PASS

レビュー:

- HTTP/2/gRPC domain review: `docs/reviews/issues/2026-05-30-phase5-src-layout-domain-review.md`
- Result: initial Low 1 fixed; Blocker/High/Medium/Low none

## ディレクトリ構造案

Phase 0後はrepository rootをextension rootとして扱う。次の整理では、PHP extensionとしての入口はroot直下に残し、C実装本体を `src/` へ移す。

```text
repo root/
  config.m4
  php_grpc.h              # extension module declaration / version
  grpc.c                  # MINIT/MSHUTDOWN/MINFO and module entry only

  src/
    common.h
    module.h
    internal.h             # compatibility aggregate; prefer narrower headers
    surface.c
    surface.h             # PHP object surface internal declarations
    wrapper_adapter.c
    wrapper_adapter.h     # Grpc\Call startBatch wrapper adapter declarations
    grpc_exchange_state.h # 1 RPC over 1 HTTP/2 streamの交換状態
    grpc_result.h         # wrapper/orchestration result DTOs
    unary_call.c
    unary_call.h
    server_streaming_call.c
    server_streaming_call.h
    protocol.c
    protocol.h            # gRPC framing/metadata/status internal API
    status.c
    status.h              # status taxonomy internal API
    transport.c
    transport.h           # HTTP/2 transport internal API

    transport/
      connection.c         # h2_connection lifecycle/cache, socket/TLS setup
      connection.h
      callbacks.c          # nghttp2 callbacks
      callbacks.h
      request_headers.c    # request metadata/header assembly
      request_headers.h
      response.c           # response frame/message/metadata processing
      response.h
      core.c               # pure helpers currently in transport_core.c
      core.h

    diagnostic/
      diagnostic.c
      diagnostic.h
      bench_call.h        # bench build専用call metrics state
      bench.c

  tests/
    phpt/
    unit/
    fuzz/
```

この案は最終形ではなく、最初の移動単位の目安とする。特に `transport.c` は現時点で責務が大きいため、最初は `src/transport.c` へ移すだけに留め、`transport/connection.c` などへの細分化はHTTP/2/gRPCドメインレビューとテストを挟んで別コミットにする。

`include/` は外部公開C API用の名前として予約し、この作業では作らない。追加する `.h` はすべて `src/` 配下の内部ヘッダとして扱い、install対象にしない。

移行順は次を基本にする。

1. `tests/` 配下を `phpt/`, `unit/`, `fuzz/` に揃える。
2. pure core helperを `src/protocol.c`, `src/status.c`, `src/transport/core.c` と対応する `src/**/*.h` へ移す。
3. production `.c` を `src/` へ移す。
4. bench / diagnostic `.c` を `src/diagnostic/` へ移す。
5. transport本体を必要に応じて `src/transport/` 内で細分化する。

## 進め方

- 作業は小さなコミットに分ける。
- 最初の実装コミットは backend selection削除に限定し、SPECのruntime transport 1系統方針と実装を揃える。
- 次の実装コミットは pure core helperの `.c` include廃止に限定する。
- extension本体の複数翻訳単位化は、function visibilityとstatic globalの調整が大きくなるため単独コミットにする。
- bench / diagnostic分離はproduction buildとbench buildの差分確認を独立コミットにする。
- ディレクトリ移動は、コンパイル単位化が安定してから実施する。移動とロジック変更を同じコミットにしない。
- behavior changeが出た場合は、分割作業に混ぜず、別issueへ切り出す。

## リスク

- `static` 関数が翻訳単位をまたいで暗黙利用されているため、ヘッダ化時にinternal APIが過剰に広がる可能性がある。
- `static` globalの所有元を誤ると、Zend class entry / object handlers / resource idの初期化順に影響する。
- `--enable-grpc-bench` の条件付きコンパイルでproduction buildとの差分が壊れる可能性がある。
- C unit / coverage runnerは現在 `.c` include前提のため、リンク対象の指定漏れでCI gateが壊れやすい。
- `transport.c` はHTTP/2 connection lifecycle、gRPC frame parse、metadata、persistent cacheが近接しており、無理な分割はドメイン境界を悪化させる可能性がある。
- backend selection削除はbehavior changeではなくSPEC整合と扱うが、既存PHPT期待値とphpinfo出力が変わるためテスト修正が必要になる。
- ファイル移動によりdocs、review issue、coverage/static analysis runnerのパス参照が古くなる可能性がある。

## 判断ログ

- この作業は性能改善として扱わない。ベンチは回帰確認に留める。
- multi translation unit化やhot pathのvisibility変更など、性能悪化の可能性がある構造変更では、代表ベンチのbefore/afterをissueに記録してからphaseを閉じる。beforeは直前phaseのコミット、afterは作業差分で同条件実行する。
- C APIは外部公開しない。追加するヘッダは `src` 内のinternal APIであり、install対象にしない。外部公開C APIを持つまで `include/` は作らない。
- 現方針では `franken-go` backendは残さない。もし再度必要になった場合は、C分割作業に混ぜず、SPEC変更と別issueから始める。
- pure helperはPHP/Zend依存を減らせる優先候補だが、Zend allocatorや `zend_string` を使うhelperは無理にpure C化しない。
- transport本体の責務分離は、`.c` include廃止後に別issueへ分ける可能性を残す。
- 既存のHTTP/2/gRPC挙動を守ることを最優先し、構造改善のためにstatus taxonomy、deadline、metadata、connection lifecycleの意味を変えない。

## 検証

実装フェーズでは最低限以下を実行する。

- `docker compose run --rm dev sh -lc 'phpize && ./configure --enable-grpc && make clean && make -j$(nproc)'`
- `docker compose run --rm dev sh -lc 'phpize && ./configure --enable-grpc --enable-grpc-bench && make clean && make -j$(nproc)'`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`
- `./tools/test/check-c-static-analysis.sh`
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`

HTTP/2/gRPCドメインモデルに影響する差分が出た場合は、代表的なunary / server streaming / TLS / mTLS / deadline / metadata / persistent reuseのPHPTまたは統合テストを追加で実行する。

## 完了条件

- production buildで `grpc.c` や他のproduction `.c` が別のproduction `.c` をincludeしていない。
- runtime backend selectionがなく、HTTP/2 transportだけがproduction call pathとして残っている。
- C unit / fuzz testが `.c` includeではなくヘッダ宣言とリンク対象 `.o` に依存している。
- `config.m4` がproduction / bench buildのコンパイル対象を明示している。
- repository root配下のproduction source、internal header、diagnostic source、testsが責務別ディレクトリに整理されている。
- `internal.h` の責務が縮小され、追加ヘッダのinclude guardと依存方向が明確である。
- Docker内のC unit、PHPT、C coverage、C static analysis、PHPUnitが通る。
- HTTP/2/gRPCドメインモデルレビューでBlocker / High / Medium / Lowがnoneになる。
- このissueに修正コミット、検証結果、レビュー結果を追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。

## 完了記録

終了: 2026-05-30 06:26 JST

修正コミット:

- `cb4439b` Phase 0: extension rootをrepository rootへ移行
- `cbf8c5c` Phase 1: backend selectionとfranken-go経路を削除
- `33500e5` Phase 2: pure core helperの直接includeを廃止
- `de4c7c8` Phase 3: extension本体を複数翻訳単位化
- `82979af` Phase 4: internal header境界を分割
- `1642c64` Phase 5: C実装をsrc配下へ移動

最終状態:

- production `.c` の直接includeは廃止済み。
- runtime backend selection / franken-go runtime pathは削除済み。
- C unit / fuzzは `.c` includeではなくheader宣言と対象sourceの明示リンクへ移行済み。
- `config.m4` はproduction / bench buildのsource listを明示している。
- C実装と内部headerは `src/`、bench / diagnostic実装は `src/diagnostic/` へ整理済み。
- `internal.h` は責務別headerのprivate aggregateに縮小済み。
- `include/` は作らず、外部公開C APIは提供しない方針を維持した。
- PR follow-upでPHP拡張repoの慣例に寄せ、rootのentrypoint sourceを `main.c` から `grpc.c` へrenameし、module declaration / version用の `php_grpc.h` を追加した。
- PR follow-upで `Grpc\*` class registrationを `grpc_lite_register_surface_classes()` として `src/surface.c` 側へ寄せ、method table、object handlers、create/free callbacksを `static` に戻した。
- PR follow-upで `Grpc\Call::startBatch()` のmethod implementation宣言を `src/wrapper_adapter.h` へ移し、`surface.c` からwrapper adapterへの依存をheaderで明示した。
- PR follow-upで `Grpc\Call::cancel()` / `Grpc\Call::getPeer()` を `src/surface.c` へ移し、wrapper adapterは `startBatch()` のbatch adapterに絞った。
- PR follow-upで `src/bridge.c` / `src/bridge.h` を `src/wrapper_adapter.c` / `src/wrapper_adapter.h` へrenameし、`startBatch()` をファイル先頭へ移動した。
- PR follow-upで `src/call.h` を `src/grpc_exchange_state.h` / `src/grpc_result.h` / `src/diagnostic/bench_call.h` へ分割し、型名は変更せず責務単位のファイル名だけを整理した。

最終検証:

- Phase 5で通常build / extension load、bench build / bench entrypoint load、C unit、C static analysis、PHPT + C coverage、C fuzz smoke、PHPUnit、`git diff --check` がPASS。
- Phase 3のmulti-TU化では代表ベンチ `spanner-shape --calls=300 --warmup-calls=20` のbefore/afterを実施し、明確な性能悪化は観測しなかった。
- Phase 4 / Phase 5は宣言整理・ファイル移動中心でhot pathロジックを変えないため、代表ベンチbefore/after対象外と判断した。
- PR follow-upの `main.c` -> `grpc.c` rename / `php_grpc.h` 追加後、通常build / extension load、bench build / bench entrypoint load、C static analysis、`git diff --check`、`.c` direct include残存なしを確認した。
- PR follow-upのsurface registration整理後、通常build / `Grpc\Call` class load、bench build / bench entrypoint load、PHPT 15/15、C static analysis、PHPUnit 30 tests / 109 assertions、`git diff --check` を確認した。
- PR follow-upの `cancel()` / `getPeer()` surface移動後、通常build / `Grpc\Call` class load、PHPT 15/15、C static analysis、`git diff --check` を確認した。
- PR follow-upの `wrapper_adapter` rename / `startBatch()` 先頭移動後、通常build / `Grpc\Call` class load、bench build / bench entrypoint load、PHPT 15/15、C static analysis、PHPUnit 30 tests / 109 assertions、`git diff --check` を確認した。
- PR follow-upの `call.h` 責務別分割後、通常build、bench build、C unit、PHPT 15/15、C static analysis、PHPUnit 30 tests / 109 assertions、`git diff --check` を確認した。
- PR全体の最終代表ベンチとして、merge-base `b5592d4` とHEAD `41855e1` を同一Docker compose project / 同一test-serverで `spanner-shape --calls=1000 --warmup-calls=50` により2回ずつ比較した。通常build条件は追加最適化flagなし。

PR全体 representative benchmark:

| measurement | base p50 avg us | head p50 avg us | p50判断 | base p99 avg us | head p99 avg us | p99判断 |
|---|---:|---:|---|---:|---:|---|
| `begin_txn_unary` | 31.1 | 26.5 | 改善方向 | 197.2 | 108.0 | 改善方向 |
| `commit_txn_unary` | 27.8 | 28.7 | ほぼ同等 | 135.3 | 130.1 | ほぼ同等 |
| `dml_delete_10col_streaming` | 26.5 | 23.3 | 改善方向 | 70.9 | 69.1 | ほぼ同等 |
| `dml_insert_10col_streaming` | 24.9 | 24.5 | ほぼ同等 | 129.9 | 96.7 | 改善方向 |
| `dml_update_10col_streaming` | 27.2 | 25.1 | 改善方向 | 89.5 | 77.2 | 改善方向 |
| `select_1row_10col_streaming` | 26.6 | 27.4 | ほぼ同等 | 94.9 | 150.4 | tail悪化方向、継続観測 |

run id:

- base: `pr-whole-before-20260530-spanner-shape`, `pr-whole-before2-20260530-spanner-shape`
- head: `pr-whole-after-20260530-spanner-shape`, `pr-whole-after2-20260530-spanner-shape`

判断: p50は全shapeでほぼ同等または改善方向で、PR全体として明確な固定費悪化は観測しなかった。p99はtail noiseが大きく、`select_1row_10col_streaming` だけHEAD側で悪化方向に見えるため、性能中立を強く主張するのではなく「代表shapeのp50では明確な悪化なし、tailは継続観測」と扱う。

レビュー:

- Phase 2: `docs/reviews/issues/2026-05-30-phase2-core-helper-boundary-domain-review.md`, Blocker/High/Medium/Low none
- Phase 3: `docs/reviews/issues/2026-05-30-phase3-multi-tu-domain-review.md`, Blocker/High/Medium/Low none
- Phase 4: `docs/reviews/issues/2026-05-30-phase4-header-boundary-domain-review.md`, Blocker/High/Medium/Low none
- Phase 5: `docs/reviews/issues/2026-05-30-phase5-src-layout-domain-review.md`, initial Low 1 fixed, Blocker/High/Medium/Low none
