# PR #29 第六パス HTTP/2 / gRPC domain model review 2026-07-12

## Scope

- `287bc939..3081608`（実装commit `e424689`）
- `src/unary_call.c`
- `src/diagnostic/bench.c`
- `src/transport.c`
- `grpc.c`
- `tests/phpt/001-load.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- `tests/phpt/040-fatal-submit-diagnostic-caller-lifetime.phpt`
- `tools/test/check-phpt.sh`
- `tools/test/check-c-sanitizer.sh`
- `docs/SPEC.md`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`

## Reviewer Role

- HTTP/2 / gRPC lifecycle・status taxonomy・call/connection ownership adversary

## Review Prompt Summary

- 第五パスのHigh（unary coreとdiagnostic callerのconnection lifetime契約不一致）およびMedium（deadline detailsがRST cleanup failureへ上書きされる）への修正を再確認する。
- deadline code/details、server由来`grpc-message`、RST submit/flush failureのprimary/secondary failure境界、unary/server streamingの一貫性、dead connection後のfresh connection lifecycleを監査する。

## Issues

- none

## Adequate Fixes Confirmed

- unary coreのreturn契約は、unusable化して`FAILURE`を返す全3経路（submit fatal、stream registration failure、`nghttp2_session_mem_recv` failure）でcache detach、owner clear、unowned時destroyをcallee側に完結させている。production wrapperとbench diagnostic callerはいずれも`FAILURE`後にraw connection pointerを再参照しない。shared ownerが残る場合は`stream_owner_count`が最終destroyを遅延させるため、別streamのlifetimeも壊さない。
- `grpc_lite_status_details_from_call()`はserver由来の非空`grpc-message`という既存のwire detailを維持し、その次にlocal `timed_out`をconnection-scoped I/O/nghttp2 error detailより先に解決する。したがって通常のdeadline cancelでは、RST submit fatalだけでなくRST flush failureもsecondary cleanup failureとなり、`DEADLINE_EXCEEDED / HTTP/2 transport deadline exceeded`を上書きしない。status code側の`timed_out`最優先規則とも整合する。
- unaryはtyped statusを構築してからdead connectionをcallerがevictし、server streamingはtyped statusをcopyしてからowner clear時にdead connectionをdetach/destroyする。どちらもstatus解決中のconnection pointerは有効で、fatal cancel後の次callはcache missからfresh connectionを得る。PHPT 038のpreface countはunary fatal後とstreaming fatal後の両方を固定している。
- `grpc-message`を伴う正常なserver statusの既存priority、deadlineなしのstatus taxonomy、transparent retry predicateには差分がなく、新しいstatus regressionは確認できなかった。
- bench diagnostic surfaceはMINFOとfunction exposureのiffで固定され、production buildでは従来どおり非公開である。今回のlifetime regression testはbench + test-fault + sanitizerの必要な組合せで実行される。

## Verification

- `git diff --check 287bc939..3081608`: pass
- ASan/UBSan buildでPHPT 038 / 040: 2/2 PASS、sanitizer reportなし
- sanitized `test_status_core`: PASS
- source review: unary FAILURE 3経路、deadline/RST submit・flush、server streaming terminal status copy、cache detach/destroy、wrapper/diagnostic callerを相互確認

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
