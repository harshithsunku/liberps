/* SPDX-License-Identifier: Apache-2.0 */
/*
 * erps-sim — interactive / scriptable G.8032 ring simulator.
 *
 *   erps-sim                      interactive shell (reads stdin)
 *   erps-sim -f demo.erps         run a script, exit non-zero if an `expect` fails
 *   erps-sim -e 'sim create 4; run 6m; show'
 *
 * Rings are named n0..n<N-1>, so every command layer command works on them:
 *   show erps, show erps ring n3 detail [json], erps ring n2 force-switch port 1, ...
 * Type `help` for the simulator commands.
 */
#include "erps/erps_cli.h"
#include "erps/erps_sim.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct shell {
    erps_sim_t *sim;
    erps_cli_t *cli;
    bool events;
    erps_log_level_t log_level;
    bool log_on;
    int failures;
    unsigned lineno;
    const char *src;
};

static const char *help_text =
    "Simulator commands:\n"
    "  sim create <nodes> [owner <n>] [no-neighbour] [non-revertive] [compat <1|2>]\n"
    "                     [wtr <dur>] [guard <dur>] [holdoff <dur>] [delay <us>] [jitter <us>]\n"
    "                     [loss <0..1>] [seed <n>] [id <n> <last-octet>]...\n"
    "                     ring of N nodes; link i joins node i port 1 and node i+1 port 0;\n"
    "                     owner defaults to N-1 (RPL = link N-1), neighbour = node 0\n"
    "  run <dur>          advance virtual time (e.g. 500ms, 6s, 5m)\n"
    "  link <i> down|up [cw|ccw]    fail / restore a link (one direction with cw/ccw)\n"
    "  node <i> down|up             node failure / recovery (restart)\n"
    "  node <i> fs <0|1> | ms <0|1> | clear   operator commands on node i\n"
    "  show | status                ring summary (A..E per node, [B-] = port0 blocked)\n"
    "  events on|off                print protocol events as they happen (default on)\n"
    "  log <error|warning|notice|info|debug|trace>|off   node logs (debug = every Table 10-2 row)\n"
    "  pcap <file>|off              capture all R-APS frames (open with Wireshark)\n"
    "  expect state <node> <A|B|C|D|E>    expect blocked <node> <0|1> yes|no\n"
    "  expect no-loops                    (scripts exit non-zero if an expectation fails)\n"
    "  echo <text>, help, quit\n"
    "Command layer (rings are n0..n<N-1>): show erps [summary|ring <n> ...] [json],\n"
    "  erps ring <n> force-switch|manual-switch port <0|1>|clear, debug erps ..., clear erps counters\n";

/* ---- helpers --------------------------------------------------------------------------------- */

static void out_stdout(void *ctx, const char *text)
{
    (void)ctx;
    fputs(text, stdout);
}

static erps_time_t cli_now(void *ctx)
{
    struct shell *sh = ctx;
    return sh->sim ? erps_sim_now(sh->sim) : 0;
}

static void print_time(const struct shell *sh)
{
    erps_time_t t = sh->sim ? erps_sim_now(sh->sim) : 0;
    printf("[%5llu.%06llu] ", (unsigned long long)(t / 1000000u), (unsigned long long)(t % 1000000u));
}

static void on_event(void *ctx, unsigned node, const erps_event_t *ev)
{
    struct shell *sh = ctx;
    if (!sh->events)
        return;
    switch (ev->type) {
    case ERPS_EV_STATE_CHANGE:
        print_time(sh);
        printf("n%u %s -> %s (row %u, %s)\n", node, erps_state_str(ev->u.state.from),
               erps_state_str(ev->u.state.to), ev->u.state.row, erps_req_type_str(ev->u.state.req));
        break;
    case ERPS_EV_PORT_STATE:
        print_time(sh);
        printf("n%u port %d %s\n", node, (int)ev->u.port.port,
               ev->u.port.blocked ? "blocked" : "unblocked");
        break;
    case ERPS_EV_SF:
        print_time(sh);
        printf("n%u port %d signal fail %s\n", node, (int)ev->u.sf.port,
               ev->u.sf.sf ? "declared" : "cleared");
        break;
    case ERPS_EV_FOP_PM:
    case ERPS_EV_FOP_TO:
        print_time(sh);
        printf("n%u %s %s\n", node, erps_event_str(ev->type), ev->u.fop.raised ? "RAISED" : "cleared");
        break;
    case ERPS_EV_CMD_REJECTED:
        print_time(sh);
        printf("n%u command %s rejected: %s\n", node, erps_cmd_str(ev->u.reject.cmd),
               erps_reject_str(ev->u.reject.reason));
        break;
    default:
        break;
    }
}

