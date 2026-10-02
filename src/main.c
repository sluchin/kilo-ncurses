/*
 * kilo-ncurses -- a small terminal text editor built on ncurses.
 *
 * Features: open / edit / save, incremental search, syntax highlighting for
 * C and Scheme, UTF-8 aware cursor movement and display, status bar.
 */

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

#define VERSION "0.1.0"
#define TABSTOP 8
#define QUIT_CONFIRMS 2
#define QUERY_MAX 256

enum { HL_NORMAL, HL_COMMENT, HL_KEYWORD, HL_TYPE, HL_STRING, HL_NUMBER, HL_PREPROC, HL_MATCH };

/* ------------------------------------------------------------------ */
/* Syntax definitions                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *const *files; /* extensions (".c") or exact base names */
    const char *const *keywords;
    const char *const *types;
    const char *line_comment;
    const char *block_open;
    const char *block_close;
    bool lisp_words;
    bool preproc;
    bool char_literals;
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

static const Syntax syntaxes[] = {
    {"C", c_files, c_keywords, c_types, "//", "/*", "*/", false, true, true},
    {"Scheme", scm_files, scm_keywords, scm_types, ";", "#|", "|#", true, false, false},
};
#define SYNTAX_COUNT (sizeof(syntaxes) / sizeof(syntaxes[0]))

/* ------------------------------------------------------------------ */
/* Editor state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char *s;
    size_t len, cap;
    unsigned char *hl;
    bool open_comment; /* line ends inside a block comment */
} Line;

static struct {
    Line *lines;
    int n, cap;
    int cx, cy;       /* cursor: byte offset in line, line index */
    int want_col;     /* preferred display column for vertical moves */
    int rowoff, coloff;
    char *filename;
    bool dirty;
    const Syntax *syntax;
    char msg[160];
    time_t msg_time;
    int quit_left;
    bool screen_on;
    bool color;
    int prompt_col;   /* cursor column while prompting, or -1 */
} E;

static struct {
    int row;
    size_t col, len;
    int dir;
    int from_cy, from_cx;
} S = {-1, 0, 0, 1, 0, 0};

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof E.msg, fmt, ap);
    va_end(ap);
    E.msg_time = time(NULL);
}

static void shutdown_screen(void)
{
    if (E.screen_on) {
        endwin();
        E.screen_on = false;
    }
}

