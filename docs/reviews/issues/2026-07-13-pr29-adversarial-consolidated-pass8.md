# PR #29 第八パス敵対的レビュー（統合） 2026-07-13

## Scope

- PR #29 HEAD `1faf80a150dedbbf278a6cb4fbbf914ebb687b69`
- 第七パス対応commit `3ed170f2f0eb8a182c53ade0c96cc69d919ff1e5`
- `199bf01f584fab4d35f42143fe57288e38a97fdd..1faf80a150dedbbf278a6cb4fbbf914ebb687b69`
- 元issue `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md` のdeadline / RST_STREAM / persistent connection lifecycle
- 第七パスLow 3件の対応（production/test-fault boundary、detached connection destroy oracle、issue文書整合）

## Reviewer Role

- PR adversary / HTTP/2・gRPC connection lifecycle / C lifetime / production-test boundary

## Review Prompt Summary

- 前回指摘が実装・test oracle・文書の各contractを実際に固定したかを再確認する。
- 新規trace eventのdomain vocabularyと発火phase、PHPT 041のdestroy検出力、全build runnerの外部期待値を敵対的に確認する。
- 一般的なCI/build matrix hardeningは投稿対象から除外し、元issueと今回の対応commitから直接生じる指摘だけを残す。

## Issues

### REVIEW-20260713-007: `wire.connection_close`がlocal connection objectのdestroy開始をwire closeとして表現している

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model adversary`
- Finding: `wire.connection_close`は`destroy_h2_connection()`の入口で、`SSL_free()`、`close(fd)`、`nghttp2_session_del()`より前に記録される。観測対象はlocal `h2_connection` destructorの呼び出しであり、TCP FIN/RST、TLS close、peer close、HTTP/2 GOAWAYではない。また同destructorはclient preface送出前のsetup failureでも呼ばれるため、実装コメントの「`wire.connection_preface`のcounterpart」は一般には成立しない。
- Evidence: `src/transport.c:188-227`、`src/transport.c:1713-1772`、`tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt:81-103`
- Expected model: trace event名は観測するdomain objectとphaseを正確に表す。local objectのdestroy invocationなら`transport.connection_destroy`等とし、wire/socket closeを表すなら実際のcloseとの関係を明記する。
- Why it matters: opt-in production traceをconnection lifecycleの診断に使う利用者が、local teardown開始をwire/TCP/HTTP/2上のcloseやprefaceとの1対1対応として誤読する。PHPT 041のmutation検出力自体は得られているためLow。
- Recommended fix: eventをlocal destructor semanticsへrenameし、PHPT 041とcode-reading guideを更新する。`wire.connection_preface`とのpairをcontractにする場合はsetup未完了を区別し、stable connection idで対応付ける。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 全`destroy_h2_connection()` call siteとevent/resource teardown順をstatic review。preface前setup failureでもeventが発生し、event後に実resource teardownが始まることを確認。
- Notes: opt-in traceをproductionへ置く既存Design Decision自体はAcceptedであり、本指摘はevent semanticsに限定する。

### REVIEW-20260713-008: 元issueにstreaming deadlineが接続破棄だという変更前の説明が現在形で残っている

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model adversary`
- Finding: Backgroundの変更前/現在、superseded Plan、Verification順は修正されたが、「公式実装との差異」の末尾には「本実装のstreaming側はuser cancelだけRST実装済みで、deadline経路だけが接続破棄」と現在形で残る。同じ文書の直前の「現在の実装」、Progress、`docs/SPEC.md` §4.2はunary / server streamingのdeadlineを共通のstream-scoped RSTとしており、元issueの中心domain modelが文書内で矛盾する。
- Evidence: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md:19-28`、同`:51-55`、`docs/SPEC.md:92`
- Expected model: 変更前の差異はhistorical contextと明示し、現在はunary / server streamingともdeadlineでstream-local RSTを送り、reuseはSPECどおりbest-effortであることを一意に説明する。
- Why it matters: deadline failureのstream scope / connection scopeは元issueの主目的であり、入口文書の現在形の旧説明は後続実装・レビューを誤ったlifecycleへ導く。
- Recommended fix: 見出しまたは当該段落を「変更前」と明示して過去形へ直し、現在のbest-effort reuse説明をSPEC §4.2へ揃える。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: original issueのBackground / Plan / Progress / Decision Logと`docs/SPEC.md` §4.2を相互確認。第五→第六→第七のVerification順は修正済み。
- Notes: 第七パスの文書修正は部分的にadequateで、残件は見落とされた旧paragraph。

## Adequate Fixes Confirmed

- `GRPC_LITE_EXPECT_TEST_FAULT`はsanitizer production=`0`、bench+fault / NTS / ZTS / coverage test build=`1`として全runnerのbuild flagと一致し、PHPT 001がMINFOを外部期待へ照合する。前回のproduction/test boundary指摘はadequate。
- `wire.connection_close`の名称問題を除き、PHPT 041はpreface 132件とdestroy invocation 132件を独立にassertし、mem-recv fatal branchからdestroy helperを除去するmutationを検出できる。前回のdestroy oracle指摘はadequate。
- `destroy_h2_connection()`のresource teardown順、mem-recv fatal ownership、diagnostic caller lifetimeに新たなC safety defectは確認していない。
- 現行pure-production buildではtest-fault parser/state/predicateがcompile outされ、fault tokenを与えたfocused unaryも正常成功する。

## Scope Triage

- Native QAへのpure-production lane常設とbench-only build variant追加は一般的CI/build matrix hardeningとして今回の投稿対象から除外した。
- productionでfault tokenがbehaviorally inertであることを恒久的に固定する追加negative PHPTは有用だが、現HEADのruntime boundaryは正しく、前回の具体的な外部期待指摘も修正済みのため、元issueのrequired gateには含めなかった。

## Verification

- ASan/UBSan sanitizer: production 22 PASS / 4 SKIP、bench+fault 26/26 PASS、sanitizer報告なし
- NTS PHPT: 26/26 PASS（2回）
- ZTS PHPT: 24 PASS / 2 SKIP、0 FAIL
- C unit: protocol/status/transport 3/3 PASS
- PHPUnit: 31 tests / 116 assertions PASS
- C static analysis（production / bench）: PASS
- pure-production focused probe: `GRPC_LITE_TEST_FAULT=submit-request-fatal,rst-submit-fatal` 下でPHPT 001 / 010が2/2 PASS
- `bash -n`、`git diff --check`: PASS

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 2 (Open)
- Design Decision: none
