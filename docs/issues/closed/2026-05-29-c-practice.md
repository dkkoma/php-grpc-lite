---
Status: Closed
Owner: Codex
Created: 2026-05-29
Closed: 2026-05-31
WorkPlan: docs/issues/closed/2026-05-29-c-maintainability-work-plan.md
Spec: docs/SPEC.md
---

# Cプロジェクトの保守性を高める実装方針

## 完了記録

この文書はCプロジェクト一般方針の親issueとして扱った。php-grpc-lite固有の永続的な設計制約は `docs/SPEC.md` の「C extension architecture policy」へ反映済み。

具体的な実装計画、phase進行、検証、完了条件は `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md` で管理し、完了済み。以下の本文は、当初の一般方針と判断背景の履歴として残す。

## 目的

このドキュメントは、既存のCプロジェクトに対して、保守性・テスト容易性・モジュール性を高めるための実装方針を示す。

特に以下を重視する。

- ファイル分割の意味を明確にする
- `.c`ファイルの直接includeを避ける
- ヘッダの役割を整理する
- internal APIとpublic APIを区別する
- 実装詳細を隠蔽する
- 単体テストしやすい構造にする
- 特定ランタイムや外部フレームワークへの依存を境界層に閉じ込める

PHP拡張、組み込みライブラリ、CLIツール、プロトコル実装、ネットワークライブラリなど、Cで書かれた一般的なプロジェクトに適用できる方針とする。

---

## 基本方針

Cプロジェクトでは、言語機能としてのカプセル化が弱いため、保守性は主に以下で担保する。

- ディレクトリ構成
- ヘッダ設計
- `static`によるファイル内隠蔽
- 命名規則
- ビルド対象の整理
- public / internal / privateの区別
- テスト可能な依存方向

基本方針は次の通り。

```text
.c には実装を書く
.h には宣言を書く
他の .c から使いたい関数・型は .h に宣言する
.c から .c は原則 include しない
外に出したくない関数は .c 内で static にする
public API と internal API を明確に分ける
```

---

## `.c` と `.h` の役割

### `.c`

`.c`ファイルはコンパイル単位である。

通常、ビルドシステムは`.c`ファイルを個別にコンパイルし、最後にリンクする。

```text
client.c -> client.o
buffer.c -> buffer.o
main.c   -> main.o

client.o + buffer.o + main.o -> executable or shared library
```

`.c`には以下を書く。

- 関数の実装
- privateな補助関数
- privateな構造体定義
- ファイル内だけで使う定数
- 実装詳細

### `.h`

`.h`ファイルは、他の`.c`から利用される宣言を置くファイルである。

`.h`には以下を書く。

- 他の`.c`から呼ぶ関数の宣言
- 共有したい型定義
- enum定義
- opaque structの宣言
- publicまたはinternal APIの約束
- 必要最小限のinclude

`.h`は通常、単独でコンパイルしない。

---

## `.c` から `.c` を include しない

Cの`#include`は、指定したファイルの中身をその場に展開するだけである。

そのため、以下のような書き方は避ける。

```c
// NG
#include "client.c"
#include "buffer.c"
```

これは「別ファイルの実装を利用している」のではなく、「実装をその場にコピペしている」のに近い。

### 問題点

`.c`をincludeすると、以下の問題が起きやすい。

- 同じ関数定義が複数箇所に展開される
- `multiple definition` エラーが発生しやすい
- コンパイル単位の境界が曖昧になる
- privateな関数の範囲が分かりにくくなる
- 差分ビルドや静的解析がやりづらくなる
- テスト対象の切り出しが難しくなる

### 推奨形

他の`.c`から使いたい関数は、ヘッダに宣言する。

```c
// client.h
#ifndef PROJECT_CLIENT_H
#define PROJECT_CLIENT_H

void client_call(void);

#endif
```

```c
// client.c
#include "client.h"

void client_call(void)
{
    // implementation
}
```