static void die(const char *what)
{
    int err = errno;
    shutdown_screen();
    fprintf(stderr, "kilo-ncurses: %s: %s\n", what, strerror(err));
    exit(1);
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers and display width                                     */
/* ------------------------------------------------------------------ */

enum { K_PLAIN, K_TAB, K_CTRL, K_BAD };

static int char_width(const char *s, size_t n, int col, size_t *adv, int *kind)
{
    unsigned char c = (unsigned char)s[0];
    *kind = K_PLAIN;
    if (c == '\t') {
        *adv = 1;
        *kind = K_TAB;
        return TABSTOP - col % TABSTOP;
    }
    if (c < 0x20 || c == 0x7f) {
        *adv = 1;
        *kind = K_CTRL;
        return 2;
    }
    if (c < 0x80) {
        *adv = 1;
        return 1;
    }
    wchar_t wc;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = mbrtowc(&wc, s, n, &st);
    if (r == (size_t)-1 || r == (size_t)-2 || r == 0) {
        *adv = 1;
        *kind = K_BAD;
        return 1;
    }
    *adv = r;
    int w = wcwidth(wc);
    return w < 0 ? 1 : w;
}

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

static size_t col_to_off(const Line *l, int target)
{
    int col = 0;
    size_t i = 0;
    while (i < l->len) {
        size_t adv;
        int kind;
        int w = char_width(l->s + i, l->len - i, col, &adv, &kind);
        if (col + w > target)
            break;
        col += w;
        i += adv;
    }
    return i;
}

static size_t next_cp(const Line *l, size_t pos)
{
    size_t adv;
    int kind;
    if (pos >= l->len)
        return l->len;
    char_width(l->s + pos, l->len - pos, 0, &adv, &kind);
    return pos + adv;
}

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
/* Syntax highlighting                                                 */
/* ------------------------------------------------------------------ */

static bool is_delim(const Syntax *sy, unsigned char c)
{
    if (c == '\0' || isspace(c))
        return true;
    if (sy->lisp_words)
        return strchr("()[]{}\"';`,", c) != NULL;
    return !(isalnum(c) || c == '_' || c >= 0x80);
}

static bool at(const Line *l, size_t i, const char *p)
{
    size_t n = strlen(p);
    return i + n <= l->len && memcmp(l->s + i, p, n) == 0;
}

static bool in_list(const char *const *list, const char *w, size_t n)
{
    for (; *list; list++)
        if (strlen(*list) == n && memcmp(*list, w, n) == 0)
            return true;
    return false;
}

/* Fills l->hl and returns whether the line ends inside a block comment. */
static bool highlight_line(Line *l, bool in_block)
{
    l->hl = xrealloc(l->hl, l->len);
    memset(l->hl, HL_NORMAL, l->len);
    const Syntax *sy = E.syntax;
    if (!sy)
        return false;

    unsigned char *hl = l->hl;
    const char *s = l->s;
    size_t n = l->len, i = 0;
    bool prev_delim = true;
    int quote = 0;
    size_t bo = strlen(sy->block_open), bc = strlen(sy->block_close);

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
        if (quote) {
            hl[i] = HL_STRING;
            if (c == '\\' && i + 1 < n) {
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
        if (at(l, i, sy->line_comment)) {
            memset(hl + i, HL_COMMENT, n - i);
            return false;
        }
        if (at(l, i, sy->block_open)) {
            memset(hl + i, HL_COMMENT, bo);
            i += bo;
            in_block = true;
            continue;
        }
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
        if (c == '"' || (sy->char_literals && c == '\'')) {
            quote = c;
            hl[i++] = HL_STRING;
            prev_delim = false;
            continue;
        }
        if (prev_delim && (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1])))) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_'))
                j++;
            memset(hl + i, HL_NUMBER, j - i);
            i = j;
            prev_delim = false;
            continue;
        }
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
        prev_delim = is_delim(sy, c);
        i++;
    }
    return in_block;
}

/* Recompute highlighting from row `from`; the first `force` rows are always
 * recomputed, later rows only while the open-comment state keeps changing. */
static void rehighlight(int from, int force)
{
    if (from < 0)
        from = 0;
    bool in = from > 0 ? E.lines[from - 1].open_comment : false;
    for (int j = from; j < E.n; j++) {
        bool old = E.lines[j].open_comment;
        E.lines[j].open_comment = highlight_line(&E.lines[j], in);
        in = E.lines[j].open_comment;
        if (j - from + 1 >= force && old == in)
            break;
    }
}

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
/* Text buffer                                                         */
/* ------------------------------------------------------------------ */

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

static void line_insert(Line *l, size_t at_, const char *s, size_t n)
{
    line_reserve(l, l->len + n);
    memmove(l->s + at_ + n, l->s + at_, l->len - at_);
    memcpy(l->s + at_, s, n);
    l->len += n;
    l->s[l->len] = '\0';
}

static void line_remove(Line *l, size_t at_, size_t n)
{
    memmove(l->s + at_, l->s + at_ + n, l->len - at_ - n);
    l->len -= n;
    l->s[l->len] = '\0';
}

static void buf_insert_row(int at_, const char *s, size_t n)
{
    if (E.n == E.cap) {
        E.cap = E.cap ? E.cap * 2 : 64;
        E.lines = xrealloc(E.lines, (size_t)E.cap * sizeof(Line));
    }
    memmove(&E.lines[at_ + 1], &E.lines[at_], (size_t)(E.n - at_) * sizeof(Line));
    Line *l = &E.lines[at_];
    memset(l, 0, sizeof *l);
    line_reserve(l, n);
    memcpy(l->s, s, n);
    l->len = n;
    l->s[n] = '\0';
    E.n++;
}

