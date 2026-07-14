# PR #29 第六パス C ownership / lifetime review 2026-07-12

## Scope

- `287bc939..3081608`（実装commit `e424689`）
- `src/unary_call.c`
- `src/diagnostic/bench.c`
- `src/wrapper_adapter.c`
- `src/transport.c` のstream owner / persistent cache lifecycle helper
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- `tests/phpt/040-fatal-submit-diagnostic-caller-lifetime.phpt`

## Reviewer Role

- `low-level C / persistent-cache ownership and lifetime adversary`

## Review Prompt Summary

- unary coreのreturn contractを、submit failure、stream registration failure、`nghttp2_session_mem_recv` failure、request header failure、deadline、production / diagnostic caller、transparent retry、shared owner、replacement cache entryの各経路で照合した。
- detach、owner clear、call cleanup、最終connection destroyの順序について、double cleanup、leak、stale pointer、owner count不整合、健康なreplacement entryの誤削除がないか確認した。
- production codeと共有moduleは変更・再buildせず、差分と隣接実装のstatic review、および`git diff --check 287bc939..3081608`のみ実施した。

## Issues

- none

## Verification Notes

| 経路 | connection / ownerの帰結 | 判定 |
|---|---|---|
| request size / request header validation failure | stream未登録なのでowner countは変化せず、usable connectionはcache ownerへ残る。両callerはFAILURE後のborrowed pointerを再参照しない | safe |
| `nghttp2_submit_request` non-fatal failure | stream未登録、connectionはcacheに残る。call/header allocationはcallee内で解放し、callerはpointerを再参照しない | safe |
| `nghttp2_submit_request` fatal failure | dead化→pointer一致entryだけdetach→owner clear(no-op)→call/header cleanup→owner 0ならdestroy。FAILURE後にproduction / diagnostic callerともpointerを再参照しない | safe |
| stream registration failure | dead化→detach→未取得ownerのclear(no decrement)→cleanup→owner 0ならdestroy。dead sessionは以後driveしないため、submitted streamのstack pointerをdereferenceするcallbackへは到達せず、残るsession操作はdestroyだけになる | safe |
| `nghttp2_session_mem_recv` failure | dead化→detach→active登録解除→`connection_owned`がtrueのときだけownerを1減算→call cleanup→最後のownerならdestroy。別stream ownerが残る場合はdetached connectionを保持する | safe |
| send / recv connection failure | semantic resultをSUCCESSで先に構築し、callee内ではconnectionをdestroyしない。callerが有効なpointerでunusable判定しcache evictionを担当する | safe |
| deadline before submit / read deadline | pre-submitはowner未取得のままresultを構築。read deadlineはRST結果を含めstatusを構築し、SUCCESS contractのままcallerへ返す。owner clearは`connection_owned` guardでidempotent | safe |
| transparent retry | SUCCESS resultのpointerだけをwrapperが参照し、GOAWAY時のremove後は旧pointerを再利用しない。次attemptはcacheから再取得する | safe |
| shared owner | detachはcache entryだけを外し、`stream_owner_count > 0`ではconnectionを破棄しない。各callのowner clearが一度ずつ減算し、最後のownerだけがdetached connectionを破棄する | safe |
| replacement cache entry | pointer-based detachは対象connectionと一致するentryだけを削除し、key-based removeも`entry->connection == connection`を確認するため、同じkeyのreplacementを破棄しない | safe |

PHPT 040は今回の直接原因だったdiagnostic callerのsubmit-fatal後dereferenceをsanitizer対象へ載せている。registration failureとunary mem-recv failureは新規の専用fault testを持たないが、今回の変更は既存owner invariant（`connection_owned` guard、detach時の遅延destroy、dead sessionの非drive）に沿った対称なcleanupであり、static review上の未解決指摘とはしない。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
