/**
 * @file main.c
 * @brief kilo-ncurses -- a small terminal text editor built on ncurses.
 *
 * Features: open / edit / save, incremental search, syntax highlighting for
 * C and Scheme, UTF-8 aware cursor movement and display, status bar.
 *
 * Text is kept as an array of lines.  Each line stores raw UTF-8 bytes, so
 * every cursor position is a *byte offset*; display columns are computed on
 * demand by char_width().  All editor state lives in the global #E.
 */

/* Expose POSIX.1-2008 (getline, mkstemp, fsync) and the XSI wide-char API. */
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

#define VERSION "0.1.0"    /**< Version string shown on the welcome screen. */
#define TABSTOP 8          /**< Tab stop width in display columns. */
#define QUIT_CONFIRMS 2    /**< Extra Ctrl-Q presses needed to quit with unsaved changes. */
#define QUERY_MAX 256      /**< Maximum prompt input length in bytes. */

/**
 * @brief Highlight classes stored per byte in Line::hl.
 *
 * The values double as ncurses colour pair numbers (HL_NORMAL uses the
 * terminal default and needs no pair).
 */
enum { HL_NORMAL, HL_COMMENT, HL_KEYWORD, HL_TYPE, HL_STRING, HL_NUMBER, HL_PREPROC, HL_MATCH };

/* ------------------------------------------------------------------ */
/* Syntax definitions                                                  */
/* ------------------------------------------------------------------ */

