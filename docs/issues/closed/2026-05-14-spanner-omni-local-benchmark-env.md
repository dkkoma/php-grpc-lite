# Spanner Omni local benchmark environment

Status: Closed

## 目的

Laravel + `colopl/laravel-spanner` のFPM並列CPU計測で、Cloud Spanner emulatorの「active transactionは1つだけ」という制約により500が混入する問題を避けるため、Spanner Omniをローカル計測先として使えるか検証する。

## 背景

- 現在のLaravel実経路fixtureは、コンテナ起動時にFPM worker数分のfile session poolをwarmupし、API requestではwarmupしない構成にした。
- `client concurrency=16` / `FPM worker=16` の並列SELECTでも、`laravel-spanner` がread-write transactionを使うため、emulatorが `ABORTED: The emulator only supports one transaction at a time.` を返す。
- この状態では、実アプリ負荷試験で観測されたCPU差の再現環境として不十分。
- Spanner Omniなら本物に近いSpanner engineで並列transactionを扱える可能性がある。

## スコープ

- Docker Composeにprofile付きSpanner Omni serviceを追加する。
- 通常sessionが拒否されるか、実コンテナで最小確認する。
- 現在のPHP high-level client / `laravel-spanner` 経路がSpanner Omniへ接続できるか確認する。
- 接続できる場合はLaravel FPM並列計測の対向先として使えるか確認する。

## 非スコープ

- Spanner Omniを通常のCI必須依存にすること。
- `google/cloud-spanner` や `colopl/laravel-spanner` のmux session対応をこのリポジトリ内で実装すること。
- 実Spanner本番環境での負荷試験。

## 計画

1. `spanner-omni` compose serviceを追加する。
2. Omniコンテナを起動し、database作成方法を確認する。
3. PHP high-level clientから通常sessionで接続し、拒否内容を確認する。
4. `laravel-spanner` fixtureでstartup warmupが通るか確認する。
5. 結果に応じて、ローカル並列計測の採否を決める。

## 進捗

- 2026-05-14: issue作成。
- 2026-05-14: `spanner-omni` compose serviceを追加し、`us-docker.pkg.dev/spanner-omni/images/spanner-omni:2026.r1-beta` を起動した。
- 2026-05-14: Omni CLIで `test-db` を作成できることを確認した。
- 2026-05-14: PHP high-level `Google\Cloud\Spanner\SpannerClient` の通常session経路はSpanner Omniに拒否されることを実機確認した。
- 2026-05-14: 低レベル `Google\Cloud\Spanner\V1\Client\SpannerClient` で `Session::setMultiplexed(true)` を指定すると、multiplexed session作成と `SELECT 1` は通ることを確認した。
- 2026-05-14: 現行Laravel + `colopl/laravel-spanner` high-level経路ではSpanner Omniを使えないと判断し、実Cloud Spanner無料枠での計測へ切り替えた。

## 検証

### Spanner Omni起動

```sh
docker compose --profile spanner-omni up -d spanner-omni
docker compose --profile spanner-omni exec -T spanner-omni /google/spanner/bin/spanner databases create test-db
```

結果:

- `Spanner is ready`
- `Creating database...done.`

### PHP high-level client通常session

```sh
docker compose run --rm \
  -e SPANNER_EMULATOR_HOST=spanner-omni:15000 \
  dev php -d extension=/workspace/ext/grpc/modules/grpc.so -r '... SpannerClient()->database()->execute("SELECT 1 AS n") ...'
```

結果:

```text
INVALID_ARGUMENT: Please use multiplexed sessions. Only Multiplexed sessions are supported in this environment.
```

stack trace上の失敗箇所:

- `Google\Cloud\Spanner\Operation->createSession()`
- `Google\Cloud\Spanner\Database->selectSession()`
- `Google\Cloud\Spanner\Database->execute()`

### PHP low-level V1 client multiplexed session

```php
$client = new Google\Cloud\Spanner\V1\Client\SpannerClient();
$session = $client->createSession(
    (new Google\Cloud\Spanner\V1\CreateSessionRequest())
        ->setDatabase('projects/local-project/instances/local-instance/databases/test-db')
        ->setSession((new Google\Cloud\Spanner\V1\Session())->setMultiplexed(true))
);
$response = $client->executeSql(
    (new Google\Cloud\Spanner\V1\ExecuteSqlRequest())
        ->setSession($session->getName())
        ->setSql('SELECT 1 AS n')
);
```

結果:

- multiplexed session名が返る。
- `SELECT 1` が1 rowを返す。

## 判断ログ

- Zenn記事ではSpanner Omniがmultiplexed session onlyと報告されているが、公式確認ではないため実コンテナで確認する。
- 実コンテナでも通常sessionは拒否されたため、現行の `google/cloud-spanner` high-level `Database` / `SessionPool` 経路ではSpanner OmniをLaravel実経路ベンチの対向先にできない。
- low-level V1 clientではmultiplexed sessionが使えるため、Spanner Omni自体と `php-grpc-lite` transportの相性問題ではない。
- 今回必要なLaravel + `colopl/laravel-spanner` 実経路の並列CPU計測は、ローカルemulator / Omniではなく、実Cloud Spannerの無料枠など通常session high-level経路が成立する環境で行うのが妥当。

## 完了条件

- [x] Spanner Omniが現PHP経路で使えるか、実機結果として判断できる。
- [x] 使えない場合はLaravel FPM並列計測runnerの対向先から除外できる。
- [x] 使えない場合は理由を具体的なエラーとともに記録し、別の計測方針へ戻れる。

## 修正コミット

未コミット。
