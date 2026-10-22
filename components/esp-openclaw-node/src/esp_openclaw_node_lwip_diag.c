/*
 * SPDX-FileCopyrightTextText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * esp_openclaw_node_lwip_diag — snapshot of the TCP/IP stack at the moment
 * a WebSocket transport dies.
 *
 * Why: 2026-10-21 field captures show BOTH wss transports (operator + node,
 * separate TCP connections over the same WG lane) aborting 1s apart,
 * ~19s into every call, with mbedTLS -0x004C / errno ECONNABORTED. The
 * task watchdog was acquitted (watchdog silenced 2026-10-21, aborts
 * unchanged). Prime suspects are lwIP resource exhaustion (mbox/pbuf/mem
 * -> ERR_ABRT) or WG lane backpressure. This snapshot names the exhausted
 * resource at the moment of death.
 *
 * Requires CONFIG_LWIP_STATS=y; every counter is guarded so builds
 * without it compile to a no-op. All access happens on the event/task
 * calling this — it only READS lwip_stats (values are eventually
 * consistent; that is enough for a fault snapshot).
 */

#include "esp_openclaw_node_lwip_diag.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/stats.h"

#define LWIP_DIAG_TAG "node_lwip_diag"

void esp_openclaw_node_lwip_diag_dump(const char *reason)
{
#if LWIP_STATS
    const char *r = (reason && reason[0]) ? reason : "unspecified";
    ESP_LOGW(LWIP_DIAG_TAG, "=== lwIP snapshot at transport failure (%s) ===",
             r);

#if MEM_STATS
    ESP_LOGW(LWIP_DIAG_TAG,
             "mem: used=%u max=%u avail=%u err=%u illegal=%u",
             (unsigned)lwip_stats.mem.used,
             (unsigned)lwip_stats.mem.max,
             (unsigned)lwip_stats.mem.avail,
             (unsigned)lwip_stats.mem.err,
             (unsigned)lwip_stats.mem.illegal);
#endif

#if SYS_STATS
    ESP_LOGW(LWIP_DIAG_TAG,
             "mbox: sem used/max/err=%u/%u/%u | mutex used/max/err=%u/%u/%u | mbox used/max/err=%u/%u/%u",
             (unsigned)lwip_stats.sys.sem.used,
             (unsigned)lwip_stats.sys.sem.max,
             (unsigned)lwip_stats.sys.sem.err,
             (unsigned)lwip_stats.sys.mutex.used,
             (unsigned)lwip_stats.sys.mutex.max,
             (unsigned)lwip_stats.sys.mutex.err,
             (unsigned)lwip_stats.sys.mbox.used,
             (unsigned)lwip_stats.sys.mbox.max,
             (unsigned)lwip_stats.sys.mbox.err);
#endif

#if IP_STATS
    ESP_LOGW(LWIP_DIAG_TAG,
             "ip: recv=%u drop=%u memerr=%u err=%u | frag drop=%u err=%u",
             (unsigned)lwip_stats.ip.recv,
             (unsigned)lwip_stats.ip.drop,
             (unsigned)lwip_stats.ip.memerr,
             (unsigned)lwip_stats.ip.err,
#if IP_FRAG_STATS
             (unsigned)lwip_stats.ip_frag.drop,
             (unsigned)lwip_stats.ip_frag.err
#else
             0u, 0u
#endif
             );
#endif

#if UDP_STATS
    ESP_LOGW(LWIP_DIAG_TAG,
             "udp: xmit=%u recv=%u drop=%u memerr=%u err=%u",
             (unsigned)lwip_stats.udp.xmit,
             (unsigned)lwip_stats.udp.recv,
             (unsigned)lwip_stats.udp.drop,
             (unsigned)lwip_stats.udp.memerr,
             (unsigned)lwip_stats.udp.err);
#endif

#if TCP_STATS
    ESP_LOGW(LWIP_DIAG_TAG,
             "tcp: xmit=%u recv=%u drop=%u memerr=%u err=%u chkerr=%u lenerr=%u",
             (unsigned)lwip_stats.tcp.xmit,
             (unsigned)lwip_stats.tcp.recv,
             (unsigned)lwip_stats.tcp.drop,
             (unsigned)lwip_stats.tcp.memerr,
             (unsigned)lwip_stats.tcp.err,
             (unsigned)lwip_stats.tcp.chkerr,
             (unsigned)lwip_stats.tcp.lenerr);
#endif

    /* Internal-RAM pressure at death: ties the network event to the
     * known per-call heap wedge (2026-10-20/21 memory notes). */
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL);
    ESP_LOGW(LWIP_DIAG_TAG,
             "heap: internal_free=%u internal_largest=%u",
             (unsigned)internal_free, (unsigned)internal_largest);

    ESP_LOGW(LWIP_DIAG_TAG, "=== lwIP snapshot end ===");
#else
    (void)reason;
#endif /* LWIP_STATS */
}