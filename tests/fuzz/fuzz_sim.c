/* SPDX-License-Identifier: Apache-2.0 */

#include "erps/erps_sim.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 9)
        return 0;

    unsigned nodes = 2 + (data[0] % 9); /* 2..10 nodes */
    unsigned owner = data[1] % nodes;
    bool neighbour = (data[2] & 1) != 0;
    bool revertive = (data[3] & 1) != 0;
    erps_time_t link_delay = ERPS_USEC(10 + (data[4] % 100) * 10);
    erps_time_t link_jitter = (data[5] & 1) ? ERPS_USEC(((data[5] >> 1) % 50) * 10) : 0;
    double loss = (double)(data[6] % 10) * 0.01;
    uint64_t seed = (uint64_t)data[7] | ((uint64_t)data[8] << 8);

    erps_sim_cfg_t cfg;
    erps_sim_cfg_init(&cfg);
    cfg.nodes = nodes;
    cfg.owner = owner;
    cfg.neighbour = neighbour;
    cfg.link_delay = link_delay;
    cfg.link_jitter = link_jitter;
    cfg.loss = loss;
    cfg.seed = seed ? seed : 1;
    cfg.node_template.revertive = revertive;

    erps_sim_t *sim = erps_sim_create(&cfg);
    if (!sim)
        return 0;

    for (unsigned i = 0; i < nodes; i++) {
        uint8_t *id = erps_sim_node_config(sim, i)->node_id;
        id[0] = 0x02;
        id[3] = (uint8_t)(i + 1);
        id[4] = (uint8_t)data[i % size];
        id[5] = (uint8_t)(i * 2 + 1);
    }

    if (erps_sim_start(sim) != ERPS_OK) {
        erps_sim_destroy(sim);
        return 0;
    }

    erps_time_t down_since[10];
    for (unsigned i = 0; i < 10; i++)
        down_since[i] = ERPS_TIME_NEVER;

    const erps_time_t max_sim_time = ERPS_SEC(3600); /* 1 hour bound */
    const unsigned max_ops = 64;
    unsigned op_count = 0;
    size_t pos = 9;

    while (pos < size && op_count < max_ops && erps_sim_now(sim) < max_sim_time) {
        uint8_t op = data[pos++] % 4;
        op_count++;

        switch (op) {
        case 0: { /* Link up/down (both or one direction) */
            if (pos + 2 > size)
                break;
            unsigned link = data[pos++] % nodes;
            uint8_t d = data[pos++];
            uint8_t dir_raw = (d >> 1) % 3;
            erps_sim_dir_t dir = (dir_raw == 0) ? ERPS_SIM_BOTH :
                                 (dir_raw == 1) ? ERPS_SIM_CW : ERPS_SIM_CCW;
            bool up = (d & 1) != 0;
            erps_sim_link_set(sim, link, dir, up);
            break;
        }
        case 1: { /* Node down/up */
            if (pos + 1 > size)
                break;
            uint8_t b = data[pos++];
            unsigned node = (b >> 1) % nodes;
            bool up = (b & 1) != 0;

            /* Node must stay down at least 2 s before coming back up */
            if (up && down_since[node] != ERPS_TIME_NEVER) {
                erps_time_t min_up = down_since[node] + ERPS_SEC(2);
                if (erps_sim_now(sim) < min_up)
                    erps_sim_run_until(sim, min_up);
            }
            if (!up && down_since[node] == ERPS_TIME_NEVER)
                down_since[node] = erps_sim_now(sim);
            if (up)
                down_since[node] = ERPS_TIME_NEVER;

            erps_sim_node_set_up(sim, node, up);
            break;
        }
        case 2: { /* FS/MS/Clear on random nodes and ports */
            if (pos + 2 > size)
                break;
            unsigned node = data[pos++] % nodes;
            uint8_t b = data[pos++];
            uint8_t cmd_idx = (b >> 1) % 3;
            erps_cmd_t cmd = (cmd_idx == 0) ? ERPS_CMD_FORCED_SWITCH :
                             (cmd_idx == 1) ? ERPS_CMD_MANUAL_SWITCH : ERPS_CMD_CLEAR;
            erps_port_t port = (b & 1) ? ERPS_PORT1 : ERPS_PORT0;
            erps_sim_command(sim, node, cmd, port, NULL);
            break;
        }
        case 3: { /* Run for fuzz-chosen durations (0 .. 10 min) */
            if (pos >= size)
                break;
            uint8_t b = data[pos++];
            static const erps_time_t durations[] = {
                0,
                ERPS_USEC(200),
                ERPS_MSEC(1),
                ERPS_USEC(3333),
                ERPS_MSEC(20),
                ERPS_MSEC(100),
                ERPS_MSEC(500),
                ERPS_SEC(1),
                ERPS_SEC(2),
                ERPS_SEC(5),
                ERPS_SEC(10),
                ERPS_SEC(30),
                ERPS_SEC(60),
                ERPS_SEC(120),
                ERPS_SEC(300),
                ERPS_SEC(600),
            };
            erps_time_t dt;
            if (b & 0x80)
                dt = durations[b & 0x0F];
            else
                dt = ERPS_SEC((uint64_t)(b & 0x7F) * 600u / 127u);

            erps_time_t cur = erps_sim_now(sim);
            if (cur + dt > max_sim_time)
                dt = (max_sim_time > cur) ? (max_sim_time - cur) : 0;
            if (dt > 0)
                erps_sim_run_for(sim, dt);
            break;
        }
        default:
            break;
        }

        if (erps_sim_loop_violations(sim) > 0) {
            fprintf(stderr, "Loop violation: %s\n", erps_sim_first_violation(sim));
            abort();
        }
    }

    if (erps_sim_loop_violations(sim) > 0) {
        fprintf(stderr, "Loop violation: %s\n", erps_sim_first_violation(sim));
        abort();
    }

    erps_sim_destroy(sim);
    return 0;
}