static bool parse_dur(const char *s, erps_time_t *out)
{
    if (!s || !isdigit((unsigned char)*s))
        return false;
    char *end;
    double v = strtod(s, &end);
    double mul;
    if (!*end || !strcmp(end, "s"))
        mul = 1e6;
    else if (!strcmp(end, "ms"))
        mul = 1e3;
    else if (!strcmp(end, "us"))
        mul = 1;
    else if (!strcmp(end, "m") || !strcmp(end, "min"))
        mul = 60e6;
    else if (!strcmp(end, "h"))
        mul = 3600e6;
    else
        return false;
    if (v < 0 || v * mul > 1e15)
        return false;
    *out = (erps_time_t)(v * mul);
    return true;
}

static int split(char *line, char **tok, int max)
{
    int n = 0;
    for (char *p = strtok(line, " \t\r\n"); p && n < max; p = strtok(NULL, " \t\r\n"))
        tok[n++] = p;
    return n;
}

__attribute__((format(printf, 2, 3))) static void err(struct shell *sh, const char *fmt, ...)
{
    if (sh->src)
        printf("%s:%u: ", sh->src, sh->lineno);
    printf("%% ");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    sh->failures++;
}

static void show_ring(struct shell *sh)
{
    char b[1024];
    erps_sim_describe(sh->sim, b, sizeof b);
    printf("%s\n", b);
}

/* ---- sim create ---------------------------------------------------------------------------- */

static void sim_create(struct shell *sh, char **t, int n)
{
    unsigned long nodes;
    if (n < 3 || !(nodes = strtoul(t[2], NULL, 10)) || nodes < 2 || nodes > 255) {
        err(sh, "usage: sim create <nodes 2..255> [options]");
        return;
    }
    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = (unsigned)nodes;
    c.owner = c.nodes - 1;
    uint8_t ids[256];
    bool has_id[256] = { false };
    for (int i = 3; i < n; i++) {
        const char *k = t[i], *v = i + 1 < n ? t[i + 1] : NULL;
        erps_time_t d;
        if (!strcmp(k, "no-neighbour")) {
            c.neighbour = false;
        } else if (!strcmp(k, "non-revertive")) {
            c.node_template.revertive = false;
        } else if (!strcmp(k, "owner") && v) {
            c.owner = (unsigned)strtoul(v, NULL, 10);
            i++;
        } else if (!strcmp(k, "compat") && v) {
            c.node_template.compat_version = (uint8_t)strtoul(v, NULL, 10);
            i++;
        } else if ((!strcmp(k, "wtr") || !strcmp(k, "guard") || !strcmp(k, "holdoff")) && v &&
                   parse_dur(v, &d)) {
            uint32_t ms = (uint32_t)(d / 1000u);
            if (k[0] == 'w')
                c.node_template.wtr_ms = ms;
            else if (k[0] == 'g')
                c.node_template.guard_ms = ms;
            else
                c.node_template.holdoff_ms = ms;
            c.node_template.relaxed_timers = true;
            i++;
        } else if (!strcmp(k, "delay") && v) {
            c.link_delay = strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(k, "jitter") && v) {
            c.link_jitter = strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(k, "loss") && v) {
            c.loss = strtod(v, NULL);
            i++;
        } else if (!strcmp(k, "seed") && v) {
            c.seed = strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(k, "id") && i + 2 < n) {
            unsigned long nd = strtoul(t[i + 1], NULL, 10);
            if (nd < 256) {
                ids[nd] = (uint8_t)strtoul(t[i + 2], NULL, 0);
                has_id[nd] = true;
            }
            i += 2;
        } else {
            err(sh, "sim create: bad option '%s'", k);
            return;
        }
    }
    if (c.owner >= c.nodes) {
        err(sh, "sim create: owner out of range");
        return;
    }
    if (sh->cli)
        erps_cli_destroy(sh->cli);
    if (sh->sim)
        erps_sim_destroy(sh->sim);
    sh->sim = erps_sim_create(&c);
    if (!sh->sim) {
        err(sh, "sim create failed");
        return;
    }
    for (unsigned i = 0; i < c.nodes; i++)
        if (has_id[i])
            erps_sim_node_config(sh->sim, i)->node_id[5] = ids[i];
    erps_sim_set_event_hook(sh->sim, on_event, sh);
    if (sh->log_on)
        erps_sim_set_log(sh->sim, stdout, sh->log_level);
    if (erps_sim_start(sh->sim) != ERPS_OK) {
        err(sh, "sim start failed (invalid node configuration?)");
        return;
    }
    erps_cli_platform_t p = { .ctx = sh, .now = cli_now };
    sh->cli = erps_cli_create(&p);
    for (unsigned i = 0; i < c.nodes; i++)
        erps_cli_attach(sh->cli, erps_sim_node(sh->sim, i));
    if (c.neighbour)
        printf("ring of %u nodes, RPL = link %u (owner n%u, neighbour n%u), %s\n", c.nodes,
               c.owner, c.owner, (c.owner + 1) % c.nodes,
               c.node_template.revertive ? "revertive" : "non-revertive");
    else
        printf("ring of %u nodes, RPL = link %u (owner n%u, no neighbour), %s\n", c.nodes, c.owner,
               c.owner, c.node_template.revertive ? "revertive" : "non-revertive");
}