利用側は`.h`をincludeする。

```c
// main.c
#include "client.h"

int main(void)
{
    client_call();
    return 0;
}
```

ビルドシステムで`.c`をコンパイル対象に含める。

```bash
cc -o app main.c client.c
```

---

## include guardを必ず書く

ヘッダにはinclude guardを書く。

```c
#ifndef PROJECT_CLIENT_H
#define PROJECT_CLIENT_H

// declarations

#endif /* PROJECT_CLIENT_H */
```

これは、同じヘッダが1つのコンパイル単位内で複数回includeされても、中身が1回だけ有効になるようにするためである。

### 名前の付け方

短すぎる名前は避ける。

```c
// NG
#ifndef CLIENT_H
#define CLIENT_H
```

プロジェクト名やモジュール名を含める。

```c
// OK
#ifndef PROJECT_CLIENT_H
#define PROJECT_CLIENT_H
```

または、サブディレクトリも含める。

```c
#ifndef PROJECT_CORE_CLIENT_H
#define PROJECT_CORE_CLIENT_H
```

### `#pragma once` について

`#pragma once`でも同様の目的を達成できる。

```c
#pragma once
```

ただし、移植性や伝統的なCプロジェクトとの親和性を考えると、include guardを基本とする。

---

## private / internal / public の区別

Cでは、クラス言語のような強い`private`や`public`はない。

そのため、以下の3段階で整理する。

```text
private  = その .c ファイル内だけで使う
internal = プロジェクト内部・モジュール内部では使う
public   = 外部利用者に安定APIとして公開する
```

---

## private関数

`.c`内だけで使う関数は、ヘッダに出さず、`static`を付ける。

```c
// client.c

static int build_request(void)
{
    return 0;
}

int client_call(void)
{
    return build_request();
}
```

`static`を付けた関数は、その`.c`ファイル内だけで見える。

```text
private関数 = .c内にstaticで書く
```

これがCにおける最も基本的な隠蔽手段である。

---

## internal API

複数の`.c`から使うが、外部利用者に公開するつもりがない関数・型はinternal APIとする。

例えば以下のような構成にする。

```text
src/
  client.c
  client.h
  buffer.c
  buffer.h
  status.c
  status.h
```

この場合、`src/*.h`はプロジェクト内部用のヘッダとして扱う。

```text
src/*.h = internal header
```

internal headerは以下の扱いにする。

- インストール対象にしない
- 外部ドキュメントに載せない
- API互換性を保証しない
- プロジェクト内部でのみincludeする
- 必要に応じて`*_internal.h`という名前を使う

例：

```c
// src/client.h
#ifndef PROJECT_CLIENT_H
#define PROJECT_CLIENT_H

typedef struct project_client project_client;

project_client *project_client_new(void);
void project_client_free(project_client *client);

#endif
```

---

## public API

外部のCコードから利用させるAPIは、public APIとして明確に分ける。

典型的には`include/`配下に置く。

```text
include/
  project/
    project.h
    client.h

src/
  client.c
  client_internal.h
```

public headerは以下の性質を持つ。

- インストール対象にする
- ドキュメントに載せる
- API / ABI互換性を意識する
- internalな型や実装詳細を漏らさない
- 依存する外部ヘッダを最小限にする

例：

```c
// include/project/client.h
#ifndef PROJECT_PUBLIC_CLIENT_H
#define PROJECT_PUBLIC_CLIENT_H

typedef struct project_client project_client;

project_client *project_client_new(void);
void project_client_free(project_client *client);

#endif
```

---

## internalとpublicは配置で区別する

推奨する区別は以下。

```text
.c内のstatic関数       private
src/*.h               internal
src/internal/*.h      stronger internal
include/project/*.h   public
```

public APIを持たないプロジェクトでは、`include/`を作らなくてもよい。

例えばPHP拡張やCLI専用ツールでは、Cのpublic APIを提供しないことが多い。

その場合は以下のような構成でよい。

