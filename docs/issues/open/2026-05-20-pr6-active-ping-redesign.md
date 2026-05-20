---
Status: Open
Owner: Codex
Created: 2026-05-20
Related:
  - https://github.com/dkkoma/php-grpc-lite/pull/6
  - https://github.com/dkkoma/php-grpc-lite/issues/5
  - docs/reviews/issues/2026-05-20-pr6-http2-specialist-review.md
  - docs/reviews/issues/2026-05-20-pr6-grpc-specialist-review.md
  - docs/reviews/issues/2026-05-20-pr6-spanner-specialist-review.md
---

# PR #6 active PING / BDP probe 検証方針の再整理

## 目的

PR #6 のBDP PING / SETTINGS関連変更を、merge前提の機能追加ではなく、GitHub issue #5 のSpanner遅延差を説明するための検証作業として再整理する。

このブランチの当面のゴールはPRをmainへmergeすることではない。目的は、観測済みの `active PING` 改善が何を意味し、何を意味しないかを切り分け、次に実装へ進めるだけの妥当な仮説と検証条件を作ることである。

特に、HTTP/2 / gRPC / Spanner専門レビューで指摘された以下を解消する。

- `grpc_lite.active_bdp_probe=1` default-on は検証結果に対して強すぎる。
- DATA受信後のclient-origin PINGは、同一RPCのfirst responseを直接速くするものではなく、後続RPCに効き得るconnection-level state変化として扱う必要がある。
- `active_bdp_probe_min_interval_ms=0` は長いserver streamingでRTT-rate PINGになり得る。
- ext-grpc 1.58.0 が `grpc.http2.bdp_probe=0` でも速い結果があり、BDPを根因とする推論は弱い。
- SETTINGS update実験は静的targetの負の実験であり、gRPC CoreのBDP estimator / flow-control parityではない。

## 背景

real Cloud Spanner `ExecuteStreamingSql SELECT 1` + service-account JSON 条件では、grpc-liteにresponse DATA callback起点のclient-origin PINGを入れるとp50/meanが大きく改善した。

一方、低RTT synthetic / emulator / high-level real-client benchでは、default-on active PINGにp99やp50の悪化が見えた。したがって、現時点の証拠は「Spanner issue #5で有効な可能性があるopt-in実験」を支持するが、「gRPC client全体のdefault behavior」を支持しない。

## gRPC Core v1.58.0 BDP re-arm model

ローカル `_research/grpc` の v1.58.0 系C-core確認結果:

- `BdpEstimator` の `inter_ping_delay_` 初期値は100ms。
- connection開始時は `bdp_ping_blocked=true` になり、最初のDATA frame受信で `schedule_bdp_ping_locked()` が呼ばれる。
- PINGがwireに乗った後 `StartPing()`、ACK完了で `CompletePing()` が呼ばれる。
- `CompletePing()` はincoming bytes / RTT / bandwidthからestimateを更新し、次回PING時刻を返す。
- estimateが伸びた場合は `inter_ping_delay_ /= 2`、安定時は100〜200msずつ増やし、最大10s方向へ伸ばす。
- 次回timer到達時にaccumulatorが0なら再度blockedに戻り、DATA受信までPINGしない。

したがって、grpc-lite PR #6の `min_interval_ms=0` はCoreのBDP estimatorとは異なる。Coreに寄せるなら、少なくともopt-in時の既定intervalは100ms相当が自然。ただし、issue #5の実測では1000msでは効果が薄く、0msが最も効いたため、0msは診断用overrideとして残す。

## スコープ

- `grpc_lite.active_bdp_probe` をdefault offに戻す。
- `grpc_lite.active_bdp_probe_min_interval_ms` の既定値をCore初期値に近い100msにする。
- positive PHPTは明示的に `active_bdp_probe=1` / `min_interval_ms=0` を指定する。
- disabled/no-op PHPTを追加し、default off時にclient-origin PINGとACK-triggered SETTINGS updateが出ないことを固定する。
- docsではBDP root causeと断定せず、active PINGがSpanner repeated streamの後続RPC connection stateに効く可能性として扱う。
- ext-grpc側のADC / service-account JSON / `grpc.http2.bdp_probe=0` matrixを再確認し、BDP以外のC-core差分を次の仮説として残す。
- PR #6レビューで残ったvalidation-quality指摘を、検証タスクとして分解する。