/** @brief Per-language highlighting rules. */
typedef struct {
    const char *name;
    const char *const *files; /* extensions (".c") or exact base names */
    const char *const *keywords;
    const char *const *types;
    const char *line_comment; /* introduces a comment up to end of line */
    const char *block_open;   /* start of a multi-line comment */
    const char *block_close;  /* end of a multi-line comment */
    bool lisp_words;          /* words end only at whitespace / brackets / quotes */
    bool preproc;             /* highlight lines starting with '#' */
    bool char_literals;       /* 'x' is a character literal */
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

/* Known languages; select_syntax() picks one from the file name. */
static const Syntax syntaxes[] = {
    {"C", c_files, c_keywords, c_types, "//", "/*", "*/", false, true, true},
    {"Scheme", scm_files, scm_keywords, scm_types, ";", "#|", "|#", true, false, false},
};
/** @brief Number of entries in syntaxes[]. */
#define SYNTAX_COUNT (sizeof(syntaxes) / sizeof(syntaxes[0]))

/* ------------------------------------------------------------------ */
/* Editor state                                                        */
/* ------------------------------------------------------------------ */

/** @brief One line of text (without the trailing newline). */
typedef struct {
    char *s;                /* UTF-8 bytes, always NUL terminated */
    size_t len, cap;        /* used bytes (excluding NUL) / allocated bytes */
    unsigned char *hl;      /* one HL_* class per byte, same length as s */
    bool open_comment; /* line ends inside a block comment */
} Line;

/** @brief The whole editor state (single global instance). */
static struct {
    Line *lines;
    int n, cap;
    int cx, cy;       /* cursor: byte offset in line, line index */
    int want_col;     /* preferred display column for vertical moves */
    int rowoff, coloff; /* first visible line / first visible display column */
    char *filename;
    bool dirty;
    const Syntax *syntax;
    char msg[160];    /* message bar text */
    time_t msg_time;  /* when msg was set; it fades after 5 seconds */
    int quit_left;    /* Ctrl-Q presses still required to quit */
    bool screen_on;   /* ncurses is active (endwin() still to be called) */
    bool color;       /* the terminal supports colours */
    int prompt_col;   /* cursor column while prompting, or -1 */
} E;

/** @brief Incremental search state (single global instance). */
static struct {
    int row;               /* line of the current match, -1 if none yet */
    size_t col, len;       /* byte offset and length of the match (len 0: none) */
    int dir;               /* search direction: +1 forward, -1 backward */
    int from_cy, from_cx;  /* cursor when the search started (restored on Esc) */
} S = {-1, 0, 0, 1, 0, 0};

/**
 * @brief Set the message bar text (printf style).
 *
 * The message is shown for five seconds, or for as long as a prompt is open.
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
 * @brief Leave ncurses mode so the terminal is restored.
 *
 * Safe to call more than once; also registered with atexit().
 */
static void shutdown_screen(void)
{
    if (E.screen_on) {
        endwin();
        E.screen_on = false;
    }
}

/**
 * @brief Restore the terminal, print `what: strerror(errno)` and exit(1).
 * @param what Short description of the failed operation.
 */
static void die(const char *what)
{
    int err = errno;
    shutdown_screen();
    fprintf(stderr, "kilo-ncurses: %s: %s\n", what, strerror(err));
    exit(1);
}

/**
 * @brief realloc() that never fails: it aborts the editor when out of memory.
 *
 * A request for 0 bytes is rounded up to 1 so NULL always means failure.
 */
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

/**
 * @brief Measure the character at the start of @p s.
 * @param s     Bytes starting at the character (UTF-8).
 * @param n     Number of bytes available in @p s.
 * @param col   Display column the character starts at (tabs depend on it).
 * @param[out] adv  Number of bytes the character occupies.
 * @param[out] kind K_PLAIN, K_TAB, K_CTRL (shown as ^X) or K_BAD (invalid UTF-8).
 * @return Width in display columns.
 */
static int char_width(const char *s, size_t n, int col, size_t *adv, int *kind)
{
    unsigned char c = (unsigned char)s[0];
    *kind = K_PLAIN;
    /* Tabs advance to the next multiple of TABSTOP. */
    if (c == '\t') {
        *adv = 1;
        *kind = K_TAB;
        return TABSTOP - col % TABSTOP;
    }
    /* Control characters are drawn as two cells: ^X. */
    if (c < 0x20 || c == 0x7f) {
        *adv = 1;
        *kind = K_CTRL;
        return 2;
    }
    /* Fast path for ASCII. */
    if (c < 0x80) {
        *adv = 1;
        return 1;
    }
    /* Multibyte: let the C library decode it and report its width. */
    wchar_t wc;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = mbrtowc(&wc, s, n, &st);
    /* Invalid or truncated sequence: treat the byte as one cell ("?"). */
    if (r == (size_t)-1 || r == (size_t)-2 || r == 0) {
        *adv = 1;
        *kind = K_BAD;
        return 1;
    }
    *adv = r;
    int w = wcwidth(wc);
    return w < 0 ? 1 : w; /* wcwidth() < 0: not printable, use one cell */
}

/**
 * @brief Display column of byte offset @p upto in line @p l.
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
 * @brief Byte offset of the character covering display column @p target.
 *
 * Used by vertical movement: a wide character that straddles @p target is
 * not skipped over, so the cursor never lands in the middle of a character.
 */
static size_t col_to_off(const Line *l, int target)
{
    int col = 0;
    size_t i = 0;
    while (i < l->len) {
        size_t adv;
        int kind;
        int w = char_width(l->s + i, l->len - i, col, &adv, &kind);
        if (col + w > target) /* this character would cover or pass the target */
            break;
        col += w;
        i += adv;
    }
    return i;
}

/**
 * @brief Byte offset of the character following the one at @p pos
 *        (clamped to the line length).
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
 * @brief Byte offset of the character preceding @p pos.
 *
 * Walks back over UTF-8 continuation bytes (10xxxxxx), at most 4 bytes.
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
/* Syntax highlighting                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Whether @p c separates words in the given language.
 *
 * C-like languages break words at anything that is not an identifier
 * character; Lisp-like languages only at whitespace, brackets and quotes
 * (so `let*` and `set!` stay single words).
 */
static bool is_delim(const Syntax *sy, unsigned char c)
{
    if (c == '\0' || isspace(c))
        return true;
    if (sy->lisp_words)
        return strchr("()[]{}\"';`,", c) != NULL;
    return !(isalnum(c) || c == '_' || c >= 0x80);
}

/** @brief Whether line @p l contains the string @p p at byte offset @p i. */
static bool at(const Line *l, size_t i, const char *p)
{
    size_t n = strlen(p);
    return i + n <= l->len && memcmp(l->s + i, p, n) == 0;
}

/** @brief Whether the @p n bytes at @p w equal one entry of the NULL terminated @p list. */
static bool in_list(const char *const *list, const char *w, size_t n)
{
    for (; *list; list++)
        if (strlen(*list) == n && memcmp(*list, w, n) == 0)
            return true;
    return false;
}


/**
 * @brief Compute the highlight classes of one line.
 * @param l        Line to highlight; l->hl is (re)allocated and filled.
 * @param in_block True if the previous line ended inside a block comment.
 * @return True if this line ends inside a block comment.
 *
 * A single left-to-right scan; the state carried between iterations is
 * @c in_block (inside a block comment), @c quote (inside a string, holds the
 * quote character) and @c prev_delim (the previous byte ended a word, so a
 * number or keyword may start here).
 */
static bool highlight_line(Line *l, bool in_block)
{
    l->hl = xrealloc(l->hl, l->len);
    memset(l->hl, HL_NORMAL, l->len);
    const Syntax *sy = E.syntax;
    if (!sy)
        return false;

    /* Shorthands for the scan below. */
    unsigned char *hl = l->hl;
    const char *s = l->s;
    size_t n = l->len, i = 0;
    bool prev_delim = true;
    int quote = 0;
    size_t bo = strlen(sy->block_open), bc = strlen(sy->block_close);

    /* A line whose first non-blank character is # is a preprocessor line. */
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

        /* Inside a block comment: colour everything up to the closing marker. */
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
        /* Inside a string or character literal. */
        if (quote) {
            hl[i] = HL_STRING;
            if (c == '\\' && i + 1 < n) { /* backslash escape: skip the next byte */
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
        /* Line comment: the rest of the line. */
        if (at(l, i, sy->line_comment)) {
            memset(hl + i, HL_COMMENT, n - i);
            return false;
        }
        /* Start of a block comment (it may close on the same line). */
        if (at(l, i, sy->block_open)) {
            memset(hl + i, HL_COMMENT, bo);
            i += bo;
            in_block = true;
            continue;
        }
        /* Scheme character literal such as #\a or #\space. */
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
        /* Start of a string (or C character literal). */
        if (c == '"' || (sy->char_literals && c == '\'')) {
            quote = c;
            hl[i++] = HL_STRING;
            prev_delim = false;
            continue;
        }
        /* Number: starts at a word boundary with a digit (or .5). */
        if (prev_delim && (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1])))) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_'))
                j++;
            memset(hl + i, HL_NUMBER, j - i);
            i = j;
            prev_delim = false;
            continue;
        }
        /* Word: classify it as keyword / type, otherwise leave it normal. */
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
        /* Any other byte: remember whether it ends a word. */
        prev_delim = is_delim(sy, c);
        i++;
    }
    return in_block;
}

/**
 * @brief Recompute highlighting starting at line @p from.
 * @param from  First line to recompute.
 * @param force Number of lines that are always recomputed.
 *
 * Later lines are only recomputed while the "ends inside a block comment"
 * state keeps changing, so typing in a normal line costs one line, whereas
 * opening a comment repaints everything below it.
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
        /* Past the forced range and the state did not change: nothing below changes. */
        if (j - from + 1 >= force && old == in)
            break;
    }
}

/**
 * @brief Choose E.syntax from the extension or base name of @p filename.
 *
 * Leaves E.syntax NULL (plain text) when nothing matches.
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
/* Text buffer                                                         */
/* ------------------------------------------------------------------ */

/** @brief Make sure line @p l can hold @p need bytes plus the NUL (capacity doubles). */
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

/** @brief Insert @p n bytes of @p s into line @p l at byte offset @p at_. */
static void line_insert(Line *l, size_t at_, const char *s, size_t n)
{
    line_reserve(l, l->len + n);
    memmove(l->s + at_ + n, l->s + at_, l->len - at_);
    memcpy(l->s + at_, s, n);
    l->len += n;
    l->s[l->len] = '\0';
}

/** @brief Remove @p n bytes from line @p l starting at byte offset @p at_. */
static void line_remove(Line *l, size_t at_, size_t n)
{
    memmove(l->s + at_, l->s + at_ + n, l->len - at_ - n);
    l->len -= n;
    l->s[l->len] = '\0';
}

/**
 * @brief Insert a new line with the given text as line number @p at_.
 *
 * Lines after it move down by one; the new line has no highlighting yet.
 */
static void buf_insert_row(int at_, const char *s, size_t n)
{
    if (E.n == E.cap) {
        E.cap = E.cap ? E.cap * 2 : 64;
        E.lines = xrealloc(E.lines, (size_t)E.cap * sizeof(Line));
    }
    memmove(&E.lines[at_ + 1], &E.lines[at_], (size_t)(E.n - at_) * sizeof(Line));
    /* Open a gap, then initialise the new line in it. */
    Line *l = &E.lines[at_];
    memset(l, 0, sizeof *l);
    line_reserve(l, n);
    memcpy(l->s, s, n);
    l->len = n;
    l->s[n] = '\0';
    E.n++;
}