```text
project/
  config.m4
  project.c
  php_project.h

  src/
    client.c
    client.h
    buffer.c
    buffer.h
    status.c
    status.h
```

この場合、`src/*.h`はinternal APIである。

---

## opaque structを使って実装詳細を隠す

構造体の中身を外部に見せたくない場合、ヘッダでは名前だけ宣言する。

```c
// client.h
#ifndef PROJECT_CLIENT_H
#define PROJECT_CLIENT_H

typedef struct project_client project_client;

project_client *project_client_new(void);
void project_client_free(project_client *client);

#endif
```

実体は`.c`に書く。

```c
// client.c
#include "client.h"

struct project_client {
    int timeout_ms;
    int connected;
};

project_client *project_client_new(void)
{
    // allocate and initialize
}

void project_client_free(project_client *client)
{
    // free
}
```

利用側は構造体の中身を直接触れない。

```c
#include "client.h"

project_client *client = project_client_new();

// NG: 中身が見えないのでアクセスできない
// client->timeout_ms = 100;
```

これにより、内部構造を変更しても利用側への影響を抑えられる。

---

## ヘッダに実装を書かない

通常の関数実装は`.h`ではなく`.c`に書く。

```c
// NG: headerに通常関数の実装を書く
int project_add(int a, int b)
{
    return a + b;
}
```

これを複数の`.c`からincludeすると、同じ関数定義が複数作られ、リンクエラーになる。

ヘッダには宣言だけを書く。

```c
// project_math.h
int project_add(int a, int b);
```

実装は`.c`に書く。

```c
// project_math.c
#include "project_math.h"

int project_add(int a, int b)
{
    return a + b;
}
```

例外的に、`static inline`関数やマクロはヘッダに書くことがある。

```c
static inline int project_min(int a, int b)
{
    return a < b ? a : b;
}
```

ただし、乱用しない。

---

## ヘッダのincludeは最小限にする

ヘッダが別のヘッダを大量にincludeすると、依存関係が広がり、再ビルド範囲も増える。

ヘッダでは可能な限り前方宣言を使う。

```c
// client.h
#ifndef PROJECT_CLIENT_H
#define PROJECT_CLIENT_H

typedef struct project_transport project_transport;
typedef struct project_client project_client;

project_client *project_client_new(project_transport *transport);

#endif
```

実装側で必要なヘッダをincludeする。

```c
// client.c
#include "client.h"
#include "transport.h"
#include "buffer.h"
```

方針：

```text
.h では必要最小限のinclude
.c では実装に必要なinclude
```

---

## レイヤーを分ける

保守性を高めるには、処理をレイヤーに分ける。

例：

```text
app / binding layer
  ↓
core layer
  ↓
protocol / codec layer
  ↓
transport layer
  ↓
platform layer
```

各レイヤーの依存方向は一方向にする。

```text
上位層は下位層を呼んでよい
下位層は上位層を知らない
```

### 悪い例

```text
core が binding layer の型を知っている
transport が PHP/Zend API を呼ぶ
codec が client の内部構造を直接触る
error が UI/API層の例外を直接投げる
```

### 良い例

```text
binding layer:
  外部APIとの変換だけを行う

core layer:
  ビジネスロジック・状態管理を行う

protocol layer:
  encode/decode, parser, frame処理を行う

transport layer:
  read/write/connect/closeを抽象化する

platform layer:
  allocator, clock, loggingなどを抽象化する
```

---

## 外部ランタイム依存は境界層に閉じ込める

PHP拡張、Python拡張、Ruby拡張、組み込み環境などでは、特定ランタイムのAPIが存在する。

例：

- PHP / Zend API
- Python C API
- Ruby C API
- 独自フレームワークAPI
- OS固有API

これらをcoreロジックに直接混ぜない。

推奨：

```text
binding / adapter層:
  ランタイム固有APIを使ってよい

core層:
  ランタイム非依存にする
```

例えばPHP拡張の場合：

