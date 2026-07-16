# Informational 1xx response handling adversarial protocol review pass 3 2026-07-15

## Scope

- Commits `20c2dc0` + `6a4902f`（current HEAD）
- `src/response_header_phase.h`
- `src/response_header_phase.c`
- `src/grpc_exchange_state.h`
- `src/transport.c`
- `src/status_core.c`
- `src/diagnostic/bench.c`
- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- pass-1 adversarial review 3 records / pass-2 domain-model review

## Reviewer Role

- HTTP/2 / gRPC protocol adversary

## Review Prompt Summary

- pass-1の全8 findingが `6a4902f` で実際に閉じたかを、nghttp2 callback contract、response header-block phase、END_STREAM commit gate、invalid-frame taxonomy、wire header budget、production / diagnostic parity、raw fixture / PHPT oracleの観点から静的に再確認した。
- hostileまたはunusual-but-conformantなserverを前提に、missing `:status`、multiple 1xx、1xx + END_STREAM、1xx後DATA、Trailers-Only、non-terminal status fields、CONTINUATION、call / retry freshness、HTTP fallbackを追跡した。test suite / Dockerは実行していない。

## Issues

### REVIEW-20260715-001: nghttp2がsilently ignoreするinvalid regular fieldはwire header budgetを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: pass-1 C-safety `REVIEW-20260715-001` の修正は通常の `on_header_callback()` に届くfieldだけをwire budgetへ計上する。nghttp2は一部のinvalid regular header fieldを通常callbackへ渡さず、`on_invalid_header_callback`未登録時はsilently ignoreするため、peerは単一informational block内へ大量または巨大なinvalid fieldを詰め、`wire_response_header_entry_count` / `wire_response_header_bytes`を増やさずHPACK decode workを継続させられる。
- Evidence: `src/transport.c:421-434` はbegin/header/invalid-frame callbackを登録するがinvalid-header callbackを登録しない。accounting owner `grpc_protocol_account_response_header_field()` (`src/transport.c:2213-2237`) は通常の `on_header_callback()` (`:2240-2260`) からしか呼ばれない。nghttp2 v1.69.0の [`nghttp2_on_invalid_header_callback` contract](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/includes/nghttp2/nghttp2.h) はinvalid regular fieldが専用callback未登録時にsilently ignoredされると明記し、[`nghttp2_http_on_header()`](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_http.c) はNULを含むregular valueを `NGHTTP2_ERR_IGN_HTTP_HEADER` とする。exact probeは1つのHEADERS blockに `:status: 103` を先頭で置き、その後へ `x-info: "a\x00b"` を129 entries、または `grpc.absolute_max_metadata_size=1024` 下でNULを含む2049-byte valueを1つ送ってから、cleanなfinal 200 / DATA / `grpc-status: 0`を送る。現stateでは`:status`だけがbudgetへ1 entryとして入り、invalid fieldは全て無計上のまま最終statusがOKになり得る。
- Expected model: semantic validationで採用・隔離・ignoreされるかに関係なく、nghttp2がdecodeしたresponse fieldはcall-local hard work budgetへ入る。invalid regular fieldを許容してignoreするなら専用callbackでも同じoverflow-safe accountingを行い、上限到達時は `RESOURCE_EXHAUSTED` + stream-local cancelとする。よりstrictに最初のinvalid fieldでresponse protocol errorへする場合も、無制限にdecodeを続けない。
- Why it matters: `SETTINGS_MAX_HEADER_LIST_SIZE` はpeerへのadvisoryであり、application hard boundの代替ではない。HPACKでは大きいfieldをdynamic tableから小さいwire costで反復できるため、今回追加した「informational / pseudo-headerを含む全wire fieldのhard budget」をhostile peerが迂回し、PHP workerへ上限外のdecode CPU / transient allocationを負わせられる。
- Recommended fix: production callback setへ `nghttp2_on_invalid_header_callback`（またはcallback2）を登録し、invalid regular fieldにも共通wire-accounting ownerを適用する。上記NUL-bearing fieldのentry / byte probeをraw fixtureへ追加し、unary / server streamingの `RESOURCE_EXHAUSTED`、wire `RST_STREAM(CANCEL)`、same-connection follow-up成功を固定する。
- Fix summary: productionへinvalid regular header callbackを登録し、normal / invalid fieldの双方を共有`grpc_protocol_account_response_header_field()`からpureな`grpc_response_header_budget_account_field()`へ通した。budget超過は`metadata_too_large`を確定して`RST_STREAM(CANCEL)`をqueueし、invalid field自体はsemantic stateへ反映しない。raw fixtureへNULを含むinvalid regular fieldのentry / byte controlを追加した。
- Fix commit: `pending`
- Verification: `tests/phpt/042-informational-1xx-adversarial.phpt`でinvalid regular entry / byte controlをunary / server streaming双方が`RESOURCE_EXHAUSTED`とし、対象streamの`RST_STREAM(CANCEL)`観測後だけ同一connectionのfollow-upが成功することを確認。`tests/unit/test_transport_core.c`で128-entry境界、exact byte上限、`SIZE_MAX`加算overflowを確認。`./tools/test/check-phpt.sh` 28/28 PASS、`./tools/test/check-c-unit.sh` 4/4 PASS。
- Notes: invalid pseudo-headerやuppercase fieldのようにnghttp2がblock自体をrejectする経路ではなく、公式contract上silently ignoreされるregular fieldだけを対象にしたfindingである。この既定動作は[nghttp2 v1.67.0](https://nghttp2.org/blog/2025/09/02/nghttp2-v1-67-0/)でstream errorからsilent ignoreへ変更された。repositoryは`config.m4`でlibnghttp2の上限versionを持たず、review環境のhost headerは1.69.0であるため、Docker image側の旧versionだけを前提に除外できない。C safety `REVIEW-20260715-001`と同一defectとして共有ownerで一度だけ修正した。invalid field例はNUL-bearing `x-ignored`を採用し、leading SP案と同じnghttp2 invalid regular-header callback経路を固定した。

### REVIEW-20260715-002: diagnostic benchはinformational wire header budgetを実装せずover-budget responseを成功sampleにする

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: pass-1 C-safety `REVIEW-20260715-001` でproductionへ追加した全response-field budgetがraw diagnostic batchには存在しない。validな大量1xx fieldはphase isolationで即returnされ、batch success gateはresource violationも見ないため、productionが `RESOURCE_EXHAUSTED` とする同じwire sequenceをbenchは `ok` に加算する。
- Evidence: productionは `src/transport.c:2213-2237,2253-2259` でphase isolationより前に全通常HEADERS fieldを計上する。対して `src/diagnostic/bench.c:319-353` はaccountingなしでAWAITING / INFORMATIONALからreturnする。raw batch初期化 `:1259-1298` は `max_response_metadata_bytes` を設定せず、iteration reset `:1447-1466` も `wire_response_header_*` / `metadata_too_large` をresetせず、success gate `:1700-1707` は `metadata_too_large` を拒否しない。既存control `informational-entry-budget` は `poc/test-server/main.go:704-711` で129回の `(:status 103, x-info: a)` 後にvalid OK responseを送る。これを `grpc_lite_bench_unary_batch(..., 1, ['x-bench-raw-response' => 'informational-entry-budget'])` へ渡すと、informational fieldは全て無計上で、final `content-type` がzero-initのsemantic metadata limitを発火させても `call.connection == NULL` のためRSTはsubmitされず、そのflagもsuccess gateが無視する。terminal `grpc-status: 0` / trailing HEADERSが揃い、静的には `ok=1, failed=0` へ到達する。
- Expected model: diagnostic transportがproduction response semanticsをmirrorすると宣言する以上、wire header work budget、stream-local stop、iteration reset、success classificationも同じdomain ruleを使う。over-budget sampleは正常benchmark dataへ入れない。
- Why it matters: hostileまたはregressed fixtureが送るheader-heavy responseをbenchだけが無制限にdecodeし、さらにproductionでは拒否されるlatency / throughput sampleを正常値として保存する。pass-2の「全response HEADERS field」とproduction / diagnostic parityの検証結果も成立していない。
- Recommended fix: accountingをproduction / diagnosticで共有できるownerへ抽出し、raw batchへ実際のdefault metadata limitを設定する。各iterationでwire countersと `metadata_too_large` をresetし、live `nghttp2_session` でstream-local cancelできるようにし、success gateでもresource violationを拒否する。既存entry / byte controlsをPHPT 043から実行して `ok=0, failed=1` を固定する。
- Fix summary: diagnostic normal / invalid header callbackもproductionと同じ共有accounting ownerをphase isolation前に呼ぶようにした。batch callへdefault 64KiB limitを設定し、wire / semantic counterと`metadata_too_large`をiterationごとにresetし、live sessionでCANCELをqueueしてresource violationをsuccess countから除外した。
- Fix commit: `pending`
- Verification: PHPT 043でentry budgetとdefault 64KiB byte budgetが各`ok=0 / failed=1 / stream_error_code=8`、69 fields/iterationのvalid repeated/polluted 1xxを2 iterations実行して`ok=2 / failed=0`となることを確認。PHPT全体28/28 PASS、static analysis PASS。
- Notes: 単にsuccess gateへ `!metadata_too_large` を足すだけでは、現在のraw batchはlimitが0のため通常responseまで全失敗になる。limit initialization、accounting、reset、transport actionを一体で直した。C safety `REVIEW-20260715-003` / test `REVIEW-20260715-001`と重複するため同じ共有ownerとiteration-local lifecycleで解消した。byte側はbatch APIを増やさずdefault 64KiBを越えるcontrolを採用した。

### REVIEW-20260715-003: diagnostic benchだけ`grpc-status-details-bin`をnon-terminal FINAL_INITIALでcommitする

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: pass-1 protocol `REVIEW-20260715-003` のbench parity修正は `grpc-status` と `grpc-message` だけをEND_STREAM付きterminal blockへgateし、3つ目のstatus fieldである `grpc-status-details-bin` をgeneric metadataとして処理する。そのためpost-1xx FINAL_INITIALにnon-terminal status detailsが現れてもbenchは `invalid_grpc_status` を立てず、後続のvalid `grpc-status: 0`を成功sampleへcommitする。
- Evidence: production `src/transport.c:2324-2333` は `grpc-status-details-bin` でもFINAL_INITIALの `initial_grpc_status_seen` を立て、shared `grpc_response_header_phase_allows_status_fields()` がfalseなら `invalid_grpc_status` とbody discardを確定する。bench `src/diagnostic/bench.c:354-389` は `grpc-status` / `grpc-message` のbranchしか持たず、details-binはgeneric metadataへfall throughする。従ってframe-end `:540-547` もinvalid stateを観測できず、success gate `:1700-1707` は後続terminal statusだけで成功する。exact probeは `103 HEADERS` → non-END_STREAM `HEADERS(:status 200, content-type application/grpc, grpc-status-details-bin: AA==)` → optional valid DATA → `HEADERS(grpc-status: 0, END_STREAM)`。productionは `UNKNOWN / invalid grpc-status trailer`、raw batchは静的に `ok=1, failed=0` となる。
- Expected model: `grpc-status` / `grpc-message` / `grpc-status-details-bin` は同じStatus / Status-Message / Status-Details lifecycleに属し、validなEND_STREAM付きTrailersまたはTrailers-Only blockでのみcommitできる。production / diagnosticでfieldごとの例外を作らない。
- Why it matters: server regressionがstatus detailsをinitial headersへ早期送出してもbenchmarkだけがprotocol-invalid responseを成功扱いし、互換性回帰を性能データで隠す。shared phase helperを導入してもconsumer field setが一致しなければ構造的parityにはならない。
- Recommended fix: benchにもproductionと同じ `grpc-status-details-bin` branchとshared END_STREAM predicateを適用し、FINAL_INITIALで `initial_grpc_status_seen` / `invalid_grpc_status` を更新する。上記exact raw sequenceをfixtureとPHPT 043へ追加する。
- Fix summary: bench callbackへ`grpc-status-details-bin`のstatus-field branchを追加し、FINAL_INITIAL観測とshared END_STREAM predicateをproductionと一致させた。non-terminal fieldは`invalid_grpc_status` / body discardへ分類し、valid terminal fieldだけをtrailing metadataとして扱う。
- Fix commit: `pending`
- Verification: PHPT 042のunary / server streamingでexact details-bin sequenceが`UNKNOWN / invalid grpc-status trailer`、PHPT 043で同じsequenceが`ok=0 / failed=1`となることを確認。PHPT全体28/28 PASS。
- Notes: `grpc-status-details-bin` のPHP-visible保存有無ではなく、status fieldとしてのblock validity classificationだけをfindingとする。C safety `REVIEW-20260715-002`と同一defectとして一度だけ修正した。

### REVIEW-20260715-004: PHPT 043はnegative caseだけでbatch全失敗regressionでも通る

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: bench false-success修正を固定するPHPT 043はmalformed sequenceが失敗したことだけをassertし、同じraw fixture / batch callback stackがvalid responseを成功sampleへできるpositive controlを持たない。そのためphase fixとは無関係に全batch callを失敗させるregressionでもtestはPASSする。
- Evidence: `tests/phpt/043-informational-1xx-bench-parity.phpt:18-28` は `post-informational-nonterminal-status` を1回呼び、`ok=0`, `failed=1` のみを確認する。repository内の他PHPT / PHPUnitに `grpc_lite_bench_unary_batch()` のvalid callで `ok=1` をassertするcaseはない。PHPT 042はproduction unary / server streaming callback setを検証するが、別実装のraw batch callback stackのpositive controlにはならない。
- Expected model: rejection testは同じentrypoint、fixture、connection setupでvalid final initial / DATA / terminal trailersが `ok=1` になることを先に固定し、その上でmalformed status lifecycleだけが `ok=0` になることを区別する。
- Why it matters: callback registration、request送信、stream close、success gateの任意の破損が「malformed responseを拒否した」という意図したoracleと区別できず、pass-1 protocol findingの回帰testとして識別力を持たない。
- Recommended fix: PHPT 043の先頭でport 50071のdefault valid raw responseを同じbatch entrypointへ1回送り、`ok=1`, `failed=0` をassertしてからnegative controlsを実行する。
- Fix summary: PHPT 043の先頭へ、repeated / polluted informational HEADERSを含むvalid raw responseを同じbatch callで2 iterations処理するpositive controlを追加した。1 iteration 69 wire fieldsとし、all-fail regressionに加えてphase / status / wire counterのiteration reset漏れも識別する。
- Fix commit: `pending`
- Verification: `valid-informational-iteration-reset`が`ok=2 / failed=0`、後続のmalformed status / details controlsが各`ok=0 / failed=1`となることを同一PHPTで確認。`./tools/test/check-phpt.sh` 28/28 PASS。
- Notes: `grpc_lite_phpt_skip_if_bench_diagnostics_unavailable()` はsymbol availabilityだけを確認するためpositive oracleの代替ではない。test `REVIEW-20260715-003`と同じpositive / reuse不足として統合し、reviewが許容したraw fixture alternativeを採用した。このpositive controlにより、raw bench sendがproduction connection ownerを誤用して全callを送信前に失敗させていた既存不整合も検出し、bench-local fd sendへ修正した。

### REVIEW-20260715-005: PHPT 042のresource probeはheader-budget超過時の`RST_STREAM(CANCEL)`を観測しない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: pass-1 C-safety修正はbudget超過を `RESOURCE_EXHAUSTED` へ分類するだけでなく、その場でstream-local `RST_STREAM(CANCEL)`を送って追加header workを止めることを完了条件にした。しかしPHPT 042のfollow-up oracleはfixtureがover-budget responseを送った事実だけで開き、client RSTを一度も要求しない。RST submitを削除してもstatusとsame-connection follow-upが通る。
- Evidence: `poc/test-server/main.go:647-679` の `resourceProbeSent` は `writeInformationalAdversarialResponse()` の戻り値だけで更新され、受信 `RSTStreamFrame` branchはrequest mapをdeleteするだけでerror code /対象streamを記録しない。entry / byte controls `:704-720` はframe送出後に無条件でtrueを返し、follow-up `:721-731` はそのboolだけを見る。`tests/phpt/042-informational-1xx-adversarial.phpt:66-110` は `RESOURCE_EXHAUSTED` と同じclientのfollow-up成功をassertするがwire RSTは見ない。従って `src/transport.c:2237` のRST submitだけを削除するmutationでも、local `metadata_too_large` がstatusを決め、fixtureのfinal END_STREAM後にconnectionを再利用できるため現assertionは成立する。
- Expected model: hard budget到達時はcall-local classificationと同時に対象streamを即cancelし、peerがfinal responseを送るまで無制限にheader decodeを続けない。verificationはstatus、RST error code、connection reuseを独立に観測する。
- Why it matters: malicious peerがbudget到達後も1xxを送り続ける場合、classification flagだけではcallを完了できずwork boundにならない。現在のcodeはRSTをsubmitしているが、今回の安全性修正で最も重要なtransport actionをtestが固定していない。
- Recommended fix: raw fixtureでresource probe対象stream IDを保持し、そのstreamに対するinbound `RST_STREAM` のerror codeがCANCELだったことを確認した場合だけ `require-prior-resource-probe` をOKにする。または当該callに限定したtrace assertionでoutbound RST_STREAM(CANCEL)を固定する。
- Fix summary: raw fixtureのresource markerを対象stream IDと受信CANCELの組へ変更し、対象streamについて`RST_STREAM(CANCEL)`を観測した場合だけ同一connection follow-upをOKにした。entry controlは65個の`:status + x-info`、byte controlは237-byte valueを4個とし、pseudo / regularの片側だけでは上限未満になるoracleへ変更した。
- Fix commit: `pending`
- Verification: PHPT 042でentry / 1024-byte budgetのunary / server streamingが`RESOURCE_EXHAUSTED`となり、その各follow-upが対象streamのCANCELをfixtureで観測した同一connection上だけで成功することを確認。PHPT全体28/28 PASS。
- Notes: 既存PHPT 033 / 036等のRST assertionはdeadline / explicit cancel経路であり、新規header-accounting callbackからのRST submitを通らない。test `REVIEW-20260715-002`のfield-class / cancel oracle不足と重複するため、同じfixture stateで修正した。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `3`
- Low: `2`
- Design Decision: `none`
