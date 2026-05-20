# BDP Flow-Control SETTINGS Design Review 2026-05-19

## Scope

- `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`
- `ext/grpc/internal.h`
- `ext/grpc/main.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/029-trace-file.phpt`
- `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md`

## Reviewer Role

- `gRPC/HTTP2 transport domain expert`

## Review Prompt Summary

- BDP probe + SETTINGS update proposalが、HTTP/2 connection-level flow-control、SETTINGS semantics、PING lifecycle、stream/call/connection boundaries、production safety、testabilityを正しくモデル化しているかを実装前にレビューした。
- 2026-05-19 re-review: fixes後のcurrent repository stateで、既存指摘の修正状況、PING ACK lifecycle、SETTINGS stream_id=0、target/current state、duplicate suppression、WINDOW_UPDATE分離、dead/draining no-op、INI matrix、test coverage、明白なC実装問題を確認した。

## Issues

### REVIEW-20260519-001: SETTINGS_INITIAL_WINDOW_SIZEとconnection receive windowの責務が未分離

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `設計候補はACK後にSETTINGS_INITIAL_WINDOW_SIZE / SETTINGS_MAX_FRAME_SIZEだけを送るとしているが、HTTP/2 flow-control targetのうちstream receive windowとconnection receive windowをどう扱うかが明示されていない。現行transportは接続確立時にSETTINGS_INITIAL_WINDOW_SIZEとconnection-level WINDOW_UPDATEを別々に送っているため、BDP由来のwindow targetでも同じ分離が必要。`
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:44`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:49`, `ext/grpc/transport.c:1557`, `ext/grpc/transport.c:1567`, `docs/protocol-model-review-guide.md:87`
- Expected model: `SETTINGS_INITIAL_WINDOW_SIZEはpeerが各streamで送信できるstream-level receive windowであり、connection receive windowはstream windowとは独立したconnection-level WINDOW_UPDATEで管理する。BDP flow-control targetを導入するなら、stream window target、connection window target、max frame size targetを別stateとして持ち、更新する/しないを明示する。`
- Why it matters: `stream windowだけを更新してconnection windowを更新しない設計は、大きなresponseや複数streamでconnection-level windowが上限になり、BDP設定の意味を誤って検証する可能性がある。一方でconnection WINDOW_UPDATEまで自動化するなら副作用が大きいため、明示的な非スコープまたは別issue化が必要。`
- Recommended fix: `issueの設計候補に、BDP settings実験はstream receive windowだけを変更しconnection receive windowは固定8MiBに据え置くのか、またはconnection WINDOW_UPDATEも別の上限/単調増加ルールで連動させるのかを追記する。後者なら`bdp_target_stream_window_size`、`bdp_target_connection_window_size`、`bdp_target_max_frame_size`を別々のconnection stateとして設計し、WINDOW_UPDATE発行条件と上限を記録する。`
- Fix summary: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md` に、通常のconnection / stream `WINDOW_UPDATE` は既存nghttp2 flow-controlと初期8MiB connection window拡張に任せ、このissueではBDP ACK後のSETTINGS updateだけを扱うことを明記した。connection-level receive windowの動的更新は別issueに分離した。
- Fix commit: `pending`
- Verification: `manual design review`
- Notes: `現行の8MiB fixed windowを前提にSETTINGSだけ検証する判断自体は可能だが、その場合は「flow-control SETTINGS連動」ではなく「MAX_FRAME_SIZE/stream-window-only実験」として境界を明示する必要がある。`

### REVIEW-20260519-002: accumulator reset位置では短いRPCのBDP sampleが空になる

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `設計候補はDATA受信時にaccumulatorへ加算し、client-origin PING submit時にaccumulatorを0へ戻してACKまでの受信bytesを測るとしている。しかし現行active probeはresponse DATA callback内でDATA処理後にPINGをsubmitするため、PINGをtriggerしたDATA bytesをsampleから捨て、短いSELECT 1ではACKまでに追加DATAがなくBDP estimateが0または極小になりやすい。`
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:41`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:42`, `ext/grpc/transport.c:1878`, `ext/grpc/transport.c:1901`, `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md:824`, `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md:881`
- Expected model: `BDP estimatorは、どの区間のincoming bytesをRTT sampleに対応付けるかをconnection-level state machineとして定義する必要がある。probe trigger bytes、PING in-flight中のbytes、ACK後にestimateへ反映するbytesを混同しない。`
- Why it matters: `このissueの主対象であるreal Cloud Spanner SELECT 1は小さいserver streaming responseで、DATA後PINGの主効果が観測されている。reset-at-submitのままだとSETTINGS updateが発火しない、または意味のあるestimateにならず、「SETTINGS連動の寄与」を検証できない可能性が高い。`
- Recommended fix: `実装前にBDP sample lifecycleをissueへ明記する。少なくとも`bdp_bytes_since_last_probe`と`bdp_bytes_during_probe`を区別し、PING submit時にどちらをestimate対象にするか、短いRPCで0 sampleをどう扱うか、ACK後に0/小sampleではSETTINGSを送らない条件を定義する。SELECT 1で期待するSETTINGS更新の有無も検証項目に入れる。`
- Fix summary: 初期実装方針をBDP estimatorから静的target SETTINGS送信実験へ修正した。incoming bytes accumulator / RTT based estimateは非採用とし、実装から `active_bdp_probe_accumulator_bytes` と関連加算/reset処理を削除した。
- Fix commit: `pending`
- Verification: `manual diff review`
- Notes: `「gRPC Core完全再実装は非スコープ」でも、最小estimatorのsampling intervalはprotocol/domain modelとして必要。`

### REVIEW-20260519-003: SETTINGS updateの送信/ACK lifecycleと重複抑制が未定義

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `ACK一致時にnghttp2_submit_settings()でSETTINGSを送る方針はあるが、同一値の再送抑制、SETTINGS ACK待ち、submit失敗時のconnection error分類、GOAWAY/draining/dead時の扱いが設計されていない。`
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:43`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:44`, `ext/grpc/transport.c:383`, `ext/grpc/transport.c:395`, `ext/grpc/transport.c:2002`, `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md:923`, `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md:970`
- Expected model: `SETTINGSはHTTP/2 connection-level control stateであり、desired/current/sent values、outstanding ACK、送信失敗時のconnection failure、draining/dead connectionでは送らない、というlifecycleをconnection stateとして明示する。`
- Why it matters: `active_bdp_probe_min_interval_ms=0ではACK後に再armされるため、低RTT syntheticやemulatorでSETTINGSが頻発すると既に観測されているactive PINGのtail悪化をさらに増幅する。重複SETTINGSやACK未考慮の連続更新はproduction safetyとtrace解釈を壊す。`
- Recommended fix: `connection stateに`bdp_settings_last_sent_initial_window_size`、`bdp_settings_last_sent_max_frame_size`、必要なら`bdp_settings_ack_outstanding`を置く設計にし、targetが変わった場合だけ送る、同時outstandingは1つまで、nghttp2_submit_settings()失敗はconnection failureとして扱う、GOAWAY/draining/deadでは送らない、をissueに追記する。`
- Fix summary: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md` に、targetが現在値と異なる場合だけSETTINGSを送る、同一target再送を抑制する、draining/dead connectionでは送らない、ACK callback内ではqueueだけ行いflushは既存send boundaryに任せる、というlifecycleを追記した。実装でもcurrent target値をconnection stateに保持し、dead/drainingではno-opにした。
- Fix commit: `pending`
- Verification: `manual diff review`
- Notes: `nghttp2がprotocol ACK処理を担うとしても、php-grpc-lite側の「いつ新しいSETTINGSを発行するか」はdomain stateとして必要。`

