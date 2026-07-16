# Informational 1xx response adversarial test / fixture review pass 3 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` (current HEAD)
- `poc/test-server/main.go`
- `src/response_header_phase.h`
- `src/response_header_phase.c`
- `src/transport.c`
- `src/diagnostic/bench.c`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- `tools/test/check-c-unit.sh`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- pass 1 adversarial review records 3 files / pass 2 domain-model review record
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- `docs/verification/compatibility-control-checklist.md`

## Reviewer Role

- test / fixture adversary (pass 3)

## Review Prompt Summary

- pass 1の8件に対する `6a4902f` の修正を、fixture fidelity、PHPT oracleの識別力、production / bench diagnostic parity、wire header budget、C unit gate、verification docsとの整合からread-onlyで再監査した。test suite / Docker / runtime probeは実行していない。
- Docker buildのGo 1.26 + `golang.org/x/net/http2` 実装では、1xxの `WriteHeader()` はlive header mapを渡す場合にframe write完了まで待つ。したがって `poc/test-server/main.go` のpollution fieldは103 blockへ送出済みになってから削除され、後続final responseには残らない。pollution fixtureはvacuousでもfinalへのleakでもない。
- pass 1で指摘されたserver-streamingのdeadな `messageCount(10)` は1へ修正され、別のpost-1xx two-message caseが2件yieldを固定している。Close Criteriaのno-trailers、status 0、metadata ownership、field isolationはunary / server streaming双方に存在する。

## Issues

### REVIEW-20260715-001: bench diagnosticがinformational wire-header budgetを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary (pass 3)`
- Finding: pass 1のC safety `REVIEW-20260715-001` はproduction callbackへ独立wire header budgetを追加したが、同じresponse phase modelを使う `grpc_lite_bench_unary_batch()` のdiagnostic callbackにはaccountingが入っていない。`INFORMATIONAL` fieldはbudgetへ到達せず、有限のbudget probeも正常sampleとして数えられるため、旧findingの停止点欠落がbench transportには残っている。
- Evidence: productionの `src/transport.c:2213-2238,2253-2260` は全HEADERS fieldをsemantic phaseの早期returnより前に `wire_response_header_entry_count` / `wire_response_header_bytes` へ計上する。一方、`src/diagnostic/bench.c:319-353` は同等のaccountingを持たず、`INFORMATIONAL` phaseからそのままreturnする。同file `:1259-1299` はbatch用 `grpc_call` の `max_response_metadata_bytes` を初期化せず、`:1447-1462` はiterationごとのwire counter / `metadata_too_large` resetも持たない。最後のsuccess gate `:1700-1707` も `metadata_too_large` を拒否しない。既存raw control `poc/test-server/main.go:704-710` は129個の103 blockの後にvalid gRPC OK responseを送るため、production PHPT 042は `RESOURCE_EXHAUSTED` になるが、batch callbackでは全informational fieldが隔離returnされる。zero初期化されたbyte limitによりfinal `content-type` がsemantic `metadata_too_large` を立てても、batch `call.connection == NULL` なのでmetadata helperはRSTをsubmitせず、success gateもそのflagを無視する。後続のfinal status 0 / HTTP 200 / terminal status blockが揃い、既存controlをbatchへ送る有限probeは `ok=1`, `failed=0` となる。PHPT 042のresource caseはproduction unary / server streamingだけで、PHPT 043はnonterminal statusの1 caseだけである。`docs/SPEC.md:90,247` と `docs/verification/verification-matrix.md:29` は全decoded response fieldのbudgetとproduction-bench parityを記すが、このdiagnostic divergenceを載せていない。
- Expected model: semantic isolationとwire work budgetは別責務であり、response phase modelを使う全callbackはfieldのroleに関係なくcall-local hard budgetへ計上する。bench batchも明示したhard limitをiterationごとに初期化し、超過sampleを成功件数へ入れずstreamを停止する。
- Why it matters: bench-enabled buildは診断用でproduction semanticsのownerではないが、PHPからpeerへ接続して同じnghttp2 decode/callback loopを駆動する。既定 `timeout_us = 0` のbatchにpeerがfinal responseを送らずvalid 1xx列を継続するとapplication側の停止点がなく、CPU / socket read / HPACK workを継続させられる。有限の既存fixtureでも、productionが拒否するresponseをbenchmarkの正常sampleへ混ぜるparity defectを再現できる。
- Recommended fix: wire field accountingをproduction / batch callbackから共有できるhelperへ寄せ、batch `grpc_call` にdefault hard limit、wire entry/byte counter、`metadata_too_large` のiteration resetを設定する。budget超過時は `RST_STREAM(CANCEL)` をsubmitし、batch success gateでも拒否する。PHPT 043に `grpc_lite_bench_unary_batch(..., 1, ['x-bench-raw-response' => 'informational-entry-budget'])` を追加し、`:50071` に対して `ok=0`, `failed=1` を固定する。byte側をbatch optionで制御しないなら、少なくともentry hard limitを必須gateにする。
- Fix summary: bench normal / invalid header callbackをproductionと同じ共有wire-budget ownerへ接続し、default 64KiB limit、iteration reset、live-session CANCEL、success exclusionを追加した。entry controlに加えてdefault 64KiBを越えるbyte controlを追加した。
- Fix commit: `pending`
- Verification: PHPT 043でentry / default-byte controlsが各`ok=0 / failed=1 / stream_error_code=8`、valid near-cap informational responseの2 iterationsが`ok=2 / failed=0`となることを確認。C unitのpure helper boundaryと合わせ、PHPT 28/28、C unit 4/4、static analysis PASS。
- Notes: 旧C safety findingをそのまま再掲するものではなく、`6a4902f` のproduction-only accountingが同commitでphase parityを拡張したbench callbackまで届いていない不足を新規findingとした。protocol `REVIEW-20260715-002` / C safety `REVIEW-20260715-003`と同じdiagnostic resource-boundary defectとして一度だけ修正した。positive controlによりraw batchが全callを送信前に失敗させていたbench connection ownership不整合も検出し、bench-local fd sendへ修正した。