## 非スコープ

- gRPC Core BDP estimatorの完全実装。
- keepalive PINGの実装。
- Google Spanner専用auto enable。
- PR #6をそのままmergeすること。
- `active PING` をproduction defaultへ昇格すること。

## 計画

- [x] レビュー結果をPRブランチへコミットする。
- [x] gRPC Core v1.58.0 のBDP re-arm / intervalを確認する。
- [x] default off + 100ms interval defaultへ変更する。
- [x] disabled/no-op PHPTを追加する。
- [ ] docsの推論表現を修正する。
- [x] PHPT / C unit / static analysisを通す。
- [x] real Cloud Spanner `SELECT 1` の必要最小matrixを再計測または既存結果に基づいて整理する。
- [x] ext-grpc / grpc-lite の SA JSON / ADC credential matrixを再計測する。
- [ ] PR #6の説明をmerge候補ではなく検証ブランチとして更新する。

## 残レビュー指摘と検証タスク

### 調査優先順位 2026-05-20 update

次の作業は、PR merge可否ではなく、今後の調査・修正指針に使える事実を最小限のデータで残すことを優先する。

優先して見るもの:

1. official ext-grpcの `grpc.http2.bdp_probe=off/on` で何が変わるか。
   - tcpdump / strace / official `GRPC_TRACE=http` を対応付ける。
   - その差をgRPC Core v1.58.0のコードで確認する。
   - 目的は「BDP probeがofficial fast pathの必要条件か」ではなく、「off/onで実際にwire/control lifecycleがどう変わるか」を確定すること。
2. grpc-liteのactive PING因果。
   - `0ms` と `10ms` は同レンジなので、今後は `10ms` を代表値にする。
   - `100ms` はSpanner `SELECT 1` では改善しないため、mitigation候補から外す。
   - tcpdump / traceで、PINGがどのRPCに効くのか、同一RPCではなくpost-ACK後続RPCに効くのかを確認する。
3. wire/control lifecycle比較。
   - 比較対象は `official SA JSON`、`grpc-lite SA JSON`、`grpc-lite ADC`。
   - `official SA JSON fast path` 自体はGoogle内部差分である可能性が高く、現時点では深追いしない。
   - ただしclientから観測できるrequest/response/control frame timing差は、今後のtransport修正候補に使えるため記録する。

後回しにするもの:

- active PINGのpeer enforcement / keepalive安全性。
- SETTINGS updateの追加検証。
- Google内部のofficial SA JSON fast pathの根因追跡。

### 1. active PINGの因果モデル

レビュー指摘:

- DATA受信後のclient-origin PINGは、同一RPCのfirst responseを直接速くできない。
- 効果があるなら、同一HTTP/2 connection上の後続RPCへ効くconnection-level state変化として扱う必要がある。

検証タスク:

- `pre-probe phase` と `post-ACK phase` を分けて計測する。
- active PING ACKを確認した後の次RPC群だけを別集計する。
- 同一connection reuseを前提にし、connection generationが変わったrunを除外または別扱いにする。

### 2. ext-grpc / Core causality

レビュー指摘:

- official ext-grpc 1.58は `grpc.http2.bdp_probe=0` でも速い。
- したがって、gRPC Core BDP probeがext-grpc優位の根因とは言えない。

検証タスク:

- official ext-grpc 1.58で `SA JSON / ADC` x `bdp_probe=default / 0` を同一run条件で再測定する。
- 可能ならofficial traceでBDP PING有無、server PING、SETTINGS/WINDOW_UPDATE、first response timingをredact済み要約として比較する。
- 結論は「active PINGはgrpc-liteで効く観測事実」と「ext-grpcが速い根因」を分離する。

### 3. SETTINGS update実験の扱い

レビュー指摘:

- 現在のSETTINGS updateは静的target送信であり、CoreのBDP estimator / flow-control parityではない。
- SELECT 1はresponseが小さいため、`SETTINGS_MAX_FRAME_SIZE` の検証対象として弱い。
- SETTINGSがpeerにACKされ、測定対象stream開始前に適用済みか確認していない。

