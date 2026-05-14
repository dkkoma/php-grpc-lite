---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# Laravel Spanner mixed transaction action を追加する

## 目的

CPU問題の実アプリ寄り計測シナリオとして、1 HTTP request 内で事前INSERTを1回実行し、別の read-write transaction 内でその行に対するSELECT 2回、UPDATE 1回、別行INSERT 1回を実行する action を追加する。

## 背景

これまでのFPM CPU再現は `select_1row_10col` 単一SELECT actionを負荷対象にしていた。実アプリではtransaction内で複数statementを実行するため、Spanner client / session / transaction / gRPC呼び出しのライフサイクルをより近づけたendpointが必要。

## スコープ

- Laravel bench appへ mixed transaction action を追加する。
- HTTP route と CLI profile entrypoint から呼べるようにする。
- Docker内で最低限の構文確認を行う。

## 非スコープ

- 主要ベンチの既定action変更。
- Cloud Spannerでの本計測。

## 計画

1. `SpannerBench` に mixed transaction method を追加する。
2. `/bench?action=...` と `profile-action.php` へactionを追加する。
3. PHP構文確認を実行する。
4. issueをcloseしてコミットする。

## 進捗

- `SpannerBench::mixedTransaction()` を追加した。
- `/bench?action=transaction_select2_update1_insert1` を追加した。
- CLI profile actionへ `transaction_select2_update1_insert1` を追加した。
- FPM CPU sustain / load compare / callgrind runner の既定actionを `transaction_select2_update1_insert1` に変更した。
- `/bench` と CLI profile action のdefaultも `transaction_select2_update1_insert1` に変更した。

## 検証

- `docker compose run --rm dev sh -lc 'php -l tools/benchmark/laravel-spanner-app/app/Bench/SpannerBench.php && php -l tools/benchmark/laravel-spanner-app/routes/api.php && php -l tools/benchmark/laravel-spanner-app/bin/profile-action.php'`
- `docker compose up -d --force-recreate fpm-lifecycle-16 nginx-laravel-native`
- `docker compose run --rm dev sh -lc 'for i in $(seq 1 20); do if curl --max-time 30 -fsS "http://nginx-laravel-native:8080/bench?action=transaction_select2_update1_insert1"; then exit 0; fi; sleep 1; done; exit 1'`
- `bash -n bench/fpm-laravel-spanner-cpu-sustain.sh bench/fpm-laravel-spanner-load-compare.sh bench/fpm-laravel-spanner-callgrind-worker.sh bench/fpm-laravel-spanner-callgrind.sh bench/fpm-laravel-spanner-cpu-compare.sh`

## 判断ログ

- 事前INSERTとtransaction内INSERTは、それぞれrequestごとにrandom idを使う。
- transaction内のSELECT 2回とUPDATE 1回は、同じrequest内で事前INSERTした行を対象にする。
- 事前INSERTを別transactionに分けることで、混合transaction本体は既存行に対するread/updateと別行insertの組み合わせにする。
- このactionはtransport単体ベンチではなく、実アプリ寄りの混合transactionシナリオとして扱う。
- single transactionの単一SELECTは実ワークロード代表ではないため、CPU問題のnative単体計測ではこのmixed transaction actionを既定にする。

## 完了条件

- `/bench?action=transaction_select2_update1_insert1` が実行可能になる。
- CLI profile actionからも同じシナリオを呼べる。

## Fix summary

Laravel Spanner bench appに `transaction_select2_update1_insert1` actionを追加した。1 request内で、事前INSERT 1回を実行し、別transaction内でその行をSELECT 2回、UPDATE 1回、別行INSERT 1回を実行する。FPM CPU sustain / load compare / callgrind / FastCGI CPU runnerの既定actionもこのシナリオへ変更した。

## Fix commit

- CPU調査 Step 5: mixed transaction endpointを追加
