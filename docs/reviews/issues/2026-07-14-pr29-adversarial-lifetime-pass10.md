# PR #29 第十パス C lifetime / connection lifecycle review 2026-07-14

## Scope

- `76d282766091de75bd670f00cd05df8ef264335f..f94d3ba4dd95a8496b1d32efb372079b1ac89242`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md` のbest-effort reuse / fallback説明
- `src/transport.c` の `cancel_grpc_call_stream()`、send callback / coalesced write、`send_pending_h2_frames_with_deadline()`、dead / draining / preflight / cache lifecycle
- `src/unary_call.c`、`src/server_streaming_call.c` のdeadline後owner cleanup
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`、`035-preflight-drain-cap-fallback.phpt`、`038-fatal-rst-submit-marks-connection-dead.phpt`、`041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`

## Reviewer Role

- C lifetime / HTTP/2 connection cache・dead / draining lifecycle adversary

## Review Prompt Summary

- 第九パスLowへの文書対応が、RST submit / flushのfailure集合、dead terminality、cache detach、fresh connection adoption、preflight drain capを実装どおり表すか静的に再監査した。
- `任意の失敗`がretry可能なI/O状態まで過剰に含めていないか、`fresh connection`が即時destroyや接続成功保証を過剰に主張していないかを確認した。
- 元issueの範囲に限定し、build / testは実行せず、production codeは変更していない。

## Issues

### Blocker

- none

### High

- none

### Medium

- none

### Low

- none

### Design Decision

- none

## Verification Notes

- 対応commit `f94d3ba` は文書とレビュー記録だけを変更し、runtimeのconnection / stream state、owner count、cache entry、socket / TLS / nghttp2 session lifecycleには触れていない。
- 第九パスのfallback指摘はadequateに修正された。元issueのcurrent-state文、superseded Plan注記、Plan箇条書きはいずれも、grace deadline超過だけでなくRST submit / flush失敗全般をconnection破棄fallbackとして記述するようになった。`docs/SPEC.md` §4.2の「RST書き込み失敗時」と一致する。
- `任意の失敗`は実装より広くない。`cancel_grpc_call_stream()` は有効なsession / fdを持つnon-dead connectionとopen streamだけをRST経路へ通し、`nghttp2_submit_rst_stream()` がnonzeroを返した場合は例外なく `mark_connection_dead()` する。submit成功後は `send_pending_h2_frames_with_deadline()` が `nghttp2_session_send()`、send callback、socket / TLS write、coalesced buffer flushの最終failureをすべてnonzeroへ集約し、その全branchでdeadへ遷移する。
- EAGAIN / EWOULDBLOCK、TLS WANT_READ / WANT_WRITEはその時点ではfailureとして確定せず、grace deadlineまでpoll / retryされる。したがって文書の「任意の失敗」は一時的なretry signalを含む主張ではなく、submit / flush処理が最終的にfailureを返した場合を指し、過大ではない。coalesced buffer allocation / accumulation failureや別pending frameのcallback failureも最終的に `nghttp2_session_send()` failureとなるため、括弧内の集合に含まれる。
- RST submit / flush failure後、`mark_connection_dead()` 自体はactive ownerを即時freeしないが、`connection_io_allowed()` が以後の全socket / nghttp2 I/Oを禁止する。unary完了callerは `remove_unusable_persistent_connection()`、server streaming owner clear / resource destroyはcache detachを行い、ownerが残る場合だけ物理destroyを最後のowner clearまで遅延する。このため文書の「connectionを捨てる」はreuse禁止というlifecycleを正しく表し、即時freeを過剰に主張しない。
- `fresh connectionへフォールバック`も実装どおりである。次の `get_persistent_connection()` はdead / draining entryをremoveまたはdetachしてlocal candidateをNULLにし、新しい `h2_connection` を作成・cache登録する。replacement entryが既にある場合は健康なreplacementを維持するため、いずれの場合も壊れたconnectionは再adoptされない。新規接続のsetup自体が失敗し得る点は通常のfallback失敗であり、文書は接続成功を保証していない。
- preflight drain cap超過は `mark_connection_dead()` ではなく `mark_connection_draining()` で新規admissionを禁止し、preflightをfalseで返す。`get_persistent_connection()` は同entryをremove / detachしてfresh connection作成へ進む。preflightは `active_stream_count > 0` ならdrainを実行しないため、cap fallbackがadmitted active streamのI/Oを途中で切る説明にもなっていない。PHPT 035はcap到達、`persistent_reused=false`、preface 2件を独立に固定する。
- full relevant gateを再確認した。deadは全ownerにterminal、drainingはadmitted streamのI/O / RSTを許可、fatal callbackは最外 `nghttp2_session_mem_recv()` をunwind、`locally_cancelled` はtruncated-body誤分類を防ぎ、detached connectionは最後のowner clearまで破棄されない。今回の文書差分と矛盾する残件はない。
- test contractの静的照合: PHPT 033はRST成功後のsame-connection reuse、PHPT 035はdrain cap後のfresh adoption、PHPT 038はfatal RST submit後のdead / fresh adoption、PHPT 041はfatal callback後のdetach / destroyをそれぞれ固定する。
- `git diff --check 76d282766091de75bd670f00cd05df8ef264335f..f94d3ba4dd95a8496b1d32efb372079b1ac89242`: PASS。
- build / test: 依頼どおり未実行（static reviewのみ）。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
