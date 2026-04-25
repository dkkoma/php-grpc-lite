# コードリーディングガイド

> **目的**: ユーザコードから 1 本の RPC が出ていって戻ってくるまで、`src/Grpc/` 配下の何処を通るかを行番号付きで追える状態にする。新規参加者が「とりあえずこの順で読めば腹に落ちる」教材。
>
> **対象 commit**: `2aea02b`(Phase 0 + TLS 検証完了時点)

---

## 0. 全体観: 3 層構造

```
        ┌─────────────────────────────────────────────────┐
ユーザ  │  GreeterClient::SayHello($req)                  │   ← protoc-gen-php-grpc 相当
        │      ↓                                          │     (本リポでは tests/Integration/Fixtures/)
        ├─────────────────────────────────────────────────┤
SDK     │  Grpc\BaseStub::_simpleRequest()                │   ← ディスパッチ + interceptor chain
        │      ↓ (生成)                                   │
        │  Grpc\UnaryCall (or ServerStreamingCall)        │   ← libcurl 駆動の I/O
        │      ↑ extends                                  │
        │  Grpc\AbstractCall                              │   ← URL / headers / TLS の組立
        ├─────────────────────────────────────────────────┤
網      │  ext-curl → libcurl → OpenSSL → HTTP/2 →        │
        │  test-server (Go grpc) :50051(h2c) / :50052(h2) │
        └─────────────────────────────────────────────────┘
```

**読む順序**: 上から下に、**unary → server streaming** の順で追うのが負荷が低い。streaming の `curl_multi` ループは unary を理解した後に読む。

---

## 1. 起点: ユーザコードからの呼び出し

[tests/Integration/Fixtures/GreeterClient.php:19](tests/Integration/Fixtures/GreeterClient.php:19)

```php
public function SayHello(HelloRequest $argument, array $metadata = [], array $options = []): UnaryCall
{
    return $this->_simpleRequest(
        '/helloworld.Greeter/SayHello',
        $argument,
        [HelloReply::class, 'decode'],   // ← ext-grpc 互換のレガシー記法
        $metadata,
        $options,
    );
}
```

このクラスは本来 `protoc-gen-php-grpc` が生成するもの。本リポでは PoC 段階なので手書きで対応している。

**渡されている引数の意味**:

| 引数 | 役割 | 後段でどこに渡るか |
|---|---|---|
| `'/helloworld.Greeter/SayHello'` | gRPC method path = HTTP/2 の `:path` | URL の path 部 |
| `$argument` | リクエスト message 本体 | `serializeToString()` でバイト化 |
| `[HelloReply::class, 'decode']` | レスポンスデコード仕様 | [`Internal\Deserialize::apply()`](src/Grpc/Internal/Deserialize.php:26) で実体化 |
| `$metadata` | カスタム HTTP/2 ヘッダ | request headers にマージ |
| `$options` | per-call オプション(timeout, call_credentials_callback) | 同上 |

---

## 2. ディスパッチ層: BaseStub

[src/Grpc/BaseStub.php:42](src/Grpc/BaseStub.php:42)

```php
protected function _simpleRequest(...): UnaryCall {
    $continuation = function (...) {
        $call = new UnaryCall($this->getInnerChannel(), ...);
        $call->start($a);
        return $call;
    };
    if ($this->channel instanceof InterceptorChannel) {
        $chained = $this->buildUnaryChain($this->channel->getInterceptors(), $continuation);
        return $chained($method, $argument, $deserialize, $metadata, $options);
    }
    return $continuation($method, $argument, $deserialize, $metadata, $options);
}
```

### 2.1 Interceptor chain を読む(skipしてもよい)

[src/Grpc/BaseStub.php:135 `buildUnaryChain`](src/Grpc/BaseStub.php:135)

```php
$next = $innermost;
foreach (array_reverse($interceptors) as $interceptor) {
    $captured = $next;
    $next = static function (...) use ($interceptor, $captured): UnaryCall {
        return $interceptor->interceptUnaryUnary($m, $a, $d, $captured, $md, $o);
    };
}
return $next;
```