/** @brief Delete line number @p at_ and free its memory. */
static void buf_delete_row(int at_)
{
    free(E.lines[at_].s);
    free(E.lines[at_].hl);
    memmove(&E.lines[at_], &E.lines[at_ + 1], (size_t)(E.n - at_ - 1) * sizeof(Line));
    E.n--;
}

/**
 * @brief Remember the cursor's display column.
 *
 * Vertical movement aims for this column so the cursor does not drift
 * left when it passes over a short line.
 */
static void update_want(void)
{
    E.want_col = disp_col(&E.lines[E.cy], (size_t)E.cx);
}

/* ------------------------------------------------------------------ */
/* Editing operations                                                  */
/* ------------------------------------------------------------------ */

/** @brief Insert @p n bytes (one whole UTF-8 character) at the cursor. */
static void insert_text(const char *s, size_t n)
{
    line_insert(&E.lines[E.cy], (size_t)E.cx, s, n);
    E.cx += (int)n;
    E.dirty = true;
    rehighlight(E.cy, 1);
    update_want();
}

/** @brief Split the current line at the cursor (Enter). */
static void insert_newline(void)
{
    Line *l = &E.lines[E.cy];
    size_t tail = l->len - (size_t)E.cx;
    buf_insert_row(E.cy + 1, l->s + E.cx, tail);
    /* buf_insert_row() may have moved the array: fetch the pointer again. */
    l = &E.lines[E.cy];
    l->len = (size_t)E.cx;
    l->s[l->len] = '\0';
    E.cy++;
    E.cx = 0;
    E.dirty = true;
    rehighlight(E.cy - 1, 2); /* both halves of the split line */
    update_want();
}

/**
 * @brief Delete the character before the cursor (Backspace).
 *
 * At the start of a line the line is joined to the previous one.
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
        /* Join with the previous line; the cursor goes to the join point. */
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
 * @brief Delete the character under the cursor (Delete).
 *
 * At the end of a line the next line is joined to this one.
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
/* File I/O                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Load @p path into the buffer and pick the syntax from its name.
 *
 * A missing file is not an error: the editor starts empty and creates the
 * file on the first save.  CR and LF at line ends are stripped.
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
        /* Strip the line ending (LF or CRLF). */
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
 * @brief Write the buffer to @p path atomically.
 * @param path  Destination file.
 * @param[out] bytes Number of bytes written on success.
 * @return True on success; on failure errno is set and the old file is untouched.
 *
 * The text goes to a temporary file in the same directory which is flushed
 * (fsync), given the original permission bits and then renamed over the
 * target, so a crash cannot leave a half written file.
 */
static bool write_file(const char *path, size_t *bytes)
{
    /* Temporary file next to the target so rename() stays on one file system. */
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return false;
    }
    int fd = mkstemp(tmp);
    if (fd == -1)
        return false;
    struct stat st;
    /* Keep the permissions of an existing file; new files get 0644. */
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
    /* Make sure the data is on disk before the old file is replaced. */
    if (fflush(fp) != 0 || fsync(fd) != 0 || fchmod(fd, mode) != 0)
        goto fail;
    if (fclose(fp) != 0) {
        unlink(tmp);
        return false;
    }
    /* Atomically replace the target. */
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

/** @brief Number of screen rows used for text (all but status and message bars). */
static int text_rows(void)
{
    return LINES > 2 ? LINES - 2 : 1;
}

/** @brief Adjust E.rowoff / E.coloff so the cursor is inside the window. */
static void scroll_to_cursor(void)
{
    int rows = text_rows();
    int rx = disp_col(&E.lines[E.cy], (size_t)E.cx);
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + rows)
        E.rowoff = E.cy - rows + 1;
    /* Horizontal scroll uses display columns, not byte offsets. */
    if (rx < E.coloff)
        E.coloff = rx;
    if (rx >= E.coloff + COLS)
        E.coloff = rx - COLS + 1;
}

