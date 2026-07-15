# Test fixtures

この文書は、local test-server と検証用 metadata control protocol の読み方をまとめる。

`poc/test-server/main.go` は、通常のgRPC serverだけでなく、HTTP/2 / gRPC lifecycleの異常系を再現するfixtureも同時に起動する。PHPT runnerは `test-server:50051` から `test-server:50071` までをpreflightする。

## Ports

| Port | Mode | Purpose | Representative tests |
|---:|---|---|---|
| `50051` | h2c gRPC | 通常のunary / server streaming、metadata、deadline、resource limit、interceptor、Spanner以外の主経路 | `tests/phpt/010-unary.phpt`, `tests/phpt/011-server-streaming.phpt`, `tests/phpt/020-request-metadata-control.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| `50052` | h2 over TLS | TLS channel credentials、ALPN h2、bad root rejection | `tests/phpt/030-tls.phpt`, `tests/Integration/TlsTest.php` |
| `50053` | h2 over mTLS | client certificate必須のmTLS、missing client cert rejection | `tests/phpt/030-tls.phpt`, `tests/Integration/MtlsTest.php` |
| `50054` | h2c HTTP server, non-gRPC by default | informational 1xx、invalid content-type、HTTP status fallback、compressed flag、unsupported encoding、partial frameなどのprotocol validation | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php`, `tests/Integration/HttpValidationTest.php` |
| `50055` | raw h2c lifecycle fixture | successful gRPC response中にGOAWAYを送る。既存streamは成功し、connectionはdraining扱いになることを見る | `tests/phpt/024-control-semantics.phpt` |
| `50056` | raw h2c lifecycle fixture | 奇数回の接続で即EOF、偶数回で正常応答。connection failure後に再接続できることを見る | `tests/phpt/024-control-semantics.phpt` |
| `50057` | raw h2c mid-stream failure fixture | gRPC response frameを途中まで送ってcloseする。malformed frame / unavailable系の扱いを見る | `tests/phpt/024-control-semantics.phpt` |
| `50058` | raw h2c server RST_STREAM fixture | 同一connectionの最初のunary streamを `REFUSED_STREAM`、次streamをOKにする | `tests/phpt/024-control-semantics.phpt` |
| `50059` | raw h2c server RST_STREAM fixture | 同一connectionの最初のserver streaming streamを `REFUSED_STREAM`、次streamをOKにする | `tests/phpt/024-control-semantics.phpt` |
| `50060` | raw h2c GOAWAY refused fixture | request streamを受けた直後に `GOAWAY(last_stream_id=0)` を送る。stream refused by GOAWAYを検証する | `tests/phpt/024-control-semantics.phpt` |
| `50061` | raw h2c GOAWAY refused then OK fixture | unary用。奇数回の接続で `GOAWAY(last_stream_id=0)` によりrequest streamをrefuseし、偶数回の接続でOKを返す。transparent retryを検証する | `tests/phpt/024-control-semantics.phpt` |
| `50062` | raw h2c GOAWAY MaxInt32 then OK fixture | request stream上で `GOAWAY(last_stream_id=2147483647)` を送ってからOKを返す。二段階GOAWAYの1回目をdrainingのみとして扱うことを検証する | `tests/phpt/024-control-semantics.phpt` |
| `50063` | raw h2c GOAWAY refused then OK fixture | server streaming用。独立counterで初回接続refused、retry接続OKを返す | `tests/phpt/024-control-semantics.phpt` |
| `50064` | raw h2c GOAWAY after message fixture | 1 messageを送った後に `GOAWAY(last_stream_id=0)` を送る。userlandへmessage delivery後はtransparent retryしないことを検証する | `tests/phpt/024-control-semantics.phpt` |
| `50065` | raw h2c GOAWAY refused then delayed OK fixture | 初回接続refused、retry接続はdeadline超過後にOKを返す。server streaming retryでabsolute deadlineが延長されないことを検証する | `tests/phpt/024-control-semantics.phpt` |
| `50066` | raw h2c shared-connection failure fixture | 1本目のstreamへ1 messageを返してopenのままにし、同じconnection上の2本目request到着時にTCP connectionを閉じる。connection failureが全ownerへterminalになることを検証する | `tests/phpt/034-dead-connection-terminal-for-owners.phpt` |
| `50067` | raw h2c GOAWAY keep-stream-open fixture | 1 messageと `GOAWAY(last_stream_id=2147483647)` を送りstreamをopenのままにする。draining connection上の明示cancelがRST_STREAMを送ることを検証する | `tests/phpt/036-draining-connection-cancel-sends-rst.phpt` |
| `50068` / `50069` | raw h2c backlog flood / control fixture | control portで次connectionをarmし、response backlogをclient receive bufferへ置く。preflight drain cap超過時のfallbackを検証する | `tests/phpt/035-preflight-drain-cap-fallback.phpt` |
| `50070` | raw h2c small-window GOAWAY draining fixture | 小さいstream windowでrequest DATAをdeferし、別streamからdrainingへ移行する。destructor cancel後にdeferred DATAが再開してもcall lifetimeを越えないことを検証する | `tests/phpt/037-draining-destructor-pending-request-data.phpt` |
| `50071` | raw h2c informational adversarial fixture | malformedな1xx / trailing HEADERS、silent peer下のstatus-field / wire-header budget failure、Trailers-Only metadata ownership、bench diagnostic parity、foreign pushed-stream eventをrequest controlごとに送出する。同一connection上のexact RST観測とfollow-up RPCも扱う | `tests/phpt/042-informational-1xx-adversarial.phpt`, `tests/phpt/043-informational-1xx-bench-parity.phpt`, `tests/phpt/044-terminal-status-rst-flush-failure.phpt` |

