# AGENTS.md

このリポジトリで作業するエージェント向けの方針。

## プロジェクトの現在地

- `php-grpc-lite` は公式 `ext-grpc` のドロップイン代替を目指す、公式 `grpc/grpc` PHP wrapper + source-built `ext/grpc` HTTP/2 transport 拡張による gRPC クライアント実装。
- Runtime transport は nghttp2 の 1 系統。libcurl fallback / transport selection option / 環境変数による transport 切替は持たない。
- unary、server streaming、TLS、mTLS、Spanner emulator 経路まで実機検証済み。
- 設計判断と進捗は `docs/SPEC.md`、実装の読み方は `docs/code-reading-guide.md`、HTTP/2/gRPC状態機械レビューは `docs/protocol-model-review-guide.md`、ベンチ結果は `docs/benchmarks/` を参照する。

## 作業方針

- 既存の設計方針を先に読む。仕様と実装がずれる作業では、原則として `docs/SPEC.md` か関連 docs を更新してから実装する。
- 変更は目的ごとに小さく保つ。過去履歴と同様に「実機検証の 1 ステップ」「ベンチ 1 系統」「実装 1 経路」程度をコミット単位にする。
- 作業の切りがよい単位でコミットする。コミット前には `git status` でユーザーや他エージェントの未コミット変更を確認し、無関係な変更を混ぜない。
- 既存の未コミット変更はユーザーの作業として扱う。明示依頼なしに戻したり、整理のために巻き戻したりしない。
- コミットメッセージは既存履歴に合わせ、日本語で具体的に書く。例: `Phase 0: unary 経路を実装(...)`、`Spanner emulator 検証 Step 1: ...`、`ベンチ比較スクリプト追加 + ...`。
- 「ext-grpc の数値に近づける」こと自体を目的にしない。比較値は観測線として扱い、主目的は安全な代替実装の性質を測定・改善すること。

## 実装ルール

- PHP は 8.4+ 前提。スタイルは既存の `declare(strict_types=1);`、PSR-4、型注釈、短い docblock に合わせる。
- `Grpc\` API 互換性を壊さない。`google/gax` / `google/cloud-*` から呼ばれる surface は `docs/api-surface.md` を基準にする。
- gRPC framing、metadata、status、deadline、TLS/mTLS の挙動は実機テストで守る。表面的なベンチ改善のために互換性を削らない。
- HTTP/2 transport / gRPC protocolに触る変更では、`docs/protocol-model-review-guide.md` に沿って connection / stream / call / channel のscope、flow-control、RST_STREAM / GOAWAY / EOF lifecycleを確認する。
- client streaming / bidi streaming は現時点では後回し。触る場合は SPEC のスコープ更新から始める。

## 検証

- ホストの PHP ではなく Docker compose 内で実行する。
- 統合テスト: `docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit`
- 単独ベンチ: `docker compose run --rm dev vendor/bin/phpbench run --report=aggregate`
- ext-grpc 比較: `./bench/run.sh compare` または互換入口の `./bench/compare.sh`
- grpc-php-rs 任意比較: `./bench/compare-rs.sh`。通常比較はあくまで php-grpc-lite vs 公式 ext-grpc とし、grpc-php-rs は明示依頼がある場合だけ使う。
- ベンチ結果を docs に反映する場合は、対向サーバ、環境、代表値、揺れ幅、判断を一緒に書く。

## ベンチ作業の注意

- Spanner emulator は実機検証には有用だが、ベンチ指標としては内部状態の揺れが大きい。安定した性能観測は Go test-server の制御可能な RPC を優先する。
- ext-grpc は目標値ではなく比較対象。差分の理由を分解し、固定費、per-message、per-byte、server pacing などに分けて判断する。
- ベンチ実行ログと抽出済み JSON/TSV は `var/bench-results/` に置く。必要に応じて `BENCH_TAG` / `BENCH_OUTPUT_DIR` で保存名と保存先を固定する。
- regression baseline は `bench/baselines/regression.json`。明示的な性能変化を受け入れる時だけ更新し、通常は `BENCH_BASELINE=bench/baselines/regression.json ./bench/run.sh cold` / `warm` / `stream-smoke` のように比較する。

## サブエージェントの利用と作業の継続について

サブエージェントを展開して自律開発してください。
各サブエージェントには高負荷の調査・設計・実装・テスト・レビューを担当させてください。サブエージェントを検索エンジンとして使わないでください。
完了ごとに次のタスクを投げ続けて常に稼働させてください。
