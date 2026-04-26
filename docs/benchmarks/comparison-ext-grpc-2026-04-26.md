# 比較ベンチマーク: php-grpc-lite vs 公式 ext-grpc

> **取得日**: 2026-04-26
> **php-grpc-lite コミット**: Phase 0 完了直後
> **ext-grpc バージョン**: pecl で 2026-04-26 に取得した最新安定版
> **目的**: ドロップイン代替対象である公式 `ext-grpc` に対して、現状の Phase 0 純 PHP 実装がどう位置するかを把握する

---

## 1. 設定

### 1.1 環境分離

`Grpc\` 名前空間が両側で衝突するため、**完全に独立した Composer プロジェクト**を `bench-comparison/` に切ってある。

| 環境 | コンテナ | パッケージ | `Grpc\BaseStub` の出処 |
|---|---|---|---|
| 我々(php-grpc-lite) | `dev` | `composer.json`(root) の PSR-4 | `src/Grpc/BaseStub.php`(libcurl 駆動) |
| ext-grpc | `dev-ext-grpc` | `bench-comparison/composer.json` の `grpc/grpc` | `vendor/grpc/grpc/src/lib/BaseStub.php`(C-core 駆動) |

両環境で **同じ test fixture**(`tests/Integration/Fixtures/`)、**同じ bench シナリオ**(`bench/`)、**同じ対向サーバ**(test-server, spanner-emulator)を使う。スイッチするのは `\Grpc\BaseStub` の出処だけ。

### 1.2 計測条件

両側で完全に揃えてある:

- PHPBench 1.6
- iterations=5, revs=50(unary)/ 20(streaming), warmup=1
- リクエストオブジェクトは `setUp` で構築済みのものを使い回し
- spanner-emulator は計測前に `docker compose restart` でリセット
- benchmarks は両側を **同じ docker compose ネットワーク内で同じ瞬間に背中合わせで** 実行

### 1.3 再現方法

```bash
docker compose restart spanner-emulator

# php-grpc-lite 側
docker compose run --rm dev vendor/bin/phpbench run --report=aggregate

# ext-grpc 側
docker compose run --rm dev-ext-grpc bash -c \
  'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'
