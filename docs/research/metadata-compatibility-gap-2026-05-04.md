# Metadata compatibility gap list (2026-05-04)

## Scope

`grpc-lite curl`、`grpc-lite native`、公式 `ext-grpc` の metadata semantics を追加照合するための洗い出し。
対象は unary と server streaming。ここでは実装変更ではなく、互換性確認で落としてはいけない観点と、次に作る fixture / test matrix を固定する。

## Confirmed

- 単一 raw binary value の `*-bin` request / initial / trailing metadata round-trip は curl 経路と native 経路で確認済み。
- native 経路は response initial / trailing metadata、`grpc-status`、`grpc-message`、compressed flag、HTTP status / content-type validation を generic に回収する。
- trailers-only error と binary metadata の代表ケースは native compatibility gate に入っている。

## Gaps

| 分類 | 未確認・問題候補 | 現状の観測 |
|---|---|---|
| duplicate request metadata | 同一 key の複数 ASCII / binary values が wire 上で複数 header field として送られるか | curl は array values を順に header 化する。native は `buildNativeRequestHeaders()` で `array<string,string>` に畳むため最後の値に潰れる |
| duplicate response metadata | 同一 key の initial / trailing metadata 複数 values が PHP API でどう見えるか | 単一 binary は確認済み。複数 binary は ext-grpc 側で最後だけ見える挙動を観測したが、ASCII と native は未確定 |
| request key normalization | mixed-case key、underscore、invalid character を ext-grpc と同じ扱いにできるか | curl は呼び出し元 key をほぼそのまま使う。native は PHP 側で固定 header を除外するだけで厳密 validation はない |
| reserved/system metadata | `grpc-status`、`grpc-message`、`grpc-timeout`、`grpc-encoding`、`te`、`content-type`、`user-agent`、pseudo header 相当を user metadata として渡した時の扱い | request metadata validation は未実装。固定 header と user metadata の衝突方針も未確定 |
| binary wire format | response の padded / unpadded base64、comma-separated binary values、不正 base64 の扱い | PHP 側は `,` split 後に `base64_decode()` する。strict decode ではないため不正値の ext-grpc 互換は未確認 |
| ASCII value shape | empty string、leading/trailing spaces、comma、UTF-8、非 ASCII、CR/LF injection | 明示テストなし。HTTP/2 metadata と gRPC metadata の許容文字差を ext-grpc と照合する必要がある |
| initial/trailing split | normal response、trailers-only error、server streaming の first payload 前 / final payload 後で custom metadata が分離されるか | 代表 error path は確認済み。duplicate custom metadata と streaming ordering は未確認 |
| metadata size | request / response metadata の key 数、value size、header list size 超過時の status / error shape | Go test server は response metadata count/value bytes を生成可能。native request は C 側に固定 header 容量と 512 bytes buffer が残っている |
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

## Required fixture work

- Go test server に ASCII metadata echo fixture を追加する。`x-bench-echo-*` の受信 values を initial / trailing にそのまま返す。
- duplicate response fixture を追加する。同一 key に複数 ASCII / binary values を明示的に設定できるようにする。
- reserved metadata fixture を追加する。server 側が実際に受け取った metadata を payload または response metadata で返せるようにする。
- metadata observation runner を追加する。`curl` / `native` / `ext-grpc` の `getMetadata()`、`getTrailingMetadata()`、status metadata を JSON で保存する。

## Native-specific implementation risks

- `buildNativeRequestHeaders()` が request metadata を `array<string,string>` に畳むため、duplicate values は現時点で保持できない。
- C extension 側の request header assembly は固定長配列・固定長文字列 buffer を使っており、metadata count / value size の互換性確認前に production 設計へ直す必要がある。
- response metadata は linked list で保持するため構造上は duplicate values を保持できるが、PHP normalize 後の ext-grpc 互換 shape は未確認。

## Next step

1. 観測用 fixture と runner を作る。
2. P0 matrix を `curl` / `native` / `ext-grpc` で実測し、JSON を `var/bench-results/` 配下へ保存する。
3. native request metadata を duplicate-safe / size-safe な表現へ直す。
4. ext-grpc と一致させるべき behavior と、仕様上明示エラーにする behavior を分けて checklist に反映する。