### REVIEW-20260519-004: INI依存関係とproduction defaultの意味が曖昧

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `提案はactive_bdp_update_settingsをdefault offにし、既存active_bdp_probeが有効な場合だけ作用するとしているが、active_bdp_probe自体のdefaultと、update_settings=1 / active_bdp_probe=0の組み合わせがno-opなのか暗黙enableなのかが未定義。既存調査ではactive_bdp_probe default onは低RTT synthetic/emulatorで副作用が大きいと記録されている。`
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:38`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:40`, `ext/grpc/main.c:15`, `ext/grpc/main.c:49`, `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md:970`, `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md:974`
- Expected model: `INIはproduction control surfaceなので、probe PINGとSETTINGS updateの依存関係、default、runtime mutability、無効時の完全no-op範囲を明確にする。特にSpanner向けopt-inなら、既定で低RTT環境にcontrol-frame副作用を入れない。`
- Why it matters: `設定の組み合わせが曖昧だと、ユーザーがSETTINGS updateだけを有効化したつもりでprobeが動かない、またはprobe default onにより実験外の副作用を踏む。設計レビュー後に実装してもPHPTがdefault文字列だけを見てsemantic regressionを検出できない。`
- Recommended fix: `issueにINI matrixを追加する。例: active_bdp_probe=0ならactive_bdp_update_settings=1でも完全no-op、active_bdp_probe=1かつactive_bdp_update_settings=0ならPINGのみ、両方1ならACK一致後にSETTINGS candidateを評価する。併せてactive_bdp_probeのdefaultをこのissueの前提として0/1どちらにするか、または別issue依存として明記する。`
- Fix summary: INI matrixをissueに追加した。`active_bdp_probe=0` なら `active_bdp_update_settings=1` でも完全no-op、PINGのみ、PING+SETTINGS候補評価の各組み合わせを明記した。
- Fix commit: `pending`
- Verification: `manual design review`
- Notes: `PHP_INI_SYSTEMで十分か、既存probeと同じscopeにするかもPHPTへ固定する。`