検証タスク:

- SETTINGS updateは「静的SETTINGS再送の負の実験」として扱う。
- SETTINGS ACK時刻と測定stream開始時刻をtraceで相関できるようにする。
- `MAX_FRAME_SIZE` を評価するなら、SELECT 1ではなくmedium/large responseで別に測る。

### 4. tail / varianceの扱い

レビュー指摘:

- remote Cloud Spannerのp99は500 sample程度では弱い。
- p50/p90の方向性とp99/tail安全性は分けて扱うべき。

検証タスク:

- 重要な比較は順序入れ替えつき複数roundで計測する。
- p99は暫定値として扱い、判断にはp50/p90とrun間再現性を優先する。
- tail safetyを判断する場合はsample数・raw distribution・run orderを記録する。

### 5. peer ping enforcement / keepalive risk

レビュー指摘:

- opt-inでも、短いintervalはclient-origin PINGを増やす。
- 一般gRPC peerやproxyでtoo-many-pings / GOAWAY / ENHANCE_YOUR_CALMがあり得る。

検証タスク:

- production default offを維持する。
- opt-in documentationに「PING頻度が増えるため、workload/peerで検証してから使う」と明記する。
- 将来defaultやauto enableを検討する場合は、peer ping enforcement fixtureを追加する。

## 判断ログ

- 2026-05-20: 3ロールレビューにより、default-on active PINGはBlockerと判断した。
- 2026-05-20: Core BDP estimatorの初期re-arm delayは100msであり、PR #6の0ms defaultはCore parityではないと判断した。
- 2026-05-20: ただしSpanner issue #5では0msが最も効いたため、0msを診断overrideとして残す。
- 2026-05-20: `active_bdp_probe` をdefault offに戻し、`active_bdp_probe_min_interval_ms` の既定値を100msに変更した。positive trace testは明示opt-inにし、disabled/no-op PHPTを追加した。
- 2026-05-20: real Cloud Spanner `SELECT 1` で10ms/100ms intervalを実測し、さらに1000 iterations x 3 roundで再計測した。0msと10msはいずれも明確に改善し、今回の反復平均では10msがわずかに良い。ただし0ms/10msの優劣断定にはまだ追加runが必要。100msはoffと同レンジで、issue #5のmitigationとしては効果が弱い。
- 2026-05-20: official ext-grpcとgrpc-liteのSA JSON / ADC matrixを再計測した。officialはSA JSONだけが速く、ADCは遅い。grpc-liteはSA JSON/ADCどちらでもactive PINGにより改善した。したがって、active PING効果はSA JSON専用ではなく、grpc-lite transport/control state一般に効く可能性がある。一方、official SA JSONがさらに速い理由は未説明のまま。
- 2026-05-20: tcpdump / grpc-lite traceでwire/control lifecycleを確認した。official ext-grpcのBDP on/offは、SELECT 1の測定RPC窓ではTCP packet形状・first response timingに大きな差が見えない。grpc-lite active PINGは、現行PR実装では後続RPCを速くするconnection-level state変化としてまだ説明できない。特にRPC間に50ms idleがあるmarker runでは、active PING後のpending TLS dataをpersistent preflightがrejectし、connection reuseを壊して各RPCで新規TLS connectionを作る副作用が見えた。したがって、現状のactive PING実装はmitigation候補として扱わず、以降は原因調査用の観測結果として扱う。

## 検証

- `./tools/test/check-phpt.sh`: PASS, 17/17
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS

### real Cloud Spanner `SELECT 1` interval matrix

条件:

- target: `vast-falcon-165704 / bench / laravel-bench-db`
- credentials: service-account JSON
- iterations: 500
- official image: `spanner-repro:official-select1`
- lite source-built: current PR branch `ext/grpc/modules/grpc.so` loaded in Docker compose `dev`
- logs:
  - `var/issue5-bdp-matrix/select1-interval-20260520-063609.log`
  - `var/issue5-bdp-matrix/select1-interval-source-20260520-063803.log`
  - `var/issue5-bdp-matrix/select1-interval-10ms-source-20260520-064158.log`
  - `var/issue5-bdp-matrix/select1-interval-repeat-source-20260520-064540.log`

