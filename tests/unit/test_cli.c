/* SPDX-License-Identifier: Apache-2.0 */
/* Command layer: configuration, enable/disable, operator commands, show, running-config. */
#include "erps/erps_cli.h"
#include "harness.h"

#include <stdlib.h>
#include <string.h>

struct fake {
    erps_time_t now;
    int binds, unbinds;
    unsigned port_masks[8][2];
    unsigned flushes;
    erps_cli_ring_extra_t last_extra;
};

struct capture {
    char buf[65536];
    size_t len;
};

static void cap_out(void *ctx, const char *text)
{
    struct capture *c = ctx;
    size_t n = strlen(text);
    if (c->len + n < sizeof c->buf) {
        memcpy(c->buf + c->len, text, n + 1);
        c->len += n;
    }
}

static erps_time_t f_now(void *ctx)
{
    return ((struct fake *)ctx)->now;
}

static int f_lookup(void *ctx, const char *name, uint32_t *handle, uint8_t mac[6])
{
    (void)ctx;
    if (strncmp(name, "eth", 3) != 0)
        return -1;
    *handle = (uint32_t)atoi(name + 3);
    mac[0] = 0x02;
    mac[5] = (uint8_t)*handle;
    return 0;
}

static int f_sysmac(void *ctx, uint8_t mac[6])
{
    (void)ctx;
    static const uint8_t m[6] = { 0x02, 0xaa, 0, 0, 0, 0x01 };
    memcpy(mac, m, 6);
    return 0;
}

struct binding {
    struct fake *f;
    int slot;
};

static int b_tx(void *u, erps_port_t p, const uint8_t *fr, size_t n)
{
    (void)u, (void)p, (void)fr, (void)n;
    return 0;
}

static int b_port(void *u, erps_port_t p, unsigned mask)
{
    struct binding *b = u;
    b->f->port_masks[b->slot][p] = mask;
    return 0;
}

static int b_flush(void *u)
{
    struct binding *b = u;
    b->f->flushes++;
    return 0;
}

static int f_bind(void *ctx, erps_config_t *cfg, const erps_cli_ring_extra_t *x,
                  erps_ops_t *ops, void **user, char *err, size_t errlen)
{
    struct fake *f = ctx;
    (void)cfg;
    if (!erps_cli_vlan_test(x, 10) && erps_cli_vlan_test(x, 4000)) {
        snprintf(err, errlen, "vlan 4000 not allowed");
        return -1;
    }
    struct binding *b = calloc(1, sizeof *b);
    b->f = f;
    b->slot = f->binds++ % 8;
    f->last_extra = *x;
    ops->tx = b_tx;
    ops->set_port_state = b_port;
    ops->flush_fdb = b_flush;
    *user = b;
    return 0;
}

static void f_unbind(void *ctx, void *user)
{
    ((struct fake *)ctx)->unbinds++;
    free(user);
}

static erps_cli_t *new_cli(struct fake *f)
{
    memset(f, 0, sizeof *f);
    f->now = ERPS_SEC(100);
    erps_cli_platform_t p = { .ctx = f, .now = f_now, .port_lookup = f_lookup,
                              .system_mac = f_sysmac, .instance_bind = f_bind,
                              .instance_unbind = f_unbind };
    erps_cli_t *cli = erps_cli_create(&p);
    REQUIRE(cli != NULL);
    return cli;
}

static erps_err_t run(erps_cli_t *cli, struct capture *c, const char *line)
{
    c->len = 0;
    c->buf[0] = '\0';
    erps_err_t rc = erps_cli_exec(cli, line, cap_out, c);
    th_log("    > %s\n%s", line, c->buf);
    return rc;
}

