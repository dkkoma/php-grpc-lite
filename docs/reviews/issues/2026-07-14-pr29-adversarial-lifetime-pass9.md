# PR #29 第九パス C lifetime / HTTP/2 connection lifecycle review 2026-07-14

## Scope

- `1faf80a150dedbbf278a6cb4fbbf914ebb687b69..76d282766091de75bd670f00cd05df8ef264335f`（対応commit `1c55f56`、レビュー記録commit `76d2827`）
- `src/transport.c` の `destroy_h2_connection()`、detached connection、active owner、dead / draining lifecycle
- `src/unary_call.c` / `src/server_streaming_call.c` のdeadline cancel、fatal callback unwind、owner cleanup
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt` のdestroy oracle
- `docs/guides/code-reading-guide.md`、`docs/SPEC.md` §4.2、元issueのcurrent-state説明

## Reviewer Role

- C lifetime / HTTP/2 connection・stream lifecycle adversary

## Review Prompt Summary

- 第八パスLow 2件への対応について、destroy eventのrenameと文書修正がlocal object / wire lifecycleを正しく区別し、runtime ownershipやproduction / diagnostic boundaryを変えていないか静的に再監査した。
- 元issueの範囲に限定し、dead terminality、draining接続上のadmitted stream、callback fatal unwind、RST flush failure、`locally_cancelled` cleanup、diagnostic callerのdetach / destroyを再確認した。
- 親作業と競合しないようbuild / testは実行していない。

## Issues

### Blocker

- none

### High

- none

### Medium

- none

### Low

#### REVIEW-20260714-001: best-effort reuseのfallback説明がRSTの即時失敗を含んでいない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `C lifetime / HTTP/2 connection・stream lifecycle adversary`
- Finding: 元issueの修正文は、現在のreuse fallbackを「RST flushがgrace deadlineを超過した場合」と説明する。しかし実装はdeadline超過だけでなく、`nghttp2_submit_rst_stream()` の即時失敗、および `nghttp2_session_send()` / socket / TLS / coalesced-buffer flushの任意の即時失敗でもconnectionをdeadにしてreuseを禁止する。`docs/SPEC.md` §4.2も「書き込み失敗時」としており、「SPEC §4.2のとおり」とする現在のissue文はfallback集合を狭く記述している。
- Evidence: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md:28`、`src/transport.c:360-369`（RST submit失敗で即dead）、`src/transport.c:1968-2023`（timeoutに限らずsend / flushの全失敗でdead）、`docs/SPEC.md:92`（RST書き込み失敗時の接続破棄fallback）。
- Expected model: deadlineはstream-scopedである一方、RSTを安全にsubmit / flushできなかったconnectionはwireとsessionの整合を保証できないためconnection-scoped failureとしてdeadにし、後続callはfresh connectionへfallbackする。文書上も「RST submitまたはflush失敗（grace deadline超過を含む）、もしくはpreflight drain cap超過」と実装・SPECに一致させる。
- Why it matters: immediate RST submit / write failureでconnectionを捨てることは、本issueの「stream-local deadlineでも壊れたconnectionは再利用しない」という安全側のlifecycle contractである。grace timeoutだけを列挙すると、将来の保守で即時失敗後にもreuseすべきだと誤読され、partial frameまたはfatal nghttp2 sessionの再駆動につながり得る。runtime実装は正しく、文書精度の問題なのでLowとする。
- Recommended fix: 元issue28行目を、例えば「reuseはSPEC §4.2どおりbest-effort（RST submit / flush失敗〔grace deadline超過を含む〕またはpreflight drain cap超過時はfresh connectionへフォールバック）」へ直す。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `cancel_grpc_call_stream()` と `send_pending_h2_frames_with_deadline()` の全failure branchを `docs/SPEC.md` §4.2および元issue28行目と静的に照合した。
- Notes: streaming deadlineの旧説明を「変更前」+過去形へ直した部分はadequateであり、本指摘は後半のcurrent fallback clauseだけを対象とする。

