# phase2 benchmark structure removal

Status: Closed

## 目的

現行ベンチマーク基盤から `phase2` という開発段階名を取り除き、継続運用するベンチマーク基盤として読める構造にする。

## 背景

HTTP/2 transport選定とPhase 2探索は完了済みで、現在はリリース前QAと継続ベンチ運用の段階にある。`bench/phase2` / `tools/phase2` / `PhpGrpcLite\\Tools\\Phase2` が残っていると、現行基盤が過去の探索フェーズ専用品に見える。

## スコープ

- `bench/phase2` を現行ベンチマーク入口へ改名する。
- `tools/phase2` を `tools/benchmark` へ改名する。
- PHP namespaceを `PhpGrpcLite\\Tools\\Benchmark` に変更する。
- 現行docs、AGENTS、test scriptsの参照を更新する。
- 履歴記録docsは必要最小限の更新に留める。

## 非スコープ

- ベンチケースの追加・削除。
- OTEL集計仕様の変更。
- 過去の探索結果そのものの再解釈。

## 計画

- 参照箇所を棚卸しする。
- ディレクトリとnamespaceを改名する。
- runner/test/docsの参照を更新する。
- lintと短いbench smokeで検証する。

## 進捗

- 参照箇所を棚卸しした。
- `bench/phase2` を `bench/` のrunnerへ移した。
- `tools/phase2` を `tools/benchmark` へ移した。
- PHP benchmark helper namespaceを `PhpGrpcLite\\Tools\\Benchmark` へ変更した。
- AGENTS、benchmark README、現行設計docs、test scriptsの参照を更新した。
- ext-grpc smokeでHTTP proxy環境変数を拾う問題が出たため、compose内サービス向けの `NO_PROXY` / `no_proxy` をrunnerから渡すようにした。

## 検証

- `docker compose run --rm dev sh -lc 'for f in tools/benchmark/*.php; do php -l "$f" || exit 1; done; bash -n bench/run.sh bench/compare.sh bench/compare-spanner-dml-unary-shape.sh bench/compare-small-select-streaming.sh tools/test/check-native-lifecycle-stress.sh tools/test/check-native-slow-consumer.sh tools/test/check-native-fpm-lifecycle.sh'`
- `BENCH_TAG=benchmark-structure-smoke-20260510 ./bench/run.sh throughput-unary --duration=0.01 --warmup-calls=1 --payload-bytes=10`
- `BENCH_TAG=benchmark-structure-compare-smoke2-20260510 ./bench/compare.sh throughput-unary --duration=0.01 --warmup-calls=1 --payload-bytes=10`

## 判断ログ

- 現行実行入口からphase名を消すことを優先する。
- 過去のresearch/benchmark記録中のPhase表現は履歴として残し、現行導線だけを更新する。

## 完了条件

- 現行runner pathに `phase2` が残らない。
- PHP benchmark helper namespaceに `Phase2` が残らない。
- AGENTSと現行benchmark READMEが新パスを示す。
- lintと代表runner smokeが通る。
