# PR #29 第九パス敵対的レビュー統合 2026-07-14

## Scope

- PR #29 前回レビュー済みHEAD `1faf80a150dedbbf278a6cb4fbbf914ebb687b69`
- 第八パス対応commit `1c55f56793bcf788b3d839942a3bee7b13487f43`
- 現在HEAD `76d282766091de75bd670f00cd05df8ef264335f`
- `src/transport.c` のconnection destroy trace event
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `docs/guides/code-reading-guide.md`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/SPEC.md` §4.2
- deadline / RST_STREAM / persistent connection reuseに関係するC lifetime、test seam、review record

## Reviewer Role

- PR adversary / HTTP/2・gRPC domain model / C lifetime / test oracle・documentation統合 reviewer

## Review Prompt Summary

- 第八パスLow 2件が完全に修正されたかを再確認し、元issueのdeadline / RST_STREAM / connection lifecycle scope内でrequired gateを再実施した。
- domain model、C lifetime、test oracle・build boundaryを独立にレビューし、cross-triage後にスコープ内で再現可能な指摘だけを採用した。

## Issues

### REVIEW-20260714-001: best-effort reuseのfallback説明がRSTの即時失敗を含んでいない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `C lifetime / HTTP/2 connection lifecycle adversary`
- Finding: 元issueの修正文は現在のreuse fallbackを「RST flushがgrace deadlineを超過した場合」と説明する。しかし実装はdeadline超過だけでなく、`nghttp2_submit_rst_stream()` の即時失敗、および `nghttp2_session_send()` / socket / TLS / coalesced-buffer flushの即時失敗でもconnectionをdeadにしてreuseを禁止する。`docs/SPEC.md` §4.2も「RST書き込み失敗時」としており、「SPEC §4.2のとおり」とするcurrent-state文がfallback集合を狭く記述している。
- Evidence: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md:28`、`src/transport.c:360-369`、`src/transport.c:1968-2023`、`docs/SPEC.md:92`
- Expected model: deadlineはstream-scopedである一方、RSTを安全にsubmit / flushできなかったconnectionはconnection-scoped failureとしてdeadにし、後続callはfresh connectionへfallbackする。current-state文も「RST submitまたはflush失敗（grace deadline超過を含む）、もしくはpreflight drain cap超過」と実装・SPECに一致させる。
- Why it matters: 即時RST submit / write failure後にconnectionを捨てることは、本issueの「stream-local deadlineでも壊れたconnectionは再利用しない」という安全側lifecycle contractである。timeoutだけを列挙すると、fatal nghttp2 sessionやpartial wire stateを再利用可能と誤読し得る。runtime実装は正しく、文書精度だけの問題なのでLowとする。
- Recommended fix: 元issue28行目を「reuseはSPEC §4.2どおりbest-effort（RST submit / flush失敗〔grace deadline超過を含む〕またはpreflight drain cap超過時はfresh connectionへフォールバック）」等へ直す。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `cancel_grpc_call_stream()` と `send_pending_h2_frames_with_deadline()` の全failure branchをSPEC §4.2および元issue28行目と静的に照合した。
- Notes: streaming deadlineの旧説明を「変更前」+過去形へ直した部分はadequate。本指摘は同paragraph後半のcurrent fallback clauseだけを対象とする。

## Prior Finding Recheck

- `wire.connection_close`のrenameはadequate。`transport.connection_destroy`はlocal `h2_connection` destructor入口を表し、TLS / fd / nghttp2 session解放前、preface前setup failureでも発火することが実装コメントとcode-reading guideに明記された。PHPT 041の132件destroy invocation oracleも維持される。
- streaming deadlineの旧挙動を「変更前」と過去形へ直した部分はadequate。
- runtime / C lifetime / dead・draining・active owner・production / diagnostic boundaryに新たな欠陥は確認しなかった。

## Scope Triage

- PHPT 041のlocal変数名`$closeCount`はevent filter、assert message、周辺コメントがdestroy semanticsを明示し、oracleの意味や検出力を変えないためgeneric polishとして非指摘。
- historical review record内の`wire.connection_close`は過渡的な指摘履歴として正当で、current-state documentation defectではない。
- 一般的なCI matrix拡張、bench-only variant、stable connection idは元issue外として非指摘。

## Verification

- ASan / UBSan production lane: 22 PASS / 4 SKIP、reportなし
- ASan / UBSan bench+fault lane: 26/26 PASS、reportなし
- NTS PHPT: 26/26 PASS（ZTS後のNTS module再構築を含め2回）
- ZTS PHPT: 24 PASS / 2 SKIP、0 FAIL
- C unit: protocol_core / status_core / transport_core PASS
- C static analysis: production / benchともexit 0
- PHPUnit: 31 tests / 116 assertions PASS
- `bash -n`、`git diff --check main...HEAD`、`git diff --check 1faf80a..HEAD`: PASS
- PHPUnit初回はZTS検証が共有`modules/grpc.so`をZTSバイナリへ置換したためNTS loaderでABI不一致となった。NTS PHPTで通常moduleを再構築後に再実行し、31/31 PASSを確認した（PR defectではない）。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 1 (Open)
- Design Decision: none

