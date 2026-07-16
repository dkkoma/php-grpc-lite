# 1xx pass-5 fix HTTP/2 / gRPC domain model review pass 6 2026-07-15

## Scope

- consolidated pass-5 finding対応後の未コミット差分
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/status_core.c`
- `src/transport_core.[ch]`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/diagnostic/bench_call.h`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- 1xx issue、pass-5 review record、関連design / code-reading / verification docs

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer (pass 6)

## Review Prompt Summary

- pass-5のsilent peer lifecycle、pre-final wire-header resource failureのcode / details owner、invalid-header callbackのTEMPORAL cutoff oracleを対象に、response phase / status transition、HTTP/2 stream action、call / connection scope、production / diagnostic boundaryを確認した。
- `grpc-status` / `grpc-message` / `grpc-status-details-bin`のterminal gate、unary / server streaming / raw diagnosticの終了条件、`RST_STREAM(CANCEL)` / stream close / connection reuse、metadata/status/error taxonomyとcurrent docsの整合を横断確認した。

## Review Verification

- END_STREAMなし`FINAL_INITIAL`のstatus fieldはcall-localな`initial_grpc_status_seen`で分類され、valid blockのframe-endでproduction / diagnostic共有の`grpc_protocol_enforce_terminal_initial_status_fields()`が`invalid_grpc_status`を確定して対象streamへ`RST_STREAM(CANCEL)`をsubmitする。connectionをdead扱いせず、stream close callbackを通じてunary / server streaming / raw diagnosticの待機を終了する責務分離になっている。
- raw fixtureは3 status fieldそれぞれについてHEADERS後にsilentとなり、productionは2秒guard内の`UNKNOWN`、exact CANCELを受信した同一connection follow-up、diagnosticは`failed=1` / `timed_out=false` / `stream_error_code=8`を要求するため、peerの追加DATA / trailers / EOFに依存する経路は残っていない。
- `metadata_too_large`は`status_core.c`でfinal HTTP status未観測時もHTTP fallbackより先に`RESOURCE_EXHAUSTED`を選ぶ。details builderも同じcode / classification pairをgeneric HTTP、wire `grpc-message`、I/O fallbackより先に評価し、`response header/metadata budget exceeded`を返すため、codeとdetailsのprimary failure ownerが一致する。
- invalid regular field 129個のcontrolでは`:status`を含むcall-local wire budgetが128 entriesで満杯となり、128回目のinvalid-header callbackがoverflowしてTEMPORALを返す。productionのvalue-free traceとbench-build専用のiteration-local counterはcallback entryをaccounting前に観測し、両consumerともshared ownerのreturnを変換しない。production TEMPORAL握り潰しとdiagnostic callback no-opのmutationがそれぞれ新oracleを失敗させることも記録済みである。
- phase / status / wire budgetはgRPC Call / HTTP/2 Stream scopeに留まり、Channel identity、connection cache、別streamへ新stateを持ち込んでいない。CANCEL後のconnection reuseとforeign stream ownershipも既存modelを維持し、production traceは既存のopt-in diagnostic boundary、counterは`PHP_GRPC_LITE_ENABLE_BENCH`内に閉じている。
- `docs/SPEC.md`、exchange state / protocol classification設計、code-reading guide、compatibility checklist、fixture catalog、verification matrix、pass-5 record、work issueは最終modelと検証結果へ更新されている。`git diff --check`はPASS。
- verification記録を照合した: test-server rebuild / restart PASS、`./tools/test/check-phpt.sh` 28/28 PASS、`./tools/test/check-c-unit.sh` 4/4 suites PASS、PHPUnit 31 tests / 116 assertions PASS、`./tools/test/check-c-static-analysis.sh` findings none。

## Issues

### Blocker

none

### High

none

### Medium

none

### Low

none

### Design Decision

none

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