/** @brief Set up one colour pair per highlight class (default background kept). */
static void init_colors(void)
{
    E.color = has_colors();
    if (!E.color)
        return;
    start_color();
    use_default_colors(); /* lets -1 mean "terminal default" below */
    init_pair(HL_COMMENT, COLOR_CYAN, -1);
    init_pair(HL_KEYWORD, COLOR_YELLOW, -1);
    init_pair(HL_TYPE, COLOR_GREEN, -1);
    init_pair(HL_STRING, COLOR_MAGENTA, -1);
    init_pair(HL_NUMBER, COLOR_RED, -1);
    init_pair(HL_PREPROC, COLOR_BLUE, -1);
    init_pair(HL_MATCH, COLOR_BLACK, COLOR_YELLOW);
}

/**
 * @brief Select the drawing attributes for highlight class @p h.
 *
 * Without colour support, falls back to bold / dim / underline / reverse.
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
 * @brief Draw line @p l (index @p idx) on screen row @p y.
 *
 * Handles horizontal scrolling, tabs (expanded to spaces), control characters
 * (shown as ^X), invalid bytes (shown as ?) and the search match overlay.
 * A wide character cut by the left edge is replaced by spaces; one that
 * does not fit at the right edge is not drawn.
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
        /* Overlay the current search match. */
        if (S.len && S.row == idx && i >= S.col && i < S.col + S.len)
            h = HL_MATCH;
        int sx = col - E.coloff; /* screen column of this character */
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

/** @brief Draw all text rows, `~` after the end, and the welcome text for an empty buffer. */
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

/** @brief Draw the reverse-video status bar (file name, line count, syntax, position). */
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
    /* Show the right part only if it does not overlap the left one. */
    if ((int)strlen(left) + rl < COLS)
        mvaddstr(LINES - 2, COLS - rl, right);
    attrset(A_NORMAL);
}

/** @brief Draw the message bar (bottom line) while the message is fresh or a prompt is open. */
static void draw_message(void)
{
    if (E.msg[0] && (time(NULL) - E.msg_time < 5 || E.prompt_col >= 0))
        mvaddnstr(LINES - 1, 0, E.msg, COLS);
}

