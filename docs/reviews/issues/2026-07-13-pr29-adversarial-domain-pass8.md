# PR #29 第八パス HTTP/2 / gRPC domain model review 2026-07-13

## Scope

- `199bf01f..1faf80a1`（対応commit `3ed170f`、レビュー記録commit `1faf80a`）
- `src/transport.c`（`destroy_h2_connection` / trace lifecycle）
- `tests/phpt/001-load.phpt`
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `tools/test/check-phpt.sh`
- `tools/test/check-zts-phpt.sh`
- `tools/test/check-c-coverage.sh`
- `tools/test/check-c-sanitizer.sh`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/SPEC.md` §4.2
- `docs/guides/code-reading-guide.md` trace説明
- `docs/reviews/issues/2026-05-19-wire-diagnostic-domain-self-review.md`

## Reviewer Role

- HTTP/2 / gRPC connection lifecycle・trace vocabulary・production/diagnostic boundary・current-state documentation adversary

## Review Prompt Summary

- 第七パスのLow 3件について、test-fault seamの外部期待、detached connectionのdestroy oracle、元issue文書のcurrent-state整合が十分に修正されたか再確認する。
- 新設`wire.connection_close`がHTTP/2 Connection / TCP socket / local C objectのどのlifecycleを表すか、名前・発火位置・既存trace namespace・production diagnostic境界を敵対的に監査する。
- 元issueのdeadline / RST_STREAM / persistent connection lifecycleと、直接導入されたremediation/test instrumentationの範囲に限定する。

## Issues

### REVIEW-20260713-007: `wire.connection_close`がlocal object destroy開始をwire上のconnection closeとして表現している

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model adversary`
- Finding: 新eventは`destroy_h2_connection()`の入口、すなわち`SSL_free` / `close(fd)` / `nghttp2_session_del`より前にemitされる。観測しているのはlocal `h2_connection` destructorの呼び出しであり、TCP FIN/RST、TLS `close_notify`、peer close、HTTP/2 GOAWAYのいずれも観測していない。また同destructorはTLS/configure/nghttp2 setup失敗でもclient preface送出前に呼ばれるため、コメントがいう`wire.connection_preface`のcounterpartは一般には成立しない。`wire.connection_close`という名前はdomain objectと観測phaseを混同する。
- Evidence: `src/transport.c:188-227`（eventをresource teardown前にemit）、`src/transport.c:1713-1772`（preface送出前を含むsetup failureの複数箇所からdestroy）、`src/transport.c:686-827,1478-1662`（既存`wire.*`はframe/chunk/socket I/O観測）、`docs/guides/code-reading-guide.md:202`（trace semantics）、`tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt:81-103`（実際のoracleはdestructor invocation count）。
- Expected model: trace eventの語彙は観測対象とphaseを正確に表す。local HTTP/2 connection objectのlifecycleを数えるなら`transport.connection_destroy` / `lifecycle.connection_destroy_begin`等とし、実socket closeを表すなら`wire.socket_close`として`close()`実行との関係を明示する。prefaceとの1対1対応を主張するなら、setup未完了を区別しstable connection idでpairできる必要がある。
- Why it matters: productionのwire diagnosticを読む利用者が、このeventを「HTTP/2/TCP closeがwireへ出た」「peer closeを観測した」「prefaceを送ったconnectionが閉じた」と誤解し得る。PHPT 041のdestroy-only mutation検出力は得られているためruntime severityはLowだが、connection lifecycleを調査する診断語彙としては修正が必要。
- Recommended fix: eventをlocal destructorの意味へrenameし（例: `transport.connection_destroy`）、PHPT 041も新名称を数える。eventの発火phase、setup未完了connectionにも出ること、`wire.connection_preface`とは必ずしもpairしないことをcode-reading guideへ記載する。pairingが必要なら両eventへstable `connection_id`を追加する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `destroy_h2_connection()`の全call siteとtrace emit位置をstatic reviewし、preface前のTLS/callback/session/settings/send failureでもeventが出ること、およびevent後にresource teardownが始まることを確認。
- Notes: opt-in trace自体をproductionへ置く判断は既存のAccepted Design Decisionに沿っており、本指摘はcompile boundaryではなくevent semanticsに限定する。

