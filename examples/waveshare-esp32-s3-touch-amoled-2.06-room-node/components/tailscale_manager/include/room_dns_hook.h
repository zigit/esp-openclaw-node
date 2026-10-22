#pragma once
/*
 * room_dns_hook — external DNS resolver hook for the room node's tailnet era.
 *
 * The gateway URI is a MagicDNS name (*.ts.net). The LAN DHCP resolver can't
 * answer it; the tailnet resolver (100.100.100.100) is only reachable through
 * WG routes that need inbound-UDP flow warmth — structurally awkward for the
 * connect-time lookup. Instead: ts_manager resolves the gateway name through
 * the microlink echo lane (fixed ports both sides, VM2 sends first), then
 * registers the answer HERE; lwIP's netconn resolve hook consults this table
 * before anything else. Entries never expire (single stable gateway).
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register host → A record. Empty/zero addr clears the entry. */
void room_dns_hook_set(const char *hostname, uint32_t host_order_ip);

/** Host-order IP for hostname, 0 when unknown. */
uint32_t room_dns_hook_get(const char *hostname);

#ifdef __cplusplus
}
#endif