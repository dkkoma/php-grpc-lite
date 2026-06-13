# write coalesce capacity (per-connection cap) HTTP/2ドメインモデルレビュー 2026-06-12

## Scope

- `src/transport.c`(commit `edca717`: `h2_write_coalesce_capacity_for_max_frame_size` 追加、`h2_connection_buffer_or_write` の cap 参照化、`create_h2_connection` での `write_buffer_cap` 初期化)
- `docs/issues/open/2026-06-12-write-coalesce-capacity-vs-frame-size.md`

## Reviewer Role

- `HTTP/2 / gRPC domain model reviewer`

## Review Prompt Summary

- 固定 16KB write coalesce バッファ(default SETTINGS_MAX_FRAME_SIZE と同値でフルサイズ DATA が常にバイパスしていた)を `clamp(4 * (effective_max_frame_size + 9), 64KB, 1MB)` の per-connection cap へ置き換えた変更を、protocol-model-review-guide.md に照らしてレビュー。確認点: (1) RFC 9113 上の coalescing の妥当性と write 順序保証、(2) `write_buffer_cap` の lifecycle(設定箇所の一意性、0 にならないこと、pemalloc/pefree 対応)、(3) `grpc_lite.http2_max_frame_size` 大設定時の縮退挙動、(4) persistent connection のメモリ上限、(5) 旧定数 16384 を前提とした箇所の残存。

## 検証結果(指摘なしの確認項目)

- **RFC 9113 / write 順序**: HTTP/2 は TCP byte stream 上のフレーム列であり、複数フレームを 1 回の `SSL_write` に集約してもフレーム境界・順序は payload 内で保存されるため protocol 上の影響はない(RFC 9113 §4.1 はフレームの直列化のみ規定し、write の粒度は規定しない)。socket への送信経路は `h2_connection_write_all`(src/transport.c:1330)のみで、その呼び出し元は (a) `h2_connection_flush_write_buffer`(1348)、(b) `h2_connection_buffer_or_write` の coalescing 無効パス(1381)、(c) 同 bypass パス(1387)の 3 箇所に閉じている。bypass パス(`length > write_buffer_cap`)は必ず先に flush(1384)してから直接 write するため、buffered bytes と oversized chunk の順序逆転は起きない。`nghttp2_session_send` 中の送信は `send_callback`(1732)→ `h2_connection_buffer_or_write` に単線化されており、interleave する経路はない。
- **lifecycle**: `h2_connection` の確保箇所は `create_h2_connection` 内の `pecalloc`(src/transport.c:1496)のみで、その直後(1498)に `write_buffer_cap` が無条件に設定される。`h2_write_coalesce_capacity_for_max_frame_size` は MIN 64KB に clamp するため cap は常に非 0。`write_coalescing` を true にするのは `send_pending_h2_frames_with_deadline`(1766: `connection->write_coalescing = connection->tls`)のみで、同関数末尾(1798)で false に戻る。よって coalescing 有効時に cap==0 でバッファ確保サイズが 0 になる経路はない。buffer は `pemalloc(cap, connection->persistent)`(1390)/ `pefree(connection->write_buffer, connection->persistent)`(destroy_h2_connection, 183-185)で persistent フラグが対になっており変更なし。旧コードで「alloc 時に cap を設定」していたものが「create 時に設定」へ前倒しされたが、cap を「buffer 確保済み」の代理判定に使う箇所はなく(参照は 1383/1390/1398/1498 のみ)、`write_buffer == NULL` 判定と矛盾しない。
- **mfs 大設定時の縮退**: `4 * (mfs + 9) > 1MB` となるのは mfs > 262,135。このとき cap = 1MB < 9 + mfs のため、フルサイズ DATA frame はすべて bypass パス(flush → 直接 write)に落ちる。これは変更前(16KB cap でフルサイズ frame が常に bypass)と同型の動作であり、順序保証も上記 (c) で担保されるため正しく縮退する。HEADERS / WINDOW_UPDATE / 端数 DATA など cap 以下のフレームは引き続き coalesce される。`effective_http2_max_frame_size`(src/transport_core.c:50)は ini を `16384..16777215` に clamp するため `4 * ((size_t) mfs + 9)` の size_t 演算は overflow しない。
- **メモリ上限**: persistent connection は `GRPC_LITE_MAX_PERSISTENT_CONNECTIONS`(src/transport_core.h:22, 128)で個数が制限され(get 時に `zend_hash_num_elements` チェック、src/transport.c:1608)、デフォルト構成では cap = 4×16393 = 65,572 byte/接続 × 128 ≒ 8MB が上限。バッファは TLS 接続で最初の coalesced write まで lazy 確保なので h2c では確保されない。
- **旧定数の残存**: `GRPC_LITE_H2_WRITE_COALESCE_CAPACITY` への参照は issue doc(変更経緯の記録)と過去レビュー記録 `docs/reviews/issues/2026-06-10-transport-review-fixes-domain-review.md`(歴史的記述)のみで、tests / bench / guides に旧 16KB を前提とするコード・アサーションはない。`tests/phpt/029-trace-file.phpt` の 16384 は SETTINGS_MAX_FRAME_SIZE のデフォルト値検証であり coalesce 容量とは独立。trace(`grpc_lite_trace_outbound_frame`)は buffer/bypass に関係なく nghttp2 のフレーム生成順で記録されるため、coalesce 容量変更の影響を受けない。
- **ドメイン分類**: `write_buffer_cap` は socket への書き込み粒度という HTTP/2 Connection (transport) 固有の state であり、guide §3 のモデルどおり `h2_connection` に置かれている。容量を peer に advertise する `SETTINGS_MAX_FRAME_SIZE` から導出しているが、これは「nghttp2 が send_callback に渡す最大 chunk サイズ」の根拠としての参照であり、flow-control window と buffer size の混同(guide §8)には当たらない。

## Issues

### REVIEW-20260612-001: 容量導出関数が transport_core に置かれておらず clamp 境界の C unit テストがない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `h2_write_coalesce_capacity_for_max_frame_size` は ini 由来値から派生する pure な容量計算だが、`src/transport.c` の static 関数として実装されており、`effective_http2_max_frame_size` / `effective_http2_window_size` などの同族関数が置かれている `src/transport_core.c`(C unit テスト共有層)に入っていない。そのため clamp 境界(mfs=16384 → 65,572、mfs=262,135 前後の 1MB 境界、mfs=16,777,215 → 1MB)を固定するテストが存在しない。
- Evidence: `src/transport.c:1362`、`src/transport_core.c:50`(同族関数の配置)、`tests/unit/test_transport_core.c`(該当テストなし)
- Expected model: effective 値の導出関数は transport_core に集約され、境界値が unit テストで固定される(guide §11「ドメインモデルの境界は happy path とは別に固定する」)。
- Why it matters: 将来の MIN/MAX 定数変更や式変更時に、フルサイズ DATA が再びバイパスする(本変更が直した潜在バグの再発)退行を検出できない。
- Recommended fix: 関数を `transport_core.c` へ移して `transport_core.h` に宣言し、`test_transport_core.c` に境界 3 点(default / 1MB clamp 境界 / 最大 mfs)のアサーションを追加する。
- Fix summary: 推奨どおり `transport_core.c` へ移動し、`test_transport_core.c` に境界アサーション 6 点(default=65,572 / full DATA が cap に収まること / min clamp / mfs=64KB / 1MB 境界 / 最大 mfs)を追加。
- Fix commit: perf/write-coalesce-capacity(レビュー反映コミット)
- Verification: C unit / PHPT 再実行 PASS
- Notes: 機能上の不具合ではなく退行防止の補強。

### REVIEW-20260612-002: mfs > 262,135 構成では 1MB バッファを確保するのにフルサイズ DATA は全件バイパスする(縮退構成のコスト/便益が文書化不足)

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `grpc_lite.http2_max_frame_size`(PHP_INI_SYSTEM)を約 256KB 超に設定すると cap は 1MB に clamp され、9 + mfs > 1MB のためフルサイズ DATA frame は 1 件も coalesce されない。一方で HEADERS 等の小フレーム送信を契機に persistent 接続ごとに 1MB の write buffer が lazy 確保され、最悪 128 接続 × 1MB = 128MB がプロセスに常駐し得る。動作自体は正しく旧実装と同型に縮退する(検証結果参照)が、issue doc の Decision Log はデフォルト構成(+48KB × 128 = 8MB)のみ定量化しており、この縮退構成の上限と「1MB buffer がほぼ小フレーム coalesce にしか使われない」非対称は記録されていない。
- Evidence: `src/transport.c:1362-1372`(clamp)、`src/transport.c:1383-1397`(bypass と lazy 確保)、`docs/issues/open/2026-06-12-write-coalesce-capacity-vs-frame-size.md` Decision Log
- Expected model: ini の全有効域(16384..16,777,215)について、transport が持つ per-connection state のメモリ上限と挙動縮退が design record に明示される(guide §6: state は最小 scope、§12: design decision は理由と再検討条件を残す)。
- Why it matters: PHP_INI_SYSTEM のため運用側が mfs を大きくした場合の常駐メモリ上限(最大 128MB)が記録にないと、FrankenPHP worker 等の常駐プロセスで容量計画を誤る余地がある。正しさの問題ではない。
- Recommended fix: issue doc の Decision Log に「mfs > 262,135 では cap=1MB、フルサイズ DATA は全件直接 write(旧挙動と同型)、persistent あたり最大 1MB × 128 = 128MB が理論上限」を追記する。コード変更は不要(`9 + mfs > cap` のとき cap を縮める最適化は小フレーム coalesce の便益を失うため見送りで良い)。
- Fix summary: issue Decision Log に縮退構成の挙動と理論上限(lazily allocated / TLS のみの注記付き)を追記。
- Fix commit: perf/write-coalesce-capacity(レビュー反映コミット)
- Verification: ドキュメントのみ
- Notes: `docs/guides/perf-hotpath-roadmap-2026-06.md:118` の #4 ステータス表が `Open` のまま、issue ファイルも `docs/issues/open/` 残置。merge 時の close 運用に含めること。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `2`
- Design Decision: `none`
