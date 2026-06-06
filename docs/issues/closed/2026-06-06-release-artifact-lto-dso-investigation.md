# release artifact LTO DSO investigation

Status: Closed
Branch: codex/tighten-symbol-visibility
Date: 2026-06-06

## 目的

公開 prebuilt artifact の build profile 候補として、GCC LTO / Clang ThinLTO / newer toolchain を使った場合に `grpc.so` の DSO 上で見える差分を確認する。

性能採否ではなく、DSO から読める範囲の symbol visibility、section size、relocation、compiler inline report を整理する。

## 背景

`0.0.14` で `-fvisibility=hidden` を導入し、release artifact の dynamic export は `get_module` のみに絞られた。その後、公開 artifact や PIE/source build の default 最適化として `-O3` / LTO / `-fno-semantic-interposition` を入れるべきかを検討した。

まずは default を置き換えず、DSO だけで確認できる範囲を調査した。

## スコープ

- `-g -O2` default build と `-g -O2 + LTO` variants の DSO 比較。
- GCC 14, GCC 16, Clang 22 の toolchain差分確認。
- `readelf`, `size`, `file` による symbol / relocation / section確認。
- compiler inline report による optimizer inline の傾向確認。

## 非スコープ

- LTO / ThinLTO / PGO artifact の採用。
- benchmark による性能判断。
- release workflow への profile追加。
- PIE/source build default の変更。

## 調査条件

実行環境:

- Docker compose `dev` container
- Architecture: `linux/arm64`
- PHP: `8.4.20`
- Base default compiler: GCC 14.2.0

確認した toolchain:

- GCC 14.2.0
- GCC 16.1.0
- Clang / LLD 22.1.7

GCC 16 は Trixie 標準 `binutils` では AArch64 の `.aeabi_subsection` / `.aeabi_attribute` directive を assembler が読めず、configure sanity check が失敗した。そのため調査用の一時 container では Debian sid の `binutils` も導入した。

Clang 22 は Debian sid / LLVM apt repository 由来の `clang-22` / `lld-22` を一時 container に導入した。

## DSO比較

`phpize && ./configure --enable-grpc && make` の default CFLAGS は `-g -O2` だった。以前の preliminary check では `CFLAGS=""` を明示してしまい、autoconf default の `-g -O2` を潰していたため、その結果は採用しない。

採用した比較条件:

| label | compiler | flags |
| --- | --- | --- |
| default gcc14 | GCC 14.2.0 | default `-g -O2` |
| gcc14 LTO | GCC 14.2.0 | `-g -O2 -flto` |
| gcc16 LTO | GCC 16.1.0 | `-g -O2 -flto` |
| clang22 ThinLTO | Clang 22.1.7 | `-g -O2 -flto=thin`, `lld-22` |

結果:

| profile | file size | `.text` | JUMP_SLOT | RELATIVE | local funcs | exported defined symbols |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| default gcc14 | 567,984 | 56,884 | 161 | 157 | 171 | `get_module` |
| gcc14 LTO | 592,272 | 54,812 | 158 | 152 | 114 | `get_module` |
| gcc16 LTO | 581,000 | 54,040 | 158 | 152 | 114 | `get_module` |
| clang22 ThinLTO | 433,024 | 54,796 | 158 | 170 | 143 | `get_module` |

Full report:

- `var/dso-o2-lto-toolchain-check/report-gcc16-clang22.txt`

## compiler inline report

`-g -O2 + LTO` 条件で inline report を取得した。

| profile | inline success | missed | non-always inline success |
| --- | ---: | ---: | ---: |
| GCC 16 LTO | 341 | 306 | 0 |
| Clang 22 ThinLTO | 805 | 176 | 267 |

Report files:

- `var/compiler-report-o2-lto-toolchain-inline/gcc16-lto/inline-all.log`
- `var/compiler-report-o2-lto-toolchain-inline/clang22-thinlto/make.err`

Clang 22 は `grpc_lite_ceil_div_timeout`, `contains_nul_or_control`, `grpc_lite_channel_credentials_init`, `grpc_lite_timeval_fetch`, `grpc_lite_saturating_add`, `grpc_lite_sha256_hex`, `parse_target_port` などで cost model による optimizer inline を報告した。

GCC 16 は report 上の inline success は `always_inline` 由来のみだった。ただし DSO では local function count が default 171 から 114 へ減っており、関数削除・統合・internal optimization は効いている。

## 判断ログ

- 2026-06-06: `0.0.14` release artifact の visibility は全8 artifactで `get_module` のみ export されていることを確認済み。
- 2026-06-06: `-O3`, LTO, `-fno-semantic-interposition` を default にする判断は、DSO差分だけではなく benchmark / QA evidence が必要と判断した。
- 2026-06-06: `-fvisibility=hidden` 済みのため、`-fno-semantic-interposition` の追加効果は限定的と見なす。
- 2026-06-06: 公開 artifact の optional optimized profile として GCC LTO / Clang ThinLTO / PGO を試す余地はあるが、PIE/source build default へ強制するのは避ける。
- 2026-06-06: PECL / PIE で PhpRedis を実測したところ、公式 PHP Docker image 上では compile line に `-g -O2` が入ることを確認した。
- 2026-06-06: `php-grpc-lite` の通常 `phpize && ./configure && make` でも `CFLAGS` を明示上書きしなければ `-g -O2` が入ることを確認した。
- 2026-06-06: GCC 16 LTO は GCC 14 LTO より `.text` と file size が少し小さいが、relocation / local function count の構造は同じだった。
- 2026-06-06: Clang 22 ThinLTO は file size が小さく、optimizer inline report も多い。一方で RELATIVE relocation は GCC LTO より多い。

## 結論

DSOだけで見える範囲では、`-g -O2 + LTO` により以下が確認できた。

- visibility は全profileで `get_module` のみ。
- GCC LTO / Clang ThinLTO とも JUMP_SLOT は defaultより 3 件少ない。
- GCC 16 LTO は GCC 14 LTO より `.text` が約 772 bytes小さい。
- Clang 22 ThinLTO は file size が最小だが、RELATIVE relocation は多い。
- Clang 22 は通常の optimizer inline を report 上で多く示す。
- GCC 16 は report 上は `always_inline` success中心だが、DSOでは local function count が大きく減る。

ただし、この結果は性能採否の根拠にはしない。公開 artifact に optimized profile を追加する場合は、別 issue で仮説、対象 workload、before/after benchmark、PHPT/ZTS/crash/static/release load QA を記録して判断する。

## 検証

- DSO build/load smoke: PASS
  - default gcc14
  - gcc14 LTO
  - gcc16 LTO
  - clang22 ThinLTO
- `readelf -Ws`: all profiles export only `get_module` as defined global symbol.
- `file`, `size -A`, `readelf -Wr`: section / relocation comparison recorded.
- compiler inline report: GCC16 and Clang22 reports recorded under `var/`.

## 完了条件

- DSO比較の条件と結果を記録する。
- preliminary mistake (`CFLAGS=""` による `-g -O2` 上書き) を明記する。
- GCC16 / Clang22 の調査条件と注意点を記録する。
- 採用判断ではなく調査issueとして `Status: Closed` にする。