### REVIEW-20260713-008: issueの「現在」節を直した後もstreaming deadlineが接続破棄だという旧説明が残っている

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model adversary`
- Finding: Backgroundは変更前/現在へ書き分けられ、Planのdrain案とVerification順も修正されたが、「公式実装との差異」の末尾は依然として「streamingはuser cancelだけRST実装済みで、deadline経路だけが接続破棄」と現在形で記載する。同じ文書の直前の「現在の実装」、Progress、SPEC §4.2はunary / server streamingのdeadlineを共通のstream-scoped RSTとしており、中心domain modelが文書内で再び矛盾する。さらに現在要約の「接続を殺すのは接続レベル障害とwrite block timeoutのみ」は、RST flush failureとpreflight drain cap超過によるbest-effort reuse fallbackを省略している。
- Evidence: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md:19,21-28,42-47,51-55,77-85`、`docs/SPEC.md:92`。
- Expected model: open issueのcurrent-state説明は、deadlineをcall/stream scope、connection failure/fallbackをconnection scopeとして一意に表す。変更前の公式実装との差異はhistorical contextと明示し、現在のreuseはSPECどおりbest-effort（RST flush失敗、write block timeout、preflight cap超過ではfresh connectionへfallback）とする。
- Why it matters: unary / server streamingのdeadline taxonomyとconnection温存は元issueの主目的であり、入口文書に「streaming deadlineは接続破棄」と残すと後続実装・レビューが誤ったcall/connection scopeを採用する。runtime defectではないためLowとする。
- Recommended fix: 見出しを「変更前の公式実装との差異」等へ変更し、28行目を過去形（変更前はuser cancelにだけRST helperがありdeadline経路がconnection破棄だった）へ直す。現在要約は「通常は温存するが、RST flush失敗 / write block timeout / preflight drain cap超過では安全側にfresh connectionへfallbackする」とSPEC §4.2へ揃える。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: issue Background / Plan / Progress / Decision Logと`docs/SPEC.md` §4.2、およびunary/server streamingが共有する`cancel_grpc_call_stream()` lifecycleを相互確認。Verificationの第五→第六→第七順は修正済み。
- Notes: 第七パスREVIEW-20260713-006の修正は部分的にはadequate（変更前ラベル、superseded Plan、時系列修正）。残件は見落とされた旧paragraphとbest-effort fallbackの現在要約である。

## Triage

- REVIEW-20260713-007は**投稿推奨（Low維持）**。単なるevent名の好みではない。既存traceは`wire.frame_*`、`wire.socket_*`、`wire.connection_preface`を異なる観測点として利用者へ公開しており、新eventはlocal destructorの入口で発火する一方、名前はwire/TCP/HTTP/2上のcloseを区別しない。さらにpreface前のsetup failureでも発火するため、実装コメントの「`wire.connection_preface`のcounterpart」は事実として成立しない。PR対応commit `3ed170f`が直接導入し、PHPT 041がdestroy証拠として消費するsemanticsなので元issueから派生したremediation scope内である。renameまたは明示的なlifecycle semanticsの文書化まではrequired all-Low-none gateを止めるのが妥当。
- REVIEW-20260713-008は**投稿推奨（Low維持）**。修正後のissueに残る正確な旧文は次のとおり: 「**なお本実装の streaming 側にはユーザー起点キャンセル用の `cancel_active_server_streaming_call_state`（`src/transport.c`）が既に RST_STREAM(CANCEL) を実装済みで、deadline 経路だけが接続破棄になっている。**」直前の「現在の実装」、Progress、SPEC §4.2と正面から矛盾し、元issueのunary/server streaming deadline lifecycleそのものを誤記するためscope内かつ非任意の指摘である。
- 結論: false positiveとして撤回する指摘はない。投稿対象はLow 2件のまま。

## Adequate Fixes Confirmed

- PHPT 001は`GRPC_LITE_EXPECT_TEST_FAULT`をmodule外部の期待値として受け、sanitizer production laneは0、bench+fault / NTS / ZTS / coverageのtest buildは1を宣言する。MINFO rowを同じ外部期待へ照合するため、production laneへfault seamが混入してもSKIP解除だけでPASSしない。前回のproduction/test boundary指摘はadequate。
- PHPT 041はpreface 132件に加えてdestructor event 132件をassertし、`destroy_detached_connection_if_unowned()`削除mutationを検出できる。event名称の問題を除けば、detachだけでなくdestroy invocationまで固定する前回test-oracle指摘への対応はadequate。
- `3ed170f`はconnection ownership、dead/draining遷移、socket/TLS/nghttp2 teardown順を変更せず、opt-in trace emitだけを追加した。trace無効時はcached pathのnull checkでfile I/Oを行わず、bench PHP surfaceやtest-fault branchをproductionへ露出する変更もない。
- issueの変更前Background、superseded drain Plan、第五→第六→第七のVerification順は現在の実装・判断履歴に揃った。

## Verification

- `git diff --check 199bf01f..1faf80a1`: PASS
- source review: `destroy_h2_connection()`全call site、trace open/lock、resource teardown順、PHPT 041 counter、test-fault外部期待と全runnerを相互確認
- docs review: original issue、`docs/SPEC.md` §4.2、code-reading guide trace説明、既存wire diagnostic Design Decisionを相互確認
- Docker / build / testは依頼どおり未実行

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 2 (Open)
- Design Decision: none
