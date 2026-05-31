# Dead code audit

- Status: Closed
- Created: 2026-05-31
- Branch: codex/dead-code-audit
- Owner: Codex
- Parent: none
- Related-Design: docs/design/php-zend-include-boundary.md

## Background

`hash_bytes()` は過去にpersistent connection identity照合で使われていたが、SHA-256 connection keyへ移行した後も `transport_core` とC unitに残っていた。今回のPHP/Zend include境界整理で見つかったため、同種の未使用helper、古いdiagnostic hook、過去の最適化実験の残骸が他にもないかを独立して確認したい。

この調査はPHP/Zend include境界issueとは独立させる。目的はコードを減らすこと自体ではなく、production path、diagnostic/bench path、test-only pathの境界を明確にし、初学者にも上級者にも「なぜ残っているか」を説明できる状態にすることである。

## Goals

- C実装、header、diagnostic/bench、testsに残る未使用または用途不明のsymbolを洗い出す。
- 削除できるもの、test-onlyへ閉じ込めるもの、production/benchのため残すものを分類する。
- 削除候補ごとに、削除前後の検証方法を明確にする。
- 静的解析で検出できる範囲と、手動確認が必要な範囲を記録する。

## Investigation Targets

- `src/*.c` / `src/*.h` の関数、struct field、macro、constant。
- `src/diagnostic/*` のbench-only helperとproduction coreからの逆依存。
- `tests/unit/*` だけが参照しているhelper。
- PHPTやPHPUnitからだけ到達するPHP-visible function、method、INI、class entry。
- `#ifdef PHP_GRPC_LITE_ENABLE_BENCH` やbuild optionで片側だけ使われるsymbol。
- 過去の最適化やdiagnosticで導入され、現在のproduction pathから外れたhelper。

## Static Analysis Notes

静的解析で見つけやすいもの:

- `static` function / variableの未使用。
- `-Wunused-function`、`-Wunused-variable`、`-Wunused-const-variable` で拾える範囲。
- `rg` で参照が明確に1箇所だけのhelper。
- C unitからしか参照されない非static helper。

静的解析だけでは見つけにくいもの:

- PHP extension entrypoint、`PHP_FUNCTION`、`PHP_METHOD`、class registration経由の到達。
- nghttp2 callback、Zend resource destructor、object handler、function pointer経由の到達。
- macroやgenerated arginfoで間接参照されるsymbol。
- bench build限定、coverage build限定、diagnostic path限定のsymbol。
- struct fieldの「書いているが読んでいない」ケース。callbackやdebug outputで読まれる場合もある。
- public互換性のために残すconstantやmethod。C側から未使用でも削除できない。

## Proposed Approach

1. 現在のbuild flagsと警告設定を確認し、未使用symbol検出に使えるcompiler warningを整理する。
2. `rg` / `git grep` で関数定義と参照数を機械的に一覧化する。
3. `static` にできるもの、削除できるもの、PHP/Zend callbackとして残すものを分類する。
4. C unit / PHPT / bench build / diagnostic buildで到達差分を確認する。
5. 削除候補は1コミット1系統で扱い、必要ならissueを子分けする。

## Verification

調査のみ:

- `git diff --check`

削除または参照境界を変える場合:

