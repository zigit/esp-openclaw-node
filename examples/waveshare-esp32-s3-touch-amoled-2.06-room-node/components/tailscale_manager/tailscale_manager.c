/**
 * tailscale_manager.c — microlink lifecycle wrapper adapted for
 * esp-openclaw-node (room node).
 *
 * Design notes (2026-10-17, phase 2 of the tailnet voice integration):
 * - WiFi ownership stays with the room node's own station bring-up
 *   (esp_openclaw_room_node.c); the manager polls the default netif for
 *   an address instead of clawkie's wifi_manager.
 * - Identity continuity: no device_name override → MAC-derived default
 *   (esp32-9b9198 on the round board) → stable VPN IP for the daemon.
 * - Serial discipline: state transitions + 60s heartbeat only. The spike
 *   measured INFO-per-datagram logging collapsing concurrent voice-rate
 *   transport (66-79% loss with standing queue) — microlink's own ml_*
 *   components are pinned to WARN in the example; here the manager adds
 *   no periodic chatter beyond the heartbeat.
 */

#include "tailscale_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/inet.h" /* esp_htonl */

#include "microlink.h"
#include "esp_netif.h"

/* sdkconfig.h defines bool Kconfig symbols only when set — provide the
 * zero fallback so && CONFIG_TS_VOICE_PRIMARY compiles at TS_ENABLE=n. */
#ifndef CONFIG_TS_VOICE_PRIMARY
#define CONFIG_TS_VOICE_PRIMARY 0
#endif

static const char *TAG = "ts_manager";

/* Serial discipline (spike-proven 2026-10-17: per-datagram INFO logging on
 * USB-CDC builds standing queues and starves the RX task). Pin microlink's
 * transport logs to WARN before its tasks start; the manager itself logs
 * transitions + heartbeat only. */
static void ts_quiet_logs(void)
{
    esp_log_level_set("ml_wg_mgr", ESP_LOG_WARN);
    esp_log_level_set("ml_derp", ESP_LOG_WARN);
    esp_log_level_set("ml_net_io", ESP_LOG_WARN);
    esp_log_level_set("ml_coord", ESP_LOG_WARN);
    esp_log_level_set("ml_stun", ESP_LOG_WARN);
    esp_log_level_set("ml_h2", ESP_LOG_WARN);
}

/* Echo socket on the canonical spike port: keeps the daemon's data-plane
 * monitor (tailscaleTransport) alive against voice fw — same UDP lane the
 * spike app used, so the architecture test survives the firmware swap.
 * Microlink routes it through WireGuard automatically. */
#define TS_ECHO_PORT 9000
static microlink_udp_socket_t *s_echo_sock = NULL;

static void ts_echo_rx(microlink_udp_socket_t *sock, uint32_t src_ip,
                       uint16_t src_port, const uint8_t *data, size_t len,
                       void *user_data)
{
    (void)user_data;
    if (len == 0 || len > 1300) return;   /* microlink MTU is 1400 */
    static char reply[1350];
    int n = snprintf(reply, sizeof(reply), "ECHO: %.*s", (int)len, (const char *)data);
    if (n > 0) {
        microlink_udp_send(sock, src_ip, src_port, (const uint8_t *)reply, (size_t)n);
    }
}

static void ts_start_echo_socket(microlink_t *ml)
{
    if (s_echo_sock) return;
    s_echo_sock = microlink_udp_create(ml, TS_ECHO_PORT);
    if (!s_echo_sock) {
        ESP_LOGW(TAG, "[TS] echo socket create failed (daemon data lane stays dark)");
        return;
    }
    microlink_udp_set_rx_callback(s_echo_sock, ts_echo_rx, NULL);
    ESP_LOGI(TAG, "[TS] echo lane up on udp/%d (daemon monitor target)", TS_ECHO_PORT);
}

/* esp-openclaw-node adaptation: the room node owns its WiFi station
 * internally (esp_openclaw_room_node.c registers WIFI_EVENT/IP_EVENT
 * handlers); there is no wifi_manager to poll. The equivalent ready
 * signal is the default sta netif holding a non-zero IPv4 address. */
static bool ts_wifi_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

#define TS_HEARTBEAT_TICKS 60   /* 60s at 1s poll */

static microlink_t *s_ml = NULL;
static volatile bool s_started = false;
static volatile int s_last_state = -1;

#ifndef CONFIG_TS_ENABLE
#define CONFIG_TS_ENABLE 0
#endif

static void ts_state_log(microlink_state_t st)
{
    static const char *names[] = {
        "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING",
        "CONNECTED", "RECONNECTING", "ERROR",
    };
    ESP_LOGI(TAG, "[TS] state -> %s", names[st]);
}

