# PR #29 第十パス敵対的レビュー統合 2026-07-14

## Scope

- PR #29 前回レビュー済みHEAD `76d282766091de75bd670f00cd05df8ef264335f`
- 第九パス対応commit / 現在HEAD `f94d3ba4dd95a8496b1d32efb372079b1ac89242`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`
- `docs/SPEC.md` §4.2
- `src/transport.c` のRST submit / flush failure、dead / draining、preflight drain lifecycle
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- deadline / RST_STREAM / persistent connection reuseに関係するC lifetime、test oracle、issue / review documentation

## Reviewer Role

- PR adversary / HTTP/2・gRPC domain model / C lifetime / test oracle・documentation統合 reviewer

## Review Prompt Summary

- 第九パスLowが完全に修正されたかを確認し、元issueのdeadline / RST_STREAM / connection lifecycle scope内でrequired gateを再実施した。
- domain model、C lifetime、test oracle・issue documentationを独立にレビューし、cross-triage後にスコープ内で再現可能な指摘だけを採用した。

## Issues

### REVIEW-20260714-002: PHPT 035が設定済み16KiB capを64KiB capと説明している

- Severity: `Low`
- Status: `Open`
- Reviewer role: `test oracle / issue documentation adversary`
- Finding: PHPT 035は`grpc_lite.preflight_drain_max_bytes=16384`を指定し、fixtureがclient kernelへ滞留させる48KiB backlogを、この設定済み16KiB capに当ててfresh-connection fallbackを固定している。しかしfollow-up直前のコメントは「more backlog than the 64KiB drain cap」と記載し、同じtestのINI、fixture説明、assertionおよびSPECのproduction default 64KiBと矛盾する。48KiB backlogはproduction default 64KiBを超えていない。
- Evidence: `tests/phpt/035-preflight-drain-cap-fallback.phpt:14`、同`:31-39`、同`:60-62`、同`:95-96`、`docs/SPEC.md:92`
- Expected model: test oracleの説明はproduction defaultとtestで縮小したeffective capを区別する。PHPT 035が証明するのは「48KiB backlog > configured 16KiB cap」であり、「backlog > production 64KiB cap」ではない。
- Why it matters: 現コメントはPHPT 035がproduction defaultの64KiB境界を直接通過させたと誤読させ、kernel receive-window制約を理由にcapを縮小したtest設計と反対の証拠範囲を主張する。runtime assertionは正しいためLowとする。
- Recommended fix: 60行目を`with more backlog than the configured 16KiB drain cap`等へ修正し、production default 64KiBとの区別を維持する。test behaviorや追加CIの変更は不要。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `--INI--`、48KiB fixture barrier、16384-byte cap assertion、SPEC §4.2を静的に相互照合した。PR baseにはファイルがなく、当該行はPR #29の新規追加diff内である。

## Prior Finding Recheck

- REVIEW-20260714-001はadequateに修正された。current-state文とPlanの2か所は、best-effort reuseのfallback集合をRST submit即時失敗、`nghttp2_session_send` / socket / TLS / coalesced-buffer flush失敗、grace deadline超過、preflight drain cap超過として実装・SPECどおりに説明する。
- RST submit / flush failureはdead、preflight drain cap超過はdrainingというstate差を維持しつつ、いずれも壊れたcandidateを後続callへ再adoptせずfresh connectionへfallbackする結果として正しくまとめられている。
- `Fix commit: 記録反映と同一コミット`はdocs修正とreview recordを同一commitで更新した自己参照不能な履歴境界に対する正当な記録である。

## Scope Triage

- REVIEW-20260714-002はdeadline後のbest-effort connection reuseとpreflight drain cap fallbackを固定するtest contractの事実誤認であり、元issueのscope内として採用した。
- PHPT 035のbehavioral assertion自体は有効であり、runtime変更、追加test、一般的CI拡張は要求しない。
- historical review record内の旧文言は過渡的な指摘・修正履歴なのでcurrent-state defectとして再指摘しない。

## Verification

- ASan / UBSan production lane: 22 PASS / 4 SKIP、reportなし
- ASan / UBSan bench+fault lane: 26/26 PASS、reportなし
- NTS PHPT: 26/26 PASS
- ZTS PHPT: 24 PASS / 2 SKIP、0 FAIL
- C unit: protocol_core / status_core / transport_core PASS
- C static analysis: production / benchともexit 0
- PHPUnit: 31 tests / 116 assertions PASS
- `bash -n`（PR変更shell script 4本）: PASS
- `git diff --check main...HEAD`、`git diff --check 76d282766091de75bd670f00cd05df8ef264335f..HEAD`: PASS
- GitHub review: `https://github.com/dkkoma/php-grpc-lite/pull/29#pullrequestreview-4689338007`
- Inline finding: `https://github.com/dkkoma/php-grpc-lite/pull/29#discussion_r3574665411`（再取得してpath / line / body一致を確認）

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 1 (Open)
- Design Decision: none