```text
Zend依存:
  PHP関数
  PHPメソッド
  zval変換
  zend_string変換
  例外変換
  オブジェクトハンドラ
  INI設定
  module lifecycle

Zend非依存にしたい:
  protocol parser
  buffer
  client/channel/call本体
  retry/deadline計算
  transport interface
  state machine
  error code
```

---

## PHP拡張での構成例

PHP拡張では、外部拡張単体リポジトリの場合、ルートをphpize用の拡張ルートにすることが多い。

例：

```text
grpc_lite/
  config.m4
  config.w32
  php_grpc_lite.h
  grpc_lite.c
  grpc_lite_client.c
  grpc_lite_call.c
  grpc_lite_exception.c
  grpc_lite_arginfo.h
  grpc_lite.stub.php

  src/
    gl_client.c
    gl_client.h
    gl_call.c
    gl_call.h
    gl_status.c
    gl_status.h
    gl_buffer.c
    gl_buffer.h
    gl_transport.c
    gl_transport.h
    gl_http2_frame.c
    gl_http2_frame.h

  tests/
    phpt/
    core/
```

方針：

```text
ルート:
  PHP拡張の入口・Zend依存コード

src/:
  Zend非依存のcore実装

tests/phpt:
  PHP拡張としてのテスト

tests/core:
  C core単体テスト
```

ただし、`src/`内にZend依存コードを置く構成も存在する。

その場合は、明確に分ける。

```text
src/
  php/
    client.c
    call.c
    exception.c

  core/
    client.c
    call.c
    status.c

  transport/
    transport.c
    socket.c
    mock.c
```

---

## エラー処理を分ける

core層では、ランタイム固有の例外やエラー表示を直接使わない。

悪い例：

```c
// core層でPHP例外を投げている
zend_throw_exception(...);
```

良い例：

```c
typedef enum {
    PROJECT_OK = 0,
    PROJECT_ERR_INVALID_ARGUMENT,
    PROJECT_ERR_TIMEOUT,
    PROJECT_ERR_PROTOCOL,
    PROJECT_ERR_OUT_OF_MEMORY,
    PROJECT_ERR_INTERNAL
} project_error_code;

typedef struct {
    project_error_code code;
    const char *message;
} project_status;
```

core層は`project_status`を返す。

```c
project_status project_client_call(project_client *client);
```

binding層でランタイムのエラーに変換する。

```c
// PHP binding layer
static void php_project_throw_status(project_status status)
{
    switch (status.code) {
        case PROJECT_ERR_INVALID_ARGUMENT:
            // throw InvalidArgumentException
            break;

        case PROJECT_ERR_TIMEOUT:
            // throw TimeoutException
            break;

        default:
            // throw generic exception
            break;
    }
}
```

---

## 所有権を明文化する

Cでは、誰がメモリを解放するかを明確にしないと保守不能になる。

ヘッダコメントに所有権を書く。

```c
// Creates a new client.
// Ownership: caller owns *out and must call project_client_free().
project_status project_client_new(project_client **out);

// Frees a client created by project_client_new().
void project_client_free(project_client *client);

// Returns a newly allocated buffer.
// Ownership: caller owns out->ptr and must call project_buffer_free().
project_status project_client_call(project_client *client, project_buffer *out);
```

最低限、以下を明確にする。

```text
入力ポインタは借用か所有権移譲か
戻り値の所有者は誰か
解放関数は何か
NULLを許容するか
文字列はNUL終端か長さ付きか
バッファはmutableかimmutableか
```

---

## allocatorを混ぜない

異なるallocatorを混ぜると危険である。

例えばPHP拡張では以下が混在しやすい。

```text
malloc / free
emalloc / efree
persistent allocation
zend_string
独自allocator
```

原則：

```text
確保したallocatorと同じ系統のfreeで解放する
```

境界層とcore層でallocatorを分ける。

```text
binding層:
  ランタイム固有allocatorを使ってよい

core層:
  malloc/freeまたは注入されたallocatorを使う
```

