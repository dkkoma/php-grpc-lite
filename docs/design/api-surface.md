# API サーフェス調査結果

> **調査対象**: `googleapis/gax-php` (HEAD)、`googleapis/google-cloud-php` の代表 3 パッケージ(PubSub / Storage / Spanner)
> **調査日**: 2026-04-25
> **方法**: ソースに対する `grep` ベースの静的解析。`_research/` 以下にクローン済み(gitignored)。

php-grpc-lite が `ext-grpc` のドロップイン代替として満たすべき API サーフェスを、上流コードの実際の呼び出し箇所から抽出した。SPEC.md §4.5 の見立てを実地で裏取りした位置づけ。

---

## 1. 重要な前提: 拡張モジュール名

`gax-php` は `extension_loaded('grpc')` で gRPC サポートの有無を判定する(`src/GrpcSupportTrait.php:46`)。**本拡張は必ず `grpc` という名前で登録する**こと。これにより `ext-grpc` と同居不可能になるが、それは「ドロップイン代替」の定義そのもの。

---

## 2. 分類: 必須 / 後回し / 範囲外

### 2.1 必須(Phase 0 から実装)

| シンボル | 種別 | 出現箇所(代表) |
|---|---|---|
| `Grpc\BaseStub` | クラス(継承される) | gax `GrpcTransport`, 各 `*Client` 生成コード |
| `Grpc\Channel` | クラス(直接 `new`) | gax `GrpcTransport::__construct` |
| `Grpc\ChannelCredentials` | クラス(静的メソッド) | gax / 各 cloud Client |
| `Grpc\AbstractCall` | クラス(継承される) | `ForwardingCall` の親 |
| `Grpc\UnaryCall` | クラス(返り値型) | `ForwardingUnaryCall`, `MockUnaryCall` |
| `Grpc\ServerStreamingCall` | クラス(返り値型) | `ServerStreamingCallWrapper`, `MockServerStreamingCall` |
| `Grpc\Interceptor` | クラス(継承 + 静的) | gax `GrpcTransport::__construct` |
| `Grpc\STATUS_*` | 定数(17 個) | Spanner `Result.php` で `STATUS_UNAVAILABLE` を直接参照 |

### 2.2 後回し(Phase 0 では未実装でよい)

| シンボル | 理由 |
|---|---|
| `Grpc\ClientStreamingCall` | 現スコープが unary + server streaming のため |
| `Grpc\BidiStreamingCall` | 同上 |
| `Grpc\CallInvoker` | gax は `grpc_call_invoker` stubOpt 経由でのみ参照。grpc-gcp 連携時のみ実体が必要 |

### 2.3 範囲外(別パッケージ / 別拡張の責務)

| シンボル | 提供元 | 備考 |
|---|---|---|
| `Grpc\Gcp\Config` | `grpc/grpc-gcp` (Composer パッケージ) | gax は `extension_loaded('sysvshm')` かつ `gcpApiConfigPath` 指定時にのみ使用。本拡張の API ではない |
| `Grpc\Gcp\ApiConfig` | 同上 | 同上 |

#### 補足: `Grpc\Timeval` の扱い

gax は `'timeout' => int(microseconds)` を options で渡すだけで Timeval オブジェクトを構築しない(`gax-php/src/Transport/GrpcTransport.php:357`)。拡張内部の deadline 表現も Timeval である必要はなく、monotonic 時刻 + offset で十分。

ただしレガシーな ext-grpc 利用コードが `new Grpc\Timeval(...)` や `Grpc\Timeval::infFuture()` を直接使っている可能性があるため、**Phase 0 で薄く実装**する方針。提供する API は値オブジェクトと数本のファクトリのみ:

- `__construct(int $microseconds)`
- `static infFuture(): Timeval`
- `static infPast(): Timeval`
- `static now(): Timeval`(monotonic clock)
- `microtime(bool $absolute = false): string` — `'timeout'` への変換用
- `add(Timeval $other): Timeval` / `subtract(Timeval $other): Timeval`(必要に応じて)

ワイヤ送出では HTTP/2 ヘッダ `grpc-timeout`(例: `30S`, `30000m`, `30000000u`)に変換する。Timeval はあくまで PHP 側の値型。

### 2.4 deprecated(対応不要)

| シンボル | 理由 |
|---|---|
| `Google\ApiCore\Transport\Grpc\UnaryInterceptorInterface` | gax 内部の deprecated。`Grpc\Interceptor` への移行が推奨されている |

---

## 3. 必須シンボルの詳細仕様

### 3.1 `Grpc\BaseStub`

