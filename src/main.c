/**
 * @file main.c
 * @brief kilo-ncurses: ncurses で作った小さなターミナルテキストエディタ.
 *
 * 機能: ファイルの表示・編集・保存, インクリメンタル検索, C と Scheme の
 * 構文ハイライト, UTF-8 を考慮したカーソル移動と表示, ステータスバー.
 *
 * テキストは行の配列として持つ. 各行は UTF-8 のバイト列をそのまま保持するので,
 * カーソル位置はすべて「バイトオフセット」で表す. 表示上の桁数は必要になったとき
 * char_width() で計算する. エディタの状態はすべてグローバル変数 #E に置く.
 */

/* POSIX.1-2008 (getline, mkstemp, fsync) と XSI のワイド文字 API を有効にする. */
#define _XOPEN_SOURCE 700
#define _XOPEN_SOURCE_EXTENDED 1

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define VERSION "0.1.0"    /**< ウェルカム画面に表示するバージョン文字列. */
#define TABSTOP 8          /**< タブ幅(表示桁数). */
#define QUIT_CONFIRMS 2    /**< 未保存のまま終了するために追加で必要な Ctrl-Q の回数. */
#define QUERY_MAX 256      /**< プロンプト入力の最大長(バイト). */

/**
 * @brief 1 バイトごとに Line::hl へ記録するハイライトの種類.
 *
 * 値は ncurses のカラーペア番号としても使う(HL_NORMAL は端末の既定色なので
 * ペアは不要).
 */
enum { HL_NORMAL, HL_COMMENT, HL_KEYWORD, HL_TYPE, HL_STRING, HL_NUMBER, HL_PREPROC, HL_MATCH };

/* ------------------------------------------------------------------ */
/* 構文定義                                                            */
/* ------------------------------------------------------------------ */

/** @brief 言語ごとのハイライト規則. */
typedef struct {
    const char *name;
    const char *const *files; /* 拡張子(".c")または完全一致のファイル名 */
    const char *const *keywords;
    const char *const *types;
    const char *line_comment; /* 行末までのコメントを始める文字列 */
    const char *block_open;   /* 複数行コメントの開始 */
    const char *block_close;  /* 複数行コメントの終了 */
    bool lisp_words;          /* 単語の区切りを空白・括弧・引用符だけにする */
    bool preproc;             /* '#' で始まる行をプリプロセッサ行として色付けする */
    bool char_literals;       /* 'x' を文字リテラルとして扱う */
} Syntax;

static const char *const c_files[] = {".c", ".h", ".cpp", ".hpp", ".cc", NULL};
static const char *const c_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "sizeof", "typedef", "struct", "union", "enum",
    "static", "extern", "const", "volatile", "register", "inline", "NULL",
    "true", "false", "class", "namespace", "template", "new", "delete", NULL};
static const char *const c_types[] = {
    "int", "long", "short", "char", "float", "double", "void", "unsigned",
    "signed", "bool", "size_t", "ssize_t", "int8_t", "int16_t", "int32_t",
    "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE", NULL};

static const char *const scm_files[] = {".scm", ".ss", ".sld", ".sps", ".rkt", ".lisp", ".el", NULL};
static const char *const scm_keywords[] = {
    "define", "lambda", "let", "let*", "letrec", "letrec*", "if", "cond", "case",
    "else", "and", "or", "not", "when", "unless", "do", "begin", "set!", "quote",
    "quasiquote", "unquote", "define-syntax", "let-syntax", "syntax-rules",
    "define-record-type", "define-module", "use-modules", "import", "let-values",
    "delay", "force", "call/cc", "call-with-current-continuation", NULL};
static const char *const scm_types[] = {
    "car", "cdr", "cons", "list", "append", "map", "for-each", "apply", "length",
    "reverse", "null?", "pair?", "eq?", "eqv?", "equal?", "number?", "string?",
    "symbol?", "procedure?", "display", "write", "newline", "vector", "assq",
    "assoc", "member", "#t", "#f", NULL};

/* 対応する言語の一覧. select_syntax() がファイル名から 1 つ選ぶ. */
static const Syntax syntaxes[] = {
    {"C", c_files, c_keywords, c_types, "//", "/*", "*/", false, true, true},
    {"Scheme", scm_files, scm_keywords, scm_types, ";", "#|", "|#", true, false, false},
};
/** @brief syntaxes[] の要素数. */
#define SYNTAX_COUNT (sizeof(syntaxes) / sizeof(syntaxes[0]))

/* ------------------------------------------------------------------ */
/* エディタの状態                                                      */
/* ------------------------------------------------------------------ */

/** @brief テキストの 1 行(末尾の改行は含まない). */
typedef struct {
    char *s;                /* UTF-8 のバイト列. 常に NUL で終わる */
    size_t len, cap;        /* 使用バイト数(NUL を除く) / 確保バイト数 */
    unsigned char *hl;      /* バイトごとの HL_* 種別. s と同じ長さ */
    bool open_comment;      /* 行末がブロックコメントの途中 */
} Line;

/** @brief エディタ全体の状態(グローバルに 1 つだけ). */
static struct {
    Line *lines;
    int n, cap;
    int cx, cy;         /* カーソル: 行内のバイトオフセット, 行番号 */
    int want_col;       /* 上下移動で保ちたい表示桁 */
    int rowoff, coloff; /* 表示先頭の行 / 表示先頭の桁 */
    char *filename;
    bool dirty;
    const Syntax *syntax;
    char msg[160];    /* メッセージバーの文字列 */
    time_t msg_time;  /* msg を設定した時刻. 5 秒で消える */
    int quit_left;    /* 終了までに必要な Ctrl-Q の残り回数 */
    bool screen_on;   /* ncurses が有効(endwin() がまだ必要) */
    bool color;       /* 端末が色に対応している */
    int prompt_col;   /* プロンプト入力中のカーソル桁, 入力中でなければ -1 */
} E;

/** @brief インクリメンタル検索の状態(グローバルに 1 つだけ). */
static struct {
    int row;               /* 現在の一致がある行. まだ一致がなければ -1 */
    size_t col, len;       /* 一致位置(バイト)と長さ(len が 0 なら一致なし) */
    int dir;               /* 検索方向: 前方 +1, 後方 -1 */
    int from_cy, from_cx;  /* 検索開始時のカーソル(Esc で戻す) */
} S = {-1, 0, 0, 1, 0, 0};