**読みどころ**: `array_reverse` してから巻き取ることで、ユーザが `[outer, inner]` と書いた順を「外側が先に呼ばれて、内側を `$continuation` として渡す」順に変換している。この順序の正しさは [tests/Integration/InterceptorTest.php](tests/Integration/InterceptorTest.php) が `outer:before → inner:before → 実行 → inner:after → outer:after` で検証している。

Interceptor を使わない 99% のケースでは ↑ は **読み飛ばして OK**。

### 2.2 InterceptorChannel の unwrap

[src/Grpc/BaseStub.php:123 `getInnerChannel`](src/Grpc/BaseStub.php:123)

`InterceptorChannel` は `Channel` を継承していて、`getInnerChannel()` で素の Channel を取り出せる。Call 実体は素の Channel に対して作る(intercept は dispatch 層の責務で、I/O 層は素のチャネルに対して動く)。

---

## 3. I/O 設定: AbstractCall

`UnaryCall` も `ServerStreamingCall` も、curl の **下準備** はほぼ全部 [src/Grpc/AbstractCall.php](src/Grpc/AbstractCall.php) に集約されている。

### 3.1 URL 組立

[src/Grpc/AbstractCall.php:52 `buildUrl`](src/Grpc/AbstractCall.php:52)

```php
$scheme = $this->channel->credentials->isInsecure() ? 'http' : 'https';
return $scheme . '://' . $this->channel->hostname . $this->method;
```

→ `http://test-server:50051/helloworld.Greeter/SayHello` または `https://...`。

### 3.2 HTTP version

[src/Grpc/AbstractCall.php:58 `getHttpVersion`](src/Grpc/AbstractCall.php:58)

| 経路 | curl 定数 | 意味 |
|---|---|---|
| h2c (insecure) | `CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE` | TLS なしで HTTP/2 直行(HTTP/1.1 Upgrade を経由しない) |
| h2 (TLS) | `CURL_HTTP_VERSION_2TLS` | TLS + ALPN で `h2` をネゴシエーション |

### 3.3 リクエストヘッダ

[src/Grpc/AbstractCall.php:71 `buildRequestHeaders`](src/Grpc/AbstractCall.php:71)

3 段階で構築:

1. **Channel の `update_metadata` callback** があれば、ユーザ metadata を変換させる
2. **per-call の `call_credentials_callback`** があれば、URL と method を渡して追加ヘッダ(典型的には `authorization`)を取得し、metadata にマージ
3. **必須ヘッダ + オプショナル `grpc-timeout` + ユーザ metadata** を curl 形式の `'Key: value'` 文字列リストに展開

ここがいわゆる **gax から `'call_credentials_callback'` で渡された ADC コールバック** が呼ばれる場所。

### 3.4 TLS オプション

[src/Grpc/AbstractCall.php:131 `applyTlsOptions`](src/Grpc/AbstractCall.php:131)

```php
if ($creds->rootCerts !== null) {
    if (defined('CURLOPT_CAINFO_BLOB')) {
        curl_setopt($ch, \CURLOPT_CAINFO_BLOB, $creds->rootCerts);
    } else {
        curl_setopt($ch, CURLOPT_CAINFO, self::writeTempPem(...));
    }
}
```

PEM はメモリ上の文字列で渡される(file path ではない)。**`*_BLOB` 系で in-memory のまま libcurl に渡すのが第一選択**で、古い環境向けに content-hash で keying した temp file fallback を持つ。

---

## 4. Unary の実行を追う

[src/Grpc/UnaryCall.php:31 `start`](src/Grpc/UnaryCall.php:31)

```php
$serialized = $argument->serializeToString();              // protobuf wire bytes
$frame = "\x00" . pack('N', strlen($serialized)) . $serialized;  // gRPC framing
```

> **gRPC framing**: `1 byte 圧縮フラグ + 4 byte big-endian length + payload`。圧縮は本実装では未対応(常に 0)。

