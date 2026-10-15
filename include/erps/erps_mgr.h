/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Instance manager: owns several ERP instances of one node and offers the operations a platform
 * event loop needs — demultiplex a received frame to the right instance (by port handle, R-APS
 * VID and ring ID), fan out a ring-port signal-fail to every instance using that port, poll all
 * instances and compute the earliest deadline. Pure C, no I/O; same threading rules as erps.h.
 */
#ifndef ERPS_MGR_H
#define ERPS_MGR_H

#include "erps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct erps_mgr erps_mgr_t;

ERPS_API erps_mgr_t *erps_mgr_create(void);
/** Destroys all instances it owns. */
ERPS_API void erps_mgr_destroy(erps_mgr_t *m);

/**
 * Create an instance (not started). Names must be unique; (ring_id, raps_vid) must be unique
 * among instances sharing a ring port (§10.1.1 rule 1).
 */
ERPS_API erps_err_t erps_mgr_add(erps_mgr_t *m, const erps_config_t *cfg, const erps_ops_t *ops,
                                 void *user, erps_t **out);
/** Destroy an instance (does not touch the dataplane; call erps_stop() first if wanted). */
ERPS_API erps_err_t erps_mgr_remove(erps_mgr_t *m, erps_t *e);

ERPS_API erps_t *erps_mgr_find(const erps_mgr_t *m, const char *name);
ERPS_API size_t erps_mgr_count(const erps_mgr_t *m);
ERPS_API erps_t *erps_mgr_at(const erps_mgr_t *m, size_t i);

/**
 * Deliver a frame received on platform port @p handle to the matching instance. Returns
 * ERPS_E_NOTFOUND if no instance uses that (port, VID, ring ID), otherwise erps_rx_frame()'s result.
 */
ERPS_API erps_err_t erps_mgr_rx_frame(erps_mgr_t *m, uint32_t handle, const uint8_t *frame,
                                      size_t len, erps_time_t now);

/** Signal fail of platform port @p handle, applied to every instance using it. */
ERPS_API erps_err_t erps_mgr_set_signal_fail(erps_mgr_t *m, uint32_t handle, bool ssf,
                                             erps_time_t now);

/** Poll every instance whose deadline has passed. */
ERPS_API erps_err_t erps_mgr_poll(erps_mgr_t *m, erps_time_t now);
ERPS_API erps_time_t erps_mgr_next_deadline(const erps_mgr_t *m);

#ifdef __cplusplus
}
#endif

#endif /* ERPS_MGR_H */
