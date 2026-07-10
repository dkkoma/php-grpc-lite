# PR #28 status taxonomy compatibility/test adversarial review 2026-07-10

## Scope

- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/Integration/CompressionTest.php`
- `tests/unit/test_status_core.c`
- `src/status_core.c`
- `src/transport.c`
- `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md`
- `docs/issues/open/2026-07-10-grpc-encoding-flag0-no-reject.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- `docs/SPEC.md`
- `docs/design/grpc-call-exchange-state.md`
- `docs/verification/compatibility-control-checklist.md`
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- parallel review records `2026-07-10-pr28-status-taxonomy-protocol-adversary.md` / `2026-07-10-pr28-status-taxonomy-c-safety-adversary.md`

## Reviewer Role

- PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer

## Review Prompt Summary

- PR #28 の current HEAD `093b808809420616dfb990417607584ea4dd209a` を `0f1cc09..HEAD` で第四パス再確認した。第三パスのPR説明Low、不完全な1xx code / fixture / PHPT / active docsの撤去、分割issueの未着手future scope、元のcompression / missing-statusのunary / server streaming matrix、current docs / release handoffを確認した。一般的1xx対応は要求せず、work issueのRelated issues説明だけが古いLowはprotocol-adversary `REVIEW-20260710-005` に集約して重複countしない。

## Issues

### REVIEW-20260710-001: 新しい `no-trailers` control が fixture inventory / verification matrix に登録されていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer`
- Finding: PR は50054 fixtureへ `x-bench-grpc-response=no-trailers` を新設し、unary / server streamingのpublic-surface assertionsも追加したが、fixtureの一次案内とbehavior coverage matrixを更新していない。
- Evidence: PR added `poc/test-server/main.go:473-480` と `tests/phpt/022-error-and-http-validation.phpt:58-63,138-143` に対し、`docs/verification/test-fixtures.md:59-67` の50054 control表に `no-trailers` が無く、`docs/verification/verification-matrix.md:15-47` に missing trailers の行も無い。さらに `docs/verification/test-fixtures.md:69-74` はfixture behaviorを変える場合にverification matrixも更新するよう明記している。
- Expected model: fixture catalogは現在利用できる異常系controlを列挙し、verification matrixはそのsemanticsがunary / server streamingのどの層で守られるかを示す。
- Why it matters: 後続の実装者・reviewerがfixtureを発見できず、missing-trailersのPHP-visible coverageが存在するかをmatrixから判断できない。
- Recommended fix: `docs/verification/test-fixtures.md` の50054 control表へ `x-bench-grpc-response=no-trailers` とPHPT 022を追加し、`docs/verification/verification-matrix.md` に missing trailers（unary covered / server streaming covered / PHPT 022）を追加する。
- Inline comment anchor: `poc/test-server/main.go:473`（PR added branch）。より説明行へ付ける場合は同じadded hunkの `poc/test-server/main.go:475`。
- Fix summary: `docs/verification/test-fixtures.md` に `no-trailers` だけでなく `headers-only` / `custom-trailers-no-status` / `grpc-message-only-trailers` / encoding併用controlを登録し、`docs/verification/verification-matrix.md` にcompressionとterminal-frame別missing-trailersのunary / server streaming coverageを追加した。
- Fix commit: `f5a2f751621cecbb447db7d89222df435fcf7849`
- Verification: `docs/verification/test-fixtures.md:59-70` と `docs/verification/verification-matrix.md:29-30` を目視確認。`./tools/test/check-phpt.sh` も17/17 passし、PHPT 022のfixture実経路も確認。
- Notes: fixture inventory / matrixの当初の欠落は解消した。その後のpolicy修正を反映していないcompatibility checklistは別issue `REVIEW-20260710-002` として扱う。