| variant | mean | p50 | p90 | p99 | 判断 |
|---|---:|---:|---:|---:|---|
| official ext-grpc 1.58 default | 10.691ms | 10.398ms | 12.162ms | 15.728ms | 比較基準 |
| official ext-grpc 1.58 `grpc.http2.bdp_probe=0` | 11.372ms | 11.244ms | 12.827ms | 15.812ms | BDP offでも速い |
| lite source active off | 18.954ms | 18.576ms | 20.128ms | 23.715ms | baseline |
| lite source active on, `0ms` | 15.506ms | 15.221ms | 17.316ms | 24.072ms | 明確に改善 |
| lite source active on, `10ms` | 14.716ms | 14.574ms | 16.075ms | 19.511ms | 0msに近い改善 |
| lite source active on, `100ms` | 20.178ms | 20.450ms | 22.547ms | 27.274ms | 改善なし |

同じsource-built `grpc.so` で、順序を入れ替えながら1000 iterations x 3 roundを追加計測した。

| variant | rounds | mean avg | p50 avg | p90 avg | p99 avg | 判断 |
|---|---:|---:|---:|---:|---:|---|
| lite source active off | 3 | 19.660ms | 19.279ms | 20.730ms | 25.015ms | baseline |
| lite source active on, `0ms` | 3 | 14.696ms | 14.120ms | 15.529ms | 21.641ms | 明確に改善 |
| lite source active on, `10ms` | 3 | 14.537ms | 14.263ms | 15.769ms | 20.339ms | 明確に改善、0msと同レンジ |
| lite source active on, `100ms` | 3 | 18.832ms | 19.000ms | 20.566ms | 25.293ms | offと同レンジ |

判断:

- `100ms` はgRPC Core BDP estimatorの初期inter-ping delayに近いが、grpc-liteの単純active PING実装ではSpanner `SELECT 1` の改善を示さなかった。
- issue #5で観測された改善を再現するには、現状の単純active PINGでは `0ms` または `10ms` 程度の短いre-arm intervalが必要に見える。今回の反復では10msがわずかに良いが、差は小さいため優劣は未確定として扱う。
- official ext-grpcは `grpc.http2.bdp_probe=0` でもp50約11.2msで速いため、BDP probe単独を根因とする推論はさらに弱い。
- よって、`active_bdp_probe_min_interval_ms=100` は「Core初期値に近い安全寄りのopt-in既定値」としては妥当でも、「Spanner issue #5 mitigationの既定値」としては弱い。PR #6で採用するなら、active PINGは明確に診断用とし、実際のmitigation検証では `min_interval_ms=0` または `10` を明示する。

### real Cloud Spanner `SELECT 1` credential / active PING matrix

条件:

- target: `vast-falcon-165704 / bench / laravel-bench-db`
- iterations: 1000
- official image: `spanner-repro:official-select1`
- lite source-built: current PR branch `ext/grpc/modules/grpc.so` loaded in Docker compose `dev`
- logs:
  - `var/issue5-bdp-matrix/select1-credential-bdp-active-20260520-131537.log`
  - `var/issue5-bdp-matrix/select1-lite-credential-active-20260520-131711.log`

| impl | credential | option | mean | p50 | p90 | p99 | 判断 |
|---|---|---|---:|---:|---:|---:|---|
| official ext-grpc 1.58 | SA JSON | default | 11.723ms | 11.552ms | 13.677ms | 16.984ms | official SA fast path |
| official ext-grpc 1.58 | SA JSON | `grpc.http2.bdp_probe=0` | 12.664ms | 12.231ms | 14.732ms | 21.776ms | BDP offでも速い |
| official ext-grpc 1.58 | ADC | default | 21.160ms | 20.826ms | 23.571ms | 27.934ms | ADCではlite offと同レンジ |
| official ext-grpc 1.58 | ADC | `grpc.http2.bdp_probe=0` | 22.157ms | 21.297ms | 24.239ms | 31.725ms | BDP offでやや悪化 |
| grpc-lite source | SA JSON | active off | 22.511ms | 21.087ms | 24.409ms | 35.348ms | baseline |
| grpc-lite source | SA JSON | active `0ms` | 17.325ms | 16.704ms | 19.903ms | 25.808ms | 改善 |
| grpc-lite source | SA JSON | active `10ms` | 17.061ms | 16.090ms | 19.100ms | 28.761ms | 改善 |
| grpc-lite source | ADC | active off | 21.966ms | 21.521ms | 24.479ms | 32.360ms | baseline |
| grpc-lite source | ADC | active `0ms` | 16.839ms | 16.424ms | 19.173ms | 25.526ms | 改善 |
| grpc-lite source | ADC | active `10ms` | 15.495ms | 15.192ms | 17.620ms | 23.233ms | 改善 |

