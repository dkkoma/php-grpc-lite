# 1xx informational response adversarial C safety pass 1 2026-07-15

## Scope

- Commit `20c2dc0`
- `src/grpc_exchange_state.h`
- `src/transport.c`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `src/wrapper_adapter.c`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/phpt/025-resource-limits.phpt`
- `docs/SPEC.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- `docs/reviews/issues/2026-07-10-pr28-status-taxonomy-protocol-adversary.md`

## Reviewer Role

- C safety / lifetime adversary

## Review Prompt Summary

- 新しい response header-block phase state の全生成・retry・persistent reuse・cancel・teardown lifecycle、nghttp2 callback ordering と stream user-data lifetime、`:status` parse、informational field の resource accounting、frame-end phase reset、production / bench parity を静的に敵対的レビューした。テストおよび Docker は実行していない。

## Issues

### REVIEW-20260715-001: informational field の semantic isolation が per-call header resource budget まで無効化する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C safety / lifetime adversary`
- Finding: `INFORMATIONAL` phase の field は PHP-visible metadata や validation/status state から隔離すべきだが、実装は同じ早期 return で decoded header の count/byte resource accounting からも隔離している。そのため peer は任意数の valid 1xx HEADERS に custom field を載せ、application側のhard stopに一度も到達せず header decode/callback work を継続させられる。semantic ownership と受信work budgetという別責務が一つのphase gateに畳まれている。
- Evidence: `src/transport.c:2227-2230` は `GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL` の全fieldを `grpc_protocol_add_response_metadata_entry()` より前で返す。byte/count上限、`metadata_too_large`、`RST_STREAM(CANCEL)` は同file `:3127-3141` にしかなく、この経路では一度も更新・評価されない。commit前は少なくとも1xx内の通常custom fieldがこのhelperへ到達し、128 entriesまたはbyte上限で停止していた。`src/transport.c:1747-1754` の `SETTINGS_MAX_HEADER_LIST_SIZE` はconnection-levelの通知に留まり、[RFC 9113 §6.5.2 / §10.5.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-10.5.1) が明記する advisory setting なのでhard enforcementの代替にならない。またnghttp2自身も [`nghttp2_on_header_callback`](https://nghttp2.org/documentation/types.html#c.nghttp2_on_header_callback) の契約でapplicationがincoming header fieldsの総量を制限するよう警告している。nghttp2のCONTINUATION上限が1 blockを有限にしても、final response前に繰り返せる1xx block列の累積量は制限しない。[RFC 9110 §15.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-15.2) も1 requestに対するzero or more interim responsesと、clientによるone or more 1xx responseのparseを規定しているため、このblock列自体はvalidである。`docs/SPEC.md:90` とwork issue `:45` はこの実装を「metadata limitの観測から隔離」と明示しており、単なるdocs漏れではない。確認probeは raw fixture に `:status: 103` + 小さい `x-info: a` を持つEND_HEADERS blockを129回送らせ、その後にvalid final `:status: 200` / `content-type: application/grpc`、DATA、`grpc-status: 0` END_STREAMを送る。現実装は1xx分をcountせずfinal OKまで進むが、commit前の通常field accountingなら128-entryでstreamをcancelするwire sequenceである。byte側は `grpc.absolute_max_metadata_size=1024` で合計1024 bytes超のinformational custom fieldsを送る同型probeにする。
- Expected model: informational field は metadata map、content-type、gRPC status/message/encodingへ反映しない一方、wireからdecodeした全response field/blockはphaseに関係なくcall-localなhard resource budgetへ計上する。semantic metadata counterをそのまま使えないなら、pseudo-headerとignored fieldも含める独立したwire header byte/entry/block counterを持ち、上限超過時は既存どおりstream-localにcancelして `RESOURCE_EXHAUSTED` へ分類する。
- Why it matters: HEADERSはDATA flow-controlによるcall-level backpressureを受けず、`src/wrapper_adapter.c:195-200` はinfinite/no deadlineを `deadline_abs_us = 0` にするため、deadlineも独立した上限にならない。悪意ある、または壊れたpeerがvalid 1xx列を送り続けると、application側のcount/byte hard limitまたは代替work limitで停止できず、PHP workerに無制限のHPACK decode、callback CPU、transient allocation、socket readを負わせられる。finiteな129-block probeでも、20c2dc0がそれまで存在したapplication側の停止点を除去したことを再現できる。
- Recommended fix: semantic field反映より前に、全response HEADERS fieldをoverflow-safeに計上するlimit-only helperを呼ぶ。既存metadata map用counterとは分離してよいが、`:status` を含むpseudo-header、ignored informational field、複数1xx blockの累積を必ず覆い、超過時は `metadata_too_large` 相当のstatusと `RST_STREAM(CANCEL)` を発生させる。上記raw fixture probeをunary / server streaming双方へ追加し、overflow後のfollow-up RPC成功でconnection reuseも確認する。`docs/SPEC.md` は「semantic stateから隔離するがresource accountingには含める」と責務を分けて更新する。
- Fix summary: semantic metadata map用counterとは別に `wire_response_header_entry_count` / `wire_response_header_bytes` を `grpc_call` へ追加し、semantic phaseの早期returnより前にoverflow-safeなaccounting helperを呼ぶ。`:status` などのpseudo-header、ignored informational field、複数1xx blockを同じcall-local hard budgetに累積し、超過時は `metadata_too_large` + `RST_STREAM(CANCEL)` + `RESOURCE_EXHAUSTED` とする。
- Fix commit: `pending`
- Verification: raw fixture `:50071` の129 informational block entry probeと1024-byte optionを超えるbyte probeをunary / server streamingで実行し、全case `RESOURCE_EXHAUSTED`。各overflow後の `require-prior-resource-probe` で同一connectionの後続RPCがOKとなることも確認。`./tools/test/check-phpt.sh` 28/28 PASS、C unit 4/4 PASS、static analysis PASS。
- Notes: semantic isolationとwire work budgetを別責務にするため、既存metadata map counterの意味は変えず独立counterを選択した。`docs/SPEC.md`、fixture catalog、verification matrixもこの区別へ更新した。既存lifetime / retry safetyに関するreviewerの非指摘結果は維持される。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none` (`1 fixed`)
- Low: `none`
- Design Decision: `none`
