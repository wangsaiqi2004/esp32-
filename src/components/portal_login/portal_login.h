/* Uplink captive-portal auto-login for ePortal (Dr.COM / city hotspot) systems.
 *
 * Problem it solves: when the ESP32's uplink sits behind a web login page, the
 * uplink session expires after a while. Clients behind NAT never see the portal
 * page pop up (their traffic is NATed and silently dropped), so everyone loses
 * internet until a human opens the portal manually. This module watches real
 * internet reachability and re-authenticates on its own.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load config from NVS and start the watchdog task. Safe to call once at boot. */
void portal_init(void);

/* Snapshot current config (any out pointer may be NULL). */
void portal_config_get(char *url, size_t url_len,
                       char *user, size_t user_len,
                       char *pass, size_t pass_len,
                       int *enabled, int *interval);

/* Persist new config. Pass NULL for fields that should stay unchanged. */
void portal_config_set(const char *url, const char *user, const char *pass,
                       int enabled, int interval);

/* One-line human readable status for the web UI. */
void portal_status_text(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
