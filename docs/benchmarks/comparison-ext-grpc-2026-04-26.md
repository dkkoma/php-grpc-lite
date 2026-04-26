# 比較ベンチマーク: php-grpc-lite vs 公式 ext-grpc

> **取得日**: 2026-04-26
> **php-grpc-lite コミット**: Phase 0 完了直後
> **ext-grpc バージョン**: pecl で 2026-04-26 に取得した最新安定版
> **目的**: ドロップイン代替対象である公式 `ext-grpc` に対して、現状の Phase 0 純 PHP 実装がどう位置するかを把握する

> **注記**: この文書は curl handle reuse 前の Phase 0 比較。reuse 実装後の観測値は [connection-reuse-2026-04-27.md](./connection-reuse-2026-04-27.md) を参照。

---

## 1. 設定

### 1.1 環境分離

`Grpc\` 名前空間が両側で衝突するため、**完全に独立した Composer プロジェクト**を `bench-comparison/` に切ってある。

| 環境 | コンテナ | パッケージ | `Grpc\BaseStub` の出処 |
|---|---|---|---|
| 我々(php-grpc-lite) | `dev` | `composer.json`(root) の PSR-4 | `src/Grpc/BaseStub.php`(libcurl 駆動) |
| ext-grpc | `dev-ext-grpc` | `bench-comparison/composer.json` の `grpc/grpc` | `vendor/grpc/grpc/src/lib/BaseStub.php`(C-core 駆動) |

両環境で **同じ test fixture**(`tests/Integration/Fixtures/GreeterClient`)、**同じ bench シナリオ**(`bench/`)、**同じ対向サーバ**(自前 Go test-server)を使う。スイッチするのは `\Grpc\BaseStub` の出処だけ。

### 1.2 計測条件

- PHPBench 1.6
- iterations=5, revs={5..50}, warmup=1
- リクエストオブジェクトは `setUp` で構築済みのものを使い回し
- 全シナリオが Go 製 test-server に対して実行される(emulator-state ノイズ無し、run 間 ±2-3% で安定)

### 1.3 再現方法

```bash
./bench/compare.sh
```

両環境の計測 + aggregate report 出力を 1 コマンドで実行する。

---

## 2. 結果

### 2.1 Unary

| シナリオ | パラメータ | **php-grpc-lite** | **ext-grpc** | Δ |
|---|---|---:|---:|---|
| `UnaryLatencyBench` / `SayHello` | — | 228 μs | 71 μs | **ext-grpc 3.2×** |
| `UnaryPayloadBench` / `BenchUnary` | payload=0 | 229 μs | 69 μs | ext-grpc 3.3× |
| | payload=100 B | 221 μs | 70 μs | ext-grpc 3.2× |
| | payload=1 KB | 243 μs | 70 μs | ext-grpc 3.5× |
| | payload=10 KB | 250 μs | 82 μs | ext-grpc 3.0× |
| | payload=100 KB | 384 μs | 217 μs | **ext-grpc 1.8×** |
| `UnaryDelayBench` / single, server 10 ms | — | 17.2 ms | 12.2 ms | ext-grpc +42% |
| `UnaryDelayBench` / 10 calls 逐次, 各 10 ms | — | 174 ms | 122 ms | ext-grpc +42% |

**Per-byte cost 概算(unary)**:

| 実装 | (payload_100k − payload_0) / 100KB |
|---|---|
| php-grpc-lite | (384 − 229) μs / 100 KB ≈ **1.51 ns/byte** |
| ext-grpc | (217 − 69) μs / 100 KB ≈ **1.45 ns/byte** |

→ 実質同等。ext-grpc の優位は per-call **接続再利用** が支配的(固定 ~150 μs / call の差)。

### 2.2 Server streaming

#### メッセージ数を振る(payload=100 B、delay=0)

| 件数 | **php-grpc-lite** | **ext-grpc** | Δ |
|---:|---:|---:|---|
| 10 | 240 μs | 116 μs | ext-grpc 2.1× |
| 100 | 344 μs | 346 μs | tied |
| 1000 | **1.38 ms** | 2.68 ms | **us 1.9×** |

**Per-message marginal**:

| 実装 | (count_1000 − count_10) / 990 |
|---|---|
| php-grpc-lite | **1.15 μs/msg** |
| ext-grpc | **2.59 μs/msg** |

→ メッセージ数が増えるほど差が出る。**1000 件で us が 1.9× 速**。curl_multi モデルが ext-grpc の completion-queue 経由ループより per-message オーバーヘッドが小さい。

#### ペイロードサイズを振る(count=100、delay=0)

| サイズ/msg | **php-grpc-lite** | **ext-grpc** | Δ |
|---:|---:|---:|---|
| 0 B | 317 μs | 312 μs | tied |
| 100 B | 614 μs | 352 μs | ext-grpc 1.7× |
| 1 KB | 752 μs | 418 μs | ext-grpc 1.8× |
| 10 KB | 1.55 ms | 1.15 ms | ext-grpc 1.35× |

**Per-byte cost 概算(streaming, 100 msgs)**:

| 実装 | (bytes_10k − bytes_0) / (100 × 10 KB) |
|---|---|
| php-grpc-lite | **1.21 ns/byte** |
| ext-grpc | **0.82 ns/byte** |

→ 重い payload では ext-grpc が速い。protobuf decode を C で done している ext-grpc の優位が per-byte で出る。

#### サーバ pacing(count=10、payload=100 B、delay 振り)

| メッセージ間 delay | **php-grpc-lite** | **ext-grpc** | Δ |
|---:|---:|---:|---|
| 0 ms | 512 μs | 135 μs | ext-grpc 3.8× |
| 1 ms | 20.3 ms | 17.9 ms | ext-grpc +13% |
| 10 ms | 107 ms | 101 ms | ext-grpc +6% |

server pacing が支配項になる(delay=10ms × 9 = 90ms が最低床)と、両者の差は 6% 程度に縮む。

### 2.3 メモリ常駐ピーク

- **php-grpc-lite**: 1.9 - 2.0 MB
- **ext-grpc**: 1.0 - 1.1 MB(C-core 内部の状態は PHP の peak には含まれない)

---

## 3. 考察

### 3.1 ext-grpc の優位は per-call 接続再利用

軽量 unary で 3× 差が付く理由はほぼ **persistent channel(C-core が裏で HTTP/2 接続を保持)** のみ:

- ext-grpc: setUp で作った channel を 50 revs で再利用 → per-call ~0.07 ms
- 我々: 各 call で `curl_init` → 接続確立 → `curl_close` → per-call ~150 μs(主に HTTP/2 negotiation)

server 待ちが効くと差は縮む(unary delay=10ms で +42% → delay=10ms × 9 streaming で +6%)。実 GCP の RPC(典型 50-200 ms)では実用差ほぼ無し。

### 3.2 我々の優位は streaming per-message

メッセージ数が多い streaming では curl_multi モデルが構造的に有利(1000 件で us 1.9× 速)。WRITEFUNCTION でフレーム再構成 → queue に積む → Generator yield、というモデルが ext-grpc の completion-queue 経由 PHP/C 境界往復より軽い。

### 3.3 Per-byte は実質同等(やや ext-grpc 優位)

unary は両者 ~1.5 ns/byte で同等。streaming は ext-grpc が速い(per-byte で 0.82 vs 1.21 ns/byte)。Phase 1 で C 拡張化すれば縮められる範囲。

### 3.4 Phase 1 で取りに行ける改善

優先順:

1. **接続プーリング(最優先)**: libcurl handle の使い回し + `CURLOPT_FORBID_REUSE` 制御 → 軽量 unary の固定費がどれだけ下がるか観測
2. **HEADER/WRITE callback の C 化**: streaming per-message を更に縮める
3. **メタデータ処理のホットパス調整**: 細かい文字列処理を C 側に
4. **per-byte decode**: ext-protobuf を最大限活かす(現状でも使っているが、callback 内処理を C に寄せる)

---

## 4. 関連

- ベースライン(php-grpc-lite 単独): [baseline-2026-04-26.md](./baseline-2026-04-26.md)
- 設計判断: [../SPEC.md](../SPEC.md)
- 実装ガイド: [../code-reading-guide.md](../code-reading-guide.md)
- 比較スクリプト: [../../bench/compare.sh](../../bench/compare.sh)
