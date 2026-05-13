---
Status: Closed
Owner: Codex
Created: 2026-05-14
---

# GAX / google-cloud-spanner 実経路 lifecycle 調査

## 目的

`spanner-real-client` の固定費について、GAX / google-cloud-spanner / grpc wrapper の実コードを読み、php-grpc-lite側で改善できる仮説と上位ライブラリ由来の固定費を切り分ける。

## 背景

`spanner-shape` ではtransport寄りの小さいRPCが十分速い一方、`spanner-real-client` は上位ライブラリやSpanner emulatorの固定費が混ざる。generated stub / GAX bypass や persistent channel / session lifecycle の改善余地を挙げたが、実コード確認前の仮説だったため、事実ベースで再評価する。

## スコープ

- `vendor/google/gax` の gRPC transport 経路を確認する。
- `vendor/google/cloud-spanner` の client/database/transaction/session/result 経路を確認する。
- `vendor/grpc/grpc` の wrapper / BaseStub / Channel 経路を確認する。
- このリポジトリの責務で改善可能な点と、上位ライブラリ側の固定費を分類する。

## 非スコープ

- 実装変更。
- 追加ベンチマーク。

## 確認結果

### GAX / generated stub 経路

- `vendor/google/cloud-spanner/src/V1/Client/SpannerClient.php:606` の `executeStreamingSql()` は `startApiCall()` に委譲する。
- `vendor/google/gax/src/GapicClientTrait.php:576` の `startApiCall()` から `startCall()` に入り、`Call` object、call options、middleware stack を組み立てる。
- `vendor/google/gax/src/GapicClientTrait.php:686` の `createCallStack()` は `TransportCallMiddleware`、credentials、fixed header、retry、auto-population、options filter、追加middlewareを毎call経由する。
- `vendor/google/gax/src/Transport/GrpcTransport.php:231` の server streaming 経路は `_serverStreamRequest()` を呼び、`ServerStream` と `ServerStreamingCallWrapper` で包む。
- `vendor/google/gax/src/ServerStream.php:79` の `readAll()` は underlying call の `responses()` をyieldし、最後に `getStatus()` を確認する。logger有効時だけ payload JSON logging が入る。
- `vendor/google/gax/src/Transport/Grpc/ServerStreamingCallWrapper.php:69` は underlying `Grpc\ServerStreamingCall::responses()` をほぼそのままyieldする薄いwrapper。

結論: GAX/generated stub層の固定費は実在する。ただしこれはこのリポジトリのC transport内ではなく、公式 `google/gax` / generated client / `grpc/grpc` PHP wrapper の上位層で発生する。ここを直接短縮するには、GAX `TransportInterface` の別実装やGoogle Cloud client向けの専用adapterが必要で、公式 `grpc/grpc` wrapperのdrop-in transportを速くする作業とは別スコープになる。

### grpc/grpc wrapper 経路

- `vendor/grpc/grpc/src/lib/BaseStub.php:44` の constructor は default root、metadata updater、call invoker、channelを設定する。
- `vendor/grpc/grpc/src/lib/BaseStub.php:120` の `getDefaultChannel()` は `new Channel(...)` を返す。
- `vendor/grpc/grpc/src/lib/BaseStub.php:537` の `_simpleRequest()` と `vendor/grpc/grpc/src/lib/BaseStub.php:585` の `_serverStreamRequest()` は毎callでcall factoryを作り、call objectを作る。
- `vendor/grpc/grpc/src/lib/BaseStub.php:271` の unary closure は call invoker、metadata updater、metadata validation、`start()` を通る。
- `vendor/grpc/grpc/src/lib/ServerStreamingCall.php:37` の `start()` は request serialize + send initial metadata/message/close、`responses()` は messageごとに `startBatch(OP_RECV_MESSAGE)`、`getStatus()` は status batchを呼ぶ。

結論: `grpc/grpc` wrapperにもcall object生成、closure、metadata normalization、batch APIの固定費がある。drop-in互換を維持する限り、この層は基本的に通る。C extension側の最適化で縮められるのは `Channel` / `Call` / batch処理の内部であり、PHP wrapper object graph全体は別問題。

### Spanner high-level client / session lifecycle

