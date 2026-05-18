# GitHub issue #5 wire-level RTT報告の批判レビュー 2026-05-19

## Scope

- `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md`
- GitHub issue #5 latest comment: tcpdump + strace correlated `Spanner/Commit` latency comparison

## Reviewer Role

- Performance methodology reviewer
- HTTP/2 / gRPC transport lifecycle reviewer
- Investigation quality reviewer

## Review Prompt Summary

- GitHub issue #5の追加報告では、45秒のreal Spanner FPM single-concurrency計測で、`ext-grpc 1.58` と `php-grpc-lite 0.0.5` の差分が `outbound TCP packet -> inbound data packet` 区間に集中すると報告された。
- この報告が「client側ではなくwire RTT / server response delay」と結論づけられるか、過大主張・未分離要因・次に必要な測定を批判的に確認した。

## Issues

### REVIEW-20260519-001: `wire RTT` という表現が過大

- Severity: High
- Status: Closed
- Reviewer role: Performance methodology reviewer / Investigation quality reviewer
- Finding: `outbound TCP packet -> inbound data packet` は純粋なnetwork RTTではなく、Google frontend受信、TLS復号、HTTP/2/HPACK処理、metadata/auth処理、Spanner Commit処理、server queueing、response生成をすべて含む。
- Evidence: GitHub issue #5 latest comment reports `outbound packet -> inbound data packet` as `wire` and says the server decided to respond later.
- Expected model: client観測では「request packet送出後からresponse packet到着前までの区間」と表現し、network RTT、server-side処理、HTTP/2/gRPC request shape差を分離前に混同しない。
- Why it matters: 「wire RTT」と呼ぶと、clientが送ったHTTP/2/gRPC byte列の違いがserver応答時刻を変えている可能性を見落とす。
- Recommended fix: 公開・内部記録では `client-observed request-to-first-response-packet latency` と呼び、server-side / request-shape差を未解決候補として残す。
- Fix summary: issue本文で `wire RTT` と断定せず、`client-observed request-to-first-response-packet latency` 相当の観測区間として扱うよう更新した。
- Fix commit: pending
- Verification: `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md` に追加報告の解釈と過大主張の制限を追記。
- Notes: `poll wait` も原因ではなく、この区間を待っていた症状として扱う。

### REVIEW-20260519-002: first outbound packet基準ではrequest完了時刻を表せない

- Severity: High
- Status: Open
- Reviewer role: Performance methodology reviewer
- Finding: 最新報告は「first outbound TCP packet with payload」を相関点にしているが、serverがCommit requestを処理開始できるのはHEADERS/DATA/TLS recordが揃った後であり、request完了時刻とは限らない。
- Evidence: GitHub issue #5 latest comment states correlation uses send syscall ts, first outbound TCP packet with payload, first inbound TCP packet with payload, and recv syscall.
- Expected model: unary request送信完了を評価するには、対象Commit streamに属する最後のoutbound TLS record / TCP segment / HTTP/2 DATA frame送出時刻を使う。
- Why it matters: 複数TLS recordや複数TCP segmentに分かれる場合、first packet基準はrequest送信途中からresponse到着までを測ることになり、実装差を誤って含む。
- Recommended fix: `request last outbound byte -> first inbound response byte` と `first outbound byte -> last outbound byte` を別々に集計する。
- Fix summary: `first outbound packet` ではなく `request last outbound byte -> first inbound response byte` を次の測定単位としてissue本文に明記した。今回のCLI traceではsend syscall sizeとHTTP/2 frame shapeを確認したが、FPM 45秒runのpcap上でのlast-byte分布測定は未実施。
- Fix commit: pending
- Verification: 未完了。複数runのtcpdump/HTTP/2復号比較が必要。
- Notes: requestが本当に1 packet / 1 TLS recordなら差は小さいが、それは復号HTTP/2 traceまたはTLS record traceで確認する。

### REVIEW-20260519-003: outbound payload size差の内訳が未説明

- Severity: High
- Status: Open
- Reviewer role: HTTP/2 / gRPC transport lifecycle reviewer
- Finding: `php-grpc-lite ~942B` と `ext-grpc ~1571B` のoutbound TLS application payload差が大きい。これを説明しない限り、同一server処理を比較していると言い切れない。
- Evidence: GitHub issue #5 latest comment notes ext-grpc outbound TLS app payload is ~1571B and php-grpc-lite is ~942B per Commit.
- Expected model: 同一Spanner Commitなら、metadata set、metadata order、`grpc-timeout`、`:authority`、`:path`、`authorization`、`x-goog-*`、`user-agent`、DATA payload length、HTTP/2 control frames、HPACK dynamic table状態を比較する。
- Why it matters: metadata / HPACK / deadline / auth / routing header差は、Spanner frontendのrouting、auth、deadline budget、request processingを変え得る。payload size差は結果ではなく説明対象。
- Recommended fix: TLS key logまたは双方のHTTP/2 frame/header traceで、Commit requestのHEADERS/DATA/control framesを完全比較する。
- Fix summary: real Spanner `Spanner/Commit` のrequest metadata/body/frame/TLS write shape比較をissue本文へ追加した。protobuf payloadとgRPC DATAは一致、metadata shapeは不一致と確認した。
- Fix commit: pending
- Verification: `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md` の `wire shape完全比較 2026-05-19` に記録。ext-grpc側のHEADERS compressed length / HPACK table状態は未復元。
- Notes: extの方が大きく速いため、単純なrequest size因果ではない。byte列の意味の差を見る必要がある。

### REVIEW-20260519-004: client local処理を除外しすぎている

- Severity: Medium
- Status: Closed
- Reviewer role: Investigation quality reviewer
- Finding: `send syscall -> outbound packet` と `inbound packet -> recv` が小さいことから、client local要因をかなり弱められる。ただし、送信前に形成されたHTTP/2 dynamic table state、metadata、control frame、connection stateの差はoutbound packet時点で既に反映されている。
- Evidence: GitHub issue #5 latest comment says the gap is not explained by PHP layer, syscall set, receive loop, TCP_QUICKACK, or kernel scheduling.
- Expected model: 除外できるのは「send syscall後のkernel enqueue遅延」と「inbound packet到着後のuserland wakeup遅延」まで。request content / connection stateを作るclient側実装差は除外しない。
- Why it matters: client実装がserver応答時刻に影響する経路を早く捨てると、metadata/HPACK/deadline/control-frame差を見逃す。
- Recommended fix: 主張を「send syscall以降のlocal kernel/recv wakeupでは説明しにくい」に弱める。
- Fix summary: issue本文で、send syscall後・inbound packet到着後のlocal kernel/userland遅延は弱いが、request content / connection stateを作るclient実装差は除外しない、と整理した。
- Fix commit: pending
- Verification: `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md` の追加報告解釈に反映。
- Notes: `ppoll` vs `epoll_pwait` 自体は主因候補から落としてよい。

### REVIEW-20260519-005: 45秒単発runとサンプル数差の扱いが不足

- Severity: Medium
- Status: Open
- Reviewer role: Performance methodology reviewer
- Finding: ext-grpc n=252、php-grpc-lite n=172の45秒固定runだけでは、Spanner backend状態や時系列揺れを分離できない。
- Evidence: GitHub issue #5 latest comment reports aggregate over Commit RPCs, warmup excluded, ext n=252 and lite n=172.
- Expected model: A/Bを交互実行し、複数run、同数Commit抽出、median/p90/p95/IQR/CI、時系列順の揺れを出す。
- Why it matters: real Spannerはbackend状態・session状態・時間帯で揺れる。n差はthroughput差の結果として自然だが、分布比較にはrun間分散が必要。
- Recommended fix: 60秒以上のFPM single-concurrencyを複数回、variant順序を入れ替えて実行し、marker-onlyとstrace/tcpdump付きrunを分ける。
- Fix summary: 複数run・交互順序・分布指標が必要な未解決測定条件としてissue本文に残した。
- Fix commit: pending
- Verification: 未完了。FPM 45〜60秒runの再測定が必要。
- Notes: これは報告を否定するものではなく、公開結論の強度を決めるための条件。

### REVIEW-20260519-006: eager SEND lifecycleは今回の主因候補から降格する

- Severity: Medium
- Status: Closed
- Reviewer role: HTTP/2 / gRPC transport lifecycle reviewer
- Finding: requestが両実装ともsend syscall直後にcontainer network stackへ出ているなら、`UnaryCall::start()`で送るか`wait()`で送るかは今回のfirst inbound差の主因としては弱い。
- Evidence: GitHub issue #5 latest comment reports send syscall -> outbound TCP packet is ~0 for both implementations.
- Expected model: eager SENDはAPI lifecycle互換性 issue として別管理し、Commit RTT差の直接説明にはしない。
- Why it matters: 実装候補を原因と混同すると、調査順序を誤る。
- Recommended fix: `docs/issues/open/2026-05-19-unary-send-batch-eager-send.md` は互換性issueとして維持し、#5の次実験はwire byte列 / metadata / HPACK / deadline比較を優先する。
- Fix summary: eager SENDはCommit wait差の直接説明ではなく、API lifecycle互換性issueとして別管理する位置づけに更新した。
- Fix commit: pending
- Verification: `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md` の現時点の更新解釈に反映。
- Notes: eager SENDの性能効果はPoCで別途before/afterを測る。

### REVIEW-20260519-007: tcpdump観測点の表現を正確にする

- Severity: Low
- Status: Closed
- Reviewer role: Investigation quality reviewer
- Finding: FPM container内tcpdumpなら「NICを出た」ではなく、container network stack / veth上のpacket観測である可能性が高い。
- Evidence: GitHub issue #5 latest comment uses "leaves the NIC".
- Expected model: 観測点を `container network stackで観測されたoutbound TCP payload packet` と明記する。
- Why it matters: 物理NIC、host network stack、container vethでは意味が違う。
- Recommended fix: local docsと今後の返信では観測点の表現を限定する。
- Fix summary: docs上では物理NICや純network RTTと断定せず、client側で観測されたTCP payload packet区間として扱うようにした。
- Fix commit: pending
- Verification: `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md` の追加報告解釈に反映。
- Notes: 結論の大筋には影響しないが、調査精度として必要。

## Required Next Measurements

- 同一Commit RPCについて、TLS key logまたはHTTP/2 frame traceで復号相当の比較を行う。
- `Commit request first outbound byte`、`Commit request last outbound byte`、`first inbound response byte`、`response complete`、`WAIT_END` を分ける。
- Commit requestのHEADERS一覧、値、順序、重複、`grpc-timeout`、`:authority`、`:path`、`authorization`、`x-goog-*`、`user-agent`、DATA payload lengthをext-grpc / php-grpc-liteで比較する。
- HPACK dynamic table / indexed vs literal / header table size / SETTINGS差を記録する。
- request直前直後のPING / SETTINGS / WINDOW_UPDATE / ACKが同じpacketまたは近接packetに混ざるか確認する。
- 同一connection warm state、stream id進行、Spanner session warmup、transaction shapeを揃える。
- real Spanner runは複数回・交互順序・分布指標で集計する。

## Review Result

- Blocker: none
- High: 2 open / 1 closed
- Medium: 1 open / 2 closed
- Low: 1 closed
- Design Decision: none