派生クラス: `Google\ApiCore\Transport\GrpcTransport`、各 `*GrpcClient`(自動生成)

**コンストラクタ:**
```php
public function __construct(string $hostname, array $opts, ?Channel $channel = null)
```

`$opts` で受け取る既知のキー:
- `'credentials'` … `ChannelCredentials` インスタンス(必須)
- `'update_metadata'` … callable(metadata array → metadata array)
- `'grpc.primary_user_agent'` … string
- `'grpc_call_invoker'` … object(grpc-gcp 用、未実装でよい)
- その他 `'grpc.*'` キー … 多数あるが Phase 0 では無視可

**派生クラスから呼ばれる protected メソッド:**

| メソッド | シグネチャ |
|---|---|
| `_simpleRequest` | `($method, $argument, $deserialize, array $metadata = [], array $options = []): UnaryCall` |
| `_serverStreamRequest` | `($method, $argument, $deserialize, array $metadata = [], array $options = []): ServerStreamingCall` |
| `_clientStreamRequest` | `($method, $deserialize, array $metadata = [], array $options = []): ClientStreamingCall` *(後回し)* |
| `_bidiRequest` | `($method, $deserialize, array $metadata = [], array $options = []): BidiStreamingCall` *(後回し)* |

`$method` は `"/package.Service/Method"` 形式の文字列。`$argument` は `Google\Protobuf\Internal\Message`。`$deserialize` は callable `(string $bytes): Message`。

**呼び出し例(`gax/GrpcTransport.php:282`):**
```php
$unaryCall = $this->_simpleRequest(
    '/' . $call->getMethod(),
    $call->getMessage(),
    [$call->getDecodeType(), 'decode'],
    $options['headers'] ?? [],
    $this->getCallOptions($options)
);
```

### 3.2 `Grpc\Channel`

```php
public function __construct(string $hostname, array $opts)
```

`$opts` は §3.1 と同じキー群を受け取る。Channel 自体に対して gax が直接呼ぶインスタンスメソッドは確認できず — `Interceptor::intercept()` の引数/返り値として渡されるのみ。

### 3.3 `Grpc\ChannelCredentials`

確認された静的メソッド:

| メソッド | 用途 | 出現箇所 |
|---|---|---|
| `createSsl(?string $rootCerts = null, ?string $privateKey = null, ?string $certChain = null): ChannelCredentials` | TLS / mTLS | `gax/GrpcTransport.php:148-150` |
| `createInsecure(): ChannelCredentials` | プレーン HTTP/2 | PubSub `PublisherClient.php:717`, Spanner `SpannerClient.php:853` |
| `createDefault(): ChannelCredentials` | ADC 経由 | 直接の参照は未確認だが ext-grpc 互換のため必要 |

すべて static。返り値は不透明なオブジェクト(中身は `Channel` コンストラクタに渡されて消費される)。

### 3.4 `Grpc\Interceptor`

abstract class。派生クラスはユーザーが定義する。

**確認された静的メソッド:**
```php
public static function intercept(Channel $channel, Interceptor|array $interceptors): Channel
```

返り値は元の Channel をラップした(Interceptor がチェーンされた)Channel。ext-grpc 実装では `InterceptorChannel` という派生クラスを返す。

**派生クラスのオーバーライド対象**(ext-grpc 仕様):
- `interceptUnaryUnary($method, $argument, $deserialize, $continuation, array $metadata, array $options)`
- `interceptUnaryStream(...)`
- `interceptStreamUnary(...)`
- `interceptStreamStream(...)`

gax-php は静的メソッド `intercept()` のみを呼ぶ(派生クラスは実装しない)。エンドユーザーがカスタム Interceptor を書ける形にしておく必要がある。

### 3.5 Call クラス群

#### `Grpc\UnaryCall`

| メソッド | 返り値 | 用途 |
|---|---|---|
| `wait()` | `[Message $response, stdClass $status]` | レスポンス受信完了まで待機 |
| `cancel()` | `void` | キャンセル |
| `getMetadata()` | `array` | initial metadata(headers) |
| `getTrailingMetadata()` | `array` | trailing metadata |
| `getPeer()` | `string` | ext-grpc 互換のため必要だが gax は呼ばず |

#### `Grpc\ServerStreamingCall`

| メソッド | 返り値 | 用途 |
|---|---|---|
| `responses()` | `iterable<Message>` | ストリームから順次受信 |
| `getStatus()` | `stdClass` | 完了後のステータス |
| `getMetadata()` | `array` | initial metadata |
| `getTrailingMetadata()` | `array` | trailing metadata |
| `cancel()` | `void` | キャンセル |