static void ts_manager_task(void *pv)
{
    /* Wait for WiFi — the room node owns the station; ready = default
     * sta netif up with an IPv4 address. */
    while (!ts_wifi_has_ip()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "[TS] WiFi up — initializing microlink");
    ts_quiet_logs();

    microlink_config_t cfg = {0};
    cfg.auth_key = CONFIG_TS_AUTH_KEY;
    /* MAC-derived default name keeps the spike identity (esp32-9b9198 on the
     * round board) so the daemon's registry (100.108.153.58) stays valid. */
    cfg.device_name = microlink_default_device_name();
    cfg.enable_derp = true;
    cfg.enable_stun = true;
    cfg.enable_disco = true;
    cfg.max_peers = 16;
    cfg.priority_peer_ip = microlink_parse_ip(CONFIG_TS_PRIORITY_PEER_IP);
    if (cfg.priority_peer_ip != 0) {
        char ipbuf[16];
        microlink_ip_to_str(cfg.priority_peer_ip, ipbuf);
        ESP_LOGI(TAG, "[TS] priority peer: %s", ipbuf);
    }

    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        ESP_LOGE(TAG, "[TS] microlink_init failed — tailnet lane OFF");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = microlink_start(s_ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[TS] microlink_start failed: %s — tailnet lane OFF",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    s_started = true;

    /* State watcher: transitions + heartbeat + echo-lane lifecycle.
     * The clawkie WG voice lane (udp/9100 protocol swap) is intentionally
     * absent here: the room node's transport is its own WSS client — the
     * tailnet is a connectivity layer, and the gateway URI is a
     * provisioning item. */
    bool echo_up = false;
    bool was_connected = false;
    bool dns_redirected = false;
    uint32_t dhcp_dns_addr = 0; /* pre-redirect MAIN DNS, restored on lane loss */
    int hb = 0;
    for (;;) {
        microlink_state_t st = microlink_get_state(s_ml);
        bool connected_now = (st == ML_STATE_CONNECTED);
        /* FIELD 2026-10-20: the gateway URI is a MagicDNS name
         * (*.ts.net) — the LAN DHCP resolver cannot answer it (resolver
         * answers only inside the tailnet at 100.100.100.100). Once the WG
         * lane is CONNECTED the tailnet carries DNS-UDP to the resolver, so
         * switch the station's DNS server there; restored to DHCP on lane
         * loss. */
        if (connected_now && !dns_redirected) {
            esp_netif_t *netif = esp_netif_get_default_netif();
            if (netif) {
                esp_netif_dns_info_t dns = {0};
                if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                    dhcp_dns_addr = dns.ip.u_addr.ip4.addr; /* snapshot for restore */
                }
                dns.ip.u_addr.ip4.addr = esp_htonl(0x64646464); /* 100.100.100.100 MagicDNS */
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                if (esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                    ESP_LOGI(TAG, "[TS] DNS -> tailnet resolver 100.100.100.100 (MagicDNS names resolvable)");
                    dns_redirected = true;
                } else {
                    ESP_LOGW(TAG, "[TS] set_dns_info failed — ts.net names stay unresolvable");
                }
            }
        } else if (!connected_now && dns_redirected) {
            esp_netif_t *netif = esp_netif_get_default_netif();
            if (netif) {
                esp_netif_dns_info_t dns = {0};
                dns.ip.u_addr.ip4.addr = dhcp_dns_addr;
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
            }
            dns_redirected = false;
            ESP_LOGI(TAG, "[TS] DNS restored to pre-redirect resolver (lane down)");
        }
        if (was_connected && !connected_now) {
            echo_up = false;
            /* FIELD 2026-10-19 05:50 crash-chain fix: CLOSE the echo socket
             * (joined close releases the pcb/port). The old handle-drop
             * orphaned the :9000 pcb — the next CONNECTED then failed
             * udp_bind ERR_USE(-8) and the echo lane stayed dark. */
            if (s_echo_sock) {
                microlink_udp_close(s_echo_sock);
                s_echo_sock = NULL;
            }
        }
        was_connected = connected_now;
        if (!echo_up && microlink_is_connected(s_ml)) {
            ts_start_echo_socket(s_ml);
            echo_up = true;
        }
        if ((int)st != s_last_state) {
            ts_state_log(st);
            s_last_state = (int)st;
            hb = 0;
        } else if (++hb >= TS_HEARTBEAT_TICKS) {
            uint32_t ip = microlink_get_vpn_ip(s_ml);
            if (ip) {
                char ipbuf[16];
                microlink_ip_to_str(ip, ipbuf);
                ESP_LOGI(TAG, "[TS] heartbeatalive %s peers=%d",
                         ipbuf, microlink_get_peer_count(s_ml));
            }
            hb = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t tailscale_manager_start(void)
{
#if !CONFIG_TS_ENABLE
    ESP_LOGD(TAG, "[TS] disabled (CONFIG_TS_ENABLE=n)");
    return ESP_OK;
#else
    if (s_started || s_ml) return ESP_OK;
    if (xTaskCreate(ts_manager_task, "ts_manager", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[TS] task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

bool tailscale_manager_is_connected(void)
{
    return s_ml && s_started && microlink_is_connected(s_ml);
}

uint32_t tailscale_manager_vpn_ip(void)
{
    return s_ml ? microlink_get_vpn_ip(s_ml) : 0;
}

int tailscale_manager_state(void)
{
    return s_ml ? (int)microlink_get_state(s_ml) : -1;
}

microlink_t *tailscale_manager_handle(void)
{
    return s_ml;
}