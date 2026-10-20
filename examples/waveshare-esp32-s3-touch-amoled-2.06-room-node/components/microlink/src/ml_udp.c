/**
 * @file ml_udp.c
 * @brief MicroLink v2 UDP Socket API
 *
 * Provides simple UDP send/receive over the Tailscale VPN tunnel.
 * Uses lwIP UDP PCB bound to the WireGuard netif for encrypted transport.
 *
 * On socket creation, sends CallMeMaybe to all peers to trigger
 * peer-initiated WireGuard handshakes (NAT traversal).
 *
 * Reference: microlink v1 microlink_udp.c
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h" /* FIELD 2026-10-19: heap snapshot on spawn failure */
#include "freertos/idf_additions.h" /* FIELD 2026-10-19: xTaskCreateWithCaps (PSRAM stacks) */
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include <string.h>

static const char *TAG = "ml_udp";

/* RX queue depth 16 (was 4): a TTS PCM burst lands clumped — 27×1200B in
 * ~110ms — and the tts.done control datagram rides right behind it. At
 * depth 4 the burst boundary silently drops (pbuf_free, no log) exactly
 * the control packet: field 13:23 turn played but tts.done never processed
 * (no [PA] OFF, blind park stranded the tail). 16 × 1400B ≈ 22KB per
 * socket, trivial on the PSRAM board. VENDORED DIVERGENCE: upstream
 * microlink has 4 — re-vendoring must re-apply. */
#define UDP_RX_QUEUE_SIZE   16
#define UDP_MAX_PKT_SIZE    1400

/* ============================================================================
 * Internal Types
 * ========================================================================== */

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t data[UDP_MAX_PKT_SIZE];
    size_t len;
    bool valid;
} udp_rx_pkt_t;

struct microlink_udp_socket {
    microlink_t *ml;
    struct udp_pcb *pcb;
    uint16_t local_port;

    /* RX ring buffer */
    udp_rx_pkt_t rx_queue[UDP_RX_QUEUE_SIZE];
    volatile uint8_t rx_head;
    volatile uint8_t rx_tail;

    /* RX notification */
    SemaphoreHandle_t rx_sem;

    /* User callback */
    microlink_udp_rx_cb_t rx_cb;
    void *rx_cb_data;

    /* Dedicated RX task */
    TaskHandle_t rx_task;
    volatile bool rx_running;
    bool rx_task_caps; /* FIELD 2026-10-19: stack via heap_caps → self-delete must use WithCaps */
    volatile bool rx_exited; /* FIELD 2026-10-19 05:50: close() joins the rx task before free (crash-chain fix) */
};

/* ============================================================================
 * Helpers
 * ========================================================================== */

static void ip_to_lwip(uint32_t ml_ip, ip_addr_t *out) {
    IP4_ADDR(&out->u_addr.ip4,
             (ml_ip >> 24) & 0xFF,
             (ml_ip >> 16) & 0xFF,
             (ml_ip >> 8) & 0xFF,
             ml_ip & 0xFF);
    out->type = IPADDR_TYPE_V4;
}

static uint32_t lwip_to_ip(const ip_addr_t *addr) {
    if (addr->type != IPADDR_TYPE_V4) return 0;
    uint32_t ip = ip4_addr_get_u32(&addr->u_addr.ip4);
    return ((ip & 0xFF) << 24) |
           (((ip >> 8) & 0xFF) << 16) |
           (((ip >> 16) & 0xFF) << 8) |
           ((ip >> 24) & 0xFF);
}

/* ============================================================================
 * lwIP UDP Callback (runs from tcpip thread)
 * ========================================================================== */

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    microlink_udp_socket_t *sock = (microlink_udp_socket_t *)arg;
    if (!sock || !p) {
        if (p) pbuf_free(p);
        return;
    }

    /* FIELD 2026-10-19: prove inbound app-UDP reaches the callback at all.
     * Rate-limited 1/5s; sizes + src identify the lane. */
    {
        static uint64_t last_arrival_log_ms = 0;
        uint64_t now_ms = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - last_arrival_log_ms > 5000) {
            last_arrival_log_ms = now_ms;
            ESP_LOGI(TAG, "UDP RX arrival: %u bytes from %s:%u (local port %u)",
                     (unsigned)p->tot_len, ipaddr_ntoa(addr), (unsigned)port,
                     (unsigned)sock->local_port);
        }
    }

    uint8_t next = (sock->rx_head + 1) % UDP_RX_QUEUE_SIZE;
    if (next == sock->rx_tail) {
        /* Queue full */
        pbuf_free(p);
        return;
    }

    udp_rx_pkt_t *pkt = &sock->rx_queue[sock->rx_head];
    pkt->src_ip = lwip_to_ip(addr);
    pkt->src_port = port;
    pkt->len = (p->tot_len > UDP_MAX_PKT_SIZE) ? UDP_MAX_PKT_SIZE : p->tot_len;
    pbuf_copy_partial(p, pkt->data, pkt->len, 0);
    pkt->valid = true;

    __sync_synchronize();
    sock->rx_head = next;

    if (sock->rx_sem) {
        xSemaphoreGive(sock->rx_sem);
    }

    pbuf_free(p);
}

/* ============================================================================
 * RX Task
 * ========================================================================== */

static void udp_rx_task(void *arg) {
    microlink_udp_socket_t *sock = (microlink_udp_socket_t *)arg;

    while (sock->rx_running) {
        if (xSemaphoreTake(sock->rx_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
            while (sock->rx_tail != sock->rx_head) {
                udp_rx_pkt_t *pkt = &sock->rx_queue[sock->rx_tail];
                if (pkt->valid && sock->rx_cb) {
                    sock->rx_cb(sock, pkt->src_ip, pkt->src_port,
                                pkt->data, pkt->len, sock->rx_cb_data);
                    pkt->valid = false;
                    sock->rx_tail = (sock->rx_tail + 1) % UDP_RX_QUEUE_SIZE;
                } else if (!sock->rx_cb) {
                    break;  /* No callback, leave for polling */
                } else {
                    sock->rx_tail = (sock->rx_tail + 1) % UDP_RX_QUEUE_SIZE;
                }
            }
        }
    }

    /* FIELD 2026-10-19 05:50: close() joins this task — read the caps flag
     * BEFORE signaling exit (after the flag, close() may free the socket). */
    bool caps = sock->rx_task_caps;
    sock->rx_exited = true;
    if (caps) {
        vTaskDeleteWithCaps(NULL);
    }
    vTaskDelete(NULL);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

microlink_udp_socket_t *microlink_udp_create(microlink_t *ml, uint16_t local_port) {
    if (!ml) {
        ESP_LOGE(TAG, "NULL handle");
        return NULL;
    }
    if (ml->state != ML_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected (state=%d)", ml->state);
        return NULL;
    }

    microlink_udp_socket_t *sock = calloc(1, sizeof(microlink_udp_socket_t));
    if (!sock) return NULL;

    sock->ml = ml;
    sock->rx_sem = xSemaphoreCreateCounting(UDP_RX_QUEUE_SIZE, 0);
    if (!sock->rx_sem) {
        free(sock);
        return NULL;
    }

    sock->pcb = udp_new();
    if (!sock->pcb) {
        vSemaphoreDelete(sock->rx_sem);
        free(sock);
        return NULL;
    }

    /* Bind to WG netif */
    if (ml->wg_netif) {
        udp_bind_netif(sock->pcb, (struct netif *)ml->wg_netif);
    }

    /* Bind to VPN IP + port */
    ip_addr_t local_ip;
    ip_to_lwip(ml->vpn_ip, &local_ip);

    err_t err = udp_bind(sock->pcb, &local_ip, local_port);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "udp_bind failed: %d", err);
        udp_remove(sock->pcb);
        vSemaphoreDelete(sock->rx_sem);
        free(sock);
        return NULL;
    }

    sock->local_port = sock->pcb->local_port;
    udp_recv(sock->pcb, udp_recv_cb, sock);

    /* Start RX task on Core 1.
     * FIELD 2026-10-19 (AMOLED STT outage): the forced-internal 4KB stack
     * failed to spawn when internal heap was exhausted (free_int=5371 at
     * boot) — the lane then dropped everything silently. Spawn PSRAM-backed
     * first (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y on this board;
     * same pattern as ptt_audio / wg_voice_tx), internal fallback for
     * non-PSRAM boards (round board unaffected). */
    sock->rx_running = true;
    sock->rx_task_caps = false;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(udp_rx_task, "ml_udp_rx", 4096, sock,
                                                          configMAX_PRIORITIES - 2, &sock->rx_task,
                                                          1, /* core 1 — original affinity */
                                                          MALLOC_CAP_SPIRAM);
    if (created == pdPASS) {
        sock->rx_task_caps = true;
        ESP_LOGI(TAG, "UDP RX task spawned (PSRAM-backed stack, core 1, port %u)",
                 (unsigned)local_port);
    } else {
        created = xTaskCreatePinnedToCore(udp_rx_task, "ml_udp_rx", 4096, sock,
                                          configMAX_PRIORITIES - 2, &sock->rx_task, 1);
    }
    if (created != pdPASS) {
        sock->rx_running = false;
        sock->rx_task = NULL;
        /* FIELD 2026-10-19: spawn failure was SILENT — inbound packets would
         * fill the ring and drop with no log at all. Make it loud. */
        ESP_LOGE(TAG, "UDP RX TASK SPAWN FAILED (port %u) — inbound app-UDP will drop silently "
                      "(free_int=%lu free_psram=%lu)",
                 (unsigned)local_port,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    char ip_buf[16];
    microlink_ip_to_str(ml->vpn_ip, ip_buf);
    ESP_LOGI(TAG, "UDP socket: %s:%u", ip_buf, sock->local_port);

    /* Trigger CallMeMaybe to all peers for WG handshake establishment */
    for (int i = 0; i < ml->peer_count; i++) {
        if (ml->peers[i].active) {
            ml_wg_mgr_send_cmm(ml, ml->peers[i].vpn_ip);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    return sock;
}

void microlink_udp_close(microlink_udp_socket_t *sock) {
    if (!sock) return;

    /* FIELD 2026-10-19 05:50 crash-chain fix (AMOLED 'reboots' at coord
     * reconnect): the old close raced the rx task (blind 150ms delay) and
     * the tcpip-thread arrival callback. Sequence now:
     *   1. unregister the lwIP arrival callback
     *   2. 20ms quiesce — any in-flight udp_recv_cb on the tcpip thread
     *      finishes (it only memcpys into the ring + gives the sem)
     *   3. stop + JOIN the rx task via rx_exited (bounded 2s)
     *   4. udp_remove → delete sem → free
     * Without the join, a socket freed under a live rx task crashed
     * wg_voice_tx_task-style consumers and left orphaned pcbs holding the
     * ports (udp_bind ERR_USE -8 on every reconnect). */
    if (sock->pcb) {
        udp_recv(sock->pcb, NULL, NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (sock->rx_running) {
        sock->rx_running = false;
        if (sock->rx_sem) xSemaphoreGive(sock->rx_sem);
    }
    if (sock->rx_task != NULL) {
        for (int i = 0; i < 200 && !sock->rx_exited; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!sock->rx_exited) {
            ESP_LOGW(TAG, "UDP RX task did not exit in 2s — freeing socket anyway (task may fault)");
        }
    } else {
        sock->rx_exited = true;   /* never started */
    }

    if (sock->pcb) {
        udp_remove(sock->pcb);
    }

    if (sock->rx_sem) vSemaphoreDelete(sock->rx_sem);
    free(sock);
    ESP_LOGI(TAG, "UDP socket closed");
}

esp_err_t microlink_udp_send(microlink_udp_socket_t *sock, uint32_t dest_ip,
                              uint16_t dest_port, const void *data, size_t len) {
    if (!sock || !sock->pcb || !data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > UDP_MAX_PKT_SIZE) return ESP_ERR_INVALID_SIZE;

    ip_addr_t dest;
    ip_to_lwip(dest_ip, &dest);

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) return ESP_ERR_NO_MEM;

    memcpy(p->payload, data, len);
    err_t err = udp_sendto(sock->pcb, p, &dest, dest_port);
    pbuf_free(p);

    if (err != ERR_OK) {
        /* FIELD (2026-10-17, AMOLED uplink): all lwIP errors were mapped to
         * ESP_FAIL with no detail — 1200B audio chunks failed ESP_FAIL while
         * 16B control on the SAME socket succeeded. Expose the raw err + len
         * (rate-limited 1/3s) so the real cause is diagnosable. */
        static uint64_t last_diag_ms = 0;
        static size_t last_diag_len = 0;
        uint64_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - last_diag_ms > 3000 || len != last_diag_len) {
            last_diag_ms = now_ms;
            last_diag_len = len;
            ESP_LOGE(TAG, "udp_sendto failed: err=%d (0x%x) len=%u dest_port=%u",
                     (int)err, (unsigned)err, (unsigned)len, (unsigned)dest_port);
        }
        if (sock->ml) {
            /* Rate-limit handshake retrigger: max once per 10 seconds per socket.
             * Without this, high-frequency heartbeats each trigger a full WG handshake
             * initiation + CallMeMaybe, flooding the DERP TX queue. */
            static uint64_t last_trigger_ms = 0;
            uint64_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms - last_trigger_ms > 10000) {
                last_trigger_ms = now_ms;
                ml_wg_mgr_trigger_handshake(sock->ml, dest_ip);
                ml_wg_mgr_send_cmm(sock->ml, dest_ip);
            }
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t microlink_udp_recv(microlink_udp_socket_t *sock, uint32_t *src_ip,
                              uint16_t *src_port, void *buffer, size_t *len,
                              uint32_t timeout_ms) {
    if (!sock || !buffer || !len || *len == 0) return ESP_ERR_INVALID_ARG;

    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (1) {
        if (sock->rx_tail != sock->rx_head) {
            udp_rx_pkt_t *pkt = &sock->rx_queue[sock->rx_tail];
            if (pkt->valid) {
                size_t copy = (pkt->len < *len) ? pkt->len : *len;
                memcpy(buffer, pkt->data, copy);
                *len = copy;
                if (src_ip) *src_ip = pkt->src_ip;
                if (src_port) *src_port = pkt->src_port;
                pkt->valid = false;
                sock->rx_tail = (sock->rx_tail + 1) % UDP_RX_QUEUE_SIZE;
                return ESP_OK;
            }
        }

        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start;
        if (timeout_ms > 0 && elapsed >= timeout_ms) return ESP_ERR_TIMEOUT;
        if (timeout_ms == 0) return ESP_ERR_TIMEOUT;

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t microlink_udp_set_rx_callback(microlink_udp_socket_t *sock,
                                         microlink_udp_rx_cb_t cb, void *user_data) {
    if (!sock) return ESP_ERR_INVALID_ARG;
    sock->rx_cb = cb;
    sock->rx_cb_data = user_data;
    return ESP_OK;
}

uint16_t microlink_udp_get_local_port(const microlink_udp_socket_t *sock) {
    return sock ? sock->local_port : 0;
}