### REVIEW-20260715-002: wire-budget fixture oracleがfield classとcancel actionを識別しない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary (pass 3)`
- Finding: productionの現行accounting自体はpseudo-headerとinformational custom fieldの両方を数え、超過時に `RST_STREAM(CANCEL)` をsubmitしているが、PHPT 042のfixture oracleはこの2つの必須性を区別できない。entry probeはどちらか一方のfield classだけを数えても超過し、byte probeはpseudo-headerを全て落としてもcustom fieldだけで超過する。さらにfollow-up oracleはclientがcancelを送ったかを一切観測しない。
- Evidence: `poc/test-server/main.go:704-710` のentry probeは、各々 `:status: 103` と `x-info: a` を持つblockを129個送る。上限は128 entries (`src/transport_core.h:17`) なので、129個の`:status`だけを数える実装も、129個の`x-info`だけを数える実装もPHPT 042 `:66-110` の `RESOURCE_EXHAUSTED` assertionを通す。byte probe `poc/test-server/main.go:712-719` も300-byte valueを4個送り、custom informational fieldだけで1,224 bytesとなるため、1024-byte option下でinformational pseudo-headerを全て無視しても超過する。またfixtureはoversized sequenceとfinal OKを書いた時点で `resourceProbeSent=true` を無条件に返し (`:704-724`)、`RSTStreamFrame` branchはrequest mapを削除するだけでerror codeもresource stream identityも記録しない (`:677-679`)。従って `src/transport.c:2237` のreset codeを `NGHTTP2_CANCEL` から `NGHTTP2_NO_ERROR` へ変えても、`metadata_too_large` のstatus priorityとsame-connection follow-upは維持され、fixtureは差を検出しない。`docs/design/protocol-classification-boundary.md:69,114` とcompatibility checklist `:36,57` が列挙する全field accounting / `RST_STREAM(CANCEL)` を、このoracleは個別に固定していない。
- Expected model: regression probeは (a) pseudo-headerとinformational custom fieldの両方が同じentry/byte budgetへ寄与すること、(b) hard limitがstatus flagだけでなくpeerの送信継続を止める `RST_STREAM(CANCEL)` transport actionを伴うこと、(c) その後も同一connectionを再利用できることを別々に識別する。
- Why it matters: 現行実装は正しくても、pseudo-header workを再び無制限にする変更や、resource overflowのreset codeをCANCELからNO_ERRORへ変える変更が全gateを通る。peer送信を止めるtransport actionはfixtureで直接観測されず、たまたま後続DATAが別classificationを起こす現行制御フローに依存している。これはpass 1のresource-hardening findingが要求した停止点と、docsがverification済みとしているtransport actionの回帰検出を弱める。
- Recommended fix: entry probeは65個の `:status + x-info` block（combined 130 entries、どちらか一方だけならfinal fieldを加えても128未満）へ変更するか、pseudo-only / custom-only probeを分ける。byte probeは `x-info` valueを237 bytes × 4へし、informational custom 972 bytes + final non-informational fields / pseudo-fieldsの境界でinformational `:status` 40 bytesの有無を識別する。さらにresource probe stream idをconnection-localに保持し、そのstreamについてclientから `RST_STREAM(CANCEL)` を受信した場合だけfollow-up OK markerを立てる。PHPTはentry / byteそれぞれで `RESOURCE_EXHAUSTED`、CANCEL観測、same-connection follow-up成功を固定する。
- Fix summary: entry probeを65個の`:status + x-info`（130 entries）、byte probeを237-byte value x 4へ変更し、pseudo / regularの片側accountingだけでは上限未満になる境界へした。fixtureはresource stream IDを保持し、そのstreamの`RST_STREAM(CANCEL)`受信後だけfollow-upをOKにする。
- Fix commit: `pending`
- Verification: PHPT 042でentry / 1024-byte controlsのunary / server streamingが各`RESOURCE_EXHAUSTED`となり、対象streamのexact CANCELをfixtureが観測した同一connection上でのみfollow-upが成功することを確認。C unitでexact limitとoverflow時counter非更新を確認。mutation test自体は未実施。
- Notes: current production codeのruntime failureではなく、security/resource fixの必須invariantを識別しないtest / fixture defectとしてLowとした。protocol `REVIEW-20260715-005`のcancel oracle不足と重複するため、field-class境界とCANCEL観測を同じfixture stateで修正した。