static void buf_delete_row(int at_)
{
    free(E.lines[at_].s);
    free(E.lines[at_].hl);
    memmove(&E.lines[at_], &E.lines[at_ + 1], (size_t)(E.n - at_ - 1) * sizeof(Line));
    E.n--;
}

static void update_want(void)
{
    E.want_col = disp_col(&E.lines[E.cy], (size_t)E.cx);
}

/* ------------------------------------------------------------------ */
/* Editing operations                                                  */
/* ------------------------------------------------------------------ */

static void insert_text(const char *s, size_t n)
{
    line_insert(&E.lines[E.cy], (size_t)E.cx, s, n);
    E.cx += (int)n;
    E.dirty = true;
    rehighlight(E.cy, 1);
    update_want();
}

static void insert_newline(void)
{
    Line *l = &E.lines[E.cy];
    size_t tail = l->len - (size_t)E.cx;
    buf_insert_row(E.cy + 1, l->s + E.cx, tail);
    l = &E.lines[E.cy];
    l->len = (size_t)E.cx;
    l->s[l->len] = '\0';
    E.cy++;
    E.cx = 0;
    E.dirty = true;
    rehighlight(E.cy - 1, 2);
    update_want();
}

static void delete_back(void)
{
    Line *l = &E.lines[E.cy];
    if (E.cx > 0) {
        size_t p = prev_cp(l, (size_t)E.cx);
        line_remove(l, p, (size_t)E.cx - p);
        E.cx = (int)p;
        rehighlight(E.cy, 1);
    } else if (E.cy > 0) {
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
/* File I/O                                                            */
/* ------------------------------------------------------------------ */

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
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        buf_insert_row(E.n, line, (size_t)len);
    }
    free(line);
    fclose(fp);
    if (E.n > 0)
        rehighlight(0, E.n);
}

static bool write_file(const char *path, size_t *bytes)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return false;
    }
    int fd = mkstemp(tmp);
    if (fd == -1)
        return false;
    struct stat st;
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
    if (fflush(fp) != 0 || fsync(fd) != 0 || fchmod(fd, mode) != 0)
        goto fail;
    if (fclose(fp) != 0) {
        unlink(tmp);
        return false;
    }
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
/* Screen                                                              */
/* ------------------------------------------------------------------ */

static int text_rows(void)
{
    return LINES > 2 ? LINES - 2 : 1;
}

static void scroll_to_cursor(void)
{
    int rows = text_rows();
    int rx = disp_col(&E.lines[E.cy], (size_t)E.cx);
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + rows)
        E.rowoff = E.cy - rows + 1;
    if (rx < E.coloff)
        E.coloff = rx;
    if (rx >= E.coloff + COLS)
        E.coloff = rx - COLS + 1;
}

static void init_colors(void)
{
    E.color = has_colors();
    if (!E.color)
        return;
    start_color();
    use_default_colors();
    init_pair(HL_COMMENT, COLOR_CYAN, -1);
    init_pair(HL_KEYWORD, COLOR_YELLOW, -1);
    init_pair(HL_TYPE, COLOR_GREEN, -1);
    init_pair(HL_STRING, COLOR_MAGENTA, -1);
    init_pair(HL_NUMBER, COLOR_RED, -1);
    init_pair(HL_PREPROC, COLOR_BLUE, -1);
    init_pair(HL_MATCH, COLOR_BLACK, COLOR_YELLOW);
}

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
        if (S.len && S.row == idx && i >= S.col && i < S.col + S.len)
            h = HL_MATCH;
        int sx = col - E.coloff;
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
    if ((int)strlen(left) + rl < COLS)
        mvaddstr(LINES - 2, COLS - rl, right);
    attrset(A_NORMAL);
}

