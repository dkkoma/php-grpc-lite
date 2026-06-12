# request DATA NO_COPY send (send_data_callback) ドメインモデルレビュー 2026-06-12

## Scope

- `src/transport.c`(`data_source_read_callback` / `h2_send_data_callback` / `send_pending_h2_frames_with_deadline` / trace)
- `src/transport.h`
- `src/diagnostic/bench.c`(`copy_request_bytes` の移設)
- `tests/phpt/031-upload-payload-boundaries.phpt`
- 対象 commit: `bd98f30`(branch `perf/request-data-no-copy-send`)

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer(`docs/verification/protocol-model-review-guide.md` 準拠)

## Review Prompt Summary

- nghttp2 `nghttp2_send_data_callback` contract(framehd 9B + length bytes を完全送信、TEMPORAL/CALLBACK_FAILURE の意味)への適合
- 新 read callback の EOF semantics が旧 copy 実装と等価か(空 payload / remaining==0 / DEFERRED 化の可能性)
- send_callback と send_data_callback の wire order / write coalescing invariant
- `source->ptr`(stack の `grpc_call` / `state->call`)の lifetime と use-after-free 経路
- trace `wire.frame_out` の `chunk_len` 変化の consumer 影響
- bench.c への `copy_request_bytes` 複製の妥当性

## 検証結果(指摘に至らなかった確認事項)

1. **nghttp2 contract**: `h2_send_data_callback` は framehd 9B を書いた後、while ループで `length` をちょうど 0 まで減算しながら grpc_header 残部 → `call->request` スライスを書く。書き込み総量は header 9B + application data `length` bytes に正確に一致する。`request_offset` の前進は本 callback のみで行われ、frame ごとに nghttp2 が pack(read callback)→ send(send_data_callback)を stream 内で逐次実行するため、read callback が見る `remaining` と send 時の offset は常に整合する。`h2_connection_buffer_or_write` が途中で失敗した場合 `request_offset` は部分前進したままになるが、`NGHTTP2_ERR_CALLBACK_FAILURE` → `nghttp2_session_send` 失敗 → `send_pending_h2_frames_with_deadline` が `mark_connection_dead` するため、その connection/session で send が再実行されることはなく offset の不整合は到達不能(transport.c:1765-1771 のコメントどおり「session state no longer matches the wire」として connection 廃棄)。`padlen > 0` での `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE` は nghttp2 仕様上有効な回復(RST_STREAM/INTERNAL_ERROR で stream-local に閉じる)で、framehd 書き込み前に判定しているため「部分送信後の TEMPORAL」という contract 違反も起きない。なお `select_padding_callback` は未登録なので padlen>0 は実質到達しない防御コード。
2. **EOF semantics**: 旧実装の post-copy 判定 `request_offset >= total_len` は、`copy_request_bytes` が常に `to_send` を全量 copy できる(source は連続バッファ)ため `to_send == remaining` と等価。`request_len == 0` の場合も `remaining = grpc_header_len = 5 >= 5` の初回フレームで `to_send == remaining` → EOF となり、5B prefix のみの DATA + END_STREAM という旧挙動と一致(PHPT 031 の size 0 で固定済み)。`remaining == 0` かつ `length > 0` で「0 を EOF なしで返す」ケースは構造的に発生しない: 初回呼び出しでは必ず `remaining >= 5` であり、EOF を立てた後は nghttp2 が同 stream の read callback を再度呼ばない。`NGHTTP2_ERR_DEFERRED` 系の挙動に入る経路はない。
3. **Ordering / coalescing invariant**: 非 DATA frame(send_callback)と DATA(send_data_callback)はいずれも `nghttp2_session_send` の単一 pass 内から wire order で呼ばれ、両者とも `h2_connection_buffer_or_write` に append するため並べ替えは構造上不可能。`nghttp2_session_send` の呼び出し点は `send_pending_h2_frames_with_deadline`(transport.c:1753)のみで、`write_coalescing = true` は同関数内でのみ立ち、pass 終端で flush + `false` 戻しされる(setup/preflight 経路も同関数経由)。invariant は send_data_callback 追加後も維持されている。
4. **Lifetime**: `source->ptr` の dangling dereference に至る経路は確認できなかった。(a) unary では recv ループの脱出条件が `stream_closed`(nghttp2 が stream close 時に outbound DATA item を detach 済み)か `mark_connection_dead`(以後 `connection_usable` が false で session_send 不可、`nghttp2_session_del` は send 系 callback を呼ばない)のいずれか。(b) GOAWAY は nghttp2 自身が `last_stream_id` 超の own stream を close して item を detach し、connection も draining(= unusable)になる。(c) registration 失敗(`mark_grpc_call_stream_registration_failed`)は user_data を NULL 化した上で connection を dead 化。(d) server streaming の destructor は RST_STREAM submit → send_pending を `state->request` 解放より前に行い、nghttp2 は非 DATA frame を DATA より先に送る(ob_reg 優先)ため RST 送信時点で stream close → item detach となる。submit/send 失敗時は dead 化に畳まれる。
5. **Trace**: `wire.frame_out` の DATA record は `chunk_len = 9` になるが、`grpc_lite_trace_outbound_frame_record` 内で payload を参照する分岐(SETTINGS/WINDOW_UPDATE/PING/GOAWAY の hex、SETTINGS entries)はすべて `length >= 9 + frame_len` でガードされており over-read しない。`tools/benchmark/trace-io-probe.sh` は `wire.socket_write`/`wire.tls_write` の行数のみ参照、`tests/phpt/029-trace-file.phpt` は DATA frame_out の存在と `rpc_method` のみ assert(record 時点で stream は未 close のため `grpc_call_from_stream_id` で解決可能)。既存 consumer への破壊なし。
6. **PHPT 031**: 16384(有効 max frame size)前後 16379/16380、複数フレーム 65536、フロー制御で deferred → WINDOW_UPDATE 再開を通る 1MB をサーバ受信長で固定しており、prefix がフレーム境界をまたぐケースと EOF 境界の双方を押さえている。

