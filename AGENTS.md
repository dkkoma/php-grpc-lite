# AGENTS.md

このリポジトリで作業するエージェント向けの方針。

## プロジェクトの現在地

- `php-grpc-lite` は公式 `ext-grpc` のドロップイン代替を目指す、純 PHP / libcurl ベースの gRPC クライアント実装。
- Phase 0 は unary、server streaming、TLS、mTLS、Spanner emulator 経路まで実機検証済み。
- 設計判断と進捗は `docs/SPEC.md`、実装の読み方は `docs/code-reading-guide.md`、ベンチ結果は `docs/benchmarks/` を参照する。

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
- client streaming / bidi streaming は現時点では後回し。触る場合は SPEC のスコープ更新から始める。

## 検証

- ホストの PHP ではなく Docker compose 内で実行する。
- 統合テスト: `docker compose run --rm dev vendor/bin/phpunit`
- 単独ベンチ: `docker compose run --rm dev vendor/bin/phpbench run --report=aggregate`
- ext-grpc 比較: `./bench/compare.sh`
- ベンチ結果を docs に反映する場合は、対向サーバ、環境、代表値、揺れ幅、判断を一緒に書く。

## ベンチ作業の注意

- Spanner emulator は実機検証には有用だが、ベンチ指標としては内部状態の揺れが大きい。安定した性能観測は Go test-server の制御可能な RPC を優先する。
- ext-grpc は目標値ではなく比較対象。差分の理由を分解し、固定費、per-message、per-byte、server pacing などに分けて判断する。
- 次の主要テーマは Channel-scoped curl handle reuse。目的は libcurl / HTTP/2 の connection reuse が純 PHP 実装でどの程度効くかを観測すること。
