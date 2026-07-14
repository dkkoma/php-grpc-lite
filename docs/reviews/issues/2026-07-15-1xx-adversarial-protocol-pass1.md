# Informational 1xx response handling adversarial protocol review pass 1 2026-07-15

## Scope

- `20c2dc0` (`src/grpc_exchange_state.h`, `src/transport.c`, `src/transport.h`, `src/diagnostic/bench.c`, fixture / PHPT / design docs)
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- rejected attempt `375c3dd` / revert `093b808` / `REVIEW-20260710-004`

## Reviewer Role

- HTTP/2 / gRPC protocol adversary

## Review Prompt Summary

- hostileまたはunusual-but-conformantなserverを前提に、nghttp2のHEADERS category / callback contract、multiple 1xx、missing `:status`、1xx + END_STREAM、1xx後のfinal / trailing ownership、PR #28のEND_STREAM taxonomy、CONTINUATION、call / retry / connection reuse時のphase freshness、HTTP status fallback、Trailers-Only、`grpc-status`のinitial / trailing lifecycle、production / diagnostic parityを静的に確認した。
- test suite / Dockerは実行せず、必要なruntime確認は各findingへexact wire probeとして記載した。

## Issues

### REVIEW-20260715-001: END_STREAMなしのtrailing HEADERSにある`grpc-status: 0`がprotocol errorより先にOKとして確定する

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: final response観測後のHEADERSは`TRAILING` phaseになるが、`on_header_callback()`はそのblockがEND_STREAMを持つかを確認せず`grpc-status`を即時にcall stateへ保存する。RFC 9113上、trailer sectionはEND_STREAM付きHEADERSでなければならず、nghttp2もblock末尾でEND_STREAMなしtrailerを`PROTOCOL_ERROR`として拒否する。しかしその拒否より先に保存された`grpc_status == 0`がstatus resolutionで優先されるため、malformed streamをpayload付きの成功として返せる。
- Evidence: `src/transport.c:2232-2251` は`TRAILING` phaseの`grpc-status`をframe flagに関係なく`call->grpc_status`へ反映する。`src/transport.c:2355-2365`のEND_STREAM gateは`trailing_headers_seen`を立てるだけであり、さらにnghttp2がframeをinvalidと判定した場合はvalid frame用の`on_frame_recv_callback()`自体が呼ばれない。`src/transport.c:419-431`は`on_invalid_frame_recv_callback`を登録せず、`on_stream_close_callback()` (`:2295-2307`)も`stream_error_code`を保存するだけで`stream_reset_seen`を立てない。最後に`src/status_core.c:32-38`は`grpc_status >= 0`をstream errorより先に返す。[RFC 9113 §8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1)はtrailersをEND_STREAM付きHEADERSに限定し、[nghttp2のHTTP messaging実装](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_http.c)の`nghttp2_http_on_trailer_headers()`もEND_STREAMなしをrejectする。[nghttp2 callback contract](https://nghttp2.org/documentation/nghttp2.h.html)ではinvalid non-DATA frameに対してlibraryがRST_STREAMまたはGOAWAYを自動submitし、`on_invalid_frame_recv_callback`へ通知する。exact probeは `final HEADERS(:status 200, content-type application/grpc)` → valid gRPC DATA 1 message → `HEADERS(grpc-status: 0, END_HEADERS, END_STREAMなし)`。optionalで先頭に103を置いても同じである。
- Expected model: `grpc-status`は「nghttp2 field callbackで見えた値」ではなく、HTTP/2上validなTrailersまたはvalidなTrailers-Only blockに属すると確定してからwire statusとしてcommitする。final response後のHEADERSはEND_STREAM必須であり、不足時はstatus fieldを無効化してresponse protocol errorへ分類する。
- Why it matters: hostile peerがHTTP/2 protocol errorを送っても、unaryはdecoded payload + OK、server streamingは既受信message + OKとしてapplicationへ見せられる。status別retry、監視、データ確定処理が「peerが成功を完了した」と誤認するため、単なるdetails差ではない。
- Recommended fix: `TRAILING` blockでは`on_begin_headers_callback()`時点で既知の`frame->hd.flags`を検査し、END_STREAMなしならfieldをcall-global status / metadataへ反映せず`malformed_response_frame`等のprotocol failureを立ててRST_STREAMする。またはheader block stateへfieldをstageし、validな`on_frame_recv_callback()`でのみcommitする。nghttp2が拒否したframeをcall taxonomyへ渡す`on_invalid_frame_recv_callback`も登録し、先行して見えた`grpc-status`よりprotocol-invalid markerを優先する。上記probeをunary / server streamingで固定する。
- Fix summary: `response_header_block_end_stream` とshared `grpc_response_header_phase_allows_status_fields()` でTrailing / Trailers-Onlyのstatus commitをEND_STREAM付きblockに限定した。END_STREAMなしTrailingはbegin callback時点でblockをquarantineし、nghttp2のinvalid-frame観測をdedicated `response_header_protocol_error` へ写像する。このmarkerは先行 `grpc-status: 0` より優先し `INTERNAL` となる。
- Fix commit: `pending`
- Verification: `tests/phpt/042-informational-1xx-adversarial.phpt` のexact END_STREAMなしtrailer probeがunary / server streamingで `INTERNAL` + `malformed HTTP/2 response header sequence` を確認。server streamingは先行1 messageをyieldするがstatusは成功確定しない。`./tools/test/check-phpt.sh` 28/28 PASS、C unit 4/4 PASS、static analysis PASS。
- Notes: 提案のblock全体staging allocationは追加せず、frame headerで既知のEND_STREAMを先にcommit gateとし、後段のnghttp2 rejectionはpriority markerで先行値を無効化する設計とした。これによりallocationを増やさずvalidityを守る。

### REVIEW-20260715-002: final responseなしで終わるinformational sequenceがprotocol errorではなくUNAVAILABLEへ落ちる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: 1xx blockは意図どおり`http_status`を更新しないが、1xxがEND_STREAMを持つ、1xx後にDATAが来る、または1xx後のblockに`:status`がない、といったmalformed responseをnghttp2がrejectした事実をcall stateが保持しない。その結果、stream close errorより先に「HTTP status未受信」のfallbackが走り、peer response protocol violationが`UNAVAILABLE` / `HTTP status -1 without grpc-status`として露出する。
- Evidence: `src/transport.c:2216-2230`は1xxを`INFORMATIONAL`にして`http_status == -1`のままfieldを隔離する。これはvalid 1xxには正しい。一方、`src/transport.c:2343-2376`には`INFORMATIONAL + END_STREAM`のfailure branchがなく、`configure_callbacks()` (`:419-431`)もinvalid frame callbackを登録しない。nghttp2は[HTTP messaging guide](https://nghttp2.org/documentation/programmers-guide.html#http-messaging)どおりnon-final responseの後にfinal responseを要求し、[RFC 9113 §8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1)も1xxを載せたEND_STREAM HEADERSをmalformedとする。library close callbackから`stream_error_code == NGHTTP2_PROTOCOL_ERROR`が渡っても、`on_stream_close_callback()`は`stream_reset_seen`を立てず、`src/status_core.c:15-28`が`http_status < 0`を先に`UNAVAILABLE`へ写像する。detailsも`src/transport.c:2441-2442`で`HTTP status -1 without grpc-status`となる。exact probeは (a) `HEADERS(:status 103, END_HEADERS|END_STREAM)`、(b) `HEADERS(:status 103, END_HEADERS)` → `HEADERS(x-after: v, END_HEADERS|END_STREAM, :statusなし)`、(c) `HEADERS(:status 103, END_HEADERS)` → `DATA(..., END_STREAM)`。
- Expected model: nghttp2がresponse HTTP messaging violationとしてstreamを`PROTOCOL_ERROR`で閉じた場合は、単なる接続不能やHTTP status未観測ではなくcall-localなmalformed responseとして保持し、repository既存taxonomyの`NGHTTP2_PROTOCOL_ERROR -> INTERNAL`相当へ分類する。
- Why it matters: `UNAVAILABLE`は一時的transport failureとしてapplication retryの対象になりやすいが、実体はpeerが送った再現性のあるprotocol-invalid responseである。statusとdetailsの誤分類により、壊れたserver/proxyへのretry増幅とdiagnosisの誤りを招く。
- Recommended fix: `on_invalid_frame_recv_callback`または同等のcall-local observerでnghttp2のHTTP messaging errorを`malformed_response_frame` / dedicated response-header protocol flagへ記録し、HTTP status fallbackより優先する。1xxを通常は隔離する現方針は維持し、上記3 sequenceだけをprotocol failureとしてunary / server streaming fixtureで固定する。
- Fix summary: HTTP messaging violationをgRPC DATA framingと分ける `response_header_protocol_error` を追加し、status taxonomyでHTTP status未観測fallbackより優先した。103+END_STREAM / missing `:status` HEADERSは `on_invalid_frame_recv_callback()` で捕捉、103→DATAはnghttp2がreceived callbacksへ渡さず生成するoutbound `RST_STREAM(PROTOCOL_ERROR)` をframe-send callbackで観測する。
- Fix commit: `pending`
- Verification: `tests/phpt/042-informational-1xx-adversarial.phpt` が3つのexact sequenceをunary / server streamingの両方で固定し、全caseが `INTERNAL` + header-sequence専用detailsとなり `UNAVAILABLE` / `HTTP status -1` へ落ちないことを確認。`test_status_core.c` もprotocol markerのpriorityを固定。PHPT 28/28 PASS。
- Notes: DATA-after-1xxではnghttp2が `on_invalid_frame_recv_callback()` / DATA callback / received-frame callbackのいずれも呼ばずRSTを自動submitすることをraw traceで確認したため、outbound frame observerを併用した。observerは重複RSTをsubmitしない。

### REVIEW-20260715-003: diagnostic mirrorがpost-1xx FINAL_INITIAL内のnon-terminal `grpc-status`を成功として数える

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: productionはFINAL_INITIAL blockに`grpc-status` / `grpc-message` / `grpc-status-details-bin`が現れた場合、それをTrailers-Only候補として記録し、同じHEADERSがEND_STREAMでなければinvalidにする。今回phase modelをmirrorした`src/diagnostic/bench.c`はphaseとstatus値だけを保存し、このFINAL_INITIAL lifecycle validationをmirrorしていないため、post-1xxのmalformed gRPC responseをbatch benchmarkの成功件数へ加算する。
- Evidence: productionの`src/transport.c:2234-2267`は`FINAL_INITIAL`で`initial_grpc_status_seen`を立て、`:2343-2354`は`!initial_headers_end_stream`なら`invalid_grpc_status = true`にする。対して`src/diagnostic/bench.c:342-356`は`grpc_status` / `grpc_message`を保存するだけで`initial_grpc_status_seen` / `grpc_status_seen`を更新せず、`bench_on_frame_recv_callback()` (`:453-487`)にもFINAL_INITIAL validationがない。`src/diagnostic/bench.c:1609-1613`は`stream_closed && grpc_status == 0 && http_status == 200`だけで`ok++`する。exact probeは `HEADERS(:status 103, END_HEADERS)` → `HEADERS(:status 200, content-type application/grpc, grpc-status 0, END_HEADERS, END_STREAMなし)` → valid gRPC DATA 1 message + END_STREAM。productionは`invalid_grpc_status`によりUNKNOWN、`grpc_lite_bench_unary_batch()`はOKとしてcountする。追加PHPTはproduction unary / server streamingだけを通り、raw batch entrypointを検証しない。
- Expected model: diagnostic transportが同じresponse header-block semantic phaseを実装すると宣言するなら、FINAL_INITIAL / TRAILINGのstatus validityもproductionと同じ状態遷移を持ち、malformed responseを成功sampleへ混ぜない。bench observationはproduction statusのownerではないが、benchmarkの`ok`分類はprotocol-valid sampleだけを数える必要がある。
- Why it matters: serverまたはfixtureのregressionで`grpc-status: 0`がinitial responseに早期送出されてもbenchmarkは成功し、壊れたresponseのlatency / throughputを正常データとして保存する。productionが拒否するshapeをdiagnosticだけが受理するため、性能観測が互換性回帰を隠す。
- Recommended fix: bench callbackにも`grpc_status_seen` / `initial_grpc_status_seen` / `initial_headers_end_stream`とFINAL_INITIAL frame-end validationをproduction同様に実装し、success gateでinvalid status stateを拒否する。可能ならphase transition / block validityの共通helperをproductionとdiagnosticで共有し、上記post-1xx raw sequenceをbench-enabled verificationへ追加する。
- Fix summary: bench callbackもshared phase / status-validity / metadata-role helperを使い、`grpc_status_seen`、`initial_grpc_status_seen`、`initial_headers_end_stream`、`trailing_headers_seen`、`invalid_grpc_status`、`response_header_protocol_error` をiterationごとにresetする。success gateはvalid terminal status blockとprotocol-valid stateを必須とした。
- Fix commit: `pending`
- Verification: `tests/phpt/043-informational-1xx-bench-parity.phpt` が103 → non-terminal FINAL_INITIAL `grpc-status: 0` → DATA END_STREAMをraw batch entrypointへ送り、`ok=0`, `failed=1` を確認。`tests/unit/test_response_header_phase.c` のphase×END_STREAM truth tableとPHPT 28/28 PASS。
- Notes: `first_response_header_us`の103時点はtransport observationのまま維持した。pass-2でEND_STREAM validityのproduction / diagnostic重複が残っていたため、`grpc_response_header_phase_allows_status_fields()` へ追加抽出し、構造的parityをC unitで守った。

### REVIEW-20260715-004: invalid `grpc-status`が同じTrailers-Only blockのmetadata ownershipをfield順で分割する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC protocol adversary`
- Finding: FINAL_INITIAL内の`trailing`判定が「`grpc-status`を観測したか」ではなく「parse済みstatusが0以上か」を使うため、invalid `grpc-status`より前のmetadataはtrailingへ移される一方、後続metadataはinitialへ追加される。同じEND_STREAM付きTrailers-Only blockのownershipがfield順序だけで二分される。
- Evidence: `src/transport.c:2232-2233`は`FINAL_INITIAL && call->grpc_status >= 0`だけを後続fieldのtrailing条件にする。`:2238-2247`はstatus値が`17`や非数値でparse失敗しても`grpc_status_seen = true`とし、既存metadataを`grpc_protocol_mark_response_metadata_as_trailing()`で移すが、`grpc_status`は`-1`のままである。従ってexact probe `HEADERS(:status 200, content-type application/grpc, x-before: a, grpc-status: 17, x-after: b, END_HEADERS|END_STREAM)`ではstatusはUNKNOWNとなる一方、`x-before`はtrailing、`x-after`はinitialになる。`docs/SPEC.md:240-242`と`docs/design/grpc-call-exchange-state.md`のphase modelはTrailers-Only responseをblock単位でtrailing ownershipとする。
- Expected model: metadataのinitial / trailing ownershipはheader blockのsemantic roleで一意に決まり、`grpc-status` valueのparse可否はstatus taxonomyだけに影響する。invalid statusを含むEND_STREAM付きTrailers-Onlyでも、同じblockのcustom metadataがfield順で別mapへ分かれない。
- Why it matters: error responseのPHP-visible `getMetadata()` / `getTrailingMetadata()`がHPACK field順に依存する。interceptorやerror details処理がtrailing metadataを取りこぼし、同じsemantic responseでもserver実装のheader orderingによって結果が変わる。
- Recommended fix: 少なくともstatus観測後のownershipは`grpc_status_seen`またはexplicitなblock-local Trailers-Only candidateで決め、parse成功をownership predicateに使わない。より堅牢にはblock fieldをstageし、END_STREAM付きFINAL_INITIALの完了時にblock全体をtrailingへ確定する。invalid status前後にcustom metadataを置くfixtureで両方がtrailingとなることを固定する。
- Fix summary: metadata trailing predicateをparse済み `grpc_status >= 0` からshared `grpc_response_header_phase_metadata_is_trailing()`（TRAILING、またはFINAL_INITIALで `grpc_status_seen`）へ変更した。status観測時に同じTrailers-Only blockの先行metadataをtrailingへ移し、後続fieldもparse成否に関係なくtrailingへ追加する。
- Fix commit: `pending`
- Verification: `tests/phpt/042-informational-1xx-adversarial.phpt` のexact field-order probeがunary / server streamingでstatus `UNKNOWN` + `invalid grpc-status trailer` を維持しつつ、`x-before: a` / `x-after: b` の両方がtrailing metadataにだけ現れることを確認。shared helper truth tableとPHPT 28/28 PASS。
- Notes: ownershipはstatus valueのparse結果ではなくblock roleの責務とし、production / diagnosticが同じpure predicateを使う。

## Review Result

- Blocker: `none`
- High: `none` (`1 fixed`)
- Medium: `none` (`2 fixed`)
- Low: `none` (`1 fixed`)
- Design Decision: `none`