```php
$this->ch = $this->initCurl();
curl_setopt_array($this->ch, [
    CURLOPT_POSTFIELDS     => $frame,
    CURLOPT_HEADERFUNCTION => $this->onHeader(...),
    CURLOPT_WRITEFUNCTION  => $this->onBodyChunk(...),
    ...
]);
$this->applyTlsOptions($this->ch);
```

> ここまでが「I/O は始まっていない」状態。 **実際の通信は次の `wait()` で `curl_exec` を呼んだ時に走る**。

[src/Grpc/UnaryCall.php:57 `wait`](src/Grpc/UnaryCall.php:57)

```php
curl_exec($this->ch);  // ← ここで完全にブロックして I/O が走る
$errno = curl_errno($this->ch);
curl_close($this->ch);

if ($errno !== 0) {
    return [null, $this->makeStatus(STATUS_UNAVAILABLE, "curl error ($errno): $errMsg")];
}
return [$this->parseResponseFrame(), $this->buildStatusFromTrailers()];
```

`curl_exec` の中で起きること:

1. libcurl が HTTP/2 接続を確立(prior knowledge / ALPN)
2. リクエストヘッダを送出
3. ボディ(framed request)を送出
4. **レスポンスヘッダ受信** → [`onHeader`](src/Grpc/UnaryCall.php:127) が行ごとに呼ばれる
5. **レスポンスボディ受信** → [`onBodyChunk`](src/Grpc/UnaryCall.php:143) が呼ばれて `$bodyStarted = true`、body 蓄積
6. **HTTP/2 trailers 受信** → 同じ `onHeader` が呼ばれる(が今度は `$bodyStarted === true`)

### 4.1 ヘッダか trailer かの分離

[src/Grpc/UnaryCall.php:127 `onHeader`](src/Grpc/UnaryCall.php:127)

```php
if (!$this->bodyStarted) {
    $this->responseHeaders[$key][] = $val;
} else {
    $this->responseTrailers[$key][] = $val;
}
```

**これが本実装の鍵**。HTTP/2 の trailers は body の後に来るので、最初の WRITEFUNCTION 呼び出し時刻を境に振り分けるだけで分離できる。複雑な状態機械は不要。

### 4.2 レスポンスのデコード

[src/Grpc/UnaryCall.php:96 `parseResponseFrame`](src/Grpc/UnaryCall.php:96)

```php
$len = unpack('N', substr($this->body, 1, 4))[1];
$payload = substr($this->body, 5, $len);
return Internal\Deserialize::apply($this->deserialize, $payload);
```

[src/Grpc/Internal/Deserialize.php:26 `apply`](src/Grpc/Internal/Deserialize.php:26) の中で、`[ClassName::class, 'decode']` 形式のときは「`new ClassName()` してから `mergeFromString($bytes)`」に変換する。これが **ext-grpc 互換の核心の 1 つ** で、google/protobuf のメッセージクラスに静的 `decode` がないことの吸収。

### 4.3 Status オブジェクトの組立

[src/Grpc/UnaryCall.php:109 `buildStatusFromTrailers`](src/Grpc/UnaryCall.php:109)

```php
$code = (int)($this->responseTrailers['grpc-status'][0] ?? STATUS_UNKNOWN);
$message = $this->responseTrailers['grpc-message'][0] ?? '';
return $this->makeStatus($code, $message);
```

`stdClass` で `code` / `details` / `metadata` の 3 プロパティを持つオブジェクトを返す。これは `gax-php/src/ApiException.php:158` あたりで `property_exists($status, 'metadata')` と読まれる形と一致する。

---

## 5. Server streaming の実行を追う

unary との差分だけを読めばよい。

### 5.1 start

[src/Grpc/ServerStreamingCall.php:38](src/Grpc/ServerStreamingCall.php:38) は **UnaryCall::start とほぼ同じ**。違うのは `WRITEFUNCTION` がフレーム境界を再構成して `$pending` キューに積むこと。