TEST(cli_configure_enable_show)
{
    struct fake f;
    struct capture c;
    erps_cli_t *cli = new_cli(&f);
    CHECK_EQ(run(cli, &c, "erps ring R1 ring-id 1 raps-vid 100 port0 eth1 port1 eth2"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 role owner rpl-port 1"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 timers wtr 1m guard 200ms holdoff 100ms"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 protected-vlans 10-20,30"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 enable"), ERPS_OK);
    CHECK_EQ(f.binds, 1);
    CHECK(erps_cli_vlan_test(&f.last_extra, 15));
    CHECK(!erps_cli_vlan_test(&f.last_extra, 21));
    CHECK(erps_cli_vlan_test(&f.last_extra, 30));
    CHECK_EQ(f.port_masks[0][1], ERPS_BLOCK_TRAFFIC | ERPS_BLOCK_RAPS); /* RPL blocked */
    CHECK_EQ(f.port_masks[0][0], 0);

    CHECK_EQ(run(cli, &c, "show erps"), ERPS_OK);
    CHECK(strstr(c.buf, "R1") && strstr(c.buf, "Pending") && strstr(c.buf, "eth2:B"));
    CHECK_EQ(run(cli, &c, "show erps ring R1 detail"), ERPS_OK);
    CHECK(strstr(c.buf, "Role: rpl-owner, RPL port 1 (eth2)") != NULL);
    CHECK(strstr(c.buf, "WTR 1m00s") != NULL);
    CHECK_EQ(run(cli, &c, "show erps ring R1 json"), ERPS_OK);
    CHECK(c.buf[0] == '{' && strstr(c.buf, "\"state\":\"Pending\""));
    CHECK_EQ(run(cli, &c, "show erps summary json"), ERPS_OK);
    CHECK(strstr(c.buf, "{\"rings\":[{\"name\":\"R1\"") != NULL);

    /* operator commands */
    f.now += ERPS_SEC(1);
    CHECK_EQ(run(cli, &c, "erps ring R1 force-switch port 0"), ERPS_OK);
    CHECK_EQ(erps_get_config(erps_mgr_find(erps_cli_mgr(cli), "R1"))->ring_id, 1);
    erps_status_t st;
    erps_get_status(erps_mgr_find(erps_cli_mgr(cli), "R1"), &st);
    CHECK_EQ(st.state, ERPS_STATE_FORCED_SWITCH);
    CHECK_EQ(run(cli, &c, "erps ring R1 manual-switch port 1"), ERPS_E_REJECTED);
    CHECK(strstr(c.buf, "% manual-switch rejected") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring R1 clear"), ERPS_OK);

    /* live changes: timers yes, ring-id no */
    CHECK_EQ(run(cli, &c, "erps ring R1 timers wtr 2m"), ERPS_OK);
    CHECK_EQ(erps_get_config(erps_mgr_find(erps_cli_mgr(cli), "R1"))->wtr_ms, 120000);
    CHECK_EQ(run(cli, &c, "erps ring R1 ring-id 5"), ERPS_E_STATE);
    CHECK(strstr(c.buf, "disable it first") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring R1 timers wtr 13m"), ERPS_E_CONFIG);

    CHECK_EQ(run(cli, &c, "debug erps ring R1 level debug"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "clear erps counters"), ERPS_OK);

    CHECK_EQ(run(cli, &c, "erps ring R1 disable"), ERPS_OK);
    CHECK_EQ(f.unbinds, 1);
    CHECK_EQ(f.port_masks[0][0], ERPS_BLOCK_TRAFFIC | ERPS_BLOCK_RAPS); /* both blocked */
    CHECK_EQ(run(cli, &c, "show erps"), ERPS_OK);
    CHECK(strstr(c.buf, "Disabled") != NULL);
    CHECK_EQ(run(cli, &c, "no erps ring R1"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "show erps ring R1"), ERPS_E_NOTFOUND);
    erps_cli_destroy(cli);
}

TEST(cli_errors)
{
    struct fake f;
    struct capture c;
    erps_cli_t *cli = new_cli(&f);
    CHECK_EQ(run(cli, &c, "erps ring R1 ring-id 300"), ERPS_E_INVAL);
    CHECK(strstr(c.buf, "1..239") != NULL);
    CHECK_EQ(run(cli, &c, "show erps"), ERPS_OK);
    CHECK(strstr(c.buf, "no rings") != NULL); /* failed create leaves nothing behind */
    CHECK_EQ(run(cli, &c, "erps ring R1 port0 bogus0"), ERPS_E_NOTFOUND);
    CHECK_EQ(run(cli, &c, "erps ring R1 raps-vid 10 wtr 5x"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R1 raps-vid 10 frobnicate"), ERPS_E_INVAL);
    CHECK(strstr(c.buf, "unknown keyword") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring R1 raps-vid 10"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 enable"), ERPS_E_CONFIG); /* ports missing */
    CHECK_EQ(run(cli, &c, "erps ring R1 port0 eth1 port1 eth1"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 enable"), ERPS_E_CONFIG); /* same port twice */
    CHECK_EQ(run(cli, &c, "erps ring R1 port1 eth2 protected-vlans 4000"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 enable"), ERPS_E_PLATFORM);
    CHECK(strstr(c.buf, "vlan 4000 not allowed") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring R1 clear"), ERPS_E_STATE);
    CHECK_EQ(run(cli, &c, "frobnicate"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "# a comment"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "   "), ERPS_OK);
    CHECK_EQ(run(cli, &c, "help"), ERPS_OK);
    CHECK(strstr(c.buf, "force-switch") != NULL);
    erps_cli_destroy(cli);
}

TEST(cli_running_config_round_trip)
{
    struct fake f;
    struct capture c, c2;
    erps_cli_t *cli = new_cli(&f);
    const char *script = "erps ring R1 ring-id 3 raps-vid 200 mel 5 port0 eth1 port1 eth2\n"
                         "erps ring R1 node-id 02:00:00:00:00:42\n"
                         "erps ring R1 role neighbour rpl-port 0\n"
                         "erps ring R1 timers wtr 7m guard 1s holdoff 300ms\n"
                         "erps ring R1 non-revertive\n"
                         "erps ring R1 protected-vlans 1-5,7,9-10\n"
                         "erps ring R1 sw-forwarding on\n"
                         "erps ring R1 enable\n"
                         "erps ring R2 ring-id 4 raps-vid 201 port0 eth3 port1 eth4\n";
    int line = 0;
    CHECK_EQ(erps_cli_exec_script(cli, script, cap_out, &c, &line), ERPS_OK);
    CHECK_EQ(run(cli, &c, "show running-config erps"), ERPS_OK);
    th_log("%s", c.buf);
    CHECK(strstr(c.buf, "erps ring R1 timers wtr 7m guard 1s holdoff 300ms") != NULL);
    CHECK(strstr(c.buf, "erps ring R1 protected-vlans 1-5,7,9-10") != NULL);
    CHECK(strstr(c.buf, "erps ring R1 enable") != NULL);
    CHECK(strstr(c.buf, "erps ring R2 enable") == NULL);

    /* replaying the running-config on a fresh CLI reproduces it exactly */
    struct fake f2;
    erps_cli_t *cli2 = new_cli(&f2);
    char saved[65536];
    snprintf(saved, sizeof saved, "%s", c.buf);
    CHECK_EQ(erps_cli_exec_script(cli2, saved, cap_out, &c2, &line), ERPS_OK);
    CHECK_EQ(run(cli2, &c2, "show running-config erps"), ERPS_OK);
    CHECK_STREQ(c2.buf, saved);

    /* script errors report the line */
    CHECK_EQ(erps_cli_exec_script(cli2, "help\nerps ring X ring-id 999\nhelp\n", cap_out, &c2,
                                  &line),
             ERPS_E_INVAL);
    CHECK_EQ(line, 2);
    erps_cli_destroy(cli);
    erps_cli_destroy(cli2);
}

TEST(cli_attach_external_instance)
{
    struct fake f;
    struct capture c;
    erps_cli_t *cli = new_cli(&f);
    erps_config_t cfg;
    erps_config_init(&cfg);
    snprintf(cfg.name, sizeof cfg.name, "ext");
    cfg.raps_vid = 7;
    cfg.node_id[0] = 2;
    cfg.node_id[5] = 9;
    erps_ops_t ops = { .tx = b_tx, .set_port_state = b_port, .flush_fdb = b_flush };
    struct binding b = { &f, 7 };
    erps_t *e = NULL;
    REQUIRE(erps_create(&cfg, &ops, &b, &e) == ERPS_OK);
    erps_start(e, f.now);
    CHECK_EQ(erps_cli_attach(cli, e), ERPS_OK);
    CHECK_EQ(erps_cli_attach(cli, e), ERPS_E_EXISTS);
    CHECK_EQ(run(cli, &c, "show erps ring ext status"), ERPS_OK);
    CHECK(strstr(c.buf, "Pending") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring ext manual-switch port 1"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring ext ring-id 9"), ERPS_E_UNSUPPORTED);
    CHECK_EQ(run(cli, &c, "show running-config erps"), ERPS_OK);
    CHECK_EQ(c.len, 0);
    erps_cli_destroy(cli); /* does not destroy external instances */
    erps_destroy(e);
}

TEST(cli_command_table)
{
    size_t n = 0;
    const erps_cli_cmd_t *t = erps_cli_commands(&n);
    CHECK(n >= 20);
    for (size_t i = 0; i < n; i++)
        CHECK(t[i].syntax && t[i].help && t[i].syntax[0] && t[i].help[0]);
}

TEST(cli_views_and_misc_paths)
{
    struct fake f;
    struct capture c;
    erps_cli_t *cli = new_cli(&f);
    CHECK_EQ(run(cli, &c,
                 "erps ring R1 ring-id 2 raps-vid 300 port0 eth5 port1 eth6 node-id 02:00:00:00:00:09 "
                 "pcp 5 fop-pm off fop-to on fop-timeout 30s relaxed-timers on wtr 20s compat-version 1 "
                 "mel 3 role neighbor rpl-port port1"),
             ERPS_OK);
    CHECK_EQ(run(cli, &c, "show erps ring R1"), ERPS_OK); /* disabled: shows config */
    CHECK(strstr(c.buf, "disabled") != NULL);
    CHECK_EQ(run(cli, &c, "show erps ring R1 json"), ERPS_OK);
    CHECK(strstr(c.buf, "\"enabled\":false") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring R1 enable"), ERPS_OK);
    static const char *views[] = { "status", "config", "counters", "history", "detail" };
    for (size_t i = 0; i < 5; i++) {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "show erps ring R1 %s", views[i]);
        CHECK_EQ(run(cli, &c, cmd), ERPS_OK);
        CHECK(c.len > 0);
    }
    CHECK_EQ(run(cli, &c, "show erps ring R1 bogus"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "show erps running-config"), ERPS_OK);
    CHECK(strstr(c.buf, "pcp 5") && strstr(c.buf, "fop-pm off") && strstr(c.buf, "fop-to on") &&
          strstr(c.buf, "fop-timeout 30s") && strstr(c.buf, "compat-version 1") &&
          strstr(c.buf, "relaxed-timers on") && strstr(c.buf, "role neighbour rpl-port 1"));
    CHECK_EQ(run(cli, &c, "show running-config"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "show running-config foo"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "show bogus"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "show erps foo bar"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "debug erps level info"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "debug erps level loud"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "debug erps ring R9 level info"), ERPS_E_NOTFOUND);
    CHECK_EQ(run(cli, &c, "clear erps ring R1 counters"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "clear erps ring R9 counters"), ERPS_E_NOTFOUND);
    CHECK_EQ(run(cli, &c, "clear erps bogus"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R1 flush-event"), ERPS_OK);
    CHECK_EQ(run(cli, &c, "erps ring R1 force-switch"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R1 force-switch 1"), ERPS_E_REJECTED); /* compat v1 */
    CHECK(strstr(c.buf, "compatible-version 1") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring R1 manual-switch 0"), ERPS_E_REJECTED);
    CHECK_EQ(run(cli, &c, "erps ring R1 non-revertive"), ERPS_OK);  /* live */
    CHECK_EQ(run(cli, &c, "erps ring R1 port0 eth7"), ERPS_E_STATE);
    CHECK_EQ(run(cli, &c, "erps ring R1"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R9 enable"), ERPS_E_NOTFOUND);
    CHECK_EQ(run(cli, &c, "erps ring R9 clear"), ERPS_E_NOTFOUND);
    CHECK_EQ(run(cli, &c, "no erps ring R9"), ERPS_E_NOTFOUND);
    CHECK_EQ(run(cli, &c, "erps ring R2 node-id zz"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R2 role boss"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R2 sw-forwarding maybe"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R2 protected-vlans 5-2"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R2 raps-vid"), ERPS_E_INVAL);
    CHECK_EQ(run(cli, &c, "erps ring R2 timers wtr 5"), ERPS_E_CONFIG); /* checked at config time */
    CHECK(strstr(c.buf, "WTR") != NULL);
    CHECK_EQ(run(cli, &c, "erps ring THIS-NAME-IS-WAY-TOO-LONG-FOR-A-RING ring-id 1"), ERPS_E_INVAL);
    CHECK_EQ(erps_cli_exec(cli, NULL, cap_out, &c), ERPS_E_INVAL);
    CHECK(erps_cli_ring_extra(cli, "R1") != NULL);
    CHECK(erps_cli_ring_extra(cli, "nope") == NULL);
    CHECK(erps_cli_mgr(cli) != NULL);
    erps_cli_destroy(cli);

    /* a CLI without platform hooks: numeric port handles, rings cannot be enabled */
    erps_cli_t *bare = erps_cli_create(NULL);
    REQUIRE(bare);
    CHECK_EQ(erps_cli_exec(bare, "erps ring X raps-vid 10 port0 3 port1 4 node-id 02:00:00:00:00:01",
                           cap_out, &c),
             ERPS_OK);
    CHECK_EQ(erps_cli_exec(bare, "erps ring X port0 eth1", cap_out, &c), ERPS_E_INVAL);
    CHECK_EQ(erps_cli_exec(bare, "erps ring X enable", cap_out, &c), ERPS_E_UNSUPPORTED);
    erps_cli_destroy(bare);
}