/**
 * @brief メッセージバーの文字列を設定する(printf 形式).
 *
 * メッセージは 5 秒間, またはプロンプトが開いている間表示される.
 *
 * @param fmt printf 形式の書式文字列.
 * @param ... 書式に対応する引数.
 */
static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof E.msg, fmt, ap);
    va_end(ap);
    E.msg_time = time(NULL);
}

/**
 * @brief ncurses モードを抜けて端末を元の状態に戻す.
 *
 * 何度呼んでも安全. atexit() にも登録している.
 */
static void shutdown_screen(void)
{
    if (E.screen_on) {
        endwin();
        E.screen_on = false;
    }
}

/**
 * @brief 端末を元に戻し, `what: strerror(errno)` を表示して exit(1) する.
 * @param what 失敗した操作の短い説明.
 */
static void die(const char *what)
{
    int err = errno;
    shutdown_screen();
    fprintf(stderr, "kilo-ncurses: %s: %s\n", what, strerror(err));
    exit(1);
}

/**
 * @brief 失敗しない realloc(). メモリ不足のときはエディタを異常終了させる.
 *
 * サイズ 0 の要求は 1 に切り上げるので, NULL は常に失敗を意味する.
 *
 * @param p 再確保するメモリ. NULL なら新規に確保する.
 * @param n 新しいサイズ(バイト).
 * @return 確保したメモリへのポインタ(NULL にはならない).
 */
static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

/* ------------------------------------------------------------------ */
/* UTF-8 の補助関数と表示幅                                            */
/* ------------------------------------------------------------------ */

enum { K_PLAIN, K_TAB, K_CTRL, K_BAD };

/**
 * @brief @p s の先頭にある 1 文字の幅を測る.
 * @param s     文字の先頭から始まるバイト列(UTF-8).
 * @param n     @p s の残りバイト数.
 * @param col   この文字が始まる表示桁(タブの幅はこれで決まる).
 * @param[out] adv  この文字が占めるバイト数.
 * @param[out] kind K_PLAIN, K_TAB, K_CTRL(^X と表示), K_BAD(不正な UTF-8).
 * @return 表示幅(桁数).
 */
static int char_width(const char *s, size_t n, int col, size_t *adv, int *kind)
{
    unsigned char c = (unsigned char)s[0];
    *kind = K_PLAIN;
    /* タブは次の TABSTOP の倍数まで進む. */
    if (c == '\t') {
        *adv = 1;
        *kind = K_TAB;
        return TABSTOP - col % TABSTOP;
    }
    /* 制御文字は 2 桁(^X)で描く. */
    if (c < 0x20 || c == 0x7f) {
        *adv = 1;
        *kind = K_CTRL;
        return 2;
    }
    /* ASCII の高速パス. */
    if (c < 0x80) {
        *adv = 1;
        return 1;
    }
    /* マルチバイト: 復号と幅の判定は C ライブラリに任せる. */
    wchar_t wc;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = mbrtowc(&wc, s, n, &st);
    /* 不正または途中で切れた列: 1 バイトを 1 桁("?")として扱う. */
    if (r == (size_t)-1 || r == (size_t)-2 || r == 0) {
        *adv = 1;
        *kind = K_BAD;
        return 1;
    }
    *adv = r;
    int w = wcwidth(wc);
    return w < 0 ? 1 : w; /* wcwidth() が負なら表示不能なので 1 桁にする */
}

/**
 * @brief 行 @p l のバイトオフセット @p upto に対応する表示桁を返す.
 *
 * @param l 対象の行.
 * @param upto 行内のバイトオフセット.
 * @return 行頭からの表示桁.
 */
static int disp_col(const Line *l, size_t upto)
{
    int col = 0;
    size_t i = 0;
    while (i < upto && i < l->len) {
        size_t adv;
        int kind;
        col += char_width(l->s + i, l->len - i, col, &adv, &kind);
        i += adv;
    }
    return col;
}

/**
 * @brief 表示桁 @p target を覆う文字のバイトオフセットを返す.
 *
 * 上下移動で使う. @p target をまたぐ全角文字は飛ばさないので,
 * カーソルが文字の途中に来ることはない.
 *
 * @param l 対象の行.
 * @param target 表示桁.
 * @return 行内のバイトオフセット.
 */
static size_t col_to_off(const Line *l, int target)
{
    int col = 0;
    size_t i = 0;
    while (i < l->len) {
        size_t adv;
        int kind;
        int w = char_width(l->s + i, l->len - i, col, &adv, &kind);
        if (col + w > target) /* この文字が target を覆うか越える */
            break;
        col += w;
        i += adv;
    }
    return i;
}

/**
 * @brief @p pos にある文字の次の文字のバイトオフセットを返す
 *        (行の長さで頭打ち).
 *
 * @param l 対象の行.
 * @param pos 行内のバイトオフセット.
 * @return 次の文字のバイトオフセット.
 */
static size_t next_cp(const Line *l, size_t pos)
{
    size_t adv;
    int kind;
    if (pos >= l->len)
        return l->len;
    char_width(l->s + pos, l->len - pos, 0, &adv, &kind);
    return pos + adv;
}

/**
 * @brief @p pos の直前にある文字のバイトオフセットを返す.
 *
 * UTF-8 の継続バイト(10xxxxxx)を最大 4 バイトまで戻る.
 *
 * @param l 対象の行.
 * @param pos 行内のバイトオフセット.
 * @return 直前の文字のバイトオフセット.
 */
static size_t prev_cp(const Line *l, size_t pos)
{
    if (pos == 0)
        return 0;
    size_t p = pos - 1;
    while (p > 0 && ((unsigned char)l->s[p] & 0xC0) == 0x80 && pos - p < 4)
        p--;
    return p;
}

/* ------------------------------------------------------------------ */
/* 構文ハイライト                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief @p c がその言語で単語の区切りかどうかを返す.
 *
 * C 系の言語では識別子に使えない文字すべてが区切りになる. Lisp 系では
 * 空白・括弧・引用符だけが区切りなので, `let*` や `set!` は 1 語のままになる.
 *
 * @param sy 言語の定義.
 * @param c 調べるバイト.
 * @return 区切りなら true.
 */
