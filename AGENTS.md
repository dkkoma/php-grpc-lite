# AGENTS.md

このリポジトリで作業するエージェント向けの方針。

## プロジェクトの現在地

- `php-grpc-lite` は公式 `ext-grpc` のドロップイン代替を目指す、公式 `grpc/grpc` PHP wrapper + source-built `ext/grpc` HTTP/2 transport 拡張による gRPC クライアント実装。
- Runtime transport は nghttp2 の 1 系統。libcurl fallback / transport selection option / 環境変数による transport 切替は持たない。
- unary、server streaming、TLS、mTLS、Spanner emulator 経路まで実機検証済み。
- 設計判断と進捗は `docs/SPEC.md`、実装の読み方は `docs/code-reading-guide.md`、HTTP/2/gRPCドメインモデルレビューは `docs/protocol-model-review-guide.md`、レビュー指摘履歴は `docs/reviews/`、ベンチ結果は `docs/benchmarks/` を参照する。

## 作業方針

- 既存の設計方針を先に読む。仕様と実装がずれる作業では、原則として `docs/SPEC.md` か関連 docs を更新してから実装する。
- 新しいまとまった作業・設計判断・継続タスクは `docs/issues/` にissueとして記録してから進める。作業中は `docs/issues/open/`、完了後は `docs/issues/closed/` に置く。
- issueは目的、背景、スコープ、非スコープ、計画、進捗、検証、判断ログ、完了条件を更新しながら進める。完了時は `Status: Closed`、修正コミット、検証結果を追記してから `closed/` へ移動する。
- 変更は目的ごとに小さく保つ。過去履歴と同様に「実機検証の 1 ステップ」「ベンチ 1 系統」「実装 1 経路」程度をコミット単位にする。
- 作業の切りがよい単位でコミットする。コミット前には `git status` でユーザーや他エージェントの未コミット変更を確認し、無関係な変更を混ぜない。
- 既存の未コミット変更はユーザーの作業として扱う。明示依頼なしに戻したり、整理のために巻き戻したりしない。
- コミットメッセージは既存履歴に合わせ、日本語で具体的に書く。例: `Phase 0: unary 経路を実装(...)`、`Spanner emulator 検証 Step 1: ...`、`ベンチ比較スクリプト追加 + ...`。
- 「ext-grpc の数値に近づける」こと自体を目的にしない。比較値は観測線として扱い、主目的は安全な代替実装の性質を測定・改善すること。

## 実装ルール