### REVIEW-20260715-003: PHPT 043がvalid informational responseとbatch iteration resetを固定しない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary (pass 3)`
- Finding: bench parity用PHPTはmalformed post-1xx responseを失敗件数へ入れるnegative caseを1 iterationだけ実行する。validな1xxをbatchが受理するpositive pathも、同じbatch `grpc_call` を次iterationへ再利用するときのphase / status state resetもend-to-endでは検証しない。
- Evidence: `tests/phpt/043-informational-1xx-bench-parity.phpt:18-28` の唯一のcallは `post-informational-nonterminal-status` をiterations=1で送り、`ok=0`, `failed=1` だけをassertする。従ってbench callbackが全informational responseを無条件にprotocol errorへする実装でもtestは成功する。`tests/unit/test_response_header_phase.c:84-92` はpure helperのresetを確認するが、`src/diagnostic/bench.c:1447-1462` がbatch iterationごとに `final_response_headers_seen` / `grpc_status_seen` / terminal header stateを実際にresetすることは固定しない。issueのProgress / Verificationと `docs/verification/verification-matrix.md:29` はproduction / diagnostic parityとiteration resetをcoveredとしている。
- Expected model: diagnostic parity gateは、malformed blockを成功sampleへ入れないnegative caseと、polluted / repeated 1xxの後にvalid final responseを正常sampleとして受理するpositive caseの両方を持つ。同一batch内の2 iteration目は1 iteration目のfinal-seen / grpc-status observation / terminal header stateを継承しない。
- Why it matters: negative testだけでは「1xxを正しくphase遷移する」と「1xxを全部拒否する」を区別できない。またiteration resetの1 fieldが欠けると、1件目だけ成功して2件目をTRAILINGまたはprotocol errorとして落とすbenchmark regressionをC unitが検出できない。
- Recommended fix: PHPT 043のSKIPIFをport 50054 / 50071の両方へ広げ、50054のvalid controlを使う2-iteration batchを追加する。metadataは `x-bench-early-hints=1`、`x-bench-early-hints-count=2`、`x-bench-early-hints-pollution=1`、`x-bench-observe-authority=1` とし、polluted repeated 103 → clean final 200 → message → status 0を2回返させて `ok=2`, `failed=0` をassertする。raw fixtureへ同等controlを追加して50071だけで完結させてもよい。
- Fix summary: raw fixtureへ1 iteration 69 wire fieldsのrepeated / polluted informational response後にclean OKを返すcontrolを追加し、PHPT 043で同じbatch callを2 iterations実行するpositive / reset oracleを追加した。
- Fix commit: `pending`
- Verification: `valid-informational-iteration-reset`が2 iterationsで`ok=2 / failed=0`となり、同じPHPTのmalformed status / status-details controlsが各`ok=0 / failed=1`となることを確認。wire counter未resetなら2 iteration目で128-entry上限を越えるfield数も固定。PHPT 28/28 PASS。
- Notes: PHPT 043の既存negative assertion自体はpre-fix false-successを識別しており、問題はpositive / reuse halfが欠けている点に限定する。protocol `REVIEW-20260715-004`と同じpositive / reuse不足として統合し、reviewが許容したport 50071 raw fixture alternativeを採用した。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1`
- Low: `2`
- Design Decision: `none`