### REVIEW-20260519-005: max frame size変更の検証モデルが不足している

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `SETTINGS_MAX_FRAME_SIZEを256KiB上限で送る案はあるが、これはclientがpeerに許可するinbound DATA frame最大値であり、request送信側のpackingやTLS record sizeを直接変えるものではない、というHTTP/2上の意味がissueに明記されていない。`
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:12`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:44`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:50`, `docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md:427`
- Expected model: `SETTINGS_MAX_FRAME_SIZEはpeerの送信するframe size上限を変えるconnection-level settingであり、client outbound DATA/HEADERS packing、HPACK、TLS record schedulingとは別の概念として扱う。`
- Why it matters: `request write -> first response差の調査ではwire分割やTLS recordも候補に上がっている。MAX_FRAME_SIZEの効果範囲を誤ると、測定結果をclient send schedulingの改善/悪化として誤解する。`
- Recommended fix: `issueにSETTINGS_MAX_FRAME_SIZEの作用範囲を追記し、検証ではserver response DATA frame lengthが変わったかをtraceで確認する。client request write size/TLS record差は別候補として切り分ける。`
- Fix summary: `SETTINGS_MAX_FRAME_SIZE` はpeer response DATA frame payload最大値に作用し、client request packing / HPACK / TLS record schedulingを直接変えるものではないことをissueへ追記した。
- Fix commit: `pending`
- Verification: `manual design review`
- Notes: `256KiBはHTTP/2範囲内だが、実際にpeerが大きいframeを使うかはserver実装依存。`

### REVIEW-20260519-006: protocol regression testがbenchmark中心で不足している

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `検証予定はPHPT/C unit/static analysis/benchを挙げているが、BDP SETTINGS update固有のHTTP/2 control-frame不変条件を固定するテストが明示されていない。`
- Evidence: `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:68`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:75`, `ext/grpc/tests/029-trace-file.phpt:146`, `ext/grpc/tests/029-trace-file.phpt:157`
- Expected model: `connection-level SETTINGS/PING/WINDOW_UPDATEは、performance benchmarkとは別に、frame type、stream_id=0、ACK payload matching、SETTINGS contents、重複抑制、connection WINDOW_UPDATEとの分離をprotocol regression testで固定する。`
- Why it matters: `benchだけでは、SETTINGSが送られていない、同じSETTINGSを毎ACK送っている、server-origin PING ACKで誤発火している、WINDOW_UPDATEまで偶発的に変わった、といったdomain regressionを検出しにくい。`
- Recommended fix: `029-trace-file.phptまたはraw lifecycle fixtureへ、opt-in時だけACK一致後のSETTINGS frameがstream_id=0で送られること、同一targetでは再送されないこと、server-origin PINGではsettings updateしないこと、connection WINDOW_UPDATE incrementが設計通り固定/更新されること、active_bdp_update_settings=0では完全no-opであることを追加する。`
- Fix summary: `ext/grpc/tests/029-trace-file.phpt` をopt-in SETTINGS update有効で実行し、ACK一致後のconnection-level SETTINGSに `MAX_FRAME_SIZE=262144` が含まれること、RPCに紐づかないこと、同一targetでは1回だけ送られることを確認するassertを追加した。default offは `002-ini.phpt` に追加した。
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh PASS, 16/16`
- Notes: `C unitではtarget calculation/bounds/clampingをI/Oなしで固定するとよい。`

### REVIEW-20260519-007: active BDP PINGがdraining connectionでもsubmitされ得る

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `SETTINGS update側はdead/draining connectionでno-opになったが、PING submit側のmaybe_submit_active_bdp_probe()はdead/drainingを見ていない。GOAWAY受信後に既存streamのDATAが続くHTTP/2 lifecycleでは、draining connectionに対して将来のflow-control tuning用PINGをqueueし得る。`
- Evidence: `ext/grpc/transport.c:351`, `ext/grpc/transport.c:357`, `ext/grpc/transport.c:380`, `ext/grpc/transport.c:891`, `ext/grpc/transport.c:2072`, `ext/grpc/transport.c:2083`
- Expected model: `active BDP probeはconnection reuse / future receive behaviorを調整するconnection-level control behaviorなので、deadまたはGOAWAYでdrainingになったconnectionでは新規probeを送らない。既存stream完了のために必要なACKやRSTと、性能probe用PINGは分離する。`
- Why it matters: `draining connectionへのprobe PINGは正しさを直ちに壊す可能性は低いが、GOAWAY後のcontrol-frame noiseとtail悪化要因になり、dead/draining no-opというSETTINGS側のproduction safety境界とずれる。`
- Recommended fix: `maybe_submit_active_bdp_probe() の早期return条件に connection->dead / connection->draining を追加し、必要ならGOAWAY後DATA fixtureまたはtrace testでdraining後にactive BDP PINGが増えないことを固定する。`
- Fix summary: `maybe_submit_active_bdp_probe()` の早期return条件に `connection->dead` / `connection->draining` を追加し、issueにもdraining/dead connectionではactive PINGとSETTINGSを送らないことを明記した。
- Fix commit: `pending`
- Verification: `manual diff review`
- Notes: `server-origin PING ACKなどprotocol-required ACKはnghttp2に任せる前提で、この指摘はclient-origin active probeだけを対象にする。`

### REVIEW-20260519-008: active_bdp_current_* はpeer-acknowledged currentではなくsent targetを表している

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `duplicate suppression用のconnection stateが active_bdp_current_initial_window_size / active_bdp_current_max_frame_size という名前だが、値はnghttp2_submit_settings()成功直後に更新され、peerからのSETTINGS ACKを待たない。実際の意味はpeer-acknowledged currentではなくlast submitted/sent targetである。`
- Evidence: `ext/grpc/internal.h:501`, `ext/grpc/internal.h:502`, `ext/grpc/transport.c:435`, `ext/grpc/transport.c:440`, `ext/grpc/transport.c:1623`, `ext/grpc/transport.c:1624`
- Expected model: `HTTP/2 SETTINGS lifecycleではdesired target、submitted/sent value、peer-ACKed current valueを区別する。ACKをdomain stateとして扱わない設計でも、名前はlast_sent / submitted targetであることを表すべき。`
- Why it matters: `現状の用途はduplicate suppressionなので動作上は大きな問題ではないが、currentという名前はpeerが設定を適用済みであるかのように読める。後続でSETTINGS ACKやadaptive estimatorを追加する際にstate transitionを誤実装しやすい。`
- Recommended fix: `フィールド名を active_bdp_last_sent_initial_window_size / active_bdp_last_sent_max_frame_size などに変更するか、SETTINGS ACKを追跡してcurrentをACK時に更新する。今回ACK trackingを非スコープにするならissueにも「duplicate suppressionはlast submitted targetで行う」と明記する。`
- Fix summary: フィールド名を `active_bdp_last_sent_initial_window_size` / `active_bdp_last_sent_max_frame_size` に変更し、issueにもduplicate suppression stateはpeer-ACKed currentではなくlast submitted targetであることを明記した。
- Fix commit: `pending`
- Verification: `manual diff review`
- Notes: `nghttp2自体のSETTINGS ACK処理に任せる判断は妥当だが、php-grpc-liteが保持するstate名は役割に合わせたい。`

### REVIEW-20260519-009: INITIAL_WINDOW_SIZE更新branchのprotocol regression coverageが不足

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `gRPC/HTTP2 transport domain expert`
- Finding: `029-trace-file.phptはopt-in SETTINGS updateでMAX_FRAME_SIZE=262144がstream_id=0のSETTINGSとして1回だけ送られることを確認しているが、INITIAL_WINDOW_SIZE update branchはdefault targetが接続初期値8MiBと同じため実際には送られず、テストで固定されていない。`
- Evidence: `ext/grpc/tests/029-trace-file.phpt:3`, `ext/grpc/tests/029-trace-file.phpt:4`, `ext/grpc/tests/029-trace-file.phpt:171`, `ext/grpc/tests/029-trace-file.phpt:179`, `ext/grpc/transport.c:413`, `ext/grpc/transport.c:1623`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md:62`
- Expected model: `BDP SETTINGS updateのprotocol regressionでは、MAX_FRAME_SIZEだけでなく、INITIAL_WINDOW_SIZEがtarget/current差分時にstream_id=0のSETTINGSとして送られること、同じtargetでは重複しないこと、connection WINDOW_UPDATEとは混同しないことを固定する。`
- Why it matters: `このissueの中心概念はflow-control SETTINGS連動であり、INITIAL_WINDOW_SIZEはstream receive windowそのもの。MAX_FRAME_SIZEだけのtrace testでは、stream window update pathの退行やWINDOW_UPDATEとの混同を検出できない。`
- Recommended fix: `029-trace-file.phptの別ケースまたはraw lifecycle fixtureで、初期 http2_stream_window_size を1MiBなどに下げ、active_bdp_settings_initial_window_size=8388608 をopt-inにして、ACK一致後にINITIAL_WINDOW_SIZE=8388608がconnection-level SETTINGSとして1回だけ出ることをassertする。併せて追加connection WINDOW_UPDATEが発生しない/設計通りであることを確認する。`
- Fix summary: `029-trace-file.phpt` に `grpc_lite.http2_stream_window_size=1048576` を指定し、初期SETTINGSは1MiB、active BDP SETTINGS updateでは `INITIAL_WINDOW_SIZE=8388608` と `MAX_FRAME_SIZE=262144` がconnection-level SETTINGSとして各1回だけ送られることをassertするようにした。
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh PASS, 16/16`
- Notes: `現在のdefault値では active_bdp_update_settings=1 かつ active_bdp_update_max_frame_size=0 は可視SETTINGSを送らないため、その意味もissueかテスト名で明確にするとよい。`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