## Service methods

| Method | Kind | Fixture behavior |
|---|---|---|
| `/helloworld.Greeter/SayHello` | unary | `Hello, <name>` を返す。`delay_ms` でhandler delayを入れられる |
| `/helloworld.Greeter/SayManyHellos` | server streaming | 5件の `Hello #n, <name>` を返す |
| `/helloworld.Greeter/BenchUnary` | unary | payload size、server delay、error status、metadata echo、server timingをmetadata/requestで制御する |
| `/helloworld.Greeter/BenchServerStream` | server streaming | message count、payload size、server delay、error status、metadata echoをmetadata/requestで制御する |

## Metadata controls on gRPC fixtures

これらは主に `50051` / `50052` / `50053` のgRPC serverで使う。

| Request metadata | Effect | Used by |
|---|---|---|
| `x-bench-error-code` | 指定したgRPC status codeでhandler errorを返す | `tests/Integration/ErrorSemanticsTest.php` |
| `x-bench-error-message` | `x-bench-error-code` と一緒に返すdetails | `tests/Integration/ErrorSemanticsTest.php` |
| `x-bench-response-metadata-count` | initial/trailing metadataを指定個数生成する | `tests/phpt/025-resource-limits.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-response-metadata-value-bytes` | 生成metadata valueのbyte長を指定する | `tests/phpt/025-resource-limits.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-echo-bin` | request binary metadataを `x-bench-initial-bin` / `x-bench-trailing-bin` として返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-echo-ascii` | request ASCII metadataを `x-bench-initial-ascii` / `x-bench-trailing-ascii` として返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-response-duplicate` | duplicate ASCII valuesをinitial/trailing metadataへ返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-response-duplicate-bin` | duplicate binary valuesをinitial/trailing metadataへ返す | `tests/Integration/MetadataCompatibilityTest.php` |
| `x-bench-observe-metadata-key` | 指定keyのrequest metadataを `x-bench-seen-*` metadataとして返す | `tests/phpt/020-request-metadata-control.phpt` |
| `x-bench-server-timing` | server handler timingをtrailing metadataへ返す | benchmark helpers |
| `x-bench-server-stats` | gRPC stats handler timingをtrailing metadataへ返す | benchmark helpers |
| `x-bench-server-cached-payload` | fixture server側でcached payloadを使う | benchmark helpers |

## Metadata controls on non-gRPC h2c fixture

これらは `50054` のHTTP/2 serverで使う。通常のgRPC serverではなく、client側のHTTP/gRPC validationを確認するためのfixtureである。

| Request metadata | Effect | Used by |
|---|---|---|
| `x-bench-http-status` | HTTP statusを指定する。`grpc-status` がないHTTP responseのfallback mappingを見る | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-content-type` | response `content-type` を指定する | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/HttpValidationTest.php` |
| `x-bench-grpc-status` | trailersの `grpc-status` を指定する | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/HttpValidationTest.php` |
| `x-bench-grpc-response=compressed-flag` | compressed flag=1のgRPC frameを返す。`x-bench-grpc-encoding` 併用で `grpc-encoding` も advertise する | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php` |
| `x-bench-grpc-response=partial-frame` | incomplete gRPC frameを返す | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-grpc-response=no-trailers` | message送信後、trailers (`grpc-status`) なしのclean END_STREAM (DATA) で閉じる | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-grpc-response=headers-only` | headers-only END_STREAMで閉じる。`x-bench-grpc-status` 指定時はtrailers-only応答、未指定時は `grpc-status` なし | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-grpc-response=custom-trailers-no-status` | `grpc-status` を含まないtrailing HEADERS (`x-bench-trailer` のみ) で閉じる | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-grpc-response=grpc-message-only-trailers` | `grpc-message` はあるが `grpc-status` を含まないtrailing HEADERSで閉じる | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-grpc-encoding` | response `grpc-encoding` を指定する (message自体はflag=0)。`x-bench-grpc-status` 併用でtrailerも返す | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-early-hints=1` | final responseの前に103 Early Hintsを送る。1xx後のfinal response HEADERSをinitial metadataとして扱う経路を固定し、他のcontrolと併用できる | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-early-hints-count=<1..16>` | `x-bench-early-hints=1` と併用し、final response前に送る103 block数を指定する。複数1xxのphase反復を検証する | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-early-hints-pollution=1` | `x-bench-early-hints=1` の103にinvalid `content-type`、`grpc-status`、`grpc-message`、`grpc-encoding`、`x-bench-informational-only` を載せ、informational fieldの完全な隔離を検証する | `tests/phpt/022-error-and-http-validation.phpt` |
| `x-bench-observe-authority=1` | observed authorityを `x-bench-authority` として返す | `tests/phpt/022-error-and-http-validation.phpt`, authority / TLS identity diagnostics |

## Response controls on raw informational fixture

これらは `50071` のraw HTTP/2 fixtureで使う。`net/http` が送出を拒否するmalformed sequenceを含むため、`50054` のcontrolとは分離している。fixtureはrequest HEADERSをdecodeしてstreamごとにcontrolを選び、stream-local failure後もconnectionをopenに保つ。silent controlはfailureを決定するHEADERSの後にDATA / trailer / final responseを送らず、clientのstream-local actionだけでcallが終了することを検証する。

| `x-bench-raw-response` value | Wire response |
|---|---|
| `trailer-without-end-stream` | final 200 initial HEADERS、1 gRPC message、`grpc-status: 0` を持つEND_STREAMなしtrailing HEADERS |
| `informational-end-stream` | `:status: 103` を持つ `HEADERS(END_STREAM)` |
| `informational-then-missing-status` | 103の後に`:status`を持たないfinal候補 `HEADERS(END_STREAM)` |
| `post-informational-incomplete-regular-before-status` | 完了した103の後、valid regular field `x-after`だけを持つEND_STREAM / END_HEADERSなしHEADERSを送り、CONTINUATIONを送らない。`:status`を合法に追加できない時点での `INTERNAL` + exact `RST_STREAM(PROTOCOL_ERROR)`、connection terminal化、fresh follow-upを検証する |
| `post-informational-incomplete-invalid-before-status` | 上記regular-before-statusをNUL-bearing invalid valueで送り、invalid-header callbackでも同じ早期protocol classification / terminal actionを通ることを検証する |
| `post-informational-incomplete-empty-name-before-status` | 上記invalid regular-before-statusをempty nameのexact HPACK field（`00 00 01 76`）で送り、wire budget計上後に同じprotocol classification / terminal actionを通ることを検証する |
| `post-informational-incomplete-strict-invalid-pseudo-before-status` | 完了した103の後、unknown pseudo-header `:foo: v` のexact HPACK field（`00 04 3a 66 6f 6f 01 76`）だけを持つEND_STREAM / END_HEADERSなしHEADERSを送り、CONTINUATIONを送らない。normal / invalid-header callbackを迂回するnghttp2 strict rejectionでも共有incomplete-header terminal action、exact `RST_STREAM(PROTOCOL_ERROR)`、fresh follow-upへ到達することを検証する |
| `post-informational-incomplete-uppercase-regular-before-status` | 上記strict rejectionをuppercase regular name `X-Bad: v` のexact HPACK field（`00 05 58 2d 42 61 64 01 76`）で発生させ、pseudo-header以外のstrict-invalid classも同じdefault terminal routeへ入ることを検証する |
| `late-incomplete-headers-after-close` | 103、final 200、valid gRPC DATA、`grpc-status: 0` terminal trailerでtarget streamをcloseした直後、同じTCP write内で同じstreamへEND_STREAM / END_HEADERSなしHEADERS（exact `:foo: v`）を送りCONTINUATIONを省く。完了callのOKを維持し、poisonされたconnectionを再利用せずfresh follow-upへ移ることを検証する |
| `live-incomplete-informational-deadline` | live unary streamへ`:status: 103`のEND_HEADERSなしHEADERSを送り、CONTINUATIONを省いてpeerをsilentに保つ。deadline abandonmentが`DEADLINE_EXCEEDED` + exact `RST_STREAM(CANCEL)`を維持しつつconnection terminal化し、同一clientのfollow-upをfresh connectionへ移すことを検証する。diagnostic batchは同じopen-block predicateとfinite nonblocking teardownを検証する |
| `live-incomplete-trailing-explicit-cancel` | complete final initial HEADERS、1 gRPC message、regular trailing metadataを含むEND_STREAM / END_HEADERSなしHEADERSをchecked single TCP writeで送り、最初のmessage yield後のexplicit `cancel()`がexact `RST_STREAM(CANCEL)`、connection terminal化、fresh follow-upへ収束することを検証する |
| `informational-then-data` | 103の直後に `DATA(END_STREAM)` |
| `informational-entry-budget` | `:status: 103` + `x-info: a` を65 block（合計130 fields）送って以後silentになる。pseudo / regularの片方だけなら65 entriesで上限内、両方の累積で128-entry上限をpre-finalに超える |
| `informational-byte-budget` | 241 bytesの`x-info`を4個の103 blockへ分けて以後silentになる。1024-byte上限に対し、custom fieldsは `4 * (6 + 241) = 988` bytes、informational `:status` は `4 * (7 + 3) = 40` bytes、合計1028 bytesとなりpre-finalに超過する |
| `informational-invalid-entry-budget` | 1個の103 blockへNULを含むinvalid regular fieldを129個載せて以後silentになる。normal callbackの`:status`で1 entry、invalid callback 1〜127で上限128に達し、callback 128の超過が返すTEMPORALで129個目のcallback前に停止する。productionは `wire.response_invalid_header` trace 128件、diagnosticは `invalid_header_callback_count=128` でcutoffを固定する |
| `informational-invalid-byte-budget` | 1個の103 blockへNULを含む2049-byte invalid regular valueを載せて以後silentになる。1024-byte上限をinvalid-header callbackでも適用することを検証する |
| `informational-default-byte-budget` | 8192-byteの`x-info`を8個の103 blockへ分けて以後silentになり、diagnostic default 64KiB wire-header budgetをentry上限とは独立に超える |
| `require-prior-resource-probe` | 同じconnectionの直前のresource probe streamについてclientからexact `RST_STREAM(CANCEL)` を受信済みの場合だけgRPC OK。PHPTは2秒のguard内に `RESOURCE_EXHAUSTED` + `response header/metadata budget exceeded` が返ることと、status分類、cancel action、connection reuseを分離して検証する |
| `post-informational-silent-grpc-status` | 103の後に `grpc-status` を持つEND_STREAMなしfinal initial HEADERSを送り、以後silentになる |
| `post-informational-silent-grpc-message` | 103の後に `grpc-message` を持つEND_STREAMなしfinal initial HEADERSを送り、以後silentになる |
| `post-informational-silent-status-details` | 103の後に `grpc-status-details-bin` を持つEND_STREAMなしfinal initial HEADERSを送り、以後silentになる |
| `post-informational-incomplete-grpc-status` | 103の後、`grpc-status` を含むEND_STREAMなし / END_HEADERSなしfinal initial HEADERSを送り、CONTINUATIONを送らない。clientの即時CANCEL、flush後のconnection terminal化、fresh follow-upを検証する |
| `post-informational-incomplete-grpc-message` | 103の後、`grpc-message` を含むEND_STREAMなし / END_HEADERSなしfinal initial HEADERSを送り、CONTINUATIONを送らない。clientの即時CANCEL、flush後のconnection terminal化、fresh follow-upを検証する |
| `post-informational-incomplete-status-details` | 103の後、`grpc-status-details-bin` を含むEND_STREAMなし / END_HEADERSなしfinal initial HEADERSを送り、CONTINUATIONを送らない。clientの即時CANCEL、flush後のconnection terminal化、fresh follow-upを検証する |
| `incomplete-informational-end-stream` | `:status: 103` を持つEND_STREAM / END_HEADERSなしHEADERSを送り、CONTINUATIONを送らない。`INTERNAL`を維持したterminal quarantine、peer受信RST、fresh follow-upを検証する |
| `incomplete-trailer-without-end-stream` | valid final initial responseの後、END_STREAMなし / END_HEADERSなしtrailing HEADERSを送り、CONTINUATIONを送らない。`INTERNAL`を維持したterminal quarantine、peer受信RST、fresh follow-upを検証する |
| `informational-incomplete-entry-budget` | `:status: 103` と多数のregular fieldを持つEND_STREAMなし / END_HEADERSなしHEADERSでwire entry budgetを超え、CONTINUATIONを送らない。`RESOURCE_EXHAUSTED` + `RST_STREAM(CANCEL)`、connection terminal化、fresh follow-upを検証する |
| `informational-incomplete-invalid-entry-budget` | `:status: 103` とNUL-bearing invalid regular field 129個を持つEND_STREAMなし / END_HEADERSなしHEADERSで、invalid callback 128回目にentry budgetを超え、129個目を処理しない。`RESOURCE_EXHAUSTED` + exact `RST_STREAM(CANCEL)`、connection terminal化、fresh follow-up、diagnostic count 128とdefault-blocking batchのterminal action後に実fdが `O_NONBLOCK` であることを検証する |
| `multiplex-hold-sibling` | 同一connection上の先行server-streaming streamへinitial HEADERSと1 messageを送り、END_STREAMなしでactiveのまま保持する。後続のincomplete block probeに対するsibling lifecycleのbarrierとして使う |
| `multiplex-incomplete-entry-budget` | 保持中siblingと同一connection上の別streamへ、32MiBのsibling / connection WINDOW_UPDATEを送り、50ms分割後に`informational-incomplete-entry-budget`相当のHEADERSを送る。target RST前の合法なsibling DATA受信をsetup discriminatorとして必須にし、peer側はtarget RST受信後のDATAを違反として記録する。client trace側もtarget RST後のsibling DATA `frame_out`不在とtarget RSTから`rpc.end`まで500ms未満を確認し、合法なpre-quarantine backpressureをterminal graceの計測へ混ぜない。connection terminal化後のsibling `UNAVAILABLE`とfresh follow-upも検証する |
| `require-prior-incomplete-status-cancel` | authorityをkeyにconnectionを跨いで保存した直前のincomplete-block probe結果を最大3秒待つ。peerが対象streamの期待RSTを受信し、かつそのtarget RST受信後にsibling DATAを受信していない場合だけgRPC OKを返し、判定後はmarkerをconsumeする。fresh follow-upはterminal connectionとは別connectionでこのcontrolを使う |
| `require-prior-status-probe` | 同じconnectionの直前のsilent status-field probeについてclientからexact `RST_STREAM(CANCEL)` を受信済みの場合だけgRPC OK。PHPTは3種すべてを2秒のguard内に `UNKNOWN` + `invalid grpc-status trailer` で終了させ、same-connection follow-up成功を確認する。diagnosticは `failed=1`、`timed_out=false`、`stream_error_code=8` を確認する |
| `invalid-status-metadata` | `x-before`、invalid `grpc-status: 17`、`x-after` をこの順に持つTrailers-Only response |
| `post-informational-terminal-message-only-metadata` | 103の後、`x-before` / `grpc-message` / `x-after` を持つEND_STREAM final block（`grpc-status`なし）。block全体のtrailing ownershipとUNKNOWN/detailsを検証する |
| `post-informational-terminal-status-details-only-metadata` | 103の後、`x-before` / `grpc-status-details-bin` / `x-after` を持つEND_STREAM final block（`grpc-status`なし）。block全体とdetails-binのtrailing ownership、UNKNOWNを検証する |
| `post-informational-nonterminal-status` | 103、`grpc-status: 0`を同居させたEND_STREAMなしfinal initial HEADERS、`DATA(END_STREAM)`。bench diagnosticがsuccess countへ含めないことを検証する |
| `post-informational-nonterminal-status-details` | 103、`grpc-status-details-bin`を同居させたEND_STREAMなしfinal initial HEADERS、DATA、valid `grpc-status: 0` trailers。details field単独でもterminal status gateを迂回できないことを検証する |
| `valid-informational-iteration-reset` | 60個のstatus-only 103と、status/content-type/message/encoding/custom metadataでpolluteした103の後にvalid gRPC OKを返す。1 iterationは69 wire fieldsで上限内、2 iterations累積では128を超えるため、benchのsemantic stateとwire entry counterのiteration resetを検証する |
| `valid-informational-byte-iteration-reset` | 8192-byteの`x-info`を4個の103 blockへ分けた後にvalid gRPC OKを返す。1 iterationは約32.8KiB / 11 wire fieldsで64KiB / 128-entry上限内だが、2 iterationsのbyteだけを累積すると64KiBを超えるため、entry counterとは独立にbenchのwire byte counter resetを検証する |
| `foreign-pushed-stream-protocol-rst` | main request streamにPUSH_PROMISEを送り、promised streamの `103 HEADERS(END_STREAM)` に対するclientの `RST_STREAM(PROTOCOL_ERROR)` を受信してからmain streamへgRPC OKを返す。foreign stream eventをcurrent RPCへ誤帰属しないことを検証する |

## Fixture ownership

- `poc/test-server/main.go` がfixture behaviorの一次ソース。
- `tests/phpt/helpers.inc` はPHPTからtest-server到達性を確認する。
- `tools/test/check-phpt.sh` と `tools/test/check-c-coverage.sh` は `50051`-`50071` を必須preflightとして扱う。
- fixture behaviorを変える場合は、該当PHPT/PHPUnitだけでなく `docs/verification/verification-matrix.md` も更新する。
