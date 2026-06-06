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

## 2026-06-07 follow-up: Bloaty / objdump diff / optimization remarks

前回のDSO比較を補助するため、同じ `-g -O2` baseline と `-g -O2 + LTO` 系profileで、Bloaty、`objdump` 正規化diff、compiler optimization remarks を追加取得した。

目的は「DSOだけで見える範囲を増やす」ことであり、性能採否の根拠にはしない。

### 条件

| profile | compiler | flags | artifact size |
| --- | --- | --- | ---: |
| default-gcc14 | GCC 14.2.0 | `-g -O2` | 568,192 |
| gcc16-lto | GCC 16.1.0 | `-g -O2 -flto` | 581,072 |
| clang22-thinlto | Clang 22.1.7 + lld-22 | `-g -O2 -flto=thin` | 433,024 |

Generated reports:

- `var/lto-dso-followup/report.txt`
- `var/lto-dso-followup/bloaty-diff-default-vs-gcc16-sections.txt`
- `var/lto-dso-followup/bloaty-diff-default-vs-clang22-sections.txt`
- `var/lto-dso-followup/bloaty-diff-default-vs-gcc16-symbols.txt`
- `var/lto-dso-followup/bloaty-diff-default-vs-clang22-symbols.txt`
- `var/lto-dso-followup/objdump-default-vs-gcc16-lto.diff`
- `var/lto-dso-followup/objdump-default-vs-clang22-thinlto.diff`
- `var/lto-dso-followup/gcc16-lto/inline-all.log`
- `var/lto-dso-followup/clang22-thinlto/make.err`

### Bloaty: section diff

| diff | file size | VM size | `.text` | `.rodata` | `.eh_frame` | 主な注意点 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| default -> gcc16-lto | +12.6 KiB | -6.78 KiB | -2.78 KiB | -92 B | -3.02 KiB | debug / symtab が増え、runtime側は小さくなる |
| default -> clang22-thinlto | -132 KiB | -2.38 KiB | -2.04 KiB | -1.23 KiB | -2.74 KiB | file size差の大半はdebug section差 |

Bloatyで見ると、LTO系profileはいずれもruntime VM sizeを少し減らす。ただし差分は数KiBで、file sizeの見た目はdebug section layoutに強く左右される。特にClang ThinLTOの 433,024 bytes (約423 KiB) は `.debug_info`, `.debug_loclists`, `.debug_line`, `[Unmapped]` が大きく減った影響が支配的で、`.text` だけを見ると default 比で約 2 KiB 減にとどまる。

Compile unit attribution は、GCC LTOでは `<artificial>` に寄るためソースファイル単位の比較に向かない。一方、default / Clang ThinLTOでは `src/transport.c`, `src/surface.c`, `src/wrapper_adapter.c` が上位に見える。

### Bloaty: symbol diff

| diff | 増えた代表symbol | 減った / 消えた代表symbol | 読み取り |
| --- | --- | --- | --- |
| default -> gcc16-lto | `get_persistent_connection.constprop.0` (+4.30 KiB VM), `grpc_lite_unary_call_perform_core_on_connection.constprop.0` (+2.29 KiB VM), `on_header_callback` (+1.30 KiB VM) | `grpc_lite_unary_call_perform_core_on_connection.isra.0`, `configure_tls_connection`, `resolve_grpc_call_status`, `server_streaming_call_next_resource` | LTO後に constprop / lto_priv / part 化されたsymbolへ寄せられている |
| default -> clang22-thinlto | `grpc_lite_unary_call_perform_on_connection` (+2.62 KiB VM), `create_h2_connection` (+1.65 KiB VM), `grpc_protocol_process_response_data_direct` (+1.14 KiB VM) | `grpc_lite_unary_call_perform_core_on_connection.isra.0`, `configure_tls_connection`, `grpc_protocol_process_response_data_direct.part.0`, `timeval_methods` | symbol名の統合・削除・再配置があり、単純に「関数が消えた = inline」とは断定しない |

上位text symbolだけを見ても、GCC LTOは `get_persistent_connection.constprop.0` が最大になり、Clang ThinLTOは `grpc_lite_unary_call_perform_on_connection`, `send_callback`, `create_h2_connection` が上位に来る。これは「同じCソースから生成されるDSOでも、LTO backendごとに大きい関数の境界が変わる」ことを示す。

### objdump diff

正規化済みdisassemblyの行数:

| file | lines |
| --- | ---: |
| default-gcc14 normalized | 15,225 |
| gcc16-lto normalized | 14,442 |
| clang22-thinlto normalized | 14,658 |
| default vs gcc16-lto diff | 26,046 |
| default vs clang22-thinlto diff | 29,042 |

`objdump` diffは行数が大きく、register allocation、basic block order、symbol boundary、literal placementの差が混ざるため、そのまま性能差として読むには粗い。

ただし補助線としては有用だった。たとえばGCC16では `h2_connection_send` 周辺のstack frameやcontrol-flow配置が変わり、Clang22では `configure_tls_connection` がstandalone symbolとして見えなくなる一方、remarks上では `configure_tls_connection` 自体は `create_h2_connection` へは「too costly」としてinlineされていない。つまり、DSO上のsymbol消失は inline, internalization, section garbage collection, symbol folding, debug attribution のどれかを切り分けて読む必要がある。

### optimization remarks

| profile | inline success系 | missed系 | non-always inline success |
| --- | ---: | ---: | ---: |
| GCC16 LTO | 341 | 306 | 0 |
| Clang22 ThinLTO | 805 | 176 | 267 |

Clang remarksはcost modelが読みやすい。代表例:

- `h2_connection_send` へ `remaining_timeout_us_for_deadline`, `set_connection_error_detail` はinlineされる。
- `h2_connection_send` 自体は caller へは cost 1560 / threshold 225 でinlineされない。
- `configure_tls_connection` へ `set_connection_error_detail` は多数inlineされるが、`poll_fd_until_deadline`, `set_fd_nonblocking_mode` はtoo costlyで残る。
- `configure_tls_connection` は `create_h2_connection` へ cost 2555 / threshold 225 でinlineされない。
- `append_request_header` は `append_grpc_timeout_request_header`, `append_user_agent_request_header` へinlineされるが、`grow_request_headers` はtoo costlyで残る。

GCC reportは今回の取り方ではalways_inline由来が中心で、Clangほど「通常のoptimizer inline」を読みやすくない。ただしBloaty / symbol count / objdump側では、GCC LTOでもconstprop化、symbol削除、local function整理が起きている。

### 追加判断

- Bloatyは、DSOサイズを runtime VM size と debug/file size に分ける用途では有用。
- objdump diffは、局所的なcontrol-flowやsymbol境界の確認には使えるが、diff全体から性能を読む用途には粗すぎる。
- optimization remarksは、inlineの「された / されない」と理由を見る一次ソースとして有用。特にClangはcostとthresholdが追いやすい。
- DSOだけで見る限り、GCC16 LTO / Clang22 ThinLTOはいずれも`.text`を数KiB減らすが、採用判断に足る差ではない。
- 公開artifactにoptimized profileを追加するなら、別issueで workload、PGO/LTO条件、before/after benchmark、PHPT/static/release load QAをそろえて判断する。
