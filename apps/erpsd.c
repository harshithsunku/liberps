/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/linux/lnx.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCRIPT_LIMIT (1024u * 1024u)

static void usage(FILE *out)
{
    fprintf(out, "usage: erpsd [-c config] [-s socket] [-l level] [-n] [-h]\n"
                 "  -c FILE   execute CLI script before serving requests\n"
                 "  -s PATH   control socket (default /run/erpsd.sock)\n"
                 "  -l LEVEL  error|warning|notice|info|debug|trace\n"
                 "  -n        leave protected VLAN memberships as they are on exit\n"
                 "  -h        show this help\n");
}

static int parse_level(const char *s, erps_log_level_t *level)
{
    static const char *const names[] = { "error", "warning", "notice", "info", "debug", "trace" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(s, names[i]) == 0) {
            *level = (erps_log_level_t)i;
            return 0;
        }
    }
    return -1;
}

static char *read_script(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 4096, len = 0;
    char *s = malloc(cap);
    if (!s) {
        fclose(f);
        return NULL;
    }
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (len == SCRIPT_LIMIT) {
            errno = EFBIG;
            free(s);
            fclose(f);
            return NULL;
        }
        if (len + 1 >= cap) {
            size_t next = cap * 2;
            char *grown = realloc(s, next);
            if (!grown) {
                free(s);
                fclose(f);
                return NULL;
            }
            s = grown;
            cap = next;
        }
        s[len++] = (char)ch;
    }
    int saved = errno;
    bool bad = ferror(f) != 0;
    if (fclose(f) < 0)
        bad = true;
    if (bad) {
        free(s);
        errno = saved ? saved : EIO;
        return NULL;
    }
    s[len] = '\0';
    return s;
}

static void script_output(void *ctx, const char *message)
{
    FILE *out = ctx;
    fputs(message, out);
}

int main(int argc, char **argv)
{
    lnx_opts_t opts;
    lnx_opts_init(&opts);
    const char *config = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "c:s:l:nh")) != -1) {
        switch (opt) {
        case 'c': config = optarg; break;
        case 's': opts.ctl_path = optarg; break;
        case 'l':
            if (parse_level(optarg, &opts.log_level) < 0) {
                fprintf(stderr, "erpsd: invalid log level: %s\n", optarg);
                return 2;
            }
            break;
        case 'n': opts.block_on_exit = false; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return 2;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("erpsd: signal SIGPIPE");
        return 1;
    }
    lnx_t *l = lnx_create(&opts);
    if (!l)
        return 1;
    if (config) {
        char *script = read_script(config);
        if (!script) {
            fprintf(stderr, "erpsd: %s: %s\n", config, strerror(errno));
            lnx_destroy(l);
            return 1;
        }
        int line = 0;
        erps_err_t rc = erps_cli_exec_script(lnx_cli(l), script, script_output, stderr, &line);
        free(script);
        if (rc != ERPS_OK) {
            fprintf(stderr, "erpsd: %s:%d: %s\n", config, line, erps_strerror(rc));
            lnx_destroy(l);
            return 1;
        }
    }
    int rc = lnx_run(l);
    lnx_destroy(l);
    return rc;
}