### 5.2 onBodyChunk: バッファとフレーム再構成

[src/Grpc/ServerStreamingCall.php:185 `onBodyChunk`](src/Grpc/ServerStreamingCall.php:185)

```php
$this->buffer .= $chunk;
while (strlen($this->buffer) >= 5) {
    $len = unpack('N', substr($this->buffer, 1, 4))[1];
    if (strlen($this->buffer) < 5 + $len) {
        break;  // 待機: フレームの続きが次の chunk に入っている
    }
    $this->pending[] = substr($this->buffer, 5, $len);
    $this->buffer = substr($this->buffer, 5 + $len);
}
```

**フレームは callback 境界をまたぎうる** ので、内部バッファに溜めて、完成したフレームだけ `$pending` に出す。PoC で実機検証済 → [`poc/client/server_streaming.php`](poc/client/server_streaming.php) のログを見ると buffer leftover = 0 が確認できる。

### 5.3 responses(): curl_multi の Generator

[src/Grpc/ServerStreamingCall.php:62 `responses`](src/Grpc/ServerStreamingCall.php:62)

ここが **Phase 0 で最も精神的負荷が高い** 部分。読みどころ:

```php
$mh = curl_multi_init();
curl_multi_add_handle($mh, $this->ch);

try {
    do {
        curl_multi_exec($mh, $running);                  // I/O を 1 ラウンド進める

        while ($info = curl_multi_info_read($mh)) {      // 完了情報を吸い出す
            if ($info['result'] !== CURLE_OK) {
                $errCode = $info['result'];
            }
        }

        while ($this->pending !== []) {                  // 完成フレームを caller に
            yield Internal\Deserialize::apply($this->deserialize, array_shift($this->pending));
        }

        if ($running > 0) {
            curl_multi_select($mh, 1.0);                 // I/O 待ち(1秒上限)
        }
    } while ($running > 0);

    // ↓ 最終ラウンドで来たフレームを drain
    while ($this->pending !== []) {
        yield Internal\Deserialize::apply(...);
    }

    $this->finalStatus = ... // trailers から組み立て
} finally {
    // curl ハンドル解放
}
```

**読み方のコツ**:

- `curl_multi_exec` 自体は **ノンブロッキング**。WRITEFUNCTION/HEADERFUNCTION を発火させて、進められるだけ進めて return する
- yield は `curl_multi_exec` と `curl_multi_select` の間でしか起きない(WRITEFUNCTION の中からは yield できない)
- `curl_multi_select` で **次の I/O イベントまでブロックする** が、`yield` で suspend してる間は当然走らない
- caller が foreach から早期 break すると Generator が destruct → finally で curl ハンドル解放

これにより **真の incremental streaming** が実現される。検証は [tests/Integration/ServerStreamingTest.php:43 `testYieldsAreIncrementalNotBatched`](tests/Integration/ServerStreamingTest.php:43) が「yield 間のギャップが server の 100ms cadence と一致」をアサート。

### 5.4 getStatus

`responses()` の生成器を完走させた後でないと `finalStatus` は埋まらない。途中で break した場合は `finally` ブロックで trailers ベースの status を入れて辻褄を合わせている([src/Grpc/ServerStreamingCall.php:115](src/Grpc/ServerStreamingCall.php:115))。

---

## 6. 横断的トピック

### 6.1 「素の curl」 と 「extension の中の curl」

本実装は ext-curl を呼んでいるだけ。後段(Phase 2)で C 拡張化するときも、libcurl の API は同じなので **PHP 側で書いた振る舞いがそのまま C への翻訳手順書になる**。これは Phase 0 を純 PHP でやる最大のリターン。

### 6.2 PoC コードとの対応関係

`src/Grpc/` の本実装と `poc/client/` のスパイクは **同じことを別の抽象度で書いている** 関係:

