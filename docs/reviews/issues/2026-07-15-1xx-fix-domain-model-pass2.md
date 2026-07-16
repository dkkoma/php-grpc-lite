# Informational 1xx adversarial findings fix domain model review pass 2 2026-07-15

## Scope

- `src/response_header_phase.h`
- `src/response_header_phase.c`
- `src/grpc_exchange_state.h`
- `src/transport.c`
- `src/transport.h`
- `src/status_core.c`
- `src/diagnostic/bench.c`
- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- `tests/unit/test_status_core.c`
- `docs/SPEC.md`
- `docs/design/grpc-call-exchange-state.md`
- `docs/design/http2-transport-design.md`
- `docs/design/protocol-classification-boundary.md`
- `docs/guides/code-reading-guide.md`
- `docs/verification/protocol-model-review-guide.md`
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- pass-1 adversarial review records 3 files / 8 findings

## Reviewer Role

- HTTP/2 / gRPC response-header lifecycle domain model gate

## Review Prompt Summary

- pass-1 adversarial review 8件の修正後working treeを対象に、response header-block phaseのcall-local ownership、END_STREAM付きvalid blockだけへのstatus commit、nghttp2 invalid-frame / DATA-after-1xx taxonomy、informational fieldを含むwire header resource accounting、Trailers-Only metadata ownership、production / diagnostic parity、unary / server streaming fixture coverageを横断確認した。
- HTTP/2 connection / stream / gRPC callのscope、stream-local failureとconnection failureの分離、status taxonomy priority、current design docsとverification資料の整合も確認した。

## Review Verification

- `response_header_phase.c` はnghttp2 / Zend非依存のpure ownerとしてbegin / `:status` classification / end / call resetを持つ。status field commit可否とparse成否に依存しないmetadata trailing roleも同helperが所有し、production / bench diagnosticの双方が同じpredicateを呼ぶ。direct final、single / repeated informational、final-to-trailing、call reuse reset、全phaseのEND_STREAM / metadata role truth tableは `tests/unit/test_response_header_phase.c` で固定されている。
- final response後のEND_STREAMなしHEADERSはblock開始時にquarantineされ、Trailers / Trailers-Onlyの `grpc-status` / `grpc-message` / `grpc-status-details-bin` はshared predicateが許可したEND_STREAM付きblockでのみsemantic stateへcommitされる。nghttp2がblock全体をrejectした場合は `response_header_protocol_error` が先行statusより優先する。
- 103 + END_STREAM、103後の`:status`欠落、103後のDATAはHTTP response header sequenceのcall-local protocol errorとして `INTERNAL` へ分類される。invalid-frame callbackとnghttp2生成outbound `RST_STREAM(PROTOCOL_ERROR)` observerはlibraryのtransport actionを重複submitせず、stream-local failureとしてconnection reuseを維持する。
- 全response HEADERS fieldはsemantic metadata反映前にoverflow-safeな `wire_response_header_entry_count` / `wire_response_header_bytes` へ累積される。pseudo-header、隔離されるinformational field、反復1xxも同じcall hard budgetに含まれ、超過は `RESOURCE_EXHAUSTED` + stream-local cancelとなる。PHP-visible metadata list/countとは責務が分離されている。
- Trailers-Only内のcustom metadata ownershipはshared block-role predicateと `grpc_status_seen` で決まり、status valueのparse成功には依存しない。invalid `grpc-status` 前のmetadataはblock内status観測時にtrailingへ移され、後続metadataもtrailingとなるためfield順序でinitial / trailingへ分裂しない。
- bench batchはphase、status validity、protocol error、terminal header stateをiterationごとにresetし、productionと同じshared phase / status / metadata role predicatesを使う。success gateはvalid terminal status、HTTP 200、protocol errorなしを要求するため、post-1xx nonterminal `grpc-status: 0` を成功sampleへ含めない。
- `tests/phpt/022-error-and-http-validation.phpt` はunary / server streaming双方でmultiple 103、post-1xx Trailers-Only、post-1xx invalid content-typeを固定し、server streaming request cardinalityをfixtureの1 messageへ合わせ、post-1xx two-message deliveryも検証する。raw fixture `:50071` とPHPT 042/043はpass-1 exact probes、resource overflow後のsame-connection follow-up、invalid status前後metadata ownership、bench false-success rejectionを固定する。
- verification evidenceを照合した: test-server rebuild / restart PASS、`./tools/test/check-phpt.sh` 28/28 PASS、focused PHPT 042/043 PASS、`./tools/test/check-c-static-analysis.sh` PASS。reviewerも `./tools/test/check-c-unit.sh` を再実行し、protocol_core / response_header_phase / status_core / transport_coreの4/4 suites PASSを確認した。`git diff --check` もPASS。

## Review Iteration Note

- 初回確認ではEND_STREAM status commitとmetadata roleのpredicateがproduction / diagnostic callbackへ重複していた。taskが要求する構造的parityを満たすためpure phase helperへpredicateとC unit truth tableを追加後、full scopeを再レビューした。最終stateに未解決findingはない。

## Issues

### Blocker

none

### High

none

### Medium

none

### Low

none

### Design Decision

none

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
