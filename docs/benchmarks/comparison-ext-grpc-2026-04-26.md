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

ワンライナーで両環境を計測する `bench/compare.sh` を用意してある。spanner-emulator
の **restart を両 env の前にそれぞれ挟む** のが必須(下記 §3 参照):

```bash
./bench/compare.sh
```

中身は手で展開すると以下と等価:

```bash
docker compose restart spanner-emulator && sleep 2

# php-grpc-lite 側
docker compose run --rm dev vendor/bin/phpbench run --report=aggregate

docker compose restart spanner-emulator && sleep 2

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

### 2.2 サーバ待ちが支配的なケース(threading model の影響を見る)

`UnaryDelayBench`: `HelloRequest.delay_ms` を 10 にして、test-server 側で `time.Sleep(10ms)` してから応答する。実プロダクション API のように **server 処理が wall-clock の支配項になるケース** を再現。

| シナリオ | **php-grpc-lite** | **ext-grpc** | Δ | Δ% |
|---|---:|---:|---:|---:|
| 1 call, server delay 10ms | 14.6 ms | 12.9 ms | +1.7 ms | +13% |
| 10 calls 逐次, 各 server delay 10ms | 149.3 ms | 128.9 ms | +20.4 ms | +16% |

**観察**: 当初の予測(差は ~150μs = helloworld の per-call gap)を大きく上回る ~1.7 ms / call の差が出た。これは **接続管理の差が露呈** したもの:

- **ext-grpc**: setUp で作った channel を 50 revs で再利用 → per-call ~0.1ms
- **我々**: 各 call で `curl_init` → 接続確立 → `curl_close`(50 revs で 50 接続)→ per-call ~2ms(主に HTTP/2 negotiation 等)

`delay=0` の helloworld bench(88μs vs 240μs)では他のオーバヘッドに埋もれていたが、server delay で他の要素が落ち着いた結果、**接続コストそのものが顕在化** した。

**実用シナリオへの含意**:

| call の典型時間 | 我々の overhead 比率 | 実用上の差 |
|---|---|---|
| 数 ms(短い)| 13-20% | 体感できる |
| 数十 ms(GCP の RPC 平均) | 2-5% | わずか |
| 100 ms 超(複雑な query) | <2% | 実質無視できる |

**Phase 1 の優先順位を明確化**:接続プーリング(libcurl handle 再利用 + `CURLOPT_FORBID_REUSE` オフ)を入れれば、この gap は ext-grpc に近づくはず。

### 2.3 リアル・ペイロードでの追加計測

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

## 3. 計測上の注意(後追記)

実運用してみて分かった **重要な計測上の注意**:

### 3.1 Spanner emulator は再起動しないと数値が荒れる

spanner-emulator は **内部状態がクエリ間で蓄積** して、ベンチを走らせ続けると per-call レイテンシが 1.5-2× ブレる。両 env を比較する時は **必ず emulator restart を挟む**(`bench/compare.sh` が自動化している)。

helloworld 系(test-server)はこの問題が無く、run 間で ±2% 以内で収まる。

### 3.2 過去スナップショット値の補足

本ドキュメント §2.1-2.3 の数値は **特定の 1 run の値**(初版作成時)。後日 `bench/compare.sh` で複数回測ったところ、Spanner 系は以下のレンジで動くことを確認:

| 指標 | 過去スナップショット | 観測レンジ(複数 run) |
|---|---:|---|
| Spanner streaming bare 1000 | us 3.23 / ext 4.36(us 1.35× 速) | us 2.8-9.4ms / ext 3.0-9.4ms(同等〜どちらが速いか run 次第) |
| Spanner realistic 1000 | us 5.38 / ext 6.13(us 1.14× 速) | us 5.4-6.2ms / ext 5.7-6.1ms(ほぼ同等) |
| Spanner SELECT 1 | us 2.02 / ext 4.92(us 2.4× 速) | us 2.0-4.6ms / ext 3.7-4.9ms(同等〜やや ext-grpc 優位) |

**修正された理解**: Spanner 系の絶対値の比較で「我々が大きく勝つ」と読み取るのは慎重にすべき。**helloworld 系の安定数値**(`UnaryLatencyBench`、`UnaryDelayBench`)が信頼できる比較指標。

### 3.3 信頼度の整理

| ベンチ | 安定性 | 結論の信頼度 |
|---|---|---|
| `UnaryLatencyBench` | ±2% | **ext-grpc 3× 速は確実**(persistent channel 効果) |
| `UnaryDelayBench`(server delay) | ±2% | **ext-grpc +13-16% 速は確実**(同じく接続再利用) |
| `SpannerUnaryBench` | ±20% 程度 | 同等〜ext-grpc やや速、と読むのが安全 |
| `SpannerStreamingBench`(bare) | ±20-50% | 同等、と読むのが安全 |
| `SpannerRealisticStreamingBench` | ±20% 程度 | 同等、と読むのが安全 |

## 4. 考察

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

## 5. 全体評価

> **Phase 0 の純 PHP 実装は、helloworld 軽量 unary では ext-grpc 比 ~3× 遅いが、server 待ちが効くと差は ~13-16% に縮小。Spanner 系では同等(run 間ノイズが大きく明確な勝敗は付かない)。**

主目的(ドロップイン互換 + SEGV 排除 + OpenSSL 衝突回避)はすべて達成。**性能面で C 拡張版を凌ぐ局面はあるが、再現性のある明確な優位とは言えない**(初版に書いた「streaming で 1.7× 速い」等は §3.2 のとおり 1 スナップショットの値だった)。

確実に言えるのは:

1. **call の典型時間が長くなるほど両者の差は縮む**(per-call connection cost は割合として小さくなる)
2. **接続プーリングを入れれば軽量 unary の差はかなり縮められる見込み**(ext-grpc が稼いでいる ~150μs - 2ms はほぼ persistent channel 効果)
3. **streaming の per-byte コストは ext-grpc と同オーダー**(ext-protobuf を共有しているため当然)

### 5.1 Phase 1 で取りに行きたい改善

1. **接続プーリング(最優先)**: libcurl handle の使い回し + `CURLOPT_FORBID_REUSE` 制御で、軽量 unary を ext-grpc 並みに
2. **HEADER/WRITE callback の C 化**: streaming のホットパス短縮
3. **メタデータ処理のホットパス調整**: 細かい文字列処理を C 側に
4. **再現性ある streaming ベンチ**: Spanner emulator 起源のノイズを避けるため、自前 test-server に server-streaming bench RPC を追加することも検討

---

## 6. 関連

- ベースライン(php-grpc-lite 単独): [baseline-2026-04-26.md](./baseline-2026-04-26.md)
- 設計判断: [../SPEC.md](../SPEC.md)
- 実装ガイド: [../code-reading-guide.md](../code-reading-guide.md)
- 比較スクリプト: [../../bench/compare.sh](../../bench/compare.sh)