- `git diff --check`
- `docker compose run --rm dev sh -lc 'make -j$(nproc)'`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`

diagnostic/benchに触る場合:

- bench buildが必要なら該当build/testを追加する。
- runtime pathやhot pathに影響する削除では、影響するbenchをissueへ記録してから実行する。

## Non-Goals

- 未使用に見えるPHP-visible APIを互換性確認なしに削除しない。
- performance-sensitiveなhelper整理を、before/afterなしに最適化として扱わない。
- `metadata-exchange-split` のdomain boundary変更と混ぜない。
- 大規模なheader再編をこのissueへ混ぜない。

## Decision Log

- 2026-05-31: `hash_bytes()` が過去のproduction helperからtest-only残骸になっていたため、同種のdead codeを独立issueで調査する。
- 2026-05-31: 静的解析は有用だが、PHP extensionのentrypoint、callback、bench build、PHPT到達は静的解析だけでは判定しにくい。機械検出と手動分類を組み合わせる。
- 2026-05-31: `-Wunused-function` / `rg` / 手動参照確認で、production側の未使用helperとbench限定の古いPoC entrypointを削除対象にした。PHP-visible production API、Zend object handler、nghttp2 callback、resource destructorは参照が少なく見えても登録テーブル経由で到達するため削除対象にしない。
- 2026-05-31: `src/wrapper_adapter.c` が `diagnostic/diagnostic.h` 経由でproduction call型を読んでいたため、diagnostic header依存は削除し、必要な `server_streaming_call.h` / `unary_call.h` を直接includeする形にした。これはdead code削除の過程で見つかった境界臭の小修正で、挙動変更はない。

## Findings

### 削除したもの

- `src/wrapper_adapter.c` の `grpc_lite_append_user_agent()`:
  - 現行transportではuser-agentは `append_user_agent_request_header()` 側でHTTP/2 request headerとして付与される。
  - repository内の現行コードから参照がなく、過去のmetadata経由付与の残骸だったため削除した。
- `src/surface.h` の `grpc_lite_call_obj.sent` と `src/wrapper_adapter.c` の代入:
  - 書き込みは `call->sent = true` の1箇所だけで、読み取りはなかった。
  - call lifecycleの判定にも使われていないため削除した。
- `src/transport_core.h` の `GRPC_LITE_DEFAULT_METADATA_SOFT_BYTES`:
  - 定義以外の参照がなかった。
  - 現在のmetadata soft/hard limitは `effective_max_response_metadata_bytes()` とchannel option側で扱っているため削除した。
- `src/diagnostic/bench.c` の `write_data_frame_nonblocking()`:
  - 定義のみで参照がなく、現在の `send_data_callback()` は `write_data_frame()` を使う。
  - bench build限定の未使用helperとして削除した。
- `src/diagnostic/bench.c` の `grpc_lite_multiplex_unary()` と関連する `mux_*` helper、arginfo、function registration:
  - 現行 `bench.php` / `bench/run.sh` から到達せず、production buildでも公開されない古い多重stream PoC entrypointだった。
  - 現行bench entrypointは `grpc_lite_bench_unary_batch()`、`grpc_lite_unary()`、server streaming診断関数に集約されているため削除した。
  - `tests/phpt/001-load.phpt` からも、削除済み関数名をproduction非公開確認リストとして残さないよう外した。

### 残したもの

- PHP method / function entrypoint:
  - `PHP_METHOD(...)`、`PHP_FUNCTION(...)`、`zend_function_entry` 経由で到達するものは、C側の直接参照が少なくても削除対象にしない。
  - production互換surfaceは `tests/phpt/001-load.phpt` と既存PHPTで確認する。
- Zend object handler / resource destructor / nghttp2 callback:
  - `create_object`、`free_obj`、resource destructor、nghttp2 callback table経由で到達するため削除対象にしない。
- `src/unary_call.c` の `connection_reused` / `persistent_reused`:
  - production warning buildでは未使用parameterに見えるが、`PHP_GRPC_LITE_ENABLE_BENCH` buildのdiagnostic resultで使う。
  - 削除するとbench diagnosticのschemaを壊すため残す。
- `grpc_call.pending_data_len` / `pending_write_payload_len`:
  - production pathでは参照されないが、bench write/flow-control観測で使う。
  - `PHP_GRPC_LITE_ENABLE_BENCH` 側へさらに寄せる余地はあるが、struct layoutに関わるためこのissueでは削らない。
- `h2_connection.last_goaway_error_code` / `last_goaway_stream_id`:
  - production transportでGOAWAY受信時に記録し、bench diagnosticで読む。
  - connection diagnosticsとして意味があるため、dead codeではなく観測stateとして残す。

### 後続候補

- `bench.php` には `call_response_queue_wait_us`、`call_response_payload_string_us` など、現在の `grpc_lite_bench_unary_batch()` が返していないseries名を `?? 0` 付きで読む箇所が残っている。
- `bench_process_response_messages_from_offset()` はpayload string生成時間を内部で測るが、現行return schemaへ返していない。
- これらは「削除」だけでなくbench出力schemaとOTEL集計の見直しに関わるため、このissueでは実装変更しない。bench schema整理として扱うなら、現行bench consumerとdocsを確認してから別issueで進める。

## Verification Result

- `git diff --check`: PASS
- 通常build: `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-dead-code-normal-distclean.log 2>&1 || true; rm -rf .libs modules *.lo *.o *.dep src/*.lo src/*.o src/*.dep src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep Makefile config.h config.log config.status autom4te.cache include; phpize >/tmp/grpc-dead-code-normal-phpize.log; ./configure --enable-grpc >/tmp/grpc-dead-code-normal-configure.log; make -j$(nproc)'`: PASS
- bench build / entrypoint check: `docker compose run --rm dev sh -lc 'cd /workspace && make distclean >/tmp/grpc-dead-code-bench-distclean.log 2>&1 || true; rm -rf .libs modules *.lo *.o *.dep src/*.lo src/*.o src/*.dep src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep Makefile config.h config.log config.status autom4te.cache include; phpize >/tmp/grpc-dead-code-bench-phpize.log; ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-dead-code-bench-configure.log; make -j$(nproc) >/tmp/grpc-dead-code-bench-make.log; php -d extension=/workspace/modules/grpc.so -r "exit(extension_loaded(\"grpc\") && function_exists(\"grpc_lite_bench_unary_batch\") && !function_exists(\"grpc_lite_multiplex_unary\") ? 0 : 1);"'`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- warning build: `docker compose run --rm dev sh -lc 'cd /workspace && make clean >/dev/null 2>&1 || true; make EXTRA_CFLAGS="-Wall -Wextra -Wunused-function -Wunused-variable -Wunused-const-variable" -j$(nproc)'`: PASS
  - 今回削除したproject-side unused function warningは消えた。
  - 残るwarningはPHP/Zend macro由来のunused parameter / const variableと、bench build限定で使う `connection_reused` / `persistent_reused`。

## Close Criteria

- dead code候補の一覧と分類が記録されている。
- 削除するもの、残すもの、別issue化するものの判断理由が記録されている。
- 実装変更を含む場合は必要な検証が通っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
