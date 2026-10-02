# kilo-ncurses

ncurses を使った、小さなターミナルテキストエディタです。Salvatore Sanfilippo (antirez) 氏の
[kilo](https://github.com/antirez/kilo) に着想を得て、ncurses を前提に一から書き直しました。
kilo のコードの移植ではなく、独立した実装です。

## 機能

- ファイルの表示・編集・保存（一時ファイルへ書いてから `rename` する安全な保存、パーミッションを保持）
- インクリメンタル検索（矢印キーで次・前の一致へ、Esc で元の位置へ戻る）
- 構文ハイライト: C / C++、Scheme（拡張子で自動判定）
- UTF-8 対応: 日本語などの入力、表示幅（全角 2 桁）、文字単位のカーソル移動と削除
- ステータスバー、未保存の変更がある場合の終了確認、端末サイズ変更への追従

## ビルド

### 必要なもの

- C99 コンパイラ（gcc または clang）
- make
- ncursesw（ワイド文字版 ncurses）の開発パッケージ
- pkg-config（任意。ncursesw の場所を自動で見つけるために使います）
- Python 3（`make test` を実行する場合のみ）

### 依存パッケージのインストール

```sh
# Debian / Ubuntu
sudo apt-get install build-essential libncurses-dev pkg-config

# Fedora
sudo dnf install gcc make ncurses-devel pkgconf-pkg-config

# Arch Linux
sudo pacman -S base-devel ncurses pkgconf

# macOS（Homebrew。動作未確認）
brew install ncurses pkg-config
export PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig"
```

### ビルドと実行

```sh
git clone https://github.com/sluchin/kilo-ncurses.git
cd kilo-ncurses
make
./kilo-ncurses [ファイル名]
```

カレントディレクトリに実行ファイル `kilo-ncurses` ができます。`install` ターゲットはないので、
必要なら PATH の通った場所へ手動でコピーしてください。

```sh
sudo install -m 755 kilo-ncurses /usr/local/bin/
```

### ビルドオプション

コンパイラやフラグは make の変数で変えられます。

```sh
make CC=clang                 # コンパイラを指定
make CFLAGS="-O0 -g"          # 最適化なし・デバッグ情報付き
make clean                    # 生成物を削除
```

pkg-config が使えない環境では `-lncursesw` でリンクします。ヘッダや
ライブラリが標準の場所にない場合は、次のように指定してください。

```sh
make CFLAGS="-O2 -I/opt/ncurses/include" LDLIBS="-L/opt/ncurses/lib -lncursesw"
```

## キー操作

| キー | 動作 |
|---|---|
| Ctrl-S | 保存（ファイル名がなければ入力を求める） |
| Ctrl-Q | 終了（未保存の変更があるときは、さらに 2 回押す） |
| Ctrl-F | 検索（入力するたびに移動、矢印キーで次・前、Enter で確定、Esc で取り消し） |
| 矢印 / Home / End / PageUp / PageDown | カーソル移動（Ctrl-A / Ctrl-E は行頭・行末） |
| Backspace / Delete | 削除 |
| Ctrl-L | 再描画 |

## テスト

疑似端末（pty）でエディタを実際に動かす自動テストがあります。

```sh
make test
```

## kilo との主な違い

| | kilo | kilo-ncurses |
|---|---|---|
| 端末の制御 | termios とエスケープシーケンスを直接扱う | ncurses |
| 依存 | なし | ncursesw |
| 文字 | バイト単位 | UTF-8（表示幅を考慮） |
| 保存 | そのまま書き込み | 一時ファイル + `rename` |

## 既知の制限

- 改行コードは保存時に LF にそろえます（CRLF のファイルは LF になります）。
- 元に戻す / やり直しはありません。
- 検索は大文字と小文字を区別します。

## ライセンス

GNU General Public License v3.0（`LICENSE` を参照）。
