# PR #28 status taxonomy doc review 2026-07-10

## Scope

- `docs/SPEC.md`
- `docs/design/protocol-classification-boundary.md`
- `docs/verification/compatibility-control-checklist.md`
- `src/status_core.c`

## Reviewer Role

- parent code reviewer

## Review Prompt Summary

- PR #28 の user-visible status taxonomy 変更が、現行仕様・設計・検証ドキュメントと整合しているかを確認した。

## Issues

### REVIEW-20260710-001: status taxonomy 変更後も主要ドキュメントが旧分類のまま残っている

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `parent code reviewer`
- Finding: PR は未対応の `grpc-encoding` / compressed flag を `GRPC_STATUS_INTERNAL` に変更し、clean END_STREAM で `grpc-status` trailers が無いケースも `GRPC_STATUS_INTERNAL` に変更している。一方で、仕様・分類・検証ドキュメントは旧分類を保持している。
- Evidence: `src/status_core.c:33` は compression 系を `INTERNAL` として返すが、`docs/SPEC.md:90` と `docs/SPEC.md:233` は `STATUS_UNIMPLEMENTED` のまま。`docs/design/protocol-classification-boundary.md:39` も unsupported compression の mapping を `UNIMPLEMENTED` としている。`docs/verification/compatibility-control-checklist.md:37` は missing trailers を `STATUS_UNKNOWN` としており、`docs/verification/compatibility-control-checklist.md:62` も compression を `STATUS_UNIMPLEMENTED` としている。
- Expected model: このリポジトリでは仕様と実装がずれる作業では `docs/SPEC.md` または関連 docs を更新してから実装する。status taxonomy は API-visible behavior であり、`status_core.c` の current model と設計ドキュメントの current model が一致している必要がある。
- Why it matters: 後続実装者やレビューエージェントが docs を一次ソースとして読むため、旧分類が残ると `INTERNAL` への変更を誤って回帰として戻す、またはテスト期待値を旧仕様へ戻す判断につながる。特に compression と missing trailers は PHPT / compatibility checklist のゲート項目であり、検証基準そのものが実装と食い違う。
- Recommended fix: `docs/SPEC.md` の重要前提と未決事項、`docs/design/protocol-classification-boundary.md` の classification table、`docs/verification/compatibility-control-checklist.md` の missing trailers / compression expectations を PR の最終挙動に合わせて更新する。必要なら変更履歴に 2026-07-10 の status taxonomy alignment を追記する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `review only`
- Notes: `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md` には新判断が記録されているが、これは作業issueであり current spec / design doc の代替にはならない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1`
- Low: `none`
- Design Decision: `none`