判断:

- official ext-grpcはSA JSONで明確に速く、ADCでは約21ms台に落ちる。これは「officialが常に速い」ではなく、「official + SA JSON条件だけが速い」ことを再確認する。
- official ext-grpcは `grpc.http2.bdp_probe=0` でもSA JSONで速い。BDP probeはofficial SA fast pathの必要条件ではない。
- grpc-liteはSA JSON/ADCどちらでもactive PINGによりp50が約5ms改善する。したがってactive PING効果はSA JSON/JWT専用ではなく、grpc-lite transport/control stateの改善候補として見るべき。
- ただしactive PING後のgrpc-liteはSA JSONでもp50 16ms台で、official SA JSONのp50 11〜12ms台には届かない。active PINGは差分の一部を縮めるが、official SA JSON fast pathの根因説明ではない。
- 次の焦点は、official SA JSONだけが速い理由と、grpc-lite active PINGがcredentialに関係なく効く理由を分けて調べること。

### tcpdump / trace lifecycle correlation

条件:

- target: `vast-falcon-165704 / bench / laravel-bench-db`
- RPC: warmed `ExecuteStreamingSql SELECT 1`
- iterations: 30
- tcpdump: container内 `tcpdump -i any -nn -tt -s 160 'tcp port 443'`
- marker: RPC直前/直後にepoch microsecondsを出力
- grpc-lite trace: `GRPC_LITE_TRACE_FILE` + `GRPC_LITE_TRACE_WIRE_BYTES=1`
- raw pcap / traceは `var/issue5-bdp-matrix/tcpdump/` に保存。credentialやproject情報を含み得るためdocsには要約のみ残す。

| variant | n | elapsed mean | elapsed p50 | first outbound payload p50 | first inbound payload p50 | TCP SYN in measured RPC | trace connection prefaces | trace client non-ACK PING | trace unique stream ids |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `official-sa-bdp-on` | 30 | 14.236ms | 14.016ms | 1.617ms | 12.474ms | 0 | - | - | - |
| `official-sa-bdp-off` | 30 | 14.870ms | 14.390ms | 1.697ms | 12.519ms | 0 | - | - | - |
| `lite-sa-off` | 30 | 16.608ms | 16.282ms | 2.714ms | 13.145ms | 0 | 1 | 0 | 31 |
| `lite-sa-active10-notrace` | 30 | 671.026ms | 130.452ms | 23.794ms | 35.195ms | 30 | - | - | - |
| `lite-sa-active10` | 30 | 269.993ms | 142.121ms | 28.349ms | 38.450ms | 30 | 32 | 32 | 1 |
| `lite-adc-off` | 30 | 18.766ms | 17.387ms | 2.629ms | 14.006ms | 0 | 1 | 0 | 31 |
| `lite-adc-active10` | 30 | 125.735ms | 132.173ms | 27.182ms | 37.287ms | 29 | 31 | 31 | 1 |

観測:

