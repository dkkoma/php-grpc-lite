# Metadata compatibility gap list (2026-05-04)

## Scope

`grpc-lite curl`、`grpc-lite native`、公式 `ext-grpc` の metadata semantics を追加照合するための洗い出し。
対象は unary と server streaming。ここでは実装変更ではなく、互換性確認で落としてはいけない観点と、次に作る fixture / test matrix を固定する。

## Confirmed

- 単一 raw binary value の `*-bin` request / initial / trailing metadata round-trip は curl 経路と native 経路で確認済み。
- native 経路は response initial / trailing metadata、`grpc-status`、`grpc-message`、compressed flag、HTTP status / content-type validation を generic に回収する。
- trailers-only error と binary metadata の代表ケースは native compatibility gate に入っている。
- 2026-05-04 の追加観測で、curl / native は同一 key 複数 values を PHP API 上 `list<string>` として保持できる。公式 ext-grpc は同一 key 複数 values のうち最後の value だけを PHP API に出す。
- php-grpc-lite はgRPC仕様準拠を優先し、同一 key 複数 values を保持する。ext-grpc PHP API の最後値のみ可視な挙動には寄せない。
- native request metadata は `array<string, list<string>>` として C extension へ渡し、nghttp2 request header list を動的に組み立てる形へ修正済み。固定 8 headers / 512 bytes buffer による欠落・切り詰めは P0 観測ケースでは解消済み。

## Gaps

| 分類 | 未確認・問題候補 | 現状の観測 |
|---|---|---|
| duplicate request metadata | 同一 key の複数 ASCII / binary values が wire 上で複数 header field として送られるか | curl / native は複数 values を保持。ext-grpc は response PHP API では最後値のみ可視だが、php-grpc-lite は仕様準拠で全 values を保持 |
| duplicate response metadata | 同一 key の initial / trailing metadata 複数 values が PHP API でどう見えるか | curl / native は全 values、ext-grpc は最後値のみ可視。php-grpc-lite は全 values 保持を仕様とする |
| request key normalization | mixed-case key、underscore、invalid character を ext-grpc と同じ扱いにできるか | curl は呼び出し元 key をほぼそのまま使う。native は PHP 側で固定 header を除外するだけで厳密 validation はない |
| reserved/system metadata | `grpc-status`、`grpc-message`、`grpc-timeout`、`grpc-encoding`、`te`、`content-type`、`user-agent`、pseudo header 相当を user metadata として渡した時の扱い | request metadata validation は未実装。固定 header と user metadata の衝突方針も未確定 |
| binary wire format | response の padded / unpadded base64、comma-separated binary values、不正 base64 の扱い | PHP 側は `,` split 後に `base64_decode()` する。strict decode ではないため不正値の ext-grpc 互換は未確認 |
| ASCII value shape | empty string、leading/trailing spaces、comma、UTF-8、非 ASCII、CR/LF injection | 明示テストなし。HTTP/2 metadata と gRPC metadata の許容文字差を ext-grpc と照合する必要がある |
| initial/trailing split | normal response、trailers-only error、server streaming の first payload 前 / final payload 後で custom metadata が分離されるか | 代表 error path は確認済み。duplicate custom metadata と streaming ordering は未確認 |
| metadata size | request / response metadata の key 数、value size、header list size 超過時の status / error shape | Go test server は response metadata count/value bytes を生成可能。native request の小さい固定 buffer は修正済みだが、HTTP/2 header list size 境界は未確認 |
| authority / user-agent | `grpc.primary_user_agent`、gax の `x-goog-api-client`、`:authority` override、user supplied `user-agent` の扱い | 実用 Spanner 経路では通っているが、衝突・override の互換性は未確認 |

## High-priority test matrix

まず観測テストとして `curl` / `native` / `ext-grpc` を同じ fixture に当て、assert を強くしすぎず出力形を比較する。

