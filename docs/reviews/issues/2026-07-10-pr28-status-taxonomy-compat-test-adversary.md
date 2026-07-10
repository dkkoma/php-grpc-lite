# PR #28 status taxonomy compatibility/test adversarial review 2026-07-10

## Scope

- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/Integration/CompressionTest.php`
- `tests/unit/test_status_core.c`
- `src/status_core.c`
- `src/transport.c`
- `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md`
- `docs/SPEC.md`
- `docs/design/grpc-call-exchange-state.md`
- `docs/verification/compatibility-control-checklist.md`
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- parallel review records `2026-07-10-pr28-status-taxonomy-protocol-adversary.md` / `2026-07-10-pr28-status-taxonomy-c-safety-adversary.md`

## Reviewer Role

- PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer

## Review Prompt Summary

- PR #28 の current HEAD `f5a2f751621cecbb447db7d89222df435fcf7849` を `origin/main...HEAD` および `ce5872d..f5a2f75` で再確認し、official ext-grpc との PHP-visible behavior、unary / server streaming parity、fixture fidelity、`grpc-message` details、current verification docs、work issue / release handoffを敵対的にレビューした。protocol-adversary / c-safety の指摘と照合し、同じruntime問題とaccepted compatibility decisionは本fileで重複countしない。

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
- Status: `Open`
- Reviewer role: `PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer`
- Finding: current verification gateの `docs/verification/compatibility-control-checklist.md` は、missing trailersをterminal frameに関係なく一律 `STATUS_INTERNAL` とし、未対応 `grpc-encoding` header宣言だけでもnon-OKにするよう求めている。しかしcurrent implementation / PHPT / verification matrixは、DATA END_STREAMだけをINTERNAL、HEADERS END_STREAMをUNKNOWNとし、header宣言 + Compressed-Flag=0は通常decodeしてwire statusに従う。`docs/SPEC.md` の解決済み項目も「未対応の `grpc-encoding` と compressed flag=1」を両方INTERNALにしたと読め、同fileのcurrent policyと一貫しない。
- Evidence: `docs/verification/compatibility-control-checklist.md:37` はmissing trailers全般に `STATUS_INTERNAL` を求め、`:62,66` は未対応 `grpc-encoding` 宣言とCompressed-Flag=1を同じ失敗条件としている。`docs/SPEC.md:233` も同様に読める。一方、`docs/SPEC.md:90`、`docs/verification/verification-matrix.md:29-30`、`tests/phpt/022-error-and-http-validation.phpt:53-93,158-201` はheader + flag=0の成功、HEADERS terminalのUNKNOWN、DATA terminalのINTERNAL、`grpc-message` only trailersのUNKNOWN + peer detailsを固定している。work issueも `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md:62-65,73-76,90-91` でその最終policyを明記する。
- Expected model: current compatibility checklist / SPEC / verification matrix / executable testsが、① `grpc-encoding` はheader宣言ではなくCompressed-Flag=1を処理する時にだけ未対応失敗になる、②missing `grpc-status` はterminal DATAならINTERNAL、terminal HEADERSならUNKNOWN、③`grpc-message` only trailersのdetailsはpeer messageを保持する、というaccepted grpc-go-exact policyを同じ言葉で表す。
- Why it matters: このchecklistは互換性変更のverification gateとして参照される。現状のままだと、後続変更で「encoding header観測時に即拒否」を再導入したり、HEADERS terminalをINTERNALへ潰すことをchecklist適合と誤判定できる。これはPHP-visible status / details / payload behaviorの回帰に直結する。
- Recommended fix: checklistのmissing-trailers行をDATA END_STREAM → INTERNAL / HEADERS END_STREAM → UNKNOWN / `grpc-message` only → UNKNOWN + peer detailsに分ける。compression本文と行は「Compressed-Flag=1のみ未対応失敗、header + flag=0はdecodeしwire statusに従う」と書き換える。`docs/SPEC.md:233` もhistorical transitionとcurrent triggerを区別する。
- Inline comment anchor: `docs/verification/compatibility-control-checklist.md:37`。compression側の直接anchorは同file`:62` または`:66`。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: current docs間の目視照合 + `./tools/test/check-phpt.sh` 17/17 pass。

## Consolidation Audit

- `grpc-encoding` headerだけでCompressed-Flag=0 messageを拒否するruntime Mediumは `f5a2f75` で修正済み。header callbackはencodingを観測するだけとなり、unary aggregate / server streaming incrementalの両DATA parserがflag=1を検出したときだけ `grpc_protocol_flag_compressed_message()` が失敗flagを立てる。PHPT 022は両call kindでheader + flag=0 + status0のOK、header + flag=0 + status5のwire NOT_FOUND、header + flag=1のINTERNALを固定する。runtime findingはprotocol-adversary / c-safety側の同じissueでfix statusを管理し、本fileでは重複countしない。
- generic clean-close predicateがDATA / HEADERS END_STREAMを区別しないMediumも `f5a2f75` で修正済み。`initial_headers_end_stream` と `trailing_headers_seen` でterminal frameを区別し、PHPT 022とC unitはDATAのINTERNAL / initialまたはtrailing HEADERSのUNKNOWNを固定する。runtime findingはprotocol-adversary側でfix statusを管理する。
- DATA END_STREAM missing-statusでgrpc-go strictnessとofficial ext-grpc drop-inが分かれるDesign Decisionは、work issue `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md:90` で **grpc-go exact** を明示的に採用済み。HEADERS terminalはUNKNOWN、DATA terminalはINTERNAL、`grpc-message` only trailersはUNKNOWN + peer detailsとする。accepted decisionとしてprotocol-adversary側で管理し、本fileではopen countしない。
- `grpc_call` responsibility mapに `trailing_headers_seen` が無いドキュメント欠落はprotocol-adversary `REVIEW-20260710-003` に集約し、本fileでは重複countしない。
- work issueの `Status: Open` と `docs/issues/open/` 配置は、PR未mergeかつ通常severityのreview findingが残る現在は正しい。merge前にClosedへ移す指摘はworkflow timingの誤りなので取り下げた。全finding修正・再review・merge後にfix commit / verificationを追記してclosedへ移すのがrepository運用に合う。
- release noteの実体はversion release時に作るrepository運用であり、このPRでGitHub Release本文を先行作成しないこと自体はfindingにしない。work issue `:67-76` のobservable change表はCompressed-Flag=1の2種、header + flag=0、HTTP_1_1_REQUIRED、DATA missing trailers、HEADERS missing statusを列挙済みで、release handoffとして十分。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1` (`Open`)
- Low: `none` (過去の1件は `Fixed`)
- Design Decision: `none` (grpc-go exactの1件は明示的にaccepted / documented)

## Verification

- `./tools/test/check-phpt.sh`: PHP 8.4.20 / Linux aarch64で17/17 pass。更新済み50054 fixtureをrebuildし、PHPT 022のunary / server streaming compression + terminal-frame matrixを実経路で確認。
- `git diff --check origin/main...HEAD`: pass。
- official comparatorはcompose `dev-ext-grpc`（`docker/Dockerfile.ext-grpc` のunpinned `pecl install grpc`、GHCR artifactではない）を使用。runtimeはext-grpc `1.80.0` / PHP `8.4.20` CLI / Linux `aarch64`。
- updated 50054 fixtureへのofficial ext-grpc comparatorは、gzip header + flag=0 + status0が `BenchReply / OK`、gzip header + flag=0 + headers-only status5が `null / NOT_FOUND`で、f5a2f75の主要compatibility fixと一致。
- broken responseのofficial ext-grpc実測はunaryで、headers-only no statusが `null / UNKNOWN / "No status received"`、custom trailers no statusが `BenchReply / UNKNOWN / "No status received"`、`grpc-message` only trailersが `BenchReply / UNKNOWN / "No status received"`、DATA no trailersが `BenchReply / UNKNOWN / "Stream removed"`。php-grpc-liteのHEADERS UNKNOWN / DATA INTERNAL / peer `grpc-message` detailsは、work issueでaccepted済みのgrpc-go-exact policyによる意図的差分と確認。
