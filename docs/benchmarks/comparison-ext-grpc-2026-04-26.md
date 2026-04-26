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

- PHPBench 1.6
- iterations=5, revs=50(unary)/ 20(streaming), warmup=1
- リクエストオブジェクトは `setUp` で構築済みのものを使い回し
- benchmarks は両側を同じ docker compose ネットワーク内で背中合わせで実行

**重要 — Spanner emulator は再起動が必要**: emulator は内部状態がクエリ間で蓄積し、ベンチを連続で走らせると per-call レイテンシが run 間で 1.5-2× ブレる。両 env を比較する時は emulator restart を挟むこと。helloworld 系(test-server)はこの問題が無く ±2% 以内で安定する。

### 1.3 再現方法

```bash
./bench/compare.sh
```

restart + 両環境の計測 + aggregate report 出力を 1 コマンドで実行する。

---

## 2. 結果

### 2.1 helloworld 系(安定指標、±2%)

| シナリオ | **php-grpc-lite** | **ext-grpc** | Δ |
|---|---:|---:|---|
| `UnaryLatencyBench` / `SayHello` (no delay) | 215 μs | 68 μs | **ext-grpc ~3× 速** |
| `UnaryDelayBench` / single call, server 10 ms | 15.0 ms | 12.9 ms | **ext-grpc +14%** |
| `UnaryDelayBench` / 10 calls 逐次, 各 server 10 ms | 147 ms | 128 ms | **ext-grpc +13%** |

run 間で ±2% 以内に収まる安定指標。

**含意**:

| call の典型時間 | 我々の overhead 比率 | 体感 |
|---|---|---|
| 数 ms(短い) | 13-20% | 体感可能 |
| 数十 ms(GCP の RPC 平均) | 2-5% | わずか |
| 100 ms 超(複雑な query) | <2% | 実質無視できる |

### 2.2 Spanner 系(参考値、emulator ノイズあり)

| シナリオ | **php-grpc-lite** レンジ | **ext-grpc** レンジ | 評価 |
|---|---:|---:|---|
| `SpannerUnaryBench` / `SELECT 1` | 2.0 - 5.0 ms | 3.7 - 5.0 ms | 同等〜やや ext-grpc 優位 |
| `SpannerStreamingBench` / rows=1000 (bare INT64) | 2.8 - 4.4 ms | 3.0 - 4.4 ms | 同等 |
| `SpannerRealisticStreamingBench` / rows=1000 (4 col, ~140KB) | 5.4 - 6.2 ms | 5.7 - 6.1 ms | 同等 |

絶対値は emulator の暖機状態で大きく動くため、複数 run の観測レンジで記載。両実装で **明確な勝敗は付かない** = streaming の decode/yield ホットパスはどちらも実用上問題ないレベル、と読むのが妥当。

### 2.3 メモリ常駐ピーク

- **php-grpc-lite**: 1.9 - 2.4 MB(libcurl ハンドル + protobuf メッセージ含む)
- **ext-grpc**: 1.0 - 1.5 MB(C-core 内部の状態は PHP の peak には含まれないため見かけ上 50% 軽い)

---

## 3. 考察

### 3.1 ext-grpc の優位は接続再利用が支配的

helloworld 軽量 unary で 3× の差が付く理由はほぼ **persistent channel(C-core が裏で HTTP/2 接続を保持)** のみ:

- ext-grpc: setUp で作った channel を 50 revs で再利用 → per-call ~0.1 ms
- 我々: 各 call で `curl_init` → 接続確立 → `curl_close` → per-call ~2 ms(主に HTTP/2 negotiation)

server 待ち 10 ms の場合に差が ~14% に縮むのも、絶対値の per-call connection cost(~2 ms)が server 時間に対する割合として小さくなるため。実 GCP の RPC(典型 50-200 ms)では 2-5% 程度に圧縮される見込み。

### 3.2 Phase 1 で取りに行ける改善

接続管理を改善すれば helloworld の差はかなり縮まるはず:

1. **接続プーリング(最優先)**: libcurl handle の使い回し + `CURLOPT_FORBID_REUSE` 制御
2. **HEADER/WRITE callback の C 化**: streaming のホットパス短縮
3. **メタデータ処理のホットパス調整**: 細かい文字列処理を C 側に
4. **再現性ある streaming ベンチ**: Spanner emulator 起源のノイズを避けるため、自前 test-server に server-streaming bench RPC を追加

---

## 4. 関連

- ベースライン(php-grpc-lite 単独): [baseline-2026-04-26.md](./baseline-2026-04-26.md)
- 設計判断: [../SPEC.md](../SPEC.md)
- 実装ガイド: [../code-reading-guide.md](../code-reading-guide.md)
- 比較スクリプト: [../../bench/compare.sh](../../bench/compare.sh)