### REVIEW-20260710-002: current compatibility checklistが最終的なcompression / missing-trailers policyと矛盾する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer`
- Finding: current verification gateの `docs/verification/compatibility-control-checklist.md` は、missing trailersをterminal frameに関係なく一律 `STATUS_INTERNAL` とし、未対応 `grpc-encoding` header宣言だけでもnon-OKにするよう求めている。しかしcurrent implementation / PHPT / verification matrixは、DATA END_STREAMだけをINTERNAL、HEADERS END_STREAMをUNKNOWNとし、header宣言 + Compressed-Flag=0は通常decodeしてwire statusに従う。`docs/SPEC.md` の解決済み項目も「未対応の `grpc-encoding` と compressed flag=1」を両方INTERNALにしたと読め、同fileのcurrent policyと一貫しない。
- Evidence: `docs/verification/compatibility-control-checklist.md:37` はmissing trailers全般に `STATUS_INTERNAL` を求め、`:62,66` は未対応 `grpc-encoding` 宣言とCompressed-Flag=1を同じ失敗条件としている。`docs/SPEC.md:233` も同様に読める。一方、`docs/SPEC.md:90`、`docs/verification/verification-matrix.md:29-30`、`tests/phpt/022-error-and-http-validation.phpt:53-93,158-201` はheader + flag=0の成功、HEADERS terminalのUNKNOWN、DATA terminalのINTERNAL、`grpc-message` only trailersのUNKNOWN + peer detailsを固定している。work issueも `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md:62-65,73-76,90-91` でその最終policyを明記する。
- Expected model: current compatibility checklist / SPEC / verification matrix / executable testsが、① `grpc-encoding` はheader宣言ではなくCompressed-Flag=1を処理する時にだけ未対応失敗になる、②missing `grpc-status` はterminal DATAならINTERNAL、terminal HEADERSならUNKNOWN、③`grpc-message` only trailersのdetailsはpeer messageを保持する、というaccepted grpc-go-exact policyを同じ言葉で表す。
- Why it matters: このchecklistは互換性変更のverification gateとして参照される。現状のままだと、後続変更で「encoding header観測時に即拒否」を再導入したり、HEADERS terminalをINTERNALへ潰すことをchecklist適合と誤判定できる。これはPHP-visible status / details / payload behaviorの回帰に直結する。
- Recommended fix: checklistのmissing-trailers行をDATA END_STREAM → INTERNAL / HEADERS END_STREAM → UNKNOWN / `grpc-message` only → UNKNOWN + peer detailsに分ける。compression本文と行は「Compressed-Flag=1のみ未対応失敗、header + flag=0はdecodeしwire statusに従う」と書き換える。`docs/SPEC.md:233` もhistorical transitionとcurrent triggerを区別する。
- Inline comment anchor: `docs/verification/compatibility-control-checklist.md:37`。compression側の直接anchorは同file`:62` または`:66`。
- Fix summary: `docs/verification/compatibility-control-checklist.md` のmissing-statusをDATA END_STREAM → INTERNALとHEADERS END_STREAM → UNKNOWNに分割し、compressionはper-message Compressed-Flag=1のみが失敗trigger、`grpc-encoding` header宣言 + flag=0は成功 / wire status維持と明記した。`docs/SPEC.md` の解決済み項目も同じtriggerに書き分けた。
- Fix commit: `375c3ddd73a040f99de5b6fb3f217b36020d3344`
- Verification: `docs/verification/compatibility-control-checklist.md:37-38,63-68`、`docs/SPEC.md:90,233`、`docs/verification/verification-matrix.md:29-30`、PHPT 022のunary / server streaming matrixをcurrent HEADで照合し、矛盾が解消したことを確認。

### REVIEW-20260710-003: PR説明が最終的なcompression / missing-status behaviorを反映していない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer`
- Finding: repository内のcurrent docs / testsは最終policyへ更新済みだが、PR #28の説明本文は、未対応 `grpc-encoding` 宣言を広くINTERNALにすると読める旧説明と、`:status 200` のclean close / trailers欠落をterminal frameに関係なくINTERNALにする旧説明を残している。
- Evidence: connected GitHub PR metadataの第三パスauditで、PR本文の項目1が「unsupported `grpc-encoding` → INTERNAL」をCompressed-Flag=1に限定せず、項目3と実装メモが「`:status 200` + clean close + trailers無し → INTERNAL」をDATA END_STREAMに限定していないことを確認した。current source of truthは `docs/SPEC.md:90,233`、`docs/verification/compatibility-control-checklist.md:37-38,63-68`、`docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md:62-65,76-81,103-104`。
- Expected model: PR説明は、①INTERNALに変わるのはCompressed-Flag=1 messageでありheader宣言 + flag=0はwire statusに従う、②missing `grpc-status` はDATA END_STREAMのみINTERNAL、headers-only / trailing HEADERS END_STREAMはUNKNOWNのまま、というPHP-visibleな最終behaviorをcurrent docs / testsと同じ粒度で要約する。
- Why it matters: PR本文はreviewerとrelease note作成者が最初に読む変更概要であり、現状のままだと変更対象のstatus / details / payload条件を過大に伝える。実装やrepository docsの追加変更は不要で、PR本文だけの引き継ぎ問題である。
- Recommended fix: PR本文の項目1をCompressed-Flag=1限定 + `grpc-encoding` header / flag=0例外へ書き換え、項目3と実装メモをDATA END_STREAM → INTERNAL / HEADERS END_STREAM → UNKNOWNに書き分ける。新しいcode / test / design workは追加しない。
- Inline comment anchor: PR description全体へのreview comment。code inline指摘にしない。
- Fix summary: PR説明のcompression項目をCompressed-Flag=1のみINTERNAL、`grpc-encoding` 宣言 + flag=0は成功 / wire statusに従うと書き分けた。missing-status項目はDATA END_STREAM → INTERNAL / HEADERS END_STREAM → UNKNOWNのgrpc-go-exact policyに更新し、実装メモも1xx対応を別issueへrevert / splitした現状に合わせた。
- Fix commit: `not applicable (PR description metadata edit; synchronized with 093b808 cleanup)`
- Verification: connected GitHub PR metadataの第四パスauditで、項目1のCompressed-Flag=1限定 + flag=0成功 / wire status、項目3のDATA END_STREAM限定 + HEADERS UNKNOWN、implementation notesのgrpc-go exact + 1xx split / revertを確認。current `docs/SPEC.md`、compatibility checklist、PHPT 022と一致。

## Consolidation Audit

- `grpc-encoding` headerだけでCompressed-Flag=0 messageを拒否するruntime Mediumは `f5a2f75` で修正済み。header callbackはencodingを観測するだけとなり、unary aggregate / server streaming incrementalの両DATA parserがflag=1を検出したときだけ `grpc_protocol_flag_compressed_message()` が失敗flagを立てる。PHPT 022は両call kindでheader + flag=0 + status0のOK、header + flag=0 + status5のwire NOT_FOUND、header + flag=1のINTERNALを固定する。runtime findingはprotocol-adversary / c-safety側の同じissueでfix statusを管理し、本fileでは重複countしない。
- generic clean-close predicateがDATA / HEADERS END_STREAMを区別しないMediumも `f5a2f75` で修正済み。`initial_headers_end_stream` と `trailing_headers_seen` でterminal frameを区別し、PHPT 022とC unitはDATAのINTERNAL / initialまたはtrailing HEADERSのUNKNOWNを固定する。runtime findingはprotocol-adversary側でfix statusを管理する。
- DATA END_STREAM missing-statusでgrpc-go strictnessとofficial ext-grpc drop-inが分かれるDesign Decisionは、work issue `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md:90` で **grpc-go exact** を明示的に採用済み。HEADERS terminalはUNKNOWN、DATA terminalはINTERNAL、`grpc-message` only trailersはUNKNOWN + peer detailsとする。accepted decisionとしてprotocol-adversary側で管理し、本fileではopen countしない。
- `grpc_call` responsibility mapに `trailing_headers_seen` が無いドキュメント欠落は `375c3dd` で修正済み。`docs/design/grpc-call-exchange-state.md:15` は `initial_headers_end_stream` / `trailing_headers_seen` のproducer / consumerを現行structに合わせて説明する。protocol-adversary `REVIEW-20260710-003` がfix statusを管理し、本fileでは重複countしない。
- `375c3dd` が追加した不完全な1xx成功経路は `093b808` で修正済み。`expect_final_response`、`x-bench-early-hints`、unary / server streamingの1xx PHPT、active design / verification docsの対応済み記述がPR #28から消え、`trailing_headers_seen` のEND_STREAM gateだけが残る。protocol-adversary `REVIEW-20260710-004` で `Status: Fixed` を管理する。
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md` は `Status: Open` / branch未着手 / 別PR scopeとし、却下されたframe-end実装、必要なsemantic phase、informational field隔離、initial / trailing metadata ownershipのunary / server streaming testをfuture workとして正確に記録する。本PRで追加の1xx実装を要求しない。
- work issueの `Status: Open` と `docs/issues/open/` 配置は、PR未mergeかつ通常severityのreview findingが残る現在は正しい。merge前にClosedへ移す指摘はworkflow timingの誤りなので取り下げた。全finding修正・再review・merge後にfix commit / verificationを追記してclosedへ移すのがrepository運用に合う。
- release noteの実体はversion release時に作るrepository運用であり、このPRでGitHub Release本文を先行作成しないこと自体はfindingにしない。work issue `:74-83` のobservable change表はCompressed-Flag=1の2種、header + flag=0、HTTP_1_1_REQUIRED、DATA missing trailers、HEADERS missing statusを列挙済みで、release handoffとして十分。
- parent work issueのRelated issues導入文は2件とも「codeは分離せず記録のみ分割」とするが、`093b808` 後はflag=0だけがPR同梱、1xxはcodeもfuture issueへ分離済みである。このcurrent summary Lowはprotocol-adversary `REVIEW-20260710-005` に集約し、本fileでは重複countしない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none` (過去の1件とprotocol-adversary集約の1xx Mediumは `Fixed`)
- Low: `none` (過去の2件は `Fixed`; work issue summaryのopen Lowはprotocol-adversary `REVIEW-20260710-005` に集約)
- Design Decision: `none` (grpc-go exactの1件は明示的にaccepted / documented)

## Verification

- parent reviewerがcurrent HEADで `./tools/test/check-phpt.sh` をDocker内で再実行し、17/17 passを確認した。あわせてPHPUnit 31 tests / 116 assertions、C unit 3 suites、C static analysisもpassした。
- `0f1cc09..093b808` を静的照合し、`src/` / `poc/test-server/main.go` / `tests/` / active design・verification docsから `expect_final_response` / `x-bench-early-hints` / 1xx対応済み表現が消えたことを確認。`rg` で同範囲の1xx関連残存はnone。
- PHPT 022はunary / server streamingの両方で、header + flag=0 + status0のOK、header + flag=0 + trailers-only status5のwire NOT_FOUND、header + flag=1のINTERNAL、HEADERS missing statusのUNKNOWN、DATA no-trailersのINTERNAL + detailsを維持。fixture inventory / verification matrixもこれらのactive controlだけを列挙する。
- `git diff --check origin/main...HEAD`: pass。
- official comparatorはcompose `dev-ext-grpc`（`docker/Dockerfile.ext-grpc` のunpinned `pecl install grpc`、GHCR artifactではない）を使用。runtimeはext-grpc `1.80.0` / PHP `8.4.20` CLI / Linux `aarch64`。
- updated 50054 fixtureへのofficial ext-grpc comparatorは、gzip header + flag=0 + status0が `BenchReply / OK`、gzip header + flag=0 + headers-only status5が `null / NOT_FOUND`で、f5a2f75の主要compatibility fixと一致。
- broken responseのofficial ext-grpc実測はunaryで、headers-only no statusが `null / UNKNOWN / "No status received"`、custom trailers no statusが `BenchReply / UNKNOWN / "No status received"`、`grpc-message` only trailersが `BenchReply / UNKNOWN / "No status received"`、DATA no trailersが `BenchReply / UNKNOWN / "Stream removed"`。php-grpc-liteのHEADERS UNKNOWN / DATA INTERNAL / peer `grpc-message` detailsは、work issueでaccepted済みのgrpc-go-exact policyによる意図的差分と確認。
