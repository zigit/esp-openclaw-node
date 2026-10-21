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
#include "esp_heap_caps.h"
#include "room_dns_hook.h"

/* sdkconfig.h defines bool Kconfig symbols only when set — provide the
 * zero fallback so && CONFIG_TS_VOICE_PRIMARY compiles at TS_ENABLE=n. */
#ifndef CONFIG_TS_VOICE_PRIMARY
#define CONFIG_TS_VOICE_PRIMARY 0
#endif

static const char *TAG = "ts_manager";

/* Bring-up heap instrumentation (diag 2026-10-21): the lane bring-up consumed
 * ~31KB internal between "room node ready" (40110) and first connect (~8.6K)
 * without a call running — while dual-AFE residency had just consumed its
 * upfront share too. One-liner at each stage boundary names the consumer;
 * remove after the whale is found. */
static void ts_heap_stage(const char *stage)
{
    ESP_LOGW(TAG, "[TS][heap @%s] int_free=%u int_largest=%u psram_free=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

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

#define TS_GATEWAY_NAME_MAX 96  /* gateway hostname length bound */
static char s_gateway_name[96];
static volatile bool s_gateway_resolved = false;
/* FIELD 2026-10-21 (Fredrik): the gateway name -> IP answer is CACHED IN NVS.
 * The peer table (the previous source) fills with the whole tailnet and
 * saturates ('Peer table full (16 slots)'), and its answer was latched
 * forever — a changed gateway IP was never picked up. Now: NVS cache is
 * the fast path (works before the coord map lands), the peer table is
 * the refresh source, and a connect failure invalidates the answer so
 * the next resolve re-reads the table. */
#define TS_NVS_NS "ts_mgr"
#define TS_NVS_KEY_GW_IP "gw_ip"
static uint32_t s_gateway_ip = 0;
static uint32_t s_hook_ip = 0;      /* what the DNS hook currently holds */
static bool s_cache_loaded = false;  /* NVS cache read done */
static bool s_gateway_stale = false;   /* cached answer known-bad */
static bool s_wait_logged = false;

static uint32_t ts_nvs_load_gateway_ip(void)
{
    nvs_handle_t handle;
    uint32_t ip = 0;
    if (nvs_open(TS_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    if (nvs_get_u32(handle, TS_NVS_KEY_GW_IP, &ip) != ESP_OK) {
        ip = 0;
    }
    nvs_close(handle);
    return ip;
}

static void ts_nvs_store_gateway_ip(uint32_t ip)
{
    nvs_handle_t handle;
    if (nvs_open(TS_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u32(handle, TS_NVS_KEY_GW_IP, ip);
    nvs_commit(handle);
    nvs_close(handle);
}

/* Echo socket on the canonical spike port: keeps the gateway-side monitor
 * alive against voice fw — same UDP lane the spike app used, so the
 * architecture test survives the firmware swap.
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

/* FIELD 2026-10-20 (Fredrik): replace the :5200 keepalive-warm DNS lane with
 * microlink's OWN peer table — the coord map already delivers every tailnet
 * peer's hostname+VPN IP (NVS-cached for boots before the map arrives), and
 * microlink_resolve() looks names up against it with ZERO network I/O. No
 * responder process on VM2, no keepalives, no inbound-flow warmth needed.
 * The gateway name is priority-pinned into the 16-slot peer table, so the
 * entry survives churn. Called from the manager's CONNECTED tick until the
 * hook has the name; also re-checks if the peer table changes. */
static microlink_t *s_ml = NULL;

/* Apply the answer to the lwIP resolver. Always re-applies when the hook
 * does not already carry this IP — the invalidation path empties the hook,
 * and a same-IP answer must still be re-installed (regression 13:28). */
static void ts_dns_hook_apply(uint32_t ip)
{
    if (ip == 0 || ip == s_hook_ip) {
        return;
    }
    room_dns_hook_set(s_gateway_name, ip);
    s_hook_ip = ip;
}

/* Parse the gateway hostname out of the configured URI ("wss://name:443" ->
 * "name"). */
static void ts_parse_gateway_name(void)
{
    const char *g = CONFIG_TS_GATEWAY_DNS_NAME;
    const char *s = strstr(g, "://");
    if (s) g = s + 3;
    if (g[0] != '\0') {
        strlcpy(s_gateway_name, g, sizeof(s_gateway_name));
        char *slash = strchr(s_gateway_name, '/');
        if (slash) *slash = '\0';
    }
}

/* Load the NVS-cached gateway IP once per boot and install it in the
 * resolver. Returns true when an answer was installed. */
static bool ts_install_cached_gateway(void)
{
    if (s_cache_loaded) {
        return s_gateway_ip != 0;
    }
    s_cache_loaded = true;
    uint32_t cached = ts_nvs_load_gateway_ip();
    if (cached == 0) {
        return false;
    }
    s_gateway_ip = cached;
    ts_dns_hook_apply(cached);
    char ipbuf[16];
    microlink_ip_to_str(cached, ipbuf);
    ESP_LOGI(TAG, "[TS] gateway name %s -> %s (NVS cache)", s_gateway_name, ipbuf);
    return true;
}

static void ts_dns_refresh_peer_table(void)
{
    if (strlen(s_gateway_name) == 0 || !s_ml) return;

    /* 1) Peer table: authoritative, and the refresh source. */
    uint32_t ip = microlink_resolve(s_ml, s_gateway_name);
    if (ip != 0) {
        if (ip != s_gateway_ip) {
            bool changed = s_gateway_ip != 0;
            s_gateway_ip = ip;
            ts_nvs_store_gateway_ip(ip);
            char ipbuf[16];
            microlink_ip_to_str(ip, ipbuf);
            ESP_LOGI(TAG, "[TS] gateway name %s -> %s (%s)",
                     s_gateway_name, ipbuf,
                     changed ? "updated" : "resolved via peer table");
        }
        s_gateway_stale = false;
        s_gateway_resolved = true;
        ts_dns_hook_apply(ip);
        return;
    }

    /* 2) NVS cache: the fast path — the answer survives reboots and does
     * not depend on the peer table (which may be full or not yet mapped).
     * Read once per boot; the cache is refreshed whenever the peer table
     * yields a different IP. */
    if (!s_cache_loaded) {
        s_cache_loaded = true;
        uint32_t cached = ts_nvs_load_gateway_ip();
        if (cached != 0) {
            s_gateway_ip = cached;
            char ipbuf[16];
            microlink_ip_to_str(cached, ipbuf);
            ESP_LOGI(TAG, "[TS] gateway name %s -> %s (NVS cache)",
                     s_gateway_name, ipbuf);
        }
    }

    /* 3) Last known answer still resolves the name: a possibly-stale IP
     * beats "name does not resolve" (which fails every connect instantly).
     * The peer table refresh above replaces it when the gateway moves. */
    if (s_gateway_ip != 0) {
        if (s_gateway_stale) {
            char ipbuf[16];
            microlink_ip_to_str(s_gateway_ip, ipbuf);
            ESP_LOGW(TAG, "[TS] gateway name %s -> %s (last known; peer "
                     "table has not confirmed it yet)", s_gateway_name, ipbuf);
            s_gateway_stale = false;
        }
        s_gateway_resolved = true;
        ts_dns_hook_apply(s_gateway_ip);
        return;
    }
    if (!s_wait_logged) {
        ESP_LOGI(TAG, "[TS] gateway name %s not in peer table yet "
                 "(cache or map pending)", s_gateway_name);
        s_wait_logged = true;
    }
}

void tailscale_manager_note_gateway_unreachable(void)
{
    if (strlen(s_gateway_name) == 0) {
        return;
    }
    /* Mark the answer suspect so the next tick re-resolves. The answer
     * itself is KEPT (hook entry + cached IP): clearing it turns a
     * possibly-stale address into "name does not resolve at all", which
     * fails every connect instantly and — with the NVS fallback gated on
     * staleness — could never recover (regression 13:28). */
    s_gateway_stale = true;
    s_gateway_resolved = false;
    s_wait_logged = false;
    ESP_LOGW(TAG, "[TS] gateway %s marked unreachable — re-resolving",
             s_gateway_name);
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

/* Gateway-name resolution (FIELD 2026-10-20, replaced the :5200 lane): see
 * ts_dns_refresh_peer_table() — microlink_resolve() against the coord-map
 * peer table, zero network I/O; the retired lane (room_dns_lane.py on VM2,
 * keepalive-warm flow, parser double-skip bug) is documented in the example's
 * memory/2026-10-20.md.
 * The resolved IP is registered in room_dns_hook so lwIP's netconn hook
 * resolves the gateway URI name with zero real-network DNS. */

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
    ts_heap_stage("pre:init");
    ts_quiet_logs();

    microlink_config_t cfg = {0};
    cfg.auth_key = CONFIG_TS_AUTH_KEY;
    /* MAC-derived default name keeps the spike identity (esp32-9b9198 on the
     * round board) so the daemon's registry (100.108.153.58) stays valid. */
    cfg.device_name = microlink_default_device_name();
    cfg.enable_derp = true;
    cfg.enable_stun = true;
    cfg.enable_disco = true;
    cfg.max_peers = 4;   /* FIELD 2026-10-21 (Fredrik): we only talk to the
                          * gateway. The table is pinned to the priority peer
                          * (the gateway) with LRU eviction for the rest. */
    cfg.priority_peer_ip = microlink_parse_ip(CONFIG_TS_PRIORITY_PEER_IP);
    if (cfg.priority_peer_ip != 0) {
        char ipbuf[16];
        microlink_ip_to_str(cfg.priority_peer_ip, ipbuf);
        ESP_LOGI(TAG, "[TS] priority peer: %s", ipbuf);
    }
    /* Gateway name for the echo-lane resolver: Kconfig, stripped of scheme +
     * port ("wss://name:443" → "name"). Falls back to empty (lane idles). */
    ts_parse_gateway_name();
    /* FIELD 2026-10-21 (Fredrik): install the NVS-cached answer BEFORE the
     * lane exists. The first connect attempts used to start ~10s in and
     * failed for ~15s because the name only became resolvable once the
     * peer table had the gateway (t=25s). With the cache the name resolves
     * immediately — the lane is only needed for the traffic itself. */
    ts_install_cached_gateway();

    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        ESP_LOGE(TAG, "[TS] microlink_init failed — tailnet lane OFF");
        vTaskDelete(NULL);
        return;
    }
    ts_heap_stage("post:init");

    esp_err_t err = microlink_start(s_ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[TS] microlink_start failed: %s — tailnet lane OFF",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    s_started = true;
    ts_heap_stage("post:start");

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
                /* 100.100.100.100 is MagicDNS anycast — reachable on real tailscaled
                 * hosts, but the board's WG netif has no local resolver handler and
                 * no peer announces it. Route DNS instead to VM2 (priority peer,
                 * guaranteed a WG slot) where dns_forwarder.py :5354 forwards to the
                 * local 100.100.100.100. UDP/5354 rides the same WG lane (tailscale's
                 * inbound-UDP rule: VM2's forwarder sends first via its own
                 * MagicDNS probes, so replies flow). */
                dns.ip.u_addr.ip4.addr = lwip_htonl(0x647CFE0F); /* 100.124.254.15 */
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                if (esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                    ESP_LOGI(TAG, "[TS] DNS -> VM2:5354 forwarder (MagicDNS via WG lane)");
                    dns_redirected = true;
                    ts_heap_stage("connected");
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
        /* Keep the gateway-name hook current: resolved from the peer table
         * (no network I/O); re-checked every tick until it lands, and again
         * if the table churned (cheap strcmp against cached peer list). */
        ts_dns_refresh_peer_table();
        if ((int)st != s_last_state) {
            ts_state_log(st);
            s_last_state = (int)st;
            hb = 0;
        } else if (++hb >= TS_HEARTBEAT_TICKS) {
            uint32_t ip = microlink_get_vpn_ip(s_ml);
            if (ip) {
                char ipbuf[16];
                microlink_ip_to_str(ip, ipbuf);
                int np = microlink_get_peer_count(s_ml);
                /* FIELD 2026-10-20: peer-table reality dump (heartbeat rate).
                 * microlink's own peer-state logging is pinned to WARN, so the
                 * only serial-visible window into the WG plane is this line.
                 * Shows which peers hold wg slots + direct paths — the DNS-over-WG
                 * unblock verification reads VM2's entry here. */
                static char peers_desc[480];
                int off = 0;
                for (int i = 0; i < np && off < (int)sizeof(peers_desc) - 24; i++) {
                    microlink_peer_info_t info;
                    if (microlink_get_peer_info(s_ml, i, &info) != ESP_OK) continue;
                    char pb[16];
                    microlink_ip_to_str(info.vpn_ip, pb);
                    off += snprintf(peers_desc + off, sizeof(peers_desc) - off,
                                    "%s[%d]=%s:dp%d", i ? " " : "", i, pb, info.direct_path);
                }
                ESP_LOGI(TAG, "[TS] heartbeatalive %s peers=%d | %s", ipbuf, np, peers_desc);
                /* Diag 2026-10-21: heartbeatalive heap line + 3 lowest
                 * stack-HWM tasks. Catch the observed slow internal drift
                 * (largest 3968->1856 over ~6min, no call running) and give
                 * the coord/wg_mgr stack-shrink work real headroom numbers.
                 * Static buffers only (timer-adjacent task path). */
                ESP_LOGW(TAG, "[TS][heap hb] int_free=%u int_largest=%u "
                         "psram_free=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
                {
                    static TaskStatus_t hb_stat[40];
                    UBaseType_t cnt = uxTaskGetNumberOfTasks();
                    UBaseType_t cap = cnt > 40 ? 40 : cnt;
                    UBaseType_t got = uxTaskGetSystemState(hb_stat, cap, NULL);
                    /* Print the 3 lowest-HWM tasks (repeated selection;
                     * printing only, array order untouched). */
                    bool printed[40] = {false};
                    for (int sel = 0; sel < 3 && got > 0; sel++) {
                        UBaseType_t lo = 0;
                        bool have = false;
                        for (UBaseType_t i = 0; i < got; i++) {
                            if (printed[i]) continue;
                            if (!have || hb_stat[i].usStackHighWaterMark <
                                hb_stat[lo].usStackHighWaterMark) {
                                lo = i; have = true;
                            }
                        }
                        if (!have) break;
                        printed[lo] = true;
                        ESP_LOGW(TAG, "[TS][stack hb] %-16s hwm=%u",
                                 hb_stat[lo].pcTaskName,
                                 (unsigned)hb_stat[lo].usStackHighWaterMark);
                    }
                    /* Named watch list: the guaranteed-internal stacks whose
                     * shrink sizing needs their measured HWM (boot_ready
                     * inventory prints before microlink spawns, so these
                     * never appear there). */
                    static const char *const watch[] = {
                        "ml_coord", "ml_wg_mgr", "ml_net_io", "ml_derp_tx",
                        "websocket", "AUD_SRC",
                    };
                    for (size_t w = 0; w < sizeof(watch)/sizeof(watch[0]); w++) {
                        for (UBaseType_t i = 0; i < got; i++) {
                            if (strncmp(hb_stat[i].pcTaskName, watch[w], 16) == 0) {
                                ESP_LOGW(TAG, "[TS][stack hb] watch %-12s hwm=%u",
                                         hb_stat[i].pcTaskName,
                                         (unsigned)hb_stat[i].usStackHighWaterMark);
                                break;
                            }
                        }
                    }
                }
#endif
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

esp_err_t tailscale_manager_prepare_gateway(void)
{
#if !CONFIG_TS_ENABLE
    return ESP_OK;
#else
    /* FIELD 2026-10-21: the room node primes its node/operator clients at
     * boot and those connect within ~100ms — the tailnet manager task only
     * runs afterwards, so the name was unresolvable for the first attempts
     * (field log: connects at 5.0s, answer installed at 5.2s). Call this
     * before esp_openclaw_room_node_start(). */
    ts_parse_gateway_name();
    if (strlen(s_gateway_name) == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* NVS is initialized by the room node start, which runs AFTER this
     * call — so the cache read would find an uninitialized store and
     * report "not found" (13:46 field log: ESP_ERR_NOT_FOUND). nvs_flash_init
     * is idempotent, so initializing here is safe; the room node's own
     * call later still succeeds. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "[TS] nvs_flash_init for gateway cache failed: %s",
                 esp_err_to_name(nvs_err));
        return nvs_err;
    }
    return ts_install_cached_gateway() ? ESP_OK : ESP_ERR_NOT_FOUND;
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