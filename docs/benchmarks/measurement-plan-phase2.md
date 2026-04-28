# Phase 2 (C 拡張化) スコープ決定のための計測計画

> **目的**: C 拡張化候補(persistent pool / per-frame streaming / per-byte decode / header parser 等)の優先順位を、**多軸データ**で決定できる状態にする。
>
> **現状の問題**: 現在のベンチは ほぼ wall-clock latency 単軸 + RTT 0ms の同一ホスト内のみ。これだと per-call overhead 系(persistent pool)に有利で、throughput/CPU/sustained-streaming 系の C 化候補(per-frame、per-byte、header parser)の真の価値が見えない。
>
> **読者**: 本ドキュメントは Phase 2 着手前に追加計測を実装する CODEX 向けの作業仕様。

---

## 1. 想定ワークロードと C 化候補のマッピング

C 化の優先度は **想定ユーザのワークロード形** に依存する。データを取った後、以下のマトリクスでスコープを決める前提:

| ワークロード傾向 | 効く C 化 | 計測すべき指標 |
|---|---|---|
| 低 RPS、cold-start 多い(オンデマンド処理) | persistent pool | cold vs persistent at RTT > 0 |
| 高 RPS、unary 中心(Cloud Run scale-out) | per-call overhead 全般 | sustained throughput, CPU per call |
| sustained streaming(Pubsub、log tailing) | per-frame hot path | streaming msg/sec, CPU per msg |
| payload-heavy(ML、画像) | per-byte decode | per-byte cost at varying payload size |
| SLO シビア、p99 重視 | 総合 | p50/p95/p99 under load |

**Phase 2 設計の前提**: いずれかひとつに絞らず、データを揃えて選ぶ。

---

## 2. 既存資産(再利用するもの)

これらは既に整っているので **再利用 / 拡張**:

- `bench/UnaryLatencyBench`, `UnaryDelayBench`, `UnaryPayloadBench`(Go test-server 経由)
- `bench/ServerStreamingBench`(count / payload / pacing 軸)
- `bench/compare.sh`(php-grpc-lite vs ext-grpc の両側計測)
- `Dockerfile.ext-grpc` + `bench-comparison/`(ext-grpc 比較環境)
- `poc/test-server`(`BenchUnary` / `BenchServerStream`、payload bytes / message count / server delay 制御可)

新規追加は **これらの拡張** + **新軸の追加** の形で。重複させない。

---

## 3. 追加すべきベンチマーク・ケース

### 3.1 RTT 軸(★ persistent pool 判断用)

**目的**: persistent pool の C 化が同一リージョン内距離(1-3 ms RTT)でどれだけ効くかを定量化。

**ケース**:

| シナリオ | RTT セット | 計測 mode |
|---|---|---|
| Persistent unary(Channel 再利用) | 0, 1, 3, 5 ms | wall-clock |
| **Cold unary**(call ごとに Channel を new) | 0, 1, 3, 5 ms | wall-clock |
| Streaming 1000 msg(payload=100 B) | 0, 1, 3 ms | wall-clock |

**期待される観察**:
- Persistent unary は RTT に比例(server time 支配)
- Cold unary は **RTT × (3-5)** で立ち上がる(TCP 1RTT + TLS 2-3RTT + HTTP/2 1RTT)
- Persistent vs Cold の Δ が μs → ms に拡大すれば、persistent pool C 化の正当化が定量化される

**注意**:
- 50ms / 100ms はクロスリージョンで、本決定には不要
- 既存 `compare.sh` の dev / dev-ext-grpc 両方で取る

### 3.2 Sustained Throughput 軸(★ per-call overhead 判断用)

**目的**: 単一 PHP プロセスで「CPU 飽和まで投入したとき何 calls/sec 出るか」を測る。これが per-call overhead 改善の効き目の上限。

**ケース**:

| シナリオ | 制約 | 計測指標 |
|---|---|---|
| Unary saturation(payload=100 B、no delay)| 1 process、I/O bound でない条件 | 最大 calls/sec |
| Unary saturation(payload=10 KB) | 同上 | 最大 calls/sec、bandwidth |
| Streaming saturation(1 stream で sustained msg/sec)| 1 process、msg=100 B | 最大 msg/sec |

**判断材料**:
- ext-grpc とのスループット比 → C 化で取り戻せる余地のサイズ
- 100 K calls/sec オーダーで PHP overhead が ms 単位の CPU を食ってるかどうか

### 3.3 CPU per call 軸(★ クラウド課金直結)

