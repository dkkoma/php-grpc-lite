# TLS implementation boundary decision

Status: Open
Branch: main

## 目的

TLS関連の実装を自前で持ちすぎていないかを確認し、nghttp2側で利用できる実装に置き換えられるなら採用可否を判断する。

## 背景

現行transportは `nghttp2 + socket/TLS` を直接組み合わせている。TLS crypto自体はOpenSSLに任せているが、`transport.c` がTLS context作成、root PEM読込、client cert/key読込、SNI、hostname verification、ALPN `h2`、nonblocking handshake、`SSL_read` / `SSL_write` とdeadline pollを持っている。

もとの疑問:

> TLS関連の自前実装しているものはnghttp2から利用できるモジュールを使って、利用できないのか？
> あんまりよくわかってないけど、TLS関連の自前実装やめたい。

## 調査メモ

- libnghttp2 C APIはHTTP/2 framing / session engineであり、I/Oを実行しない。applicationがsocket/TLS I/Oを所有し、nghttp2へ入力bytesを渡し、nghttp2の出力bytesを送信する設計。
- nghttp2には高レベルの `libnghttp2_asio` があるが、C++ / Boost.Asio / OpenSSL 依存であり、nghttp2本体repositoryではmaintenance理由によりdeprecated扱い。PHP extensionのC実装へそのまま入れる選択肢としては重い。
- libcurlへ戻すとTLS/HTTP/2統合は外部化できるが、現在のSPECで決めている `nghttp2` runtime transport 1系統、transport selectionなし、libcurl fallbackなしの方針と衝突する。
- したがって「TLSをnghttp2のC moduleへ移して消す」は現実的ではない。消せるのはTLS実装ではなく、`transport.c` 内に混在しているOpenSSL glueの置き場所。

## 現状の責務

- OpenSSLが担うもの:
  - TLS protocol / crypto
  - certificate verification
  - ALPN / SNI / hostname verification API
  - client certificate / private key handling API
- php-grpc-liteが担うもの:
  - `ChannelCredentials::createSsl()` のPHP surfaceからOpenSSL設定への変換
  - root PEM / client cert / keyのlifetimeとvalidation
  - nonblocking socketとdeadline pollに合わせた `SSL_connect` / `SSL_read` / `SSL_write`
  - TLS errorをgRPC status / diagnostic fieldsへ写すこと
  - persistent connection lifecycleとの統合

## 判断

現時点ではTLS stackをnghttp2側へ移すのではなく、OpenSSL glueを維持する。ただし `transport.c` の責務が広くなっているため、TLS処理は内部moduleへ分離するのが妥当。

## 推奨対応

1. `src/tls_connection.c` / `src/tls_connection.h` を追加する。
2. 以下をTLS moduleへ移す。
   - `add_pem_certs_to_store`
   - `configure_client_certificate`
   - `configure_tls_connection`
   - TLS read/write helperのうち、`SSL_read` / `SSL_write` と `SSL_ERROR_WANT_*` handling
3. `transport.c` はsocket lifecycle、persistent connection、nghttp2 session、request/response処理を主責務に戻す。
4. 挙動変更を目的にしない。最初は抽出だけにする。
5. HTTP/2/gRPC domain model reviewを必須にする。

## 検証候補

- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-unit.sh`
- `tests/phpt/030-tls.phpt`
- 追加するなら:
  - ALPN `h2` failure fixture
  - `grpc.ssl_target_name_override` success / failure
  - client cert/key片方だけ指定した場合
  - TLS handshake deadline

## 非スコープ

- libcurl transport復活。
- transport selection option追加。
- TLS protocolの独自実装。
- 公式 `ext-grpc` / BoringSSL の内部構成への追従。

## 完了条件

- nghttp2側へTLSを移せない理由がdocsに残っている。
- TLS glueを維持する場合、`transport.c` から独立した内部moduleとして責務が切られている。
- TLS / mTLS / bad root / deadline / metadata credentialsの既存検証が維持されている。