static bool is_delim(const Syntax *sy, unsigned char c)
{
    if (c == '\0' || isspace(c))
        return true;
    if (sy->lisp_words)
        return strchr("()[]{}\"';`,", c) != NULL;
    return !(isalnum(c) || c == '_' || c >= 0x80);
}

/**
 * @brief 行 @p l のバイトオフセット @p i に文字列 @p p があるかどうかを返す.
 *
 * @param l 対象の行.
 * @param i 行内のバイトオフセット.
 * @param p 比較する文字列.
 * @return 一致すれば true.
 */
static bool at(const Line *l, size_t i, const char *p)
{
    size_t n = strlen(p);
    return i + n <= l->len && memcmp(l->s + i, p, n) == 0;
}

/**
 * @brief @p w から @p n バイトが, NULL 終端の @p list のどれかと一致するかどうかを返す.
 *
 * @param list NULL 終端の文字列の配列.
 * @param w 調べる文字列の先頭.
 * @param n 調べる文字列のバイト数.
 * @return 一致する要素があれば true.
 */
static bool in_list(const char *const *list, const char *w, size_t n)
{
    for (; *list; list++)
        if (strlen(*list) == n && memcmp(*list, w, n) == 0)
            return true;
    return false;
}

/**
 * @brief 1 行分のハイライト種別を計算する.
 * @param l        対象の行. l->hl を(再)確保して埋める.
 * @param in_block 前の行がブロックコメントの途中で終わっていれば true.
 * @return この行がブロックコメントの途中で終わるなら true.
 *
 * 左から右への 1 回の走査で行う. 繰り返しの間に持ち回る状態は
 * @c in_block(ブロックコメント中), @c quote(文字列中. 引用符の文字を保持),
 * @c prev_delim(直前のバイトが単語の終わりなので, ここから数値やキーワードが
 * 始まりうる)の 3 つ.
 */
static bool highlight_line(Line *l, bool in_block)
{
    l->hl = xrealloc(l->hl, l->len);
    memset(l->hl, HL_NORMAL, l->len);
    const Syntax *sy = E.syntax;
    if (!sy)
        return false;

    /* 以降の走査で使う短い別名. */
    unsigned char *hl = l->hl;
    const char *s = l->s;
    size_t n = l->len, i = 0;
    bool prev_delim = true;
    int quote = 0;
    size_t bo = strlen(sy->block_open), bc = strlen(sy->block_close);

    /* 最初の空白以外の文字が # の行は, プリプロセッサ行. */
    if (sy->preproc && !in_block) {
        size_t k = 0;
        while (k < n && isspace((unsigned char)s[k]))
            k++;
        if (k < n && s[k] == '#') {
            memset(hl + k, HL_PREPROC, n - k);
            return false;
        }
    }

    while (i < n) {
        unsigned char c = (unsigned char)s[i];

        /* ブロックコメント中: 終了記号まですべてコメント色にする. */
        if (in_block) {
            hl[i] = HL_COMMENT;
            if (at(l, i, sy->block_close)) {
                memset(hl + i, HL_COMMENT, bc);
                i += bc;
                in_block = false;
                prev_delim = true;
            } else {
                i++;
            }
            continue;
        }
        /* 文字列または文字リテラルの中. */
        if (quote) {
            hl[i] = HL_STRING;
            if (c == '\\' && i + 1 < n) { /* バックスラッシュのエスケープ: 次のバイトを飛ばす */
                hl[i + 1] = HL_STRING;
                i += 2;
                continue;
            }
            if (c == quote) {
                quote = 0;
                prev_delim = true;
            }
            i++;
            continue;
        }
        /* 行コメント: 行末まですべて. */
        if (at(l, i, sy->line_comment)) {
            memset(hl + i, HL_COMMENT, n - i);
            return false;
        }
        /* ブロックコメントの開始(同じ行で閉じることもある). */
        if (at(l, i, sy->block_open)) {
            memset(hl + i, HL_COMMENT, bo);
            i += bo;
            in_block = true;
            continue;
        }
        /* Scheme の文字リテラル. 例: #\a, #\space. */
        if (sy->lisp_words && c == '#' && i + 2 < n && s[i + 1] == '\\') {
            size_t j = i + 3;
            while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80)
                j++;
            while (j < n && !is_delim(sy, (unsigned char)s[j]))
                j++;
            memset(hl + i, HL_STRING, j - i);
            i = j;
            prev_delim = false;
            continue;
        }
        /* 文字列(または C の文字リテラル)の開始. */
        if (c == '"' || (sy->char_literals && c == '\'')) {
            quote = c;
            hl[i++] = HL_STRING;
            prev_delim = false;
            continue;
        }
        /* 数値: 単語の境目で数字(または .5)から始まる. */
        if (prev_delim && (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1])))) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_'))
                j++;
            memset(hl + i, HL_NUMBER, j - i);
            i = j;
            prev_delim = false;
            continue;
        }
        /* 単語: キーワードか型に分類し, どちらでもなければ通常のままにする. */
        if (prev_delim && !is_delim(sy, c)) {
            size_t j = i;
            while (j < n && !is_delim(sy, (unsigned char)s[j]))
                j++;
            int kind = HL_NORMAL;
            if (in_list(sy->keywords, s + i, j - i))
                kind = HL_KEYWORD;
            else if (in_list(sy->types, s + i, j - i))
                kind = HL_TYPE;
            memset(hl + i, kind, j - i);
            i = j;
            prev_delim = false;
            continue;
        }
        /* それ以外のバイト: 単語の終わりかどうかだけを覚えておく. */
        prev_delim = is_delim(sy, c);
        i++;
    }
    return in_block;
}

/**
 * @brief 行 @p from からハイライトを再計算する.
 * @param from  最初に再計算する行.
 * @param force 必ず再計算する行数.
 *
 * それ以降の行は「ブロックコメントの途中で終わる」状態が変わり続ける間だけ
 * 再計算する. 通常の行で入力しても 1 行分の計算で済み, コメントを開いたときだけ
 * 下の行がすべて塗り直される.
 */
