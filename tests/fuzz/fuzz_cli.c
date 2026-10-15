/* SPDX-License-Identifier: Apache-2.0 */

#include "erps/erps_cli.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct fake {
    erps_time_t now;
    int binds;
    int unbinds;
};

struct fake_user {
    struct fake *f;
};

static int fake_tx(void *u, erps_port_t p, const uint8_t *fr, size_t n)
{
    (void)u;
    (void)p;
    (void)fr;
    (void)n;
    return 0;
}

static int fake_set_port(void *u, erps_port_t p, unsigned mask)
{
    (void)u;
    (void)p;
    (void)mask;
    return 0;
}

static int fake_flush(void *u)
{
    (void)u;
    return 0;
}

static erps_time_t fake_now(void *ctx)
{
    struct fake *f = ctx;
    f->now += ERPS_MSEC(1);
    return f->now;
}

static int fake_port_lookup(void *ctx, const char *name, uint32_t *handle, uint8_t mac[6])
{
    (void)ctx;
    if (!name || strncmp(name, "eth", 3) != 0)
        return -1;
    char *end = NULL;
    long v = strtol(name + 3, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 65535)
        return -1;
    if (handle)
        *handle = (uint32_t)v;
    if (mac) {
        mac[0] = 0x02;
        mac[1] = 0;
        mac[2] = 0;
        mac[3] = 0;
        mac[4] = (uint8_t)(v >> 8);
        mac[5] = (uint8_t)v;
    }
    return 0;
}

static int fake_system_mac(void *ctx, uint8_t mac[6])
{
    (void)ctx;
    if (!mac)
        return -1;
    mac[0] = 0x02;
    mac[1] = 0xaa;
    mac[2] = 0;
    mac[3] = 0;
    mac[4] = 0;
    mac[5] = 0x01;
    return 0;
}

static int fake_bind(void *ctx, erps_config_t *cfg, const erps_cli_ring_extra_t *x,
                     erps_ops_t *ops, void **user, char *err, size_t errlen)
{
    struct fake *f = ctx;
    (void)cfg;
    (void)x;
    (void)err;
    (void)errlen;
    struct fake_user *fu = malloc(sizeof *fu);
    if (!fu)
        return -1;
    fu->f = f;
    f->binds++;
    ops->tx = fake_tx;
    ops->set_port_state = fake_set_port;
    ops->flush_fdb = fake_flush;
    *user = fu;
    return 0;
}

static void fake_unbind(void *ctx, void *user)
{
    struct fake *f = ctx;
    if (f)
        f->unbinds++;
    free(user);
}

static void cli_out(void *ctx, const char *text)
{
    (void)ctx;
    if (!text)
        abort();
    /* Must receive NUL-terminated strings */
    size_t len = strlen(text);
    if (len > 0 && text[len] != '\0')
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0)
        return 0;

    struct fake f;
    memset(&f, 0, sizeof f);
    f.now = ERPS_SEC(100);

    erps_cli_platform_t plat = {
        .ctx = &f,
        .now = fake_now,
        .port_lookup = fake_port_lookup,
        .system_mac = fake_system_mac,
        .instance_bind = fake_bind,
        .instance_unbind = fake_unbind,
    };

    erps_cli_t *cli = erps_cli_create(&plat);
    if (!cli)
        return 0;

    uint8_t mode = data[0];
    const uint8_t *payload = data + 1;
    size_t plen = size - 1;

    static const char * const prefixes[] = {
        "",
        "erps ring R1 ",
        "show erps ",
        "show erps ring R1 ",
        "show running-config erps",
        "no erps ring R1",
        "clear erps ",
        "debug erps ring R1 level ",
        "help",
    };
    const char *prefix = prefixes[mode % (sizeof(prefixes) / sizeof(prefixes[0]))];
    size_t prefix_len = strlen(prefix);

    size_t line_start = 0;
    unsigned line_count = 0;
    const unsigned max_lines = 128;

    while (line_start < plen && line_count < max_lines) {
        size_t line_end = line_start;
        while (line_end < plen && payload[line_end] != '\n' && payload[line_end] != '\r')
            line_end++;

        size_t raw_len = line_end - line_start;
        char line_buf[512];
        bool use_prefix = (prefix_len > 0) && ((mode & 0x80) || (line_count == 0));
        size_t off = 0;

        if (use_prefix) {
            memcpy(line_buf, prefix, prefix_len);
            off = prefix_len;
        }

        size_t copy_len = raw_len;
        if (off + copy_len >= sizeof(line_buf))
            copy_len = sizeof(line_buf) - off - 1;

        for (size_t i = 0; i < copy_len; i++) {
            char c = (char)payload[line_start + i];
            line_buf[off + i] = (c == '\0') ? ' ' : c;
        }
        line_buf[off + copy_len] = '\0';

        erps_cli_exec(cli, line_buf, cli_out, NULL);
        f.now += ERPS_MSEC(10);
        line_count++;

        while (line_end < plen && (payload[line_end] == '\n' || payload[line_end] == '\r'))
            line_end++;
        line_start = line_end;
    }

    if ((mode & 0x40) && plen > 0 && plen < 2048) {
        char *script = malloc(plen + 1);
        if (script) {
            for (size_t i = 0; i < plen; i++) {
                char c = (char)payload[i];
                script[i] = (c == '\0') ? ' ' : c;
            }
            script[plen] = '\0';
            int err_line = 0;
            erps_cli_exec_script(cli, script, cli_out, NULL, &err_line);
            free(script);
        }
    }

    erps_cli_destroy(cli);
    return 0;
}
