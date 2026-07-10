# PR #28 status taxonomy compatibility/test adversarial review 2026-07-10

## Scope

- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/Integration/CompressionTest.php`
- `tests/unit/test_status_core.c`
- `src/status_core.c`
- `src/transport.c`
- `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md`
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- parallel review records `2026-07-10-pr28-status-taxonomy-protocol-adversary.md` / `2026-07-10-pr28-status-taxonomy-c-safety-adversary.md`

## Reviewer Role

- PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer

## Review Prompt Summary

- PR #28 の current HEAD `ce5872d` を `origin/main...HEAD` で確認し、official ext-grpc との PHP-visible behavior、unary / server streaming coverage、fixture fidelity、current verification docs、work issue / release handoffをレビューした。第二パスでは protocol-adversary / c-safety の指摘と照合し、同じruntime問題とcompatibility decisionは本fileの件数から除外した。既存PRコメントで求められた主要current docs更新とdata-bearing missing-trailersのpublic-surface PHPTは追加済みと確認し、それ自体は再掲しない。

## Issues

### REVIEW-20260710-001: 新しい `no-trailers` control が fixture inventory / verification matrix に登録されていない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `PHP-visible API compatibility / test fixture / release-documentation adversarial reviewer`
- Finding: PR は50054 fixtureへ `x-bench-grpc-response=no-trailers` を新設し、unary / server streamingのpublic-surface assertionsも追加したが、fixtureの一次案内とbehavior coverage matrixを更新していない。
- Evidence: PR added `poc/test-server/main.go:473-480` と `tests/phpt/022-error-and-http-validation.phpt:58-63,138-143` に対し、`docs/verification/test-fixtures.md:59-67` の50054 control表に `no-trailers` が無く、`docs/verification/verification-matrix.md:15-47` に missing trailers の行も無い。さらに `docs/verification/test-fixtures.md:69-74` はfixture behaviorを変える場合にverification matrixも更新するよう明記している。
- Expected model: fixture catalogは現在利用できる異常系controlを列挙し、verification matrixはそのsemanticsがunary / server streamingのどの層で守られるかを示す。
- Why it matters: 後続の実装者・reviewerがfixtureを発見できず、missing-trailersのPHP-visible coverageが存在するかをmatrixから判断できない。
- Recommended fix: `docs/verification/test-fixtures.md` の50054 control表へ `x-bench-grpc-response=no-trailers` とPHPT 022を追加し、`docs/verification/verification-matrix.md` に missing trailers（unary covered / server streaming covered / PHPT 022）を追加する。
- Inline comment anchor: `poc/test-server/main.go:473`（PR added branch）。より説明行へ付ける場合は同じadded hunkの `poc/test-server/main.go:475`。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `manual fixture inventory / verification matrix audit`
- Notes: `docs/SPEC.md`、`docs/design/protocol-classification-boundary.md`、`docs/verification/compatibility-control-checklist.md` のtaxonomy更新はce5872dで確認済みであり、修正済みの既存PRコメントは再掲していない。

## Consolidation Audit

- `grpc-encoding` headerだけでCompressed-Flag `0`のmessageを拒否する問題は独立に再現したが、runtime findingはprotocol-adversary `REVIEW-20260710-001` とc-safety `REVIEW-20260710-001`に集約した。本fileでは重複countしない。GitHubでtest fidelityへ直接コメントする場合のchanged-line anchorは `tests/phpt/022-error-and-http-validation.phpt:55`（unary）で、補助anchorは同file`:129`（server streaming）。production ownerへ置く場合はprotocol review記載どおり `src/status_core.c:35`。
- DATA END_STREAM missing-statusでgrpc-go strictnessとofficial ext-grpc drop-inが分かれるdecisionはprotocol-adversary `REVIEW-20260710-003`に集約した。本fileでは重複countしない。changed-line anchorは `poc/test-server/main.go:475`、code policy anchorは `src/status_core.c:67`。
- generic clean-close predicateがDATA / HEADERS END_STREAMを区別しないPR導入問題はprotocol-adversary `REVIEW-20260710-002`に集約した。本fileでは重複countしない。changed-line anchorは `src/status_core.c:67`。
- work issueの `Status: Open` と `docs/issues/open/` 配置は、PR未mergeかつ通常severityのreview findingが残る現在は正しい。merge前にClosedへ移す指摘はworkflow timingの誤りなので取り下げた。全finding修正・再review・merge後にfix commit / verificationを追記してclosedへ移すのがrepository運用に合う。
- release noteの実体はversion release時に作るrepository運用であり、このPRでGitHub Release本文を先行作成しないこと自体はfindingにしない。`docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md:69` はrelease-note必要性を記録済み。ただしrelease handoffではcompressionの `UNIMPLEMENTED -> INTERNAL` だけでなく、missing trailersの `UNKNOWN -> INTERNAL` + details変更、`HTTP_1_1_REQUIRED`の `UNKNOWN -> INTERNAL`、accepted ext-grpc divergenceを採る場合はそのcode/details/payload/retry差も列挙する必要がある。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1`
- Design Decision: `none`

## Verification

- `git diff --check origin/main...HEAD`: pass。
- official comparatorはcompose `dev-ext-grpc`（`docker/Dockerfile.ext-grpc` のunpinned `pecl install grpc`、GHCR artifactではない）を使用。runtimeはext-grpc `1.80.0` / PHP `8.4.20` CLI / Linux `aarch64`。
- current 50054 fixtureで `x-bench-grpc-encoding=gzip` はofficial ext-grpcが`BenchReply`をdeserialize後、fixtureに`grpc-status`が無いため `UNKNOWN / "Stream removed"`、ce5872dは `null / INTERNAL / "unsupported grpc-encoding: gzip"` になることを確認した。gzip + flag0 + `grpc-status:0`のexact comparatorはprotocol-adversary側の独立probeであり、本fileの実測としては主張しない。
- current `no-trailers` fixtureでofficial ext-grpcはunary `BenchReply / UNKNOWN / "Stream removed"`、server streaming `1 message / UNKNOWN / "Stream removed"`。このdecision findingはprotocol-adversaryへ集約済み。