static void rehighlight(int from, int force)
{
    if (from < 0)
        from = 0;
    bool in = from > 0 ? E.lines[from - 1].open_comment : false;
    for (int j = from; j < E.n; j++) {
        bool old = E.lines[j].open_comment;
        E.lines[j].open_comment = highlight_line(&E.lines[j], in);
        in = E.lines[j].open_comment;
        /* 強制範囲を過ぎて状態が変わらなければ, これより下も変わらない. */
        if (j - from + 1 >= force && old == in)
            break;
    }
}

/**
 * @brief @p filename の拡張子またはベース名から E.syntax を選ぶ.
 *
 * 一致するものがなければ E.syntax は NULL(プレーンテキスト)のまま.
 *
 * @param filename ファイル名. NULL ならプレーンテキストにする.
 */
static void select_syntax(const char *filename)
{
    E.syntax = NULL;
    if (!filename)
        return;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    const char *dot = strrchr(base, '.');
    for (size_t k = 0; k < SYNTAX_COUNT; k++)
        for (const char *const *f = syntaxes[k].files; *f; f++)
            if ((dot && strcmp(dot, *f) == 0) || strcmp(base, *f) == 0) {
                E.syntax = &syntaxes[k];
                return;
            }
}

/* ------------------------------------------------------------------ */
/* テキストバッファ                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 行 @p l が NUL を含めて @p need バイトを保持できるようにする(容量は倍々に増やす).
 *
 * @param l 対象の行.
 * @param need 保持したいバイト数(NUL を除く).
 */
static void line_reserve(Line *l, size_t need)
{
    if (need + 1 > l->cap) {
        size_t nc = l->cap ? l->cap : 16;
        while (nc < need + 1)
            nc *= 2;
        l->s = xrealloc(l->s, nc);
        l->cap = nc;
    }
}

/**
 * @brief 行 @p l のバイトオフセット @p at_ に @p s の @p n バイトを挿入する.
 *
 * @param l 対象の行.
 * @param at_ 挿入位置(バイトオフセット).
 * @param s 挿入するバイト列.
 * @param n 挿入するバイト数.
 */
static void line_insert(Line *l, size_t at_, const char *s, size_t n)
{
    line_reserve(l, l->len + n);
    memmove(l->s + at_ + n, l->s + at_, l->len - at_);
    memcpy(l->s + at_, s, n);
    l->len += n;
    l->s[l->len] = '\0';
}

/**
 * @brief 行 @p l のバイトオフセット @p at_ から @p n バイトを削除する.
 *
 * @param l 対象の行.
 * @param at_ 削除を始める位置(バイトオフセット).
 * @param n 削除するバイト数.
 */
static void line_remove(Line *l, size_t at_, size_t n)
{
    memmove(l->s + at_, l->s + at_ + n, l->len - at_ - n);
    l->len -= n;
    l->s[l->len] = '\0';
}

/**
 * @brief 指定の文字列を持つ新しい行を, 行番号 @p at_ として挿入する.
 *
 * それ以降の行は 1 つ下にずれる. 新しい行のハイライトはまだ計算されていない.
 *
 * @param at_ 挿入先の行番号.
 * @param s 新しい行の文字列.
 * @param n 文字列のバイト数.
 */
static void buf_insert_row(int at_, const char *s, size_t n)
{
    if (E.n == E.cap) {
        E.cap = E.cap ? E.cap * 2 : 64;
        E.lines = xrealloc(E.lines, (size_t)E.cap * sizeof(Line));
    }
    memmove(&E.lines[at_ + 1], &E.lines[at_], (size_t)(E.n - at_) * sizeof(Line));
    /* 隙間を空けてから, そこに新しい行を初期化する. */
    Line *l = &E.lines[at_];
    memset(l, 0, sizeof *l);
    line_reserve(l, n);
    memcpy(l->s, s, n);
    l->len = n;
    l->s[n] = '\0';
    E.n++;
}

/**
 * @brief 行番号 @p at_ の行を削除してメモリを解放する.
 *
 * @param at_ 削除する行番号.
 */
static void buf_delete_row(int at_)
{
    free(E.lines[at_].s);
    free(E.lines[at_].hl);
    memmove(&E.lines[at_], &E.lines[at_ + 1], (size_t)(E.n - at_ - 1) * sizeof(Line));
    E.n--;
}

/**
 * @brief カーソルの表示桁を覚えておく.
 *
 * 上下移動はこの桁を目指すので, 短い行を通り過ぎても
 * カーソルが左へずれていかない.
 */
static void update_want(void)
{
    E.want_col = disp_col(&E.lines[E.cy], (size_t)E.cx);
}

/* ------------------------------------------------------------------ */
/* 編集操作                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief カーソル位置に @p n バイト(UTF-8 の 1 文字)を挿入する.
 *
 * @param s 挿入するバイト列.
 * @param n 挿入するバイト数.
 */
static void insert_text(const char *s, size_t n)
{
    line_insert(&E.lines[E.cy], (size_t)E.cx, s, n);
    E.cx += (int)n;
    E.dirty = true;
    rehighlight(E.cy, 1);
    update_want();
}

/** @brief 現在の行をカーソル位置で分割する(Enter). */
static void insert_newline(void)
{
    Line *l = &E.lines[E.cy];
    size_t tail = l->len - (size_t)E.cx;
    buf_insert_row(E.cy + 1, l->s + E.cx, tail);
    /* buf_insert_row() が配列を移動したかもしれないので, ポインタを取り直す. */
    l = &E.lines[E.cy];
    l->len = (size_t)E.cx;
    l->s[l->len] = '\0';
    E.cy++;
    E.cx = 0;
    E.dirty = true;
    rehighlight(E.cy - 1, 2); /* 分割した両方の行 */
    update_want();
}

/**
 * @brief カーソルの前の文字を削除する(Backspace).
 *
 * 行頭では前の行と連結する.
 */
