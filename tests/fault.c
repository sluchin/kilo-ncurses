/**
 * @file fault.c
 * @brief テスト用: LD_PRELOAD で読み込み, 環境変数 KILO_FAULT で指定した関数を失敗させる.
 *
 * 保存の途中の失敗(fwrite, fsync, rename など)とメモリ不足は, 普通の操作では起こせない.
 * そこで, 次の値のときだけ, その関数を失敗させる. ほかの呼び出しはそのまま本物を呼ぶ.
 *
 * - fdopen, fwrite, fputc, fflush, fsync, fchmod, fclose:
 *   mkstemp() で作った一時ファイル(保存先)への呼び出しだけを失敗させる.
 *   計測(gcov)が終了時に書くファイルは, 失敗させない.
 * - strdup: すべて失敗させる.
 * - realloc:N: 大きさがちょうど N バイトの realloc() だけを失敗させる.
 * - wcrtomb:N: 文字 N (Unicode のコードポイント)の wcrtomb() だけを失敗させる.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

/**
 * @brief KILO_FAULT が「@p name:N」なら, N を返す.
 * @param[in] name 関数の名前.
 * @param[in] len @p name の長さ.
 * @return N. 違えば (unsigned long)-1.
 */
static unsigned long fault_arg(const char *name, size_t len)
{
    const char *f = getenv("KILO_FAULT");
    if (!f || strncmp(f, name, len) != 0 || f[len] != ':')
        return (unsigned long)-1;
    return strtoul(f + len + 1, NULL, 10);
}

/** @brief mkstemp() で作った一時ファイルの fd. まだなければ -1. */
static int tmp_fd = -1;

/**
 * @brief KILO_FAULT が @p name かどうかを返す.
 * @param[in] name 関数の名前.
 * @return 失敗させるなら 1.
 */
static int fault(const char *name)
{
    const char *f = getenv("KILO_FAULT");
    return f && strcmp(f, name) == 0;
}

/**
 * @brief 本物の関数を探す.
 * @param[in] name 関数の名前.
 * @return 関数のアドレス.
 */
static void *real(const char *name)
{
    return dlsym(RTLD_NEXT, name);
}

/**
 * @brief @p fp が一時ファイルで, @p name を失敗させるかどうかを返す.
 * @param[in] fp ストリーム.
 * @param[in] name 関数の名前.
 * @return 失敗させるなら 1.
 */
static int fault_on(FILE *fp, const char *name)
{
    return tmp_fd != -1 && fileno(fp) == tmp_fd && fault(name);
}

int mkstemp(char *tmpl)
{
    int (*f)(char *) = (int (*)(char *))real("mkstemp");
    tmp_fd = f(tmpl);
    return tmp_fd;
}

FILE *fdopen(int fd, const char *mode)
{
    FILE *(*f)(int, const char *) = (FILE * (*)(int, const char *)) real("fdopen");
    if (fd == tmp_fd && fault("fdopen")) {
        errno = ENOMEM;
        return NULL;
    }
    return f(fd, mode);
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *fp)
{
    size_t (*f)(const void *, size_t, size_t, FILE *) =
        (size_t(*)(const void *, size_t, size_t, FILE *))real("fwrite");
    if (fault_on(fp, "fwrite")) {
        errno = ENOSPC;
        return 0;
    }
    return f(p, size, n, fp);
}

int fputc(int c, FILE *fp)
{
    int (*f)(int, FILE *) = (int (*)(int, FILE *))real("fputc");
    if (fault_on(fp, "fputc")) {
        errno = ENOSPC;
        return EOF;
    }
    return f(c, fp);
}

int fflush(FILE *fp)
{
    int (*f)(FILE *) = (int (*)(FILE *))real("fflush");
    if (fp && fault_on(fp, "fflush")) {
        errno = ENOSPC;
        return EOF;
    }
    return f(fp);
}

int fsync(int fd)
{
    int (*f)(int) = (int (*)(int))real("fsync");
    if (fd == tmp_fd && fault("fsync")) {
        errno = EIO;
        return -1;
    }
    return f(fd);
}

int fchmod(int fd, mode_t mode)
{
    int (*f)(int, mode_t) = (int (*)(int, mode_t))real("fchmod");
    if (fd == tmp_fd && fault("fchmod")) {
        errno = EPERM;
        return -1;
    }
    return f(fd, mode);
}

/* 一時ファイルを閉じたら, その fd を忘れる. 番号は, 後で開くファイル(gcov の計測の結果など)で
 * 使い回されるので, 覚えたままだと, そのファイルまで失敗させてしまう. */

int fclose(FILE *fp)
{
    int (*f)(FILE *) = (int (*)(FILE *))real("fclose");
    int fail = fault_on(fp, "fclose");
    if (tmp_fd != -1 && fileno(fp) == tmp_fd)
        tmp_fd = -1;
    int r = f(fp);
    if (fail) { /* 閉じてから, 失敗を返す */
        errno = EIO;
        return EOF;
    }
    return r;
}

int close(int fd)
{
    int (*f)(int) = (int (*)(int))real("close");
    if (fd == tmp_fd)
        tmp_fd = -1;
    return f(fd);
}

char *strdup(const char *s)
{
    char *(*f)(const char *) = (char *(*)(const char *))real("strdup");
    if (fault("strdup")) {
        errno = ENOMEM;
        return NULL;
    }
    return f(s);
}

void *realloc(void *p, size_t n)
{
    /* dlsym() は, 中で malloc を使うことがあるので, 本物は 1 回だけ探す. */
    static void *(*f)(void *, size_t);
    if (!f)
        f = (void *(*)(void *, size_t))real("realloc");
    if (fault_arg("realloc", 7) == n) {
        errno = ENOMEM;
        return NULL;
    }
    return f(p, n);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    size_t (*f)(char *, wchar_t, mbstate_t *) = (size_t(*)(char *, wchar_t, mbstate_t *))real("wcrtomb");
    if (fault_arg("wcrtomb", 7) == (unsigned long)wc) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return f(s, wc, ps);
}