| トピック | スパイク | 本実装 |
|---|---|---|
| unary | [poc/client/unary.php](poc/client/unary.php) | [src/Grpc/UnaryCall.php](src/Grpc/UnaryCall.php) |
| server streaming(blocking) | [poc/client/server_streaming.php](poc/client/server_streaming.php) | (採用せず) |
| server streaming(curl_multi) | [poc/client/server_streaming_multi.php](poc/client/server_streaming_multi.php) | [src/Grpc/ServerStreamingCall.php](src/Grpc/ServerStreamingCall.php) |

**スパイク → 本実装の差分** を読むと「BaseStub 階層化のためにどこを切り出したか」が見える。

### 6.3 wire を覗く

実装の振る舞いに疑問が出たら、生 curl で叩いたログがある:

```bash
docker compose run --rm dev bash -c '
printf "\x00\x00\x00\x00\x07\x0a\x05World" > /tmp/req.bin
curl --http2-prior-knowledge -H "content-type: application/grpc" -H "te: trailers" \
     --data-binary @/tmp/req.bin -i http://test-server:50051/helloworld.Greeter/SayHello | od -An -tx1
'
```

応答のバイト列・trailer の到着順序などを目視で確認できる。本実装と挙動がズレてないかの ground truth として常に頼りになる。

### 6.4 テストとの対応

| 検証したい挙動 | テスト |
|---|---|
| unary 基本フロー | [tests/Integration/UnaryTest.php:18](tests/Integration/UnaryTest.php:18) |
| 受信 metadata と trailer の取り出し | [tests/Integration/UnaryTest.php:36](tests/Integration/UnaryTest.php:36) |
| server streaming の順序と件数 | [tests/Integration/ServerStreamingTest.php:18](tests/Integration/ServerStreamingTest.php:18) |
| **真の incremental yield** であること | [tests/Integration/ServerStreamingTest.php:43](tests/Integration/ServerStreamingTest.php:43) |
| Interceptor chain の入れ子順序 | [tests/Integration/InterceptorTest.php:21](tests/Integration/InterceptorTest.php:21) |
| TLS の正常系 / 拒否 | [tests/Integration/TlsTest.php](tests/Integration/TlsTest.php) |

「この挙動はどこで担保されてる?」が浮かんだら、まず該当テストを読みに行くのが速い。

---

## 7. 推奨の読書順

迷ったらこの順:

1. [tests/Integration/UnaryTest.php](tests/Integration/UnaryTest.php) — ユーザ視点のゴール
2. [tests/Integration/Fixtures/GreeterClient.php](tests/Integration/Fixtures/GreeterClient.php) — generated stub の形
3. [src/Grpc/BaseStub.php](src/Grpc/BaseStub.php) `_simpleRequest` のみ(interceptor 周りは飛ばす)
4. [src/Grpc/UnaryCall.php](src/Grpc/UnaryCall.php) `start` → `wait` → `onHeader` → `onBodyChunk` → `parseResponseFrame`
5. [src/Grpc/AbstractCall.php](src/Grpc/AbstractCall.php) `buildUrl` `buildRequestHeaders` を辞書的に
6. [src/Grpc/Internal/Deserialize.php](src/Grpc/Internal/Deserialize.php) — ext-grpc 互換の小細工
7. ここで `vendor/bin/phpunit tests/Integration/UnaryTest.php` を 1 度実行
8. [src/Grpc/ServerStreamingCall.php](src/Grpc/ServerStreamingCall.php) `responses` の `curl_multi` ループ
9. (任意)[src/Grpc/Interceptor.php](src/Grpc/Interceptor.php) と [src/Grpc/BaseStub.php:135](src/Grpc/BaseStub.php:135) の chain 構築
10. (任意)[src/Grpc/AbstractCall.php:131 `applyTlsOptions`](src/Grpc/AbstractCall.php:131) で TLS 経路

ステップ 7 の段階で「unary は完全に頭の中で trace できる」状態になっていれば、残りはバリエーション。

---

## 関連ドキュメント

- [SPEC.md](SPEC.md) — 設計判断と未決事項
- [api-surface.md](api-surface.md) — gax/google-cloud-php との互換要件