static void draw_message(void)
{
    if (E.msg[0] && (time(NULL) - E.msg_time < 5 || E.prompt_col >= 0))
        mvaddnstr(LINES - 1, 0, E.msg, COLS);
}

static void refresh_screen(void)
{
    scroll_to_cursor();
    erase();
    draw_rows();
    draw_status();
    draw_message();
    if (E.prompt_col >= 0) {
        move(LINES - 1, E.prompt_col < COLS ? E.prompt_col : COLS - 1);
    } else {
        int rx = disp_col(&E.lines[E.cy], (size_t)E.cx);
        move(E.cy - E.rowoff, rx - E.coloff);
    }
    refresh();
}

/* ------------------------------------------------------------------ */
/* Prompt                                                              */
/* ------------------------------------------------------------------ */

typedef void (*PromptCb)(const char *buf, wint_t key, bool special);

static size_t utf8_encode(wint_t k, char *out)
{
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = wcrtomb(out, (wchar_t)k, &st);
    return r == (size_t)-1 ? 0 : r;
}

static char *prompt(const char *label, PromptCb cb)
{
    size_t cap = 64, len = 0;
    char *buf = xrealloc(NULL, cap);
    buf[0] = '\0';
    for (;;) {
        set_msg("%s%s", label, buf);
        {
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
        if (t == ERR)
            continue;
        if (t == KEY_CODE_YES) {
            if (k == KEY_BACKSPACE || k == KEY_DC) {
                len = prev_cp(&(Line){buf, len, cap, NULL, false}, len);
                buf[len] = '\0';
                if (cb)
                    cb(buf, k, false);
            } else if (cb) {
                cb(buf, k, true);
            }
            continue;
        }
        if (k == 27) {
            E.prompt_col = -1;
            set_msg("");
            if (cb)
                cb(buf, k, false);
            free(buf);
            return NULL;
        }
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
/* Search                                                              */
/* ------------------------------------------------------------------ */

static void find_step(const char *q)
{
    size_t n = strlen(q);
    if (n == 0 || E.n == 0) {
        S.len = 0;
        return;
    }
    bool fresh = S.row < 0;
    int row = fresh ? S.from_cy : S.row;
    size_t col = fresh ? (size_t)S.from_cx : S.col;

    for (int k = 0; k <= E.n; k++) {
        int r = ((row + S.dir * k) % E.n + E.n) % E.n;
        const Line *l = &E.lines[r];
        const char *hit = NULL;
        if (S.dir > 0) {
            size_t start = k == 0 ? (fresh ? col : col + 1) : 0;
            if (start <= l->len)
                hit = strstr(l->s + start, q);
        } else {
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
            E.rowoff = r - text_rows() / 2 > 0 ? r - text_rows() / 2 : 0;
            update_want();
            return;
        }
    }
    S.len = 0;
    set_msg("Not found: %s", q);
}

static void find_cb(const char *q, wint_t key, bool special)
{
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
        S.row = -1;
        S.dir = 1;
    }
    find_step(q);
}

static void find(void)
{
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
/* Commands                                                            */
/* ------------------------------------------------------------------ */

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
        int step = (key == KEY_UP || key == KEY_DOWN) ? 1 : rows;
        int target = (key == KEY_UP || key == KEY_PPAGE) ? E.cy - step : E.cy + step;
        E.cy = target < 0 ? 0 : target >= E.n ? E.n - 1 : target;
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
        case KEY_RESIZE:
            break;
        default:
            break;
        }
        E.quit_left = QUIT_CONFIRMS;
        return;
    }
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
    case 27:
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
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

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
        E.n = 0;
        open_file(argv[1]);
        if (E.n == 0)
            buf_insert_row(0, "", 0);
    }

    initscr();
    E.screen_on = true;
    atexit(shutdown_screen);
    raw();
    noecho();
    keypad(stdscr, TRUE);
    nonl();
    set_escdelay(25);
    timeout(500);
    init_colors();
    set_msg("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    for (;;) {
        refresh_screen();
        handle_key();
    }
}