- PHP は 8.4+ 前提。スタイルは既存の `declare(strict_types=1);`、PSR-4、型注釈、短い docblock に合わせる。
- `Grpc\` API 互換性を壊さない。`google/gax` / `google/cloud-*` から呼ばれる surface は `docs/api-surface.md` を基準にする。
- gRPC framing、metadata、status、deadline、TLS/mTLS の挙動は実機テストで守る。表面的なベンチ改善のために互換性を削らない。
- HTTP/2 transport / gRPC protocolに触る変更では、実装後にHTTP/2/gRPCドメインモデルレビューを必須ゲートとして実施する。レビューでは命名、責務分離、connection / stream / call / channel のscope、flow-control、metadata/status/deadline、RST_STREAM / GOAWAY / EOF lifecycle、production / bench boundaryを確認する。`docs/protocol-model-review-guide.md` は補助資料として参照し、Blocker / High / Medium / Low の指摘が `none` になるまで修正と再レビューを繰り返す。
- client streaming / bidi streaming は現時点では後回し。触る場合は SPEC のスコープ更新から始める。

## レビュー記録

- サブエージェントへのレビュー依頼は英語でよい。
- レビュー指摘は `docs/reviews/issues/` にMarkdownで残す。テンプレートは `docs/reviews/templates/review-issue.md` を使う。
- issue本文は日本語を基本にし、HTTP/2 / gRPC / PHP extension の仕様語は英語のまま使う。
- レビューエージェントを起動するときは、対象scope、review role、確認観点を明示し、指摘を `docs/reviews/issues/` のissueファイルへ直接書くよう依頼する。エージェントが書けない場合だけ、親エージェントが返されたissue形式の指摘を転記する。
- 修正時は同じissueに `Status`, `Fix summary`, `Fix commit`, `Verification` を追記する。
- design docには現在と未来の設計だけを残し、過渡的なレビュー指摘や修正履歴は `docs/reviews/` に残す。

## 検証

- ホストの PHP ではなく Docker compose 内で実行する。
- C拡張PHPT: `./tools/test/check-phpt.sh`。`vendor/autoload.php` と Go test-server ports `50051`〜`50054`、raw lifecycle fixture ports `50055`〜`50060` をpreflightで必須にする。
- C拡張C unit: `./tools/test/check-c-unit.sh`。I/Oに依存しないprotocol helperとstatus taxonomyを対象にする。
- C拡張C coverage: `./tools/test/check-c-coverage.sh`。C unitとPHPTを実行し、`var/coverage/c-lcov/` にlcov traceとHTMLを出力する。
- 統合テスト(PHPUnit): `docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit`
- C拡張静的解析: `./tools/test/check-c-static-analysis.sh`
- 単独ベンチ: `./bench/run.sh <suite>`
- ext-grpc 比較: `./bench/compare.sh <suite>`。Spanner代表shapeは `spanner-shape`、実経路smoke/regressionは `spanner-real-client` を使う。franken-go backend を含める場合は対象suiteを明確にした専用runnerを追加する。
- official ext-grpc を比較対象としてimageへ組み込む場合は、特にpatchやcustom instrumentationが必要ない限り `ghcr.io/dkkoma/ext-grpc-artifacts` の `grpc.so` artifactを使う。通常のdiagnostic/bench Dockerfileで `pecl install grpc` はしない。
- artifact tagは `<grpc-version>-php<php-version>-<distro>-<arch>-<profile>`。`pecl` は全arch、`optimized-amd64-skylake` はamd64専用。amd64 performance comparatorでは `optimized-amd64-skylake` を優先し、arm64や互換確認では `pecl` を使う。Dockerfileでは `EXT_GRPC_ARTIFACT_ARCH` build argでartifact archを明示する。
- ベンチ結果を docs に反映する場合は、対向サーバ、環境、代表値、揺れ幅、判断を一緒に書く。

## ベンチ作業の注意

- Spanner emulator は実機検証には有用だが、ベンチ指標としては内部状態の揺れが大きい。安定したtransport性能観測は Go test-server の `spanner-shape` など制御可能な RPC を優先し、実アプリ経路の回帰確認は `spanner-real-client` を使う。
- ext-grpc は目標値ではなく比較対象。差分の理由を分解し、固定費、per-message、per-byte、server pacing などに分けて判断する。
- ベンチ計測結果はOTEL spanを一次ソースにする。`bench/` runnerは `otelop` へexportし、`tools/benchmark/otelop-summary.php` で集計する。必要に応じて `BENCH_TAG` / `BENCH_OTEL_RUN_ID` でrun idを固定する。JSON/TSVのベンチ結果保存や旧baseline運用は使わない。
- 旧baseline運用は廃止済み。性能回帰を見る場合は `bench/` の代表ケースを同条件で再実行して比較する。
- hotpath最適化や性能改善を目的にする変更は、必ず実装前に仮説、対象ワークロード、before計測、採否基準をissueへ書く。実装後は同条件のafter計測、改善幅、副作用、コード複雑性を記録し、効果が小さい場合は採用せず調査結果として閉じる。before/afterなしに性能改善としてコミットしない。
- micro optimizationは、改善対象の固定費が支配的であること、cacheや分岐追加のoverheadを上回ること、lifetime/invalidationが単純であることを実測で確認してから採用する。互換性テストや観測ケース追加は、性能最適化本体と切り分けてコミットする。

## サブエージェントの利用と作業の継続について

サブエージェントを展開して自律開発してください。
各サブエージェントには高負荷の調査・設計・実装・テスト・レビューを担当させてください。サブエージェントを検索エンジンとして使わないでください。
完了ごとに次のタスクを投げ続けて常に稼働させてください。