### Design Decision

- none

## Verification Notes

- 第八パスのdestroy event指摘はadequateに修正されている。eventは `wire.connection_close` から `transport.connection_destroy` へrenameされ、`destroy_h2_connection()` の入口でlocal `h2_connection` objectがdestructorへ入る事象を表す。実装コメントとcode-reading guideも、TLS / fd / nghttp2 session解放前に発火すること、preface前のsetup failureでも発火すること、`wire.connection_preface`と1対1ではないことを明記しており、名前・観測phase・domain objectが一致した。
- `1c55f56` のC差分はtrace event文字列と説明コメントだけである。NULL guard、trace open / lock / close、`SSL_free()`、`SSL_CTX_free()`、`close(fd)`、buffer解放、`nghttp2_session_del()`、callbacks解放、connection解放の順序と分岐は変わっていない。したがってfd / SSL / session ownership、double-destroy、detached connectionの最終destroy条件へ新しい影響はない。
- PHPT 041はrename後もdestroy-only mutationを検出する。`transport.connection_destroy` は `destroy_h2_connection()` 内だけでemitされ、testは132回のfresh prefaceとは独立に132回のdestroy invocationをassertする。mem-recv fatal cleanupから `destroy_detached_connection_if_unowned()` を除去する既知mutationではpreface countは維持されてもdestroy countが0となり、assertがFAILする構造はrename前と同一である。local変数名 `$closeCount` は旧語彙を残すが、event filter・コメント・assert messageはdestroy semanticsを明示しており、oracleの意味や検出力を曖昧にする欠陥ではない。
- 元issueの旧streaming説明を「変更前」と過去形へ直し、unary / server streaming deadlineを共通のstream-scoped `RST_STREAM(CANCEL)` とした部分はadequateである。一方、current fallback clauseはgrace deadline超過だけを挙げ、即時RST submit / flush failureを含むSPECの「書き込み失敗時」より狭いため、REVIEW-20260714-001を残した。
- `connection_io_allowed()` のdead terminal gate、draining接続上のadmitted stream継続、callback内fatal RST submitのdead遷移と `nghttp2_session_mem_recv()` unwind、RST flush failure時のdead fallback、`locally_cancelled` によるtruncated-body誤分類除外は今回差分で変更されていない。関連call siteを再読し、既存invariantとの不整合は確認しなかった。
- active stream登録とconnection lifetime owner countは別管理のままである。`unregister_grpc_call_stream()` はcallback lookupだけを外し、`clear_connection_*_owner()` がowner countを減らし、cacheからdetach済みのconnectionは最後のowner clear後だけ `destroy_detached_connection_if_unowned()` が破棄する。今回のrename / docs対応はこの順序へ触れていない。
- production / diagnostic boundaryも不変である。eventは既存opt-in `GRPC_LITE_TRACE_FILE` 経路だけでemitされ、bench PHP surfaceやtest-fault seamをproductionへ追加していない。setup failureを含むlocal lifecycle eventであることが現在のguideに明記されたため、trace consumerがwire closeとして誤読する前回の問題は解消した。
- renameによってdestructorのordering / reentrancy / trace-file lifetimeは変わらない。変更されたのは同じ `fprintf()` callのJSON event文字列とコメントだけで、file lockはresource teardown前に従来どおり解除され、trace codeからtransport destructorへ再入するcallbackはない。trace pathはMINITでprocess-owned copyへ固定されMSHUTDOWNでNULL化されるため、後続GSHUTDOWNのpersistent connection teardownはeventをskipしてresource解放だけ行う既存挙動のままである。event名の数byte増加や`fprintf()`失敗はcleanup分岐に使われず、SSL / fd / session解放順へ影響しない。
- `git diff --check 1faf80a150dedbbf278a6cb4fbbf914ebb687b69..76d282766091de75bd670f00cd05df8ef264335f`: PASS。
- build / test: 依頼どおり未実行（static reviewのみ）。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 1 (Open)
- Design Decision: none