static void delete_back(void)
{
    Line *l = &E.lines[E.cy];
    if (E.cx > 0) {
        size_t p = prev_cp(l, (size_t)E.cx);
        line_remove(l, p, (size_t)E.cx - p);
        E.cx = (int)p;
        rehighlight(E.cy, 1);
    } else if (E.cy > 0) {
        /* 前の行と連結する. カーソルは連結位置に移る. */
        Line *prev = &E.lines[E.cy - 1];
        size_t pos = prev->len;
        line_insert(prev, prev->len, l->s, l->len);
        buf_delete_row(E.cy);
        E.cy--;
        E.cx = (int)pos;
        rehighlight(E.cy, 2);
    } else {
        return;
    }
    E.dirty = true;
    update_want();
}

/**
 * @brief カーソル位置の文字を削除する(Delete).
 *
 * 行末では次の行をこの行に連結する.
 */
static void delete_forward(void)
{
    Line *l = &E.lines[E.cy];
    if ((size_t)E.cx < l->len) {
        size_t nx = next_cp(l, (size_t)E.cx);
        line_remove(l, (size_t)E.cx, nx - (size_t)E.cx);
        rehighlight(E.cy, 1);
    } else if (E.cy < E.n - 1) {
        Line *next = &E.lines[E.cy + 1];
        line_insert(l, l->len, next->s, next->len);
        buf_delete_row(E.cy + 1);
        rehighlight(E.cy, 2);
    } else {
        return;
    }
    E.dirty = true;
}

/* ------------------------------------------------------------------ */
/* ファイル入出力                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief @p path をバッファに読み込み, 名前から構文を選ぶ.
 *
 * ファイルが存在しなくてもエラーにはしない. 空の状態で始まり,
 * 最初の保存でファイルが作られる. 行末の CR と LF は取り除く.
 *
 * @param path 読み込むファイルのパス.
 */
static void open_file(const char *path)
{
    free(E.filename);
    E.filename = strdup(path);
    if (!E.filename)
        die("out of memory");
    select_syntax(path);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT)
            die(path);
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1) {
        /* 行末(LF または CRLF)を取り除く. */
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        buf_insert_row(E.n, line, (size_t)len);
    }
    free(line);
    fclose(fp);
    if (E.n > 0)
        rehighlight(0, E.n);
}

/**
 * @brief バッファを @p path へアトミックに書き出す.
 * @param path  書き込み先のファイル.
 * @param[out] bytes 成功時に書き込んだバイト数.
 * @return 成功なら true. 失敗時は errno が設定され, 元のファイルは変更されない.
 *
 * テキストは同じディレクトリの一時ファイルに書き, flush(fsync)し,
 * 元のパーミッションを設定してから目的のファイルへ rename する.
 * そのため, 途中で落ちても書きかけのファイルが残ることはない.
 */
static bool write_file(const char *path, size_t *bytes)
{
    /* rename() が同じファイルシステム内で済むよう, 一時ファイルは対象の隣に作る. */
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return false;
    }
    int fd = mkstemp(tmp);
    if (fd == -1)
        return false;
    struct stat st;
    /* 既存ファイルのパーミッションは保つ. 新規ファイルは 0644 にする. */
    mode_t mode = stat(path, &st) == 0 ? st.st_mode & 07777 : 0644;
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        return false;
    }
    size_t total = 0;
    for (int i = 0; i < E.n; i++) {
        if (fwrite(E.lines[i].s, 1, E.lines[i].len, fp) != E.lines[i].len || fputc('\n', fp) == EOF)
            goto fail;
        total += E.lines[i].len + 1;
    }
    /* 古いファイルを置き換える前に, データをディスクへ確実に書き出す. */
    if (fflush(fp) != 0 || fsync(fd) != 0 || fchmod(fd, mode) != 0)
        goto fail;
    if (fclose(fp) != 0) {
        unlink(tmp);
        return false;
    }
    /* 対象のファイルをアトミックに置き換える. */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    *bytes = total;
    return true;
fail:
    fclose(fp);
    unlink(tmp);
    return false;
}

/* ------------------------------------------------------------------ */
/* 画面                                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief テキストに使う画面の行数(ステータスバーとメッセージバーを除く).
 *
 * @return テキスト行の数(最小 1).
 */
static int text_rows(void)
{
    return LINES > 2 ? LINES - 2 : 1;
}

/** @brief カーソルがウィンドウ内に収まるよう E.rowoff / E.coloff を調整する. */
static void scroll_to_cursor(void)
{
    int rows = text_rows();
    int rx = disp_col(&E.lines[E.cy], (size_t)E.cx);
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + rows)
        E.rowoff = E.cy - rows + 1;
    /* 横スクロールはバイトオフセットではなく表示桁で行う. */
    if (rx < E.coloff)
        E.coloff = rx;
    if (rx >= E.coloff + COLS)
        E.coloff = rx - COLS + 1;
}

/** @brief ハイライト種別ごとにカラーペアを設定する(背景は端末の既定色のまま). */
static void init_colors(void)
{
    E.color = has_colors();
    if (!E.color)
        return;
    start_color();
    use_default_colors(); /* 以下の -1 を「端末の既定色」の意味にする */
    init_pair(HL_COMMENT, COLOR_CYAN, -1);
    init_pair(HL_KEYWORD, COLOR_YELLOW, -1);
    init_pair(HL_TYPE, COLOR_GREEN, -1);
    init_pair(HL_STRING, COLOR_MAGENTA, -1);
    init_pair(HL_NUMBER, COLOR_RED, -1);
    init_pair(HL_PREPROC, COLOR_BLUE, -1);
    init_pair(HL_MATCH, COLOR_BLACK, COLOR_YELLOW);
}

/**
 * @brief ハイライト種別 @p h に対応する描画属性を選ぶ.
 *
 * 色に対応していない端末では, 太字・暗め・下線・反転で代用する.
 *
 * @param h ハイライト種別(HL_*).
 */
static void set_hl_attr(int h)
{
    if (h == HL_NORMAL) {
        attrset(A_NORMAL);
    } else if (E.color) {
        attrset(COLOR_PAIR(h) | (h == HL_KEYWORD ? A_BOLD : 0));
    } else {
        attrset(h == HL_MATCH ? A_REVERSE : h == HL_COMMENT ? A_DIM : h == HL_STRING ? A_UNDERLINE : A_BOLD);
    }
}