/** @brief Redraw everything and put the cursor in the right place. */
static void refresh_screen(void)
{
    scroll_to_cursor();
    erase();
    draw_rows();
    draw_status();
    draw_message();
    /* While prompting the cursor sits in the message bar. */
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

/**
 * @brief Callback invoked by prompt() after every key.
 * @param buf     Current input text.
 * @param key     The key that was pressed.
 * @param special True if @p key is a function key (KEY_*), false for a character.
 */
typedef void (*PromptCb)(const char *buf, wint_t key, bool special);

/**
 * @brief Encode wide character @p k as UTF-8 into @p out (at least 8 bytes).
 * @return Number of bytes written, 0 if @p k cannot be encoded.
 */
static size_t utf8_encode(wint_t k, char *out)
{
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = wcrtomb(out, (wchar_t)k, &st);
    return r == (size_t)-1 ? 0 : r;
}

/**
 * @brief Read a line of text in the message bar.
 * @param label Text shown before the input.
 * @param cb    Optional per-key callback (used for incremental search).
 * @return Malloc'ed input, or NULL if the user pressed Esc.
 *
 * Enter on an empty input is ignored.  The callback also sees the final
 * Enter / Esc key so it can finish its work.
 */
static char *prompt(const char *label, PromptCb cb)
{
    size_t cap = 64, len = 0;
    char *buf = xrealloc(NULL, cap);
    buf[0] = '\0';
    for (;;) {
        set_msg("%s%s", label, buf);
        {
            /* Cursor column = display width of the message so far. */
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
        if (t == ERR) /* timeout: just redraw (the message may expire) */
            continue;
        if (t == KEY_CODE_YES) {
            if (k == KEY_BACKSPACE || k == KEY_DC) {
                /* Reuse prev_cp() through a temporary Line view of the buffer. */
                len = prev_cp(&(Line){buf, len, cap, NULL, false}, len);
                buf[len] = '\0';
                if (cb)
                    cb(buf, k, false);
            } else if (cb) {
                cb(buf, k, true); /* arrow keys etc. go to the callback */
            }
            continue;
        }
        /* Esc cancels. */
        if (k == 27) {
            E.prompt_col = -1;
            set_msg("");
            if (cb)
                cb(buf, k, false);
            free(buf);
            return NULL;
        }
        /* Enter accepts, but not an empty input. */
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
            /* Printable character: append its UTF-8 bytes. */
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

/**
 * @brief Move to the next match of @p q in direction S.dir.
 *
 * The first call after a change of the query starts at the original cursor
 * (so a match under it is accepted); later calls start just after (or
 * before) the current match.  The search wraps around the buffer.
 */
static void find_step(const char *q)
{
    size_t n = strlen(q);
    if (n == 0 || E.n == 0) {
        S.len = 0;
        return;
    }
    /* "fresh": no match yet for this query, start from the original cursor. */
    bool fresh = S.row < 0;
    int row = fresh ? S.from_cy : S.row;
    size_t col = fresh ? (size_t)S.from_cx : S.col;

    /* Visit every line once, wrapping around; k == E.n revisits the start line
     * so matches before the cursor on that line are found too. */
    for (int k = 0; k <= E.n; k++) {
        int r = ((row + S.dir * k) % E.n + E.n) % E.n;
        const Line *l = &E.lines[r];
        const char *hit = NULL;
        if (S.dir > 0) {
            /* Forward: first match at or after `start`. */
            size_t start = k == 0 ? (fresh ? col : col + 1) : 0;
            if (start <= l->len)
                hit = strstr(l->s + start, q);
        } else {
            /* Backward: the last match that begins before `limit`. */
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
            /* Put the match in the middle of the window. */
            E.rowoff = r - text_rows() / 2 > 0 ? r - text_rows() / 2 : 0;
            update_want();
            return;
        }
    }
    S.len = 0;
    set_msg("Not found: %s", q);
}

/**
 * @brief prompt() callback for search: typing restarts, arrow keys step.
 */
static void find_cb(const char *q, wint_t key, bool special)
{
    /* Enter / Esc only end the prompt; nothing to search. */
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
        /* The query changed: search again from the original cursor. */
        S.row = -1;
        S.dir = 1;
    }
    find_step(q);
}

/**
 * @brief Ctrl-F: incremental search.
 *
 * Esc restores the cursor and scroll position from before the search;
 * Enter keeps the cursor on the match.
 */
static void find(void)
{
    /* Save the view so Esc can put everything back. */
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

/** @brief Ctrl-S: save, asking for a file name first if there is none. */
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
 * @brief Ctrl-Q: quit, but require QUIT_CONFIRMS extra presses when there are
 *        unsaved changes.  Any other key resets the counter.
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

/** @brief Move the cursor for a KEY_* code (arrows, Home, End, PageUp, PageDown). */
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
        /* Up/Down move one line, PageUp/PageDown one screenful. */
        int step = (key == KEY_UP || key == KEY_DOWN) ? 1 : rows;
        int target = (key == KEY_UP || key == KEY_PPAGE) ? E.cy - step : E.cy + step;
        E.cy = target < 0 ? 0 : target >= E.n ? E.n - 1 : target;
        /* Keep the preferred column (not updated: want_col stays). */
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

/** @brief Wait for one key (500 ms timeout) and run its command. */
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
        case KEY_RESIZE: /* ncurses already updated LINES / COLS; redraw happens next loop */
            break;
        default:
            break;
        }
        E.quit_left = QUIT_CONFIRMS; /* any other key cancels a pending quit */
        return;
    }
    /* Ordinary characters and control keys. */
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
    case 27: /* Esc and Ctrl-C are ignored */
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

/**
 * @brief Entry point: `kilo-ncurses [file]`.
 *
 * The file is loaded before ncurses starts so a read error can be printed to
 * the normal terminal.
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
        E.n = 0; /* drop the placeholder line; open_file() fills the buffer */
        open_file(argv[1]);
        if (E.n == 0)
            buf_insert_row(0, "", 0);
    }

    initscr();
    E.screen_on = true;
    atexit(shutdown_screen);
    raw();                  /* deliver Ctrl-C/Ctrl-S/Ctrl-Q to us, no signals */
    noecho();               /* we draw typed characters ourselves */
    keypad(stdscr, TRUE);   /* decode arrow / function keys */
    nonl();                 /* Enter arrives as \r, distinct from Ctrl-J */
    set_escdelay(25);       /* do not wait long to tell Esc from an escape sequence */
    timeout(500);           /* get_wch() returns ERR after 500 ms so messages can expire */
    init_colors();
    set_msg("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    for (;;) {
        refresh_screen();
        handle_key();
    }
}