**目的**: 1 call あたりの user CPU + sys CPU を測る。Cloud Run 等の per-vCPU-second 課金で直接コストに効く。

**ケース**:

| シナリオ | 計測 |
|---|---|
| 既存の各 unary bench | 各 iteration の前後で `getrusage()` を取って差分 |
| 各 streaming bench | 同上 |

**期待される観察**:
- wall-clock = CPU + I/O wait の内訳が見える
- I/O wait の割合が大きい(server delay / RTT 系)→ C 化メリット小
- CPU 割合が大きい(streaming hot path、large payload decode)→ C 化メリット大

### 3.4 Per-frame streaming 軸(★ per-frame hot path 判断用)

**目的**: 1 stream あたりの msg 数と CPU の関係を、現状より高い解像度で測る。

**ケース**(既存 ServerStreamingBench を拡張):

| message count | payload | server delay |
|---:|---|---|
| 10000 | 100 B | 0 |
| 100000 | 100 B | 0 |
| 100 | 100 B | 0(現状あり)|
| 1000 | 100 B | 0(現状あり)|

**狙い**: 10K / 100K msg/stream のスループットを取り、per-msg cost が PHP/C 境界をどれくらい食ってるかを示す。Pubsub StreamingPull の現実シナリオに近い。

### 3.5 Header parsing 軸(★ header parser 判断用)

**目的**: gRPC では metadata header が多い(`x-goog-*`、authorization、grpc-trace-bin、custom)。それの parse + 配列 insert コストが per-call overhead に占める割合を分離計測。

**ケース**:

| シナリオ | やり方 |
|---|---|
| Unary with N headers | test-server に「N 個の dummy header を返す」モードを足し、N=0/10/30/100 で計測 |
| 同 trailing metadata | trailers 数も振る |

**判断材料**:
- header 数を 10 → 100 に増やしたとき per-call latency がどれだけ伸びるか
- 1 header あたり何 μs かかってるか
- `grpc-trace-bin` 等の binary metadata の base64 コストも見える

### 3.6 p99 under load 軸(★ SLO 判断用)

**目的**: 高 RPS で投入したとき tail latency がどう劣化するかを測る。CPU 飽和近傍で PHP overhead が tail を押し上げているか確認。

**ケース**:

| 投入 RPS | 計測指標 |
|---|---|
| 100 / 500 / 1000 / 2000 / saturation | p50, p95, p99 |

**判断材料**:
- p99 が p50 の何倍か(健全なら 2-3 倍以内)
- saturation 近傍で p99 が爆発するか

### 3.7 Memory pressure 軸(参考)

**目的**: 長時間 streaming や高 RPS で peak memory がどう推移するか。GC frequency も。

**ケース**:

| シナリオ | 計測 |
|---|---|
| 1 stream で N=100K msg 受信 | `memory_get_peak_usage()`、`gc_collect_cycles()` 回数 |
| 1000 連続 unary call | 同上 |

---

## 4. 計測指標(まとめ)

新たに追加すべき指標:

| 指標 | 取り方 | 既存指標との関係 |
|---|---|---|
| **CPU per call**(user / sys) | 各 iteration 前後で `getrusage()` の `ru_utime` / `ru_stime` を差分 | wall-clock latency の補完 |
| **Sustained throughput**(calls/sec) | 1 PHP プロセスで N 秒間 call を投入し続け、完了数 / N | latency と独立 |
| **Streaming sustained rate**(msg/sec) | 1 stream で N msg を受け切る時間 / N | per-frame cost の直接測定 |
| **Memory peak**(bytes) | `memory_get_peak_usage(true)` | リーク・GC 圧 |
| **GC cycles**(回数) | `gc_collect_cycles()` の戻り値、または `gc_status()` | GC 圧 |
| **p50 / p95 / p99**(latency) | 投入した全 call の latency をヒストグラムで集計 | tail latency |

---

## 5. 計測インフラ

### 5.1 Toxiproxy(RTT 軸用)

**追加するもの**:

- `compose.yaml` に `toxiproxy` サービス(`ghcr.io/shopify/toxiproxy:latest`)
- 起動時に proxy を作る init script(or bench setUp で API 叩く)
- ports は内部ネットワーク完結、expose 不要

**proxy 構成例**:

| upstream | proxy port | latency |
|---|---|---|
| test-server:50051 | toxiproxy:51051 | 1 ms |
| test-server:50051 | toxiproxy:51053 | 3 ms |
| test-server:50051 | toxiproxy:51055 | 5 ms |
| test-server:50052 | toxiproxy:51552 | 1 ms(TLS 経路用)|

**注意点**:
- `tc netem` と違って **L4 プロキシなので jitter 制御は API パラメータで指定**(`latency` + `jitter` を指定、jitter は ±0.1ms 程度で固定すると 1ms 級でも揺れが出にくい)
- TLS 終端しない(L4)ので certificate 周りは透過
- `bench/compare.sh` から呼べるようにする(`./bench/compare-rtt.sh` のような分離スクリプトも検討)

### 5.2 Throughput / saturation 計測ハーネス

PHPBench は per-rev mode の計測なので、sustained throughput の計測には不向き。**専用の小さい PHP CLI スクリプト** を作るのが筋:

- `bench/throughput.php`
- 引数: `--duration=<sec> --concurrency=<N> --scenario=<name>`
- N 秒間 call を投入し続け、完了数 / N を出す
- ヒストグラム(p50/p95/p99)も出す
- JSON で結果ダンプ

(将来的には `wrk` や `ghz` 相当を PHP 側で書く、または既存 OSS を使う)

### 5.3 CPU per call の取り方

PHPBench の **executor extension** か **iteration callback** で `getrusage()` を差分計測する。または bench メソッドの先頭/末尾で `getrusage()` を呼んで差分を別変数に積む形。

調べどころ:
- PhpBench のカスタム executor
- PHP の `getrusage()` は current process の resource usage なので、bench iteration 単位で差分を取れば call 数で割れる

### 5.4 Memory / GC 計測

PHPBench は標準で `mem_peak` を出すが、bench iteration 末の値。**iteration 内の peak** を取りたいなら bench メソッド内で明示的に `memory_get_peak_usage(true)` を取る。GC は `gc_status()['runs']` の差分。

---

## 6. アウトプットの形

各計測の結果は以下に書く:

- `docs/benchmarks/multi-axis-2026-XX-XX.md`(複数日に分けてもよい)
- 構造: 環境 → 各軸ごとの結果テーブル → 観察 → C 化候補ごとの含意
- 既存 `baseline-` / `comparison-` と同じ tone(対向サーバ、環境、代表値、揺れ幅、判断 を併記)

最終的に **C 化候補ごとの判断材料サマリ** を作る:

```
| C 化候補 | 効くワークロード | 観測された改善余地 | 推奨優先度 |
|---|---|---|---|
| persistent pool | RTT > 1ms / cold call | cold が ms 単位短縮見込み | ◎ |
| per-frame streaming | high msg/sec | 100K msg/sec で 0.X core 削減 | ○/△ |
| ... | | | |
```

これが Phase 2 のスコープ決定文書になる。

---

## 7. 実装ロードマップ

実装順は「既存 bench に小さく足せるか」だけでは決めない。Phase 2 の目的は C 化候補の優先順位を決めることであり、計測構造を変えた方が判断しやすい場合はその時点で見直す。

### 7.1 最初に固定すること

最初の実装コミットでは、計測値そのものよりも **出力形式と runner の責務分離** を固定する。

| 項目 | 方針 |
|---|---|
| artifact | `var/bench-results/` に JSON を保存し、必要なら TSV / log を併置する |
| docs | 結果を採用する時だけ `docs/benchmarks/multi-axis-2026-XX-XX.md` に環境、代表値、揺れ幅、判断を残す |
| comparison | 通常比較は php-grpc-lite vs 公式 ext-grpc。C 化候補の判断材料として ext-grpc を観測線に使う |
| baseline | regression baseline は Phase 1 と同じく php-grpc-lite 自身の回帰検知用。Phase 2 の探索結果を baseline に混ぜない |
| script boundary | PHPBench で自然に表せるものは `bench/run.sh` 系、sustained / p99 / CPU 集計のような独立 runner が自然なものは専用 CLI に分ける |

### 7.2 コミット単位の候補

| 順 | 作業単位 | 主目的 | 構造 |
|---:|---|---|---|
| 1 | Phase 2 runner の出力 contract を定義 | 多軸計測の JSON schema、保存名、summary 表示を先に固める | 既存 `phpbench-with-artifacts.sh` を参考にするが、PHPBench 前提にしない |
| 2 | CPU / memory sampling helper を追加 | wall-clock と CPU / memory を同じ JSON に載せる | 既存 bench に埋め込む前に、CLI helper と単体 smoke を作る |
| 3 | unary / streaming の CPU per call smoke | C 化候補を比較できる最小の CPU 指標を取る | 既存 bench 拡張でも専用 runner でもよい。データ形を優先 |
| 4 | Toxiproxy + RTT unary bench | persistent pool 判断に必要な 1/3/5ms RTT を取る | compose と proxy 初期化を含む独立スイートにする |
| 5 | throughput / p99 harness | saturation、p50/p95/p99、calls/sec を測る | PHPBench から分離した専用 CLI を作る |
| 6 | metadata/header parsing axis | header parser C 化の価値を分離する | test-server 拡張 + bench。既存 `MetadataVolumeBench` との重複を避ける |
| 7 | large streaming axis | per-frame hot path と memory pressure を見る | 10K/100K msg を扱える専用スイート。長時間化するなら通常 suite から分ける |

作業 1-3 で計測値の格納形式と CPU 指標の扱いを固める。作業 4-7 は各 C 化候補に対応する独立した観測軸として実装する。

### 7.3 構造見直しゲート

以下のどれかに当たったら、既存 bench への追記を止めて構造を見直す。

| 条件 | 見直す内容 |
|---|---|
| PHPBench の aggregate から必要な CPU / p99 / throughput が自然に取れない | 専用 runner を主にし、PHPBench は latency smoke に限定する |
| `bench/run.sh` の分岐が suite orchestration 以上の責務を持ち始める | `bench/phase2/*.sh` または PHP CLI runner に分離する |
| JSON schema が PHPBench 抽出結果と sustained runner で乖離する | Phase 2 用 result schema を別に定義し、変換 layer を作る |
| Toxiproxy / load generator / test-server 初期化が通常比較に影響する | Phase 2 専用 compose profile または専用 script に隔離する |
| 1 suite が長時間化して regression baseline 運用に混ざる | exploratory suite と regression suite を明確に分ける |

### 7.4 最初の着手候補

最初は **CPU / memory sampling helper + Phase 2 result JSON** から入る。理由:

- Toxiproxy や load generator より依存が軽い
- 既存 latency ベンチの解釈にすぐ追加価値が出る
- ただし既存 bench へ無理に埋め込まず、専用 runner が自然ならその形を優先できる

この段階で「PHPBench に載せる方が自然か」「専用 runner を主にする方が自然か」を判断し、以後の構造を決める。

---

## 8. 罠 / 注意点

- **Docker Desktop の jitter**: ホスト側の thermal throttling、kernel scheduling 等で 1ms 級は揺れる。複数回計測 + median 推奨
- **Toxiproxy の負荷**: proxy 自体が CPU 食う。高 RPS で計測する時は toxiproxy 側がボトルネックにならないか確認(別コアに pin する等)
- **PHP プロセスの CPU 上限**: コンテナの CPU shares を明示しないと、ホスト負荷で揺れる。`compose.yaml` で `cpus: 1.0` 等を pin すると再現性向上
- **PHPBench の warmup**: streaming saturation の計測では JIT / OPcache が十分に温まっているか確認
- **`getrusage` の精度**: Linux では 4ms quantum(BSD は 1ms)で、サブ ms call では正確に出ない。多数 iteration の合計で割る方式が必要
- **gRPC TLS handshake のキャッシュ**: TLS session resumption が効くと cold 計測が persistent 寄りになる。session ticket / session ID 共有を切ると素の cold が見える
- **Toxiproxy の TCP セッション再利用**: persistent pool 計測時は接続が切れていないこと(toxiproxy の close_delay = 0 確認)
- **既存 spanner-emulator は変更しない**: 計測軸の追加で Spanner 経路を巻き込む必要なし(機能テストとして残す方針は AGENTS.md 通り)

---

## 9. スコープ外(明示)

以下は本計画の対象 **外**(別途検討):

- 実 GCP API への接続テスト(auth / 課金 / 再現性で除外)
- マルチホスト構成での計測(同一ホスト内 + Toxiproxy で十分な近似)
- Windows / macOS host での計測(Linux container 前提)
- Phase 2 着手時の C 拡張側のベンチ(まだ実装が無いので比較対象なし)

---

## 10. 関連

- 全体方針: [AGENTS.md](../../AGENTS.md)
- 設計: [SPEC.md](../SPEC.md)
- 既存ベンチ: [baseline-2026-04-26.md](./baseline-2026-04-26.md), [comparison-ext-grpc-2026-04-26.md](./comparison-ext-grpc-2026-04-26.md)
- 接続再利用の現状: [connection-reuse-2026-04-27.md](./connection-reuse-2026-04-27.md)
- ホットパス分解: [hot-path-breakdown-2026-04-27.md](./hot-path-breakdown-2026-04-27.md)
