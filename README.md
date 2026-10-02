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

必要なもの: C99 コンパイラ、ncursesw（ワイド文字版 ncurses）の開発パッケージ、make。

```sh
# Debian / Ubuntu
sudo apt-get install build-essential libncurses-dev

make
./kilo-ncurses [ファイル名]
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
