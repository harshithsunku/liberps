/* SPDX-License-Identifier: Apache-2.0 */
#include "simutil.h"

TEST(sim_init_converges_to_idle)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(1));
    sim_show(s);
    /* Before WTR expiry (row 1 starts WTR at the owner) every node is still Pending. */
    CHECK_EQ(erps_sim_node_state(s, NG), ERPS_STATE_PENDING);
    erps_sim_run_for(s, ERPS_SEC(5 * 60));
    sim_show(s);
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);
    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}