- `vendor/google/cloud-spanner/src/SpannerClient.php:193` の constructor は GAPIC Spanner / InstanceAdmin / DatabaseAdmin client を生成し、middlewareを追加する。
- `vendor/google/cloud-spanner/src/SpannerClient.php:683` の `connect()` は `Instance::database()` を返す。
- `vendor/google/cloud-spanner/src/Instance.php:538` の `database()` は `SessionCache` が渡されなければ新規作成する。
- `vendor/google/cloud-spanner/src/Session/SessionCache.php:70` は sysv shm/semaphore があれば `SysVCacheItemPool`、なければ `/tmp/spanner_cache/` の file cache を使う。
- `vendor/google/cloud-spanner/src/Session/SessionCache.php:108` の `name()` は `ensureValidSession()` を通り、cache hitなら既存sessionを復元し、missなら multiplexed session を作る。
- `vendor/google/cloud-spanner/src/Operation.php:253` の `execute()` は request構築、session name取得、closure作成、`Result`生成を行う。
- `vendor/google/cloud-spanner/src/Operation.php:333` の `executeUpdate()` は `execute()` したうえで `iterator_to_array($res->rows())` によりstreamをdrainし、statsを読む。
- `vendor/google/cloud-spanner/src/Result.php:127` の `rows()` は PartialResultSet buffering、chunk stitching、row decode、resume retry判定を行う。

結論: Spanner sessionは `SessionCache` でcacheされ、`spanner-real-client` は1つの `SpannerClient` / `Database` をrun内で使い回しているため、計測区間でGAPIC clientやDatabaseを毎回作る構造にはなっていない。channel/session lifecycleを疑うより、per operationのGAX middleware、request/result object生成、Result row materialization、Spanner emulator SQL処理が支配的と見るべき。

## 仮説の再評価

### 1. generated stub / GAX bypass

- 妥当性: あり。ただし改善対象はこのリポジトリの現在のdrop-in C transportではなく、GAX `TransportInterface` またはGoogle Cloud Spanner専用adapterになる。
- 期待できる改善: `spanner-real-client` の固定費の一部削減。`spanner-shape` のようなtransport単体性能にはほぼ影響しない。
- リスク: `google/gax` / `google/cloud-spanner` のAPI surfaceに強く依存する。汎用gRPC client実装から外れる。
- 次の検証案: `Gapic SpannerClient::executeStreamingSql()` 直呼び、`Database/Transaction`高レベル経路、`grpc/grpc BaseStub`直経路を同じemulator/transaction条件で比較する。

### 2. persistent channel / session lifecycle

- 妥当性: 部分的。FPM/worker requestをまたぐchannel persistは引き続き重要だが、現在の `spanner-real-client` のper-call差分を説明する主因ではない。
- 根拠: `spanner-real-client` は `tools/benchmark/spanner-real-client.php:91` で `SpannerClient` を1回作り、`tools/benchmark/spanner-real-client.php:96` で `Database` を1回作り、以後の計測で使い回す。
- session側: `SessionCache` は shared cache を持つため、client/databaseを作り直す場合でもsession作成RPCが毎回発生する設計ではない。
- 次の検証案: client/databaseを毎call作るcold-ish経路と、現在のwarm object経路を別suiteで比較する。ただしこれはアプリケーション利用ガイド寄りで、transport最適化とは分ける。

## 次に実装・計測すべき候補

1. `spanner-real-client` を補助する分解suiteを作る。`BaseStub直` / `GAPIC executeStreamingSql直` / `Database/Transaction高レベル` を同じSQL・同じtransaction条件で比較する。
2. その結果、GAX/generated層が支配的なら、このリポジトリ本体ではなく optional adapter として扱うか判断する。
3. channel/session lifecycleについては、実アプリ向けdocsで `SpannerClient` / `Database` のreuseを推奨する。transportの主要改善テーマとしては扱わない。

## 判断ログ

- `spanner-shape` が十分速く、`spanner-real-client` が数百µsになる差分は、C transport内部だけでは説明しにくい。
- 実コード上、GAXとSpanner high-level clientは毎callでmiddleware、request/result object、row decode、stats drainを行う。
- 現在のreal-client benchはclient/database warm reuseのため、channel constructionやsession creationの繰り返しは主因ではない。

## 検証

- コードリーディングのみ。実行ベンチは未追加。

## 完了条件

- [x] generated stub / GAX bypass仮説の妥当性が整理されている。
- [x] channel / session reuse仮説の妥当性が整理されている。
- [x] 次に実装・計測すべき候補が明確になっている。