/**
 * @brief 行 @p l(番号 @p idx)を画面の行 @p y に描く.
 *
 * 横スクロール, タブ(空白に展開), 制御文字(^X と表示), 不正なバイト(? と表示),
 * 検索の一致位置の重ね描きを処理する. 左端で切れた全角文字は空白に置き換え,
 * 右端に収まらない文字は描かない.
 *
 * @param l 描く行.
 * @param idx 行番号(検索の一致位置の判定に使う).
 * @param y 描画先の画面の行.
 */
static void draw_line(const Line *l, int idx, int y)
{
    int col = 0;
    size_t i = 0;
    move(y, 0);
    while (i < l->len) {
        size_t adv;
        int kind;
        int w = char_width(l->s + i, l->len - i, col, &adv, &kind);
        int h = l->hl ? l->hl[i] : HL_NORMAL;
        /* 現在の検索の一致位置を重ねて描く. */
        if (S.len && S.row == idx && i >= S.col && i < S.col + S.len)
            h = HL_MATCH;
        int sx = col - E.coloff; /* この文字の画面上の桁 */
        if (sx + w > COLS)
            break;
        if (sx >= 0) {
            set_hl_attr(h);
            if (kind == K_TAB) {
                for (int k = 0; k < w; k++)
                    addch(' ');
            } else if (kind == K_CTRL) {
                attron(A_REVERSE);
                addch('^');
                addch((unsigned char)l->s[i] == 0x7f ? '?' : l->s[i] + '@');
                attroff(A_REVERSE);
            } else if (kind == K_BAD) {
                attron(A_REVERSE);
                addch('?');
                attroff(A_REVERSE);
            } else {
                addnstr(l->s + i, (int)adv);
            }
        } else if (sx + w > 0) {
            attrset(A_NORMAL);
            for (int k = 0; k < sx + w; k++)
                addch(' ');
        }
        col += w;
        i += adv;
    }
    attrset(A_NORMAL);
}

/** @brief テキスト行をすべて描く. 末尾より後ろは `~`, 空のバッファではウェルカム表示を出す. */
static void draw_rows(void)
{
    int rows = text_rows();
    bool empty_doc = E.n == 1 && E.lines[0].len == 0 && !E.filename;
    for (int y = 0; y < rows; y++) {
        int idx = E.rowoff + y;
        if (idx < E.n) {
            draw_line(&E.lines[idx], idx, y);
        } else if (empty_doc && y == rows / 3) {
            char w[80];
            int n = snprintf(w, sizeof w, "kilo-ncurses -- version %s", VERSION);
            mvaddstr(y, 0, "~");
            int pad = (COLS - n) / 2;
            if (pad > 1)
                mvaddstr(y, pad, w);
        } else {
            mvaddstr(y, 0, "~");
        }
    }
}

/** @brief 反転表示のステータスバーを描く(ファイル名, 行数, 構文, 位置). */
static void draw_status(void)
{
    char left[160], right[64];
    snprintf(left, sizeof left, " %.30s - %d lines%s", E.filename ? E.filename : "[No Name]", E.n,
             E.dirty ? " (modified)" : "");
    snprintf(right, sizeof right, "%s | %d/%d ", E.syntax ? E.syntax->name : "plain", E.cy + 1, E.n);
    attrset(A_REVERSE);
    mvhline(LINES - 2, 0, ' ', COLS);
    mvaddnstr(LINES - 2, 0, left, COLS);
    int rl = (int)strlen(right);
    /* 右側は, 左側と重ならないときだけ表示する. */
    if ((int)strlen(left) + rl < COLS)
        mvaddstr(LINES - 2, COLS - rl, right);
    attrset(A_NORMAL);
}

/** @brief メッセージバー(最下行)を描く. メッセージが新しいか, プロンプトが開いている間だけ表示する. */
static void draw_message(void)
{
    if (E.msg[0] && (time(NULL) - E.msg_time < 5 || E.prompt_col >= 0))
        mvaddnstr(LINES - 1, 0, E.msg, COLS);
}

/** @brief 画面全体を再描画し, カーソルを正しい位置に置く. */
static void refresh_screen(void)
{
    scroll_to_cursor();
    erase();
    draw_rows();
    draw_status();
    draw_message();
    /* プロンプト入力中は, カーソルをメッセージバーに置く. */
    if (E.prompt_col >= 0) {
        move(LINES - 1, E.prompt_col < COLS ? E.prompt_col : COLS - 1);
    } else {
        int rx = disp_col(&E.lines[E.cy], (size_t)E.cx);
        move(E.cy - E.rowoff, rx - E.coloff);
    }
    refresh();
}

/* ------------------------------------------------------------------ */
/* プロンプト                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief prompt() がキーを受けるたびに呼ぶコールバック.
 * @param buf     現在の入力テキスト.
 * @param key     押されたキー.
 * @param special @p key がファンクションキー(KEY_*)なら true, 文字なら false.
 */
typedef void (*PromptCb)(const char *buf, wint_t key, bool special);

/**
 * @brief ワイド文字 @p k を UTF-8 にして @p out(8 バイト以上)へ書く.
 * @return 書き込んだバイト数. 符号化できなければ 0.
 *
 * @param k 符号化するワイド文字.
 * @param[out] out 結果を書き込む領域(8 バイト以上).
 */
static size_t utf8_encode(wint_t k, char *out)
{
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = wcrtomb(out, (wchar_t)k, &st);
    return r == (size_t)-1 ? 0 : r;
}

/**
 * @brief メッセージバーで 1 行のテキストを読み取る.
 * @param label 入力の前に表示する文字列.
 * @param cb    キーごとに呼ばれる任意のコールバック(インクリメンタル検索で使う).
 * @return malloc された入力文字列. Esc が押されたら NULL.
 *
 * 空の入力での Enter は無視する. コールバックには最後の Enter / Esc も渡るので,
 * 後始末ができる.
 */