必要ならallocator interfaceを定義する。

```c
typedef struct {
    void *(*malloc_fn)(void *ctx, size_t size);
    void (*free_fn)(void *ctx, void *ptr);
    void *ctx;
} project_allocator;
```

---

## 依存を注入できるようにする

テストしやすくするには、外部依存を差し替え可能にする。

差し替えたいものの例：

- transport
- clock
- allocator
- logger
- random generator
- DNS resolver
- filesystem
- network I/O

例：

```c
typedef struct {
    project_status (*connect)(void *ctx, const char *host, int port);
    project_status (*write)(void *ctx, const uint8_t *data, size_t len);
    project_status (*read)(void *ctx, project_buffer *out);
    void (*close)(void *ctx);
} project_transport_vtable;

typedef struct {
    void *ctx;
    const project_transport_vtable *vtable;
} project_transport;
```

本番ではsocket transportを使う。

```c
project_transport transport = project_socket_transport_new(...);
```

テストではmock transportを使う。

```c
project_transport transport = project_mock_transport_new(...);
```

---

## テストしやすい構造にする

テストは少なくとも2層に分ける。

```text
core単体テスト:
  ランタイム非依存のCロジックを直接テストする

統合/APIテスト:
  PHP拡張、CLI、外部APIなど公開面からテストする
```

PHP拡張の場合：

```text
tests/core:
  C単体テスト

tests/phpt:
  PHP拡張としてのテスト
```

core単体テストでは以下をテストする。

- parser
- codec
- buffer
- state machine
- deadline/retry計算
- error mapping
- mock transport
- boundary condition
- memory ownership

APIテストでは以下をテストする。

- 引数チェック
- 例外
- 戻り値
- public API
- lifecycle
- 設定
- 実際の利用例

---

## ビルドシステムで `.c` を明示的に管理する

`.c`をincludeするのではなく、ビルド対象として明示する。

Makefile例：

```make
OBJS = \
  src/client.o \
  src/buffer.o \
  src/status.o \
  main.o
```

PHP拡張の`config.m4`例：

```m4
PHP_NEW_EXTENSION(project,
  project.c \
  project_client.c \
  src/client.c \
  src/buffer.c \
  src/status.c,
  $ext_shared)
```

これにより、各`.c`が独立したコンパイル単位になる。

---

## symbol visibilityを意識する

共有ライブラリでは、`static`でない関数が外部シンボルとして見えることがある。

確認例：

```bash
nm -D modules/project.so
```

外部に見せる必要がないシンボルは、可能なら隠す。

方法：

- `.c`内だけでよいものは`static`にする
- ビルドオプションで`-fvisibility=hidden`を使う
- public APIには明示的なexport macroを使う

ただし、初期段階では過度に複雑にしなくてよい。

優先順位は以下。

```text
1. private関数をstaticにする
2. internal/public headerを分ける
3. 必要になったらvisibility制御する
```

---

## 命名規則を決める

Cには名前空間がないため、関数名・型名・マクロ名にはプロジェクトprefixを付ける。

悪い例：

```c
client_new()
buffer_free()
status_message()
```

良い例：

```c
project_client_new()
project_buffer_free()
project_status_message()
```

短縮prefixを使う場合：

```c
gl_client_new()
gl_buffer_free()
gl_status_message()
```

マクロもprefixを付ける。

```c
#ifndef PROJECT_CLIENT_H
#define PROJECT_CLIENT_H
```

enum値にもprefixを付ける。

```c
typedef enum {
    PROJECT_OK = 0,
    PROJECT_ERR_TIMEOUT,
    PROJECT_ERR_INTERNAL
} project_error_code;
```

---

## 既存プロジェクトを改善する手順

既存のCプロジェクトを整理する場合、一気に大規模変更しない。

段階的に行う。

### Step 1: `.c` includeを洗い出す

```bash
grep -R '#include ".*\.c"' .
```