- official ext-grpc SA JSONのBDP on/offは、測定RPC窓でTCP SYNが出ておらず、同一connection reuse前提で揃っている。
- official ext-grpc SA JSONのBDP on/offは、SELECT 1の測定RPC窓ではfirst outbound / first inbound timingがほぼ同レンジ。少なくともtcpdump上は、BDP onだけがRPCごとに明確な追加packet lifecycleを作っている形には見えない。
- grpc-lite active offはSA/ADCどちらも1 connection preface + stream id増加で、同一HTTP/2 connection上のstream reuseが確認できる。
- grpc-lite active 10msはSA/ADCどちらも測定RPC窓ごとにTCP SYNが出ている。grpc-lite traceでもconnection prefaceがRPC回数分出ており、stream idは常に1に戻る。
- grpc-lite active 10msのconnection churnはtrace有効化だけの副作用ではない。`lite-sa-active10-notrace` でも30/30 RPCでTCP SYNが出た。
- ただし、このmarker scriptは各RPC後に `usleep(50000)` を入れている。tcpdumpなし・traceなし・sleepなしの通常loopでは、active 10msは再びactive offより速く出た。したがって「active 10msは常に新規connectionになる」ではなく、「active PING後、次RPC開始前にTLS application dataが到着していると、現在のpreflight policyがconnectionを捨てる」と整理する。
- 以前のinterval matrixではactive `0ms` / `10ms` にlatency改善が見えたが、今回のwire/control lifecycleではRPC間idleがあるactive `10ms` がconnection reuseを壊している。条件差を説明できるまで、過去の改善値はproduction mitigation根拠として使わない。

追加切り分け:

| variant | tcpdump | trace | inter-RPC sleep | active PING | elapsed p50 | connection prefaces | preflight result |
|---|---|---|---:|---|---:|---:|---|
| `lite-sa-off-rejecttrace` | yes | yes | 50ms | off | 15.528ms | 1 | accept `no pending TLS data` x11 |
| `lite-sa-active10-rejecttrace` | yes | yes | 50ms | 10ms | 132.803ms | 11 | reject `persistent TLS connection has pending control data before reuse` x10 |
| `lite-sa-active10-marker-notcpdump-rejecttrace` | no | yes | 50ms | 10ms | 78.679ms | 11 | reject `persistent TLS connection has pending control data before reuse` x10 |
| `select1-no-tcpdump-active10-rerun` | no | no | none | off | p50 avg 21.177ms | - | - |
| `select1-no-tcpdump-active10-rerun` | no | no | none | 10ms | p50 avg 15.968ms | - | - |

この切り分けにより、tcpdumpは絶対値を悪化させるが、connection churnの主因ではない。RPC間に50ms idleがあるだけでtcpdumpなしでも同じpreflight rejectが出る。

コード確認:

- gRPC Core v1.58.0では、BDP probeが有効な場合、connection初期化時に `bdp_ping_blocked=true` となり、DATA frame受信時に `schedule_bdp_ping_locked()` が呼ばれる。PING完了後は `BdpEstimator::CompletePing()` と `flow_control.PeriodicUpdate()` により、次回PING時刻とflow-control update候補を更新する。
- grpc-lite PR #6のactive PINGは、DATA受信時に `maybe_submit_active_bdp_probe()` でclient-origin PINGをsubmitし、ACK受信時に必要ならSETTINGS updateをsubmitする単純実装である。
- grpc-liteのpersistent reuse前 preflightは、active streamがない状態でTLS readable dataが残っていると `persistent TLS connection has pending control data before reuse` としてconnectionをdrainingにする。追加した `persistent.preflight` traceにより、RPC間50ms idleのactive 10ms runではこのreject reasonが直接出ることを確認した。

判断:

- active PINGは、少なくとも現行PR実装では「同一connection上の後続RPCを安定して速くする」実装になっていない。RPC間idleがあるとconnection reuseを壊すため、production mitigation候補から外す。
- 10msは0msと同レンジという過去のlatency結果は残すが、現行wire/control lifecycle上は、10ms active PINGを採用候補として扱う根拠はない。
- official SA JSON fast pathはBDP on/offだけでは説明できない。tcpdump上も、BDP on/off差はSELECT 1のRPC latencyを支配する差としては見えていない。
- 次に必要な調査は、active PING safetyではなく、まずgrpc-liteのpersistent preflightがpending TLS dataをHTTP/2 frameとして安全にdrainできているか、またofficial C-coreがBDP/server PING/SETTINGS/WINDOW_UPDATEなどのconnection-level frameをRPC間でどう処理してconnection reuseを維持しているかの比較である。
- SETTINGS updateの検証は、active PINGでconnection reuseが維持できる状態になってからでないと評価が汚染されるため後回しにする。