static char *prompt(const char *label, PromptCb cb)
{
    size_t cap = 64, len = 0;
    char *buf = xrealloc(NULL, cap);
    buf[0] = '\0';
    for (;;) {
        set_msg("%s%s", label, buf);
        {
            /* カーソルの桁は, ここまでのメッセージの表示幅. */
            int col = 0;
            for (const char *p = E.msg; *p;) {
                size_t adv;
                int kind;
                col += char_width(p, strlen(p), col, &adv, &kind);
                p += adv;
            }
            E.prompt_col = col;
        }
        refresh_screen();

        wint_t k;
        int t = get_wch(&k);
        if (t == ERR) /* タイムアウト: 再描画するだけ(メッセージが消えることがある) */
            continue;
        if (t == KEY_CODE_YES) {
            if (k == KEY_BACKSPACE || k == KEY_DC) {
                /* 一時的な Line としてバッファを見せて, prev_cp() を再利用する. */
                len = prev_cp(&(Line){buf, len, cap, NULL, false}, len);
                buf[len] = '\0';
                if (cb)
                    cb(buf, k, false);
            } else if (cb) {
                cb(buf, k, true); /* 矢印キーなどはコールバックへ渡す */
            }
            continue;
        }
        /* Esc で取り消す. */
        if (k == 27) {
            E.prompt_col = -1;
            set_msg("");
            if (cb)
                cb(buf, k, false);
            free(buf);
            return NULL;
        }
        /* Enter で確定する. ただし空の入力は受け付けない. */
        if (k == '\n' || k == '\r') {
            if (len > 0) {
                E.prompt_col = -1;
                set_msg("");
                if (cb)
                    cb(buf, k, false);
                return buf;
            }
            continue;
        }
        if (k == 127 || k == 8) {
            len = prev_cp(&(Line){buf, len, cap, NULL, false}, len);
            buf[len] = '\0';
        } else if (k >= 32) {
            /* 表示できる文字: UTF-8 のバイト列を末尾に足す. */
            char enc[8];
            size_t n = utf8_encode(k, enc);
            if (n && len + n < QUERY_MAX) {
                if (len + n + 1 > cap) {
                    cap *= 2;
                    buf = xrealloc(buf, cap);
                }
                memcpy(buf + len, enc, n);
                len += n;
                buf[len] = '\0';
            }
        }
        if (cb)
            cb(buf, k, false);
    }
}

/* ------------------------------------------------------------------ */
/* 検索                                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief S.dir の向きに, 次の @p q の一致位置へ移動する.
 *
 * 検索語が変わった直後の最初の呼び出しは元のカーソル位置から始まる
 * (カーソルの真下の一致も受け付ける). それ以降は現在の一致の直後(または直前)
 * から探す. 検索はバッファの端で折り返す.
 *
 * @param q 検索する文字列.
 */
static void find_step(const char *q)
{
    size_t n = strlen(q);
    if (n == 0 || E.n == 0) {
        S.len = 0;
        return;
    }
    /* fresh: この検索語ではまだ一致がない. 元のカーソル位置から始める. */
    bool fresh = S.row < 0;
    int row = fresh ? S.from_cy : S.row;
    size_t col = fresh ? (size_t)S.from_cx : S.col;

    /* 各行を 1 回ずつ, 折り返しながら調べる. k == E.n では開始行をもう一度調べ,
     * その行のカーソルより前の一致も見つけられるようにする. */
    for (int k = 0; k <= E.n; k++) {
        int r = ((row + S.dir * k) % E.n + E.n) % E.n;
        const Line *l = &E.lines[r];
        const char *hit = NULL;
        if (S.dir > 0) {
            /* 前方: `start` 以降で最初の一致. */
            size_t start = k == 0 ? (fresh ? col : col + 1) : 0;
            if (start <= l->len)
                hit = strstr(l->s + start, q);
        } else {
            /* 後方: `limit` より前から始まる最後の一致. */
            size_t limit = k == 0 ? col : l->len + 1;
            for (const char *p = strstr(l->s, q); p; p = strstr(p + 1, q))
                if ((size_t)(p - l->s) < limit)
                    hit = p;
                else
                    break;
        }
        if (hit) {
            S.row = r;
            S.col = (size_t)(hit - l->s);
            S.len = n;
            E.cy = r;
            E.cx = (int)S.col;
            /* 一致位置をウィンドウの中央に置く. */
            E.rowoff = r - text_rows() / 2 > 0 ? r - text_rows() / 2 : 0;
            update_want();
            return;
        }
    }
    S.len = 0;
    set_msg("Not found: %s", q);
}

/**
 * @brief 検索用の prompt() コールバック. 文字入力でやり直し, 矢印キーで次へ進む.
 *
 * @param q 現在の検索語.
 * @param key 押されたキー.
 * @param special key がファンクションキー(KEY_*)なら true.
 */
static void find_cb(const char *q, wint_t key, bool special)
{
    /* Enter と Esc はプロンプトを終えるだけで, 検索はしない. */
    if (!special && (key == 27 || key == '\n' || key == '\r'))
        return;
    if (special) {
        if (key == KEY_RIGHT || key == KEY_DOWN)
            S.dir = 1;
        else if (key == KEY_LEFT || key == KEY_UP)
            S.dir = -1;
        else
            return;
    } else {
        /* 検索語が変わった: 元のカーソル位置からやり直す. */
        S.row = -1;
        S.dir = 1;
    }
    find_step(q);
}

/**
 * @brief Ctrl-F: インクリメンタル検索.
 *
 * Esc で検索前のカーソル位置とスクロール位置に戻る.
 * Enter でカーソルを一致位置に置いたままにする.
 */
static void find(void)
{
    /* Esc で元に戻せるよう, 表示の状態を保存しておく. */
    int cx = E.cx, cy = E.cy, ro = E.rowoff, co = E.coloff, wc = E.want_col;
    S.row = -1;
    S.len = 0;
    S.dir = 1;
    S.from_cx = cx;
    S.from_cy = cy;
    char *q = prompt("Search: ", find_cb);
    S.len = 0;
    S.row = -1;
    if (!q) {
        E.cx = cx;
        E.cy = cy;
        E.rowoff = ro;
        E.coloff = co;
        E.want_col = wc;
    }
    free(q);
}

/* ------------------------------------------------------------------ */
/* コマンド                                                            */
/* ------------------------------------------------------------------ */