/* ---- dispatcher ------------------------------------------------------------------------- */

static int exec_line(struct shell *sh, const char *raw)
{
    char line[2048];
    snprintf(line, sizeof line, "%s", raw);
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
    char copy[2048];
    snprintf(copy, sizeof copy, "%s", line);
    char *t[64];
    int n = split(line, t, 64);
    if (n == 0)
        return 0;

    if (!strcmp(t[0], "quit") || !strcmp(t[0], "exit"))
        return 1;
    if (!strcmp(t[0], "help") || !strcmp(t[0], "?")) {
        fputs(help_text, stdout);
        return 0;
    }
    if (!strcmp(t[0], "echo")) {
        const char *p = strstr(copy, "echo") + 4;
        while (*p == ' ')
            p++;
        printf("%s\n", p);
        return 0;
    }
    if (!strcmp(t[0], "sim") && n >= 2 && !strcmp(t[1], "create")) {
        sim_create(sh, t, n);
        return 0;
    }
    if (!strcmp(t[0], "events") && n == 2) {
        sh->events = !strcmp(t[1], "on");
        return 0;
    }
    if (!strcmp(t[0], "log") && n == 2) {
        sh->log_on = strcmp(t[1], "off") != 0;
        static const char *lv[] = { "error", "warning", "notice", "info", "debug", "trace" };
        for (int i = 0; i < 6; i++)
            if (!strcmp(t[1], lv[i]))
                sh->log_level = (erps_log_level_t)i;
        if (sh->sim)
            erps_sim_set_log(sh->sim, sh->log_on ? stdout : NULL, sh->log_level);
        return 0;
    }
    if (!sh->sim) {
        err(sh, "no ring yet: use 'sim create <nodes>'");
        return 0;
    }
    if (!strcmp(t[0], "run") && n == 2) {
        erps_time_t d;
        if (!parse_dur(t[1], &d)) {
            err(sh, "run: bad duration '%s'", t[1]);
            return 0;
        }
        erps_sim_run_for(sh->sim, d);
        return 0;
    }
    if ((!strcmp(t[0], "show") && n == 1) || !strcmp(t[0], "status")) {
        show_ring(sh);
        return 0;
    }
    if (!strcmp(t[0], "link") && n >= 3) {
        unsigned l = (unsigned)strtoul(t[1], NULL, 10);
        erps_sim_dir_t dir = ERPS_SIM_BOTH;
        if (n >= 4)
            dir = !strcmp(t[3], "cw") ? ERPS_SIM_CW : !strcmp(t[3], "ccw") ? ERPS_SIM_CCW : ERPS_SIM_BOTH;
        if (l >= erps_sim_nodes(sh->sim)) {
            err(sh, "link: no link %s", t[1]);
            return 0;
        }
        erps_sim_link_set(sh->sim, l, dir, !strcmp(t[2], "up"));
        return 0;
    }
    if (!strcmp(t[0], "node") && n >= 3) {
        unsigned nd = (unsigned)strtoul(t[1], NULL, 10);
        if (nd >= erps_sim_nodes(sh->sim)) {
            err(sh, "node: no node %s", t[1]);
            return 0;
        }
        if (!strcmp(t[2], "down") || !strcmp(t[2], "up")) {
            erps_sim_node_set_up(sh->sim, nd, t[2][0] == 'u');
            return 0;
        }
        erps_reject_t why = ERPS_REJECT_NONE;
        erps_err_t rc;
        if ((!strcmp(t[2], "fs") || !strcmp(t[2], "ms")) && n == 4)
            rc = erps_sim_command(sh->sim, nd, t[2][0] == 'f' ? ERPS_CMD_FORCED_SWITCH : ERPS_CMD_MANUAL_SWITCH,
                                  t[3][0] == '1' ? ERPS_PORT1 : ERPS_PORT0, &why);
        else if (!strcmp(t[2], "clear"))
            rc = erps_sim_command(sh->sim, nd, ERPS_CMD_CLEAR, ERPS_PORT_NONE, &why);
        else {
            err(sh, "node: unknown action '%s'", t[2]);
            return 0;
        }
        if (rc == ERPS_E_REJECTED)
            printf("%% rejected: %s\n", erps_reject_str(why));
        return 0;
    }
    if (!strcmp(t[0], "pcap") && n == 2) {
        if (!strcmp(t[1], "off"))
            erps_sim_pcap_close(sh->sim);
        else if (erps_sim_pcap_open(sh->sim, t[1]) != 0)
            err(sh, "pcap: cannot open %s", t[1]);
        return 0;
    }
    if (!strcmp(t[0], "expect") && n >= 2) {
        char b[1024];
        if (!strcmp(t[1], "no-loops") && n == 2) {
            if (erps_sim_loop_violations(sh->sim))
                err(sh, "expect no-loops FAILED: %s", erps_sim_first_violation(sh->sim));
            return 0;
        }
        if (!strcmp(t[1], "state") && n == 4) {
            unsigned nd = (unsigned)strtoul(t[2], NULL, 10);
            char want = (char)toupper((unsigned char)t[3][0]);
            if (erps_state_letter(erps_sim_node_state(sh->sim, nd)) != want) {
                erps_sim_describe(sh->sim, b, sizeof b);
                err(sh, "expect state FAILED: %s", b);
            }
            return 0;
        }
        if (!strcmp(t[1], "blocked") && n == 5) {
            unsigned nd = (unsigned)strtoul(t[2], NULL, 10);
            bool want = !strcmp(t[4], "yes");
            if (erps_sim_port_blocked(sh->sim, nd, t[3][0] == '1' ? ERPS_PORT1 : ERPS_PORT0) != want) {
                erps_sim_describe(sh->sim, b, sizeof b);
                err(sh, "expect blocked FAILED: %s", b);
            }
            return 0;
        }
        err(sh, "expect: bad arguments");
        return 0;
    }
    /* everything else goes to the command layer */
    if (erps_cli_exec(sh->cli, copy, out_stdout, NULL) != ERPS_OK)
        sh->failures++;
    return 0;
}

