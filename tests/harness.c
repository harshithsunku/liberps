/* SPDX-License-Identifier: Apache-2.0 */
#include "harness.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#define TH_MAX_TESTS 2048

struct th_test {
    const char *name;
    th_fn fn;
    const char *file;
};

static struct th_test th_tests[TH_MAX_TESTS];
static int th_ntests;
static int th_cur_failures;
static int th_verbose_flag;
static jmp_buf th_jmp;

void th_register(const char *name, th_fn fn, const char *file)
{
    if (th_ntests >= TH_MAX_TESTS) {
        fprintf(stderr, "harness: too many tests\n");
        abort();
    }
    th_tests[th_ntests++] = (struct th_test){ name, fn, file };
}

void th_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    th_cur_failures++;
    fprintf(stderr, "    %s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void th_abort_test(void)
{
    longjmp(th_jmp, 1);
}

int th_verbose(void)
{
    return th_verbose_flag;
}

void th_log(const char *fmt, ...)
{
    va_list ap;
    if (!th_verbose_flag)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int th_cmp(const void *a, const void *b)
{
    const struct th_test *x = a, *y = b;
    int c = strcmp(x->file, y->file);
    return c ? c : strcmp(x->name, y->name);
}

static bool th_selected(const char *name, int nfilt, char **filt)
{
    if (nfilt == 0)
        return true;
    for (int i = 0; i < nfilt; i++)
        if (strstr(name, filt[i]))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    char *filt[64];
    int nfilt = 0, list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            th_verbose_flag = 1;
        else if (!strcmp(argv[i], "-l"))
            list = 1;
        else if (nfilt < 64)
            filt[nfilt++] = argv[i];
    }
    qsort(th_tests, (size_t)th_ntests, sizeof th_tests[0], th_cmp);
    if (list) {
        for (int i = 0; i < th_ntests; i++)
            printf("%s  (%s)\n", th_tests[i].name, th_tests[i].file);
        return 0;
    }
    int run = 0, failed = 0;
    clock_t t0 = clock();
    for (int i = 0; i < th_ntests; i++) {
        if (!th_selected(th_tests[i].name, nfilt, filt))
            continue;
        run++;
        th_cur_failures = 0;
        if (th_verbose_flag)
            fprintf(stderr, "[ RUN  ] %s\n", th_tests[i].name);
        if (setjmp(th_jmp) == 0)
            th_tests[i].fn();
        if (th_cur_failures) {
            failed++;
            fprintf(stderr, "[ FAIL ] %s (%s)\n", th_tests[i].name, th_tests[i].file);
        } else if (th_verbose_flag) {
            fprintf(stderr, "[  OK  ] %s\n", th_tests[i].name);
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%d tests, %d passed, %d failed (%.2fs cpu)\n", run, run - failed, failed, secs);
    return failed ? 1 : 0;
}