## Issues

### REVIEW-20260612-001: nghttp2 data source ptr の lifetime invariant が暗黙

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `data_provider.source.ptr = &call`(unary では stack 変数)が nghttp2 session 内の outbound DATA item に保持されるのに対し、「call が破棄される時点で必ず (a) stream close 済み(item detach 済み)か (b) connection dead/draining(以後 session_send されず、`nghttp2_session_del` は send 系 callback を呼ばない)」という use-after-free を防ぐ invariant がコード上どこにも明文化されていない。NO_COPY 化により dereference が send_data_callback(payload bytes の読み出し)まで広がったため、将来の exit path 追加(例: timeout で connection を usable のまま early return する変更)が静かに UAF を導入し得る。
- Evidence: `src/unary_call.c:119-121`(stack call を source.ptr へ)、`src/transport.c:1834-1888`(`h2_send_data_callback` の `call->request` 読み出し)、`src/unary_call.c:141-186`(全 exit path が dead 化または stream_closed を経由)
- Expected model: nghttp2 callback user_data / data source の lifetime は active call の lifetime を超えない(review guide §8)。超え得る構造を許すなら、その安全条件を invariant としてコードに記述する。
- Why it matters: 現状は安全だが、安全性が unary perform の制御フロー全 path と nghttp2 内部挙動(stream close 時の item detach、`nghttp2_session_del` が send callback を呼ばないこと)の組み合わせに依存しており、回 regression に対する防壁がない。
- Recommended fix: `data_provider.source.ptr` 設定箇所または `h2_send_data_callback` 冒頭コメントに invariant(「call 破棄前に stream close または connection dead/draining が保証される。新しい early-return path を足す場合はこの条件を維持すること」)を明記する。コード変更は不要。
- Fix summary: `h2_send_data_callback` 定義の直前に lifetime invariant(stream closed か connection dead/draining が call 解放より先に成立する)をコメントで明文化。
- Fix commit: perf/request-data-no-copy-send(レビュー反映コミット)
- Verification: コメント追加のみ(挙動変更なし)。
- Notes: read callback 時代から存在する既存 model であり、本 commit の新規バグではない。

### REVIEW-20260612-002: DATA の `wire.frame_out` chunk_len semantics 変化が未文書

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: DATA frame の `wire.frame_out` record は従来 `chunk_len = 9 + frame_payload_len`(send_callback が受けた chunk 全体)だったが、本 commit 以降は常に `chunk_len = 9`(framehd のみ)になる。既存 consumer(trace-io-probe.sh、029 PHPT)への影響はないが、`chunk_len >= 9 + frame_payload_len` を前提に payload 到達を判定する将来の解析スクリプトは DATA だけ挙動が異なることになる。trace semantics を記述している `docs/guides/code-reading-guide.md` の `wire.frame_out` 注記が未更新。
- Evidence: `src/transport.c:1884-1886`(`grpc_lite_trace_outbound_frame_record(connection, framehd, 9)`)、`docs/guides/code-reading-guide.md:202`
- Expected model: trace record の field semantics は frame type に依らず一貫しているか、差異が文書化されている。
- Why it matters: 過去の bench-results 解析(var/ 配下)のように trace を後追い解析する運用があるため、field の意味変化は記録しておかないと将来の計測比較で誤読を生む。
- Recommended fix: code-reading-guide の trace 説明に「DATA の `wire.frame_out` は framehd のみを chunk として記録するため `chunk_len` は常に 9。`frame_payload_len` が実 payload 長」と追記する。
- Fix summary: `docs/guides/code-reading-guide.md` の trace 説明に「DATA の wire.frame_out は chunk_len: 9(frame_payload_len は正しい payload 長)」を追記。
- Fix commit: perf/request-data-no-copy-send(レビュー反映コミット)
- Verification: ドキュメント追記のみ。
- Notes: `frame_payload_len` は framehd から導出されるため値自体は正しい。

### REVIEW-20260612-003: bench.c への copy_request_bytes 複製

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `copy_request_bytes` が production から削除され、bench raw client(`no_copy=0` 比較用)の static として `diagnostic/bench.c` に複製された。
- Evidence: `src/diagnostic/bench.c`(static `copy_request_bytes` + 移設理由コメント)、`src/transport.c` / `src/transport.h` からの削除
- Expected model: production transport は NO_COPY 一本化(分岐とテスト面積の削減)、bench は copy/no-copy の差分計測器として旧経路を保持する。
- Why it matters: 複製による drift リスクはあるが、bench 側は独立した raw client(独自 session / callbacks / `user_data` ベース)であり production と挙動を揃える必要がない。production に死蔵 fallback を残す方が分岐・テスト負債が大きい。
- Recommended fix: 現状維持。bench 側コメントで production が NO_COPY-only である旨が明記済み。
- Fix summary: 受容(変更不要)。
- Fix commit: `bd98f30`
- Verification: bench ビルド(--enable-grpc-bench)コンパイル確認済み(commit message)。
- Notes: `remaining_request_bytes` は引き続き transport.c の共有シンボルを利用しており二重化していない。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 2
- Design Decision: 1