static int run_stream(struct shell *sh, FILE *in, bool interactive)
{
    char line[2048];
    for (;;) {
        if (interactive) {
            printf("erps-sim> ");
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, in))
            break;
        sh->lineno++;
        if (exec_line(sh, line))
            return 1;
        fflush(stdout);
    }
    return 0;
}

static void usage(void)
{
    fputs("usage: erps-sim [-f script] [-e 'cmd; cmd; ...'] [-q]\n"
          "  -f FILE   run commands from FILE ('-' = stdin), then exit\n"
          "  -e CMDS   run ';'-separated commands, then exit\n"
          "  -q        do not print protocol events\n"
          "Without -f/-e: interactive shell. Type 'help'.\n",
          stderr);
}

int main(int argc, char **argv)
{
    struct shell sh;
    memset(&sh, 0, sizeof sh);
    sh.events = true;
    sh.log_level = ERPS_LOG_DEBUG;
    const char *script = NULL, *cmds = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "f:e:qh")) != -1) {
        switch (opt) {
        case 'f':
            script = optarg;
            break;
        case 'e':
            cmds = optarg;
            break;
        case 'q':
            sh.events = false;
            break;
        default:
            usage();
            return opt == 'h' ? 0 : 2;
        }
    }
    if (cmds) {
        char *dup = strdup(cmds);
        for (char *save = NULL, *c = strtok_r(dup, ";", &save); c; c = strtok_r(NULL, ";", &save))
            if (exec_line(&sh, c))
                break;
        free(dup);
    } else if (script) {
        FILE *f = strcmp(script, "-") ? fopen(script, "r") : stdin;
        if (!f) {
            perror(script);
            return 2;
        }
        sh.src = script;
        run_stream(&sh, f, false);
        if (f != stdin)
            fclose(f);
    } else {
        printf("liberps %s ring simulator. Type 'help'.\n", erps_version());
        run_stream(&sh, stdin, isatty(0));
    }
    if (sh.cli)
        erps_cli_destroy(sh.cli);
    if (sh.sim)
        erps_sim_destroy(sh.sim);
    if (sh.failures && (script || cmds)) {
        fprintf(stderr, "erps-sim: %d failure(s)\n", sh.failures);
        return 1;
    }
    return 0;
}