`responses()` は Generator を返すのが自然(ext-grpc は Generator を返す)。

### 3.6 Status オブジェクトの形

`UnaryCall::wait()` の第 2 戻り値および `ServerStreamingCall::getStatus()` の返り値は **stdClass**(または同等のプロパティアクセスができるオブジェクト)で、以下のプロパティを持つ:

| プロパティ | 型 | 内容 |
|---|---|---|
| `code` | `int` | gRPC status code(0–16、`Grpc\STATUS_*` に対応) |
| `details` | `string` | エラーメッセージ(trailer の `grpc-message` をデコードしたもの) |
| `metadata` | `array` | trailing metadata(`grpc-status-details-bin` 等を含む) |

参照元:
- `gax/Transport/GrpcTransport.php:312-321` (`$status->metadata`, `$status->code`)
- `gax/ClientStream.php:97-102`
- `gax/BidiStream.php:165-176`
- `gax/ApiException.php:158-163`

### 3.7 `Grpc\STATUS_*` 定数

全 17 個。値は gRPC 標準の status code(`google.rpc.Code`)と一致する整数。

| 定数 | 値 |
|---|---|
| `STATUS_OK` | 0 |
| `STATUS_CANCELLED` | 1 |
| `STATUS_UNKNOWN` | 2 |
| `STATUS_INVALID_ARGUMENT` | 3 |
| `STATUS_DEADLINE_EXCEEDED` | 4 |
| `STATUS_NOT_FOUND` | 5 |
| `STATUS_ALREADY_EXISTS` | 6 |
| `STATUS_PERMISSION_DENIED` | 7 |
| `STATUS_RESOURCE_EXHAUSTED` | 8 |
| `STATUS_FAILED_PRECONDITION` | 9 |
| `STATUS_ABORTED` | 10 |
| `STATUS_OUT_OF_RANGE` | 11 |
| `STATUS_UNIMPLEMENTED` | 12 |
| `STATUS_INTERNAL` | 13 |
| `STATUS_UNAVAILABLE` | 14 |
| `STATUS_DATA_LOSS` | 15 |
| `STATUS_UNAUTHENTICATED` | 16 |

実際にコードから直接参照されているのは `STATUS_INTERNAL`(gax テストモック)と `STATUS_UNAVAILABLE`(Spanner 再試行判定)のみだが、ユーザー実装からの参照のため全て定義する必要がある。

---

## 4. 呼び出しオプション(`$options` の中身)

`_simpleRequest` 等の第 5 引数 `$options` で gax が渡す既知のキー:

| キー | 型 | 用途 |
|---|---|---|
| `call_credentials_callback` | callable | per-call 認証ヘッダ生成。`(string $serviceUrl, string $methodName) => array<string, string>` を返す |
| `timeout` | `int` | マイクロ秒単位の deadline(`timeoutMillis * 1000`) |
| `flags` | `int` | gRPC write flags(WRITE_BUFFER_HINT 等)。Phase 0 では無視可 |
| `cancelled` | `bool` | 内部用 |

`call_credentials_callback` は ADC 統合の中核。gax `CredentialsWrapper::getAuthorizationHeaderCallback()` が返すコールバックがそのまま渡される。
`Grpc\CallCredentials::createFromPlugin()` は同じ callback shape の wrapper として扱い、per-call option `call_credentials` に渡された場合に metadata を生成する。

---

## 5. 未確認事項 / 追加調査の余地

- [x] `Grpc\CallCredentials::createFromPlugin()` は callback wrapperとして実装済み
- [ ] `Grpc\Timeval` を実装するか — gax は使わないが ext-grpc ユーザーコードや古い世代の generated stub が触る可能性
- [ ] ext-grpc が定義している INI 設定(`grpc.use_local_subchannel_pool` 等)の互換要件
- [ ] `BaseStub::getTarget()` / `BaseStub::waitForReady()` 等の補助メソッドが実コードで呼ばれているか
- [ ] generated stub(`*GrpcClient.php`)の典型的な実装 — `protoc-gen-php-grpc` 出力例の確認

---

## 6. SPEC.md への反映事項

- 拡張モジュール名は `grpc` で確定(§1)
- `Grpc\Timeval` は当面 **後回し** に分類変更(§2.3)
- `Grpc\Gcp\*` 系は完全に **範囲外**(別パッケージ提供)で確定
- gax は `_clientStreamRequest` / `_bidiRequest` を呼ぶが、現スコープではこれらは「BaseStub に存在するが実装は未対応エラーを投げる」スタブで十分

これらは SPEC.md §4.5 と §6 の更新で反映する。