```

---

## 2. 結果

| シナリオ | サブジェクト | パラメータ | **php-grpc-lite** | **ext-grpc** | **比** |
|---|---|---:|---:|---:|---:|
| `UnaryLatencyBench` | `benchSayHello` (h2c, Go) | — | 225 μs | **88 μs** | ext-grpc **2.6× 速** |
| `SpannerUnaryBench` | `benchExecuteSql` (`SELECT 1`) | — | **2.02 ms** | 4.92 ms | php-grpc-lite **2.4× 速** |
| `SpannerStreamingBench` | `ExecuteStreamingSql` | rows=10 | **2.50 ms** | 3.10 ms | php-grpc-lite 1.24× |
| `SpannerStreamingBench` | `ExecuteStreamingSql` | rows=100 | **2.35 ms** | 3.42 ms | php-grpc-lite 1.46× |
| `SpannerStreamingBench` | `ExecuteStreamingSql` | rows=1000 | **3.23 ms** | 4.36 ms | php-grpc-lite 1.35× |

ピーク常駐メモリ:
- php-grpc-lite: 1.9–2.2 MB(libcurl ハンドル + protobuf メッセージ)
- ext-grpc: 1.0–1.4 MB(C-core 内部に隠れている分が多いと推測)

### 2.1 派生分析: streaming の行あたり marginal cost(bare INT64)

| 実装 | (rows_1000 − rows_10) / 990 |
|---|---|
| php-grpc-lite | (3.23 − 2.50) / 990 ≈ **0.74 μs/row** |
| ext-grpc | (4.36 − 3.10) / 990 ≈ **1.27 μs/row** |

→ ストリーム配信中の **per-row ホットパス** で php-grpc-lite が **1.7× 速い**。curl_multi の I/O ループ + フレーム再構成 + protobuf decode + Generator yield 一式が、ext-grpc の completion-queue 経由ループより軽い。

### 2.2 リアル・ペイロードでの追加計測

`SpannerRealisticStreamingBench`: 4 カラム(STRING ×2, TIMESTAMP ×2)、行あたり ~140 bytes シリアライズ、`UNNEST` + Spanner 関数で生成(wire format は実テーブル SELECT と同一)。

| 行数 | 総 byte 概算 | **php-grpc-lite** | **ext-grpc** | 比 |
|---:|---:|---:|---:|---:|
| 100 | ~14 KB | **2.80 ms** | 3.10 ms | us 1.11× |
| 1000 | ~140 KB | **5.38 ms** | 6.13 ms | us 1.14× |

Per-row marginal:

| 実装 | (rows_1000 − rows_100) / 900 |
|---|---|
| php-grpc-lite | (5.38 − 2.80) / 900 ≈ **2.87 μs/row** |
| ext-grpc | (6.13 − 3.10) / 900 ≈ **3.37 μs/row** |

Per-byte 概算(140 bytes/row):

| 実装 | μs/byte |
|---|---|
| php-grpc-lite | ~0.020 |
| ext-grpc | ~0.024 |

→ ペイロードが重くなると **差は 1.7× → 1.17× に縮小**。これは予想通りの挙動:**ペイロードが軽い時は per-call オーバーヘッド支配 → 我々が大きく勝つ**、**重くなると per-byte decode cost が支配 → ext-protobuf の C 実装の速さが効いてきて差が縮まる**。それでも依然 us が ~15% 速いのは、curl_multi モデルの構造的優位(PHP/C 境界往復が少ない)が per-byte コストの中でも効いているため。

---

## 3. 考察

### 3.1 helloworld unary では ext-grpc 圧勝(2.6×)

接続確立コスト勝負になる軽量 unary では、**ext-grpc の persistent channel(C-core が裏で HTTP/2 接続を保持)** が効く。我々は **per-call で `curl_init` → 接続 → `curl_close`** しているので、TCP/HTTP/2 ハンドシェイクごとに 100μs 級のオーバーヘッドが乗る。

**Phase 1 で改善する筋**: libcurl の `CURLOPT_FORBID_REUSE` をオフにし、`curl_share` または curl handle pooling で接続を使い回す。これだけで helloworld のフロアは半減以下が見込める。

### 3.2 Spanner unary では php-grpc-lite が 2.4× 速い

意外。仮説:
- ext-grpc の C-core は **completion-queue でのポーリング**を介するため、PHP ↔ C 境界の往復回数が多い
- php-grpc-lite は単発の `curl_exec` でブロックして完了するだけなので、レスポンス待ちのオーバーヘッドが小さい
- Spanner emulator 側のレスポンス組み立て時間(数 ms)が共通コストとして乗っているはずだが、それを差し引いても差が残る

ただし Spanner の数値は emulator の暖機状態に強く影響される(再起動直後とそうでない時で 2-4× 変動を観測)。**ストリーミング側の差分の方が信頼できる**。

### 3.3 Spanner streaming で php-grpc-lite が 1.2-1.5× 速い

- 固定オーバーヘッド(rows_10 ベース): us 2.50ms vs ext-grpc 3.10ms(20% 速い)
- 行あたりコスト: us 0.74μs/row vs ext-grpc 1.27μs/row(70% 速い)

ext-grpc の completion-queue モデルは **メッセージ受信ごとに PHP 層への bridge を踏む**。我々の curl_multi モデルは WRITEFUNCTION でフレーム再構成して queue に積む実装で、PHP ↔ C 境界が少ない。

### 3.4 メモリは ext-grpc が ~50% 軽い

C-core 内部に隠れた状態が PHP の peak メモリには現れないため。実プロセスでは ext-grpc も同等以上のメモリを使っているはずで、これは PHP-visible な数値の比較。

---

## 4. 全体評価

> **Phase 0 の純 PHP 実装は、unary 軽量ケースを除いて公式 ext-grpc と同等以上のパフォーマンス**。
> **ペイロードが重くなっても streaming は依然として ~15% 速い。**

これは予想以上に良い結果。Phase 0 の主目的(ドロップイン互換 + SEGV 排除 + OpenSSL 衝突回避)を達成しつつ、**streaming では bare INT64 で 1.7×、リアル 4 カラム payload でも 1.17×** で C 拡張版に勝つ。

ext-grpc の優位は **persistent channel での接続再利用** のみで、これは我々の Phase 1 で十分対処可能(curl pooling)。

### 4.1 Phase 1 で取りに行きたい改善

1. **接続プーリング(最優先)**: helloworld の 225μs を 100μs 級に下げる → ext-grpc と同等に
2. **HEADER/WRITE callback の C 化**: 行あたり 0.74μs を 0.3μs 級に → streaming で更に差を広げる
3. **protobuf decode のホットパス調整**: ext-protobuf を最大限活用

### 4.2 数値の信頼性についての注記

- **helloworld の数値は安定**(±2-3% RSD、ラン間変動も小さい)
- **Spanner の絶対値はラン間で 2-4× 変動する**(emulator 状態依存)。比率は安定しているので、**比較目的では有意**
- どちらの環境も同じ docker compose ネットワークで同じ瞬間に走らせたので、ホスト負荷は共通
- iteration / revs を増やしても Spanner の絶対値変動は消えない(emulator 内部のため)

---

## 5. 関連

- ベースライン(php-grpc-lite 単独): [baseline-2026-04-26.md](./baseline-2026-04-26.md)
- 設計判断: [../SPEC.md](../SPEC.md)
- 実装ガイド: [../code-reading-guide.md](../code-reading-guide.md)