`.c`をincludeしている箇所を特定する。

### Step 2: 対応する `.h` を作る

例えば、

```c
#include "client.c"
```

をやめるために、

```text
client.c
client.h
```

へ分ける。

`client.h`には他ファイルから必要な宣言だけを書く。

### Step 3: ビルド対象に `.c` を追加する

Makefile、CMake、Meson、config.m4などに対象`.c`を追加する。

```text
src/client.c
src/buffer.c
src/status.c
```

### Step 4: private関数を`static`にする

その`.c`内でしか使わない関数に`static`を付ける。

```c
static int parse_header(...);
static void reset_state(...);
```

### Step 5: internal headerを整理する

複数`.c`で共有する関数はinternal headerに置く。

```text
src/client.h
src/buffer.h
src/status.h
```

本当に外部公開するものだけ`include/`へ出す。

### Step 6: ランタイム依存を境界層に寄せる

PHP/Zend API、Python C API、OS固有APIなどがcore層に混ざっている場合、adapter層へ移動する。

### Step 7: core単体テストを追加する

まずは副作用の少ない部品からテストする。

- buffer
- status
- parser
- codec
- state machine

---

## 推奨ディレクトリ構成

### public C APIを持たない小〜中規模プロジェクト

```text
project/
  project.c
  project.h

  src/
    client.c
    client.h
    buffer.c
    buffer.h
    status.c
    status.h
    transport.c
    transport.h

  tests/
    test_client.c
    test_buffer.c
```

### public C APIを持つライブラリ

```text
project/
  include/
    project/
      project.h
      client.h

  src/
    client.c
    client_internal.h
    buffer.c
    buffer_internal.h
    status.c

  tests/
    test_client.c
    test_buffer.c
```

### PHP拡張

```text
project/
  config.m4
  config.w32
  php_project.h
  project.c
  project_class.c
  project_exception.c
  project_arginfo.h
  project.stub.php

  src/
    core/
      client.c
      client.h
      call.c
      call.h
      status.c
      status.h

    transport/
      transport.c
      transport.h
      socket.c
      socket.h

    util/
      buffer.c
      buffer.h

  tests/
    phpt/
    core/
```

---

## レビュー時のチェックリスト

コードレビューでは以下を確認する。

```text
[ ] .c を include していない
[ ] ヘッダにはinclude guardがある
[ ] ヘッダに不要なincludeがない
[ ] .c内だけの関数はstaticになっている
[ ] public/internal/privateの境界が分かる
[ ] src/*.hを外部公開APIとして扱っていない
[ ] include/配下にinternal詳細が漏れていない
[ ] 構造体の中身を不要に公開していない
[ ] 所有権がコメントで分かる
[ ] allocatorの混在がない
[ ] core層が特定ランタイムに依存していない
[ ] error handlingが層をまたいで崩れていない
[ ] mock可能な依存になっている
[ ] core単体テストを書ける構造になっている
[ ] ビルドシステムに.cが明示されている
[ ] 名前にプロジェクトprefixが付いている
```

---

## まとめ

Cプロジェクトの保守性は、主に「境界」を明確にすることで向上する。

重要な原則は以下。

```text
.cから.cをincludeしない
.hには宣言、.cには実装を書く
ヘッダにはinclude guardを書く
.c内だけの関数はstaticにする
src/*.hはinternal、include/*.hはpublicとして扱う
構造体の中身は必要がなければ隠す
所有権を明文化する
外部ランタイム依存は境界層に閉じ込める
coreは単体テスト可能にする
依存は一方向にする
```

特に既存プロジェクトでは、最初から完璧な構成を目指す必要はない。

まずは以下から始める。

```text
1. .c includeをやめる
2. .hに宣言を切り出す
3. private関数をstaticにする
4. ビルド対象を整理する
5. core部分をテスト可能にする
```

この5つだけでも、Cプロジェクトの見通しと保守性は大きく改善する。