/** @brief Ctrl-S: 保存する. ファイル名がなければ先に入力を求める. */
static void save(void)
{
    if (!E.filename) {
        char *name = prompt("Save as: ", NULL);
        if (!name) {
            set_msg("Save cancelled");
            return;
        }
        E.filename = name;
        select_syntax(name);
        rehighlight(0, E.n);
    }
    size_t bytes;
    if (write_file(E.filename, &bytes)) {
        E.dirty = false;
        set_msg("%d lines, %zu bytes written", E.n, bytes);
    } else {
        set_msg("Can't save! %s", strerror(errno));
    }
}

/**
 * @brief Ctrl-Q: 終了する. 未保存の変更があるときは, さらに QUIT_CONFIRMS 回
 *        押す必要がある. 他のキーを押すとカウンタは元に戻る.
 */
static void quit(void)
{
    if (E.dirty && E.quit_left > 0) {
        set_msg("File has unsaved changes. Press Ctrl-Q %d more time%s to quit.", E.quit_left,
                E.quit_left == 1 ? "" : "s");
        E.quit_left--;
        return;
    }
    shutdown_screen();
    exit(0);
}

/**
 * @brief KEY_* コード(矢印, Home, End, PageUp, PageDown)に従ってカーソルを動かす.
 *
 * @param key 移動の種類を表す KEY_* コード.
 */
static void move_cursor(int key)
{
    Line *l = &E.lines[E.cy];
    int rows = text_rows();
    switch (key) {
    case KEY_LEFT:
        if (E.cx > 0) {
            E.cx = (int)prev_cp(l, (size_t)E.cx);
        } else if (E.cy > 0) {
            E.cy--;
            E.cx = (int)E.lines[E.cy].len;
        }
        update_want();
        break;
    case KEY_RIGHT:
        if ((size_t)E.cx < l->len) {
            E.cx = (int)next_cp(l, (size_t)E.cx);
        } else if (E.cy < E.n - 1) {
            E.cy++;
            E.cx = 0;
        }
        update_want();
        break;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PPAGE:
    case KEY_NPAGE: {
        /* 上下は 1 行, PageUp / PageDown は 1 画面分動く. */
        int step = (key == KEY_UP || key == KEY_DOWN) ? 1 : rows;
        int target = (key == KEY_UP || key == KEY_PPAGE) ? E.cy - step : E.cy + step;
        E.cy = target < 0 ? 0 : target >= E.n ? E.n - 1 : target;
        /* 希望の桁を保つ(want_col は更新しない). */
        E.cx = (int)col_to_off(&E.lines[E.cy], E.want_col);
        break;
    }
    case KEY_HOME:
        E.cx = 0;
        update_want();
        break;
    case KEY_END:
        E.cx = (int)l->len;
        update_want();
        break;
    }
}

/** @brief キーを 1 つ待ち(タイムアウト 500 ms), そのコマンドを実行する. */
static void handle_key(void)
{
    wint_t k;
    int t = get_wch(&k);
    if (t == ERR)
        return;
    if (t == KEY_CODE_YES) {
        switch (k) {
        case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
        case KEY_HOME: case KEY_END: case KEY_PPAGE: case KEY_NPAGE:
            move_cursor((int)k);
            break;
        case KEY_DC:
            delete_forward();
            break;
        case KEY_BACKSPACE:
            delete_back();
            break;
        case KEY_ENTER:
            insert_newline();
            break;
        case KEY_RESIZE: /* LINES / COLS は ncurses が更新済み. 再描画は次のループで行う */
            break;
        default:
            break;
        }
        E.quit_left = QUIT_CONFIRMS; /* 他のキーが押されたら終了待ちを取り消す */
        return;
    }
    /* 通常の文字と制御キー. */
    switch (k) {
    case 17: /* Ctrl-Q */
        quit();
        return;
    case 19: /* Ctrl-S */
        save();
        break;
    case 6: /* Ctrl-F */
        find();
        break;
    case 12: /* Ctrl-L */
        clearok(stdscr, TRUE);
        break;
    case 1: /* Ctrl-A */
        move_cursor(KEY_HOME);
        break;
    case 5: /* Ctrl-E */
        move_cursor(KEY_END);
        break;
    case 8:
    case 127:
        delete_back();
        break;
    case '\n':
    case '\r':
        insert_newline();
        break;
    case '\t':
        insert_text("\t", 1);
        break;
    case 27: /* Esc と Ctrl-C は無視する */
    case 3:
        break;
    default:
        if (k >= 32) {
            char enc[8];
            size_t n = utf8_encode(k, enc);
            if (n)
                insert_text(enc, n);
        }
        break;
    }
    E.quit_left = QUIT_CONFIRMS;
}

/* ------------------------------------------------------------------ */
/* エントリポイント                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief エントリポイント: `kilo-ncurses [file]`.
 *
 * ファイルは ncurses を始める前に読み込む. 読み込みエラーを
 * 通常の端末に表示できるようにするため.
 *
 * @param argc コマンドライン引数の数.
 * @param argv コマンドライン引数. 省略可能な 1 つ目はファイル名.
 * @return 正常終了なら 0, 使い方が誤っていれば 1.
 */
int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    if (argc > 2) {
        fprintf(stderr, "Usage: kilo-ncurses [file]\n");
        return 1;
    }
    E.quit_left = QUIT_CONFIRMS;
    E.prompt_col = -1;
    buf_insert_row(0, "", 0);
    if (argc == 2) {
        E.n = 0; /* 仮の 1 行を捨てる. バッファは open_file() が埋める */
        open_file(argv[1]);
        if (E.n == 0)
            buf_insert_row(0, "", 0);
    }

    initscr();
    E.screen_on = true;
    atexit(shutdown_screen);
    raw();                  /* Ctrl-C / Ctrl-S / Ctrl-Q をシグナルにせず, そのまま受け取る */
    noecho();               /* 入力した文字は自分で描く */
    keypad(stdscr, TRUE);   /* 矢印キーやファンクションキーを解釈する */
    nonl();                 /* Enter を \r として受け取る(Ctrl-J と区別する) */
    set_escdelay(25);       /* Esc とエスケープシーケンスの区別で長く待たない */
    timeout(500);           /* get_wch() は 500 ms で ERR を返す. メッセージを消すため */
    init_colors();
    set_msg("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    for (;;) {
        refresh_screen();
        handle_key();
    }
}