| 優先 | ケース | call type | 期待する観測 |
|---|---|---|---|
| P0 | duplicate ASCII request metadata echo | unary | 同一 key 複数 values の保持順、ext-grpc の PHP API shape |
| P0 | duplicate binary request metadata echo | unary | raw binary values の保持数、順序、comma/base64 の扱い |
| P0 | duplicate ASCII / binary response initial/trailing | unary | initial / trailing それぞれの複数 values が見えるか |
| P0 | native request metadata capacity | unary | header 数と value length が固定 buffer によって欠落・切り詰めされないか |
| P1 | reserved `grpc-*` / fixed headers injection | unary | reject / override / pass-through の ext-grpc 互換 |
| P1 | uppercase / invalid key / empty value | unary | key normalization と validation policy |
| P1 | trailers-only error with custom trailers | unary, server streaming | status metadata と custom trailing metadata の分離 |
| P1 | server streaming initial/trailing duplicate metadata | server streaming | first payload 前、final payload 後の metadata visibility |
| P2 | large response metadata count/value bytes | unary | header list size 境界、memory、status shape |
| P2 | authority / user-agent / gax headers collision | unary | Spanner 実用 metadata と診断 header の衝突回避 |

## Fixture / runner status

- Go test server に ASCII / binary metadata echo fixture を追加済み。`x-bench-echo-*` の受信 values を initial / trailing にそのまま返す。
- duplicate response fixture を追加済み。同一 key に複数 ASCII / binary values を明示的に設定できる。
- metadata observation runner を追加済み。`curl` / `native` / `ext-grpc` の `getMetadata()`、`getTrailingMetadata()`、status metadata を JSON で保存する。
- reserved metadata fixture を追加する。server 側が実際に受け取った metadata を payload または response metadata で返せるようにする。

## Observation result

実行コマンド:

```sh
BENCH_TAG=metadata-compat-20260504-final ./bench/phase2/compare-metadata-compat.sh
```

保存先:

- `var/bench-results/phase2-metadata-compat-metadata-compat-20260504-final-curl-php-grpc-lite.json`
- `var/bench-results/phase2-metadata-compat-metadata-compat-20260504-final-native-php-grpc-lite.json`
- `var/bench-results/phase2-metadata-compat-metadata-compat-20260504-final-ext-ext-grpc.json`

| ケース | curl | native | ext-grpc |
|---|---|---|---|
| duplicate ASCII unary | initial/trailing とも全 values 可視 | initial/trailing とも全 values 可視 | initial/trailing とも最後 value のみ可視 |
| duplicate binary unary | initial/trailing とも全 raw binary values 可視 | initial/trailing とも全 raw binary values 可視 | initial/trailing とも最後 raw binary value のみ可視 |
| many headers + 600B value | echo metadata が欠落・切り詰めなく round-trip | echo metadata が欠落・切り詰めなく round-trip | echo metadata が欠落・切り詰めなく round-trip |
| duplicate ASCII server streaming | initial/trailing とも全 values 可視 | initial/trailing とも全 values 可視 | initial/trailing とも最後 value のみ可視 |

この差分は wire 上の送受信失敗ではなく、PHP API の metadata map shape 差として扱う。php-grpc-lite はgRPC metadata semanticsを優先し、同一 key 複数 values を最後値へ畳まない。ext-grpc PHP API の最後値のみ可視な挙動は互換差分としてdocsに残す。

## Native-specific implementation risks

- `buildHttp2RequestHeaders()` が request metadata を `array<string,string>` に畳む問題は修正済み。
- C extension 側の request header assembly が固定長配列・固定長文字列 buffer を使う問題は、persistent unary / stream / diagnostic unary / unary batch で修正済み。
- response metadata は linked list と PHP normalize で duplicate values を保持する。

## Next step

1. HTTP/2 header list size 境界の観測 fixture を追加する。
2. request metadata validation / filtering 後の残差分として、curl stable route の leading space value strip を扱うか判断する。
