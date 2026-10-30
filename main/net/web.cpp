#include "web.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "asic/bm13xx.h"
#include "core/board.h"
#include "core/logbuf.h"
#include "core/monitor.h"
#include "core/ota_guard.h"
#include "core/psu.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/tuner.h"
#include "mining/miner.h"
#include "mining/work.h"
#include "net/mempool.h"
#include "net/weather.h"
#include "net/swarm.h"
#include "net/wifi.h"
#include "stratum/pools.h"
#include "ui/layout_store.h"
#include "ui/ui.h"
#include "ui/display.h"
#include "esp_attr.h"
#include "lwip/sockets.h"
#include "core/json_util.h"  // depth-limited parse for untrusted JSON
#include "core/selftest.h"
#include "mining/stats.h"
#include "net/ble_setup.h"
#include "net/eth.h"
#include "net/notify.h"
#include "net/otp.h"
#include "net/price.h"
#include "src/extra/libs/qrcode/qrcodegen.h"

static const char *TAG = "web";

// what this board's build has, for the settings page
#if AF_ASIC_BM1366 || AF_ASIC_BM1368 || AF_ASIC_BM1373
static const bool VR_SPEED_SETTING = false;  // register 0x10 is the nonce space there
#else
static const bool VR_SPEED_SETTING = true;
#endif
#if (AF_BITAXE_FAMILY || AF_HEX_FAMILY) && !CONFIG_AF_BOARD_NAJA
static const bool HAS_BAP = true;
#else
static const bool HAS_BAP = false;
#endif
#if AF_NQ_FAMILY
static const bool HAS_ETH = true;
#else
static const bool HAS_ETH = false;
#endif

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static bool via_setup_ap(httpd_req_t *req);

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

// Reads the full request body (NUL terminated, heap). NULL if empty/too big.
static char *recv_body(httpd_req_t *req, size_t max)
{
    if (req->content_len <= 0 || req->content_len >= max) return nullptr;
    char *b = (char *) malloc(req->content_len + 1);
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, b + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(b);
            return nullptr;
        }
        got += n;
    }
    b[got] = 0;
    return b;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    // revalidate on every load, so a firmware update never leaves an old UI cached
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static const char *phase_name(TunerPhase p)
{
    switch (p) {
    case TunerPhase::Off: return "manual";
    case TunerPhase::WarmUp: return "warmup";
    case TunerPhase::Measuring: return "testing";
    case TunerPhase::StepUp: return "stepping";
    case TunerPhase::Locked: return "locked";
    case TunerPhase::Backoff: return "backoff";
    }
    return "";
}

static const char *status_name(SysStatus s)
{
    switch (s) {
    case SysStatus::Booting: return "booting";
    case SysStatus::NoWifi: return "no wifi";
    case SysStatus::SetupAP: return "setup";
    case SysStatus::Connecting: return "connecting";
    case SysStatus::Mining: return "mining";
    case SysStatus::Fault: return "fault";
    case SysStatus::Overheat: return "overheat";
    case SysStatus::Paused: return "paused";
    }
    return "";
}

// tiny SVG icon so browsers stop asking (and filling the log with 404s)
static esp_err_t favicon_get(httpd_req_t *req)
{
    static const char svg[] = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' rx='7' fill='#070a10'/>"
                              "<path d='M8 24L16 6l8 18h-5l-3-7-3 7z' fill='#00e5ff'/><circle cx='24' cy='9' r='3' fill='#ff2bd6'/></svg>";
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, svg, sizeof(svg) - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    const MinerState &s = g_state;
    wifi_ap_record_t ap;
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", status_name(s.status));
    cJSON_AddStringToObject(r, "fault", s.fault);
    cJSON_AddNumberToObject(r, "hashrate", s.hashrate_ghs);
    cJSON_AddNumberToObject(r, "expected", s.expected_ghs);
    cJSON_AddNumberToObject(r, "errorPct", s.error_pct);
    cJSON_AddNumberToObject(r, "chipTemp", s.chip_temp_max);
    cJSON_AddNumberToObject(r, "vrTemp", s.vr_temp);
    cJSON_AddBoolToObject(r, "vrSensor", AF_HAS_VR_SENSOR);
    cJSON_AddBoolToObject(r, "tempOk", s.temp_ok && s.vr_temp_ok);
#if CONFIG_AF_BOARD_NERDQAXE
    cJSON_AddStringToObject(r, "board", s.board_rev7 ? "rev7 (TPS546)" : "rev5.1/6 (TPS53647)");
#else
    cJSON_AddStringToObject(r, "board", AF_BOARD_NAME);
#endif
    cJSON_AddStringToObject(r, "asicModel", asic::MODEL);
    cJSON_AddNumberToObject(r, "asicCount", AF_NUM_ASICS);
    cJSON_AddNumberToObject(r, "smallCores", asic::SMALL_CORES);
    cJSON_AddNumberToObject(r, "power", s.pin);
    cJSON_AddNumberToObject(r, "vin", s.vin);
    cJSON_AddNumberToObject(r, "vout", s.vout);
    cJSON_AddNumberToObject(r, "iout", s.iout);
    cJSON_AddNumberToObject(r, "freq", s.freq_mhz);
    cJSON_AddNumberToObject(r, "coreMv", s.core_mv);
    cJSON_AddNumberToObject(r, "fanPct", s.fan_pct);
    cJSON_AddNumberToObject(r, "fanRpm", s.fan_rpm[0]);
    cJSON_AddNumberToObject(r, "fan2Rpm", s.fan_rpm[1]);
    {
        // same shape as the AxeOS / NerdQAxe "fans" list, so swarm tools can read it
        const Settings &c = settings::get();
        cJSON *fans = cJSON_AddArrayToObject(r, "fans");
        int count = 0;
        for (int i = 0; i < 2; i++) {
            cJSON *f = cJSON_CreateObject();
            char label[12];
            snprintf(label, sizeof(label), "Fan %d", i + 1);
            cJSON_AddStringToObject(f, "label", label);
            cJSON_AddBoolToObject(f, "connected", s.fan_present[i]);
            cJSON_AddNumberToObject(f, "rpm", s.fan_rpm[i]);
            cJSON_AddNumberToObject(f, "speedPerc", s.fan_duty[i]);
            cJSON_AddNumberToObject(f, "mode", c.fan_mode[i]);
            cJSON_AddNumberToObject(f, "manualSpeed", c.fan_manual[i]);
            cJSON_AddItemToArray(fans, f);
            count += s.fan_present[i];
        }
        cJSON_AddNumberToObject(r, "fanCount", count);
    }
    cJSON_AddNumberToObject(r, "uptime", s.uptime_s);
    cJSON_AddStringToObject(r, "resetReason", s.reset_reason);
    cJSON_AddStringToObject(r, "restartedBy", web::previous_restart());
    cJSON_AddStringToObject(r, "lastEmergency", monitor::last_emergency());
    cJSON_AddNumberToObject(r, "emergencies", monitor::emergency_count());
#if AF_HAS_LCD
    cJSON_AddStringToObject(r, "screen", display::small() ? "tdisplay" : "big");
#else
    cJSON_AddStringToObject(r, "screen", "oled");
#endif
    cJSON_AddNumberToObject(r, "rssi", rssi);
    cJSON_AddStringToObject(r, "ip", s.ip);
    cJSON_AddStringToObject(r, "ssid", s.ssid);
    cJSON_AddStringToObject(r, "hostname", s.hostname);
    char worker[33];
    settings::own_worker(worker, sizeof(worker));
    cJSON_AddStringToObject(r, "worker", worker);
    cJSON_AddStringToObject(r, "version", esp_app_get_description()->version);
    cJSON_AddBoolToObject(r, "otaPending", ota_guard::pending());
    {
        const psu::State &p = psu::state();
        cJSON *po = cJSON_AddObjectToObject(r, "psu");
        cJSON_AddStringToObject(po, "level", psu::level_name(p.level));
        cJSON_AddNumberToObject(po, "nominal", p.nominal);
        cJSON_AddNumberToObject(po, "idle", p.idle_v);
        cJSON_AddNumberToObject(po, "load", p.load_v);
        cJSON_AddNumberToObject(po, "min", p.min_v);
        cJSON_AddNumberToObject(po, "watts", p.load_w);
        cJSON_AddNumberToObject(po, "sag", p.sag_pct);
        cJSON_AddNumberToObject(po, "mohm", p.ohms * 1000);
    }
    cJSON_AddItemToObject(r, "price", price::json());
    cJSON_AddNumberToObject(r, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "freeInternal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "freeDma", heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(r, "chipsDetected", s.chips_detected);
    cJSON *net = cJSON_AddObjectToObject(r, "eth");
    cJSON_AddBoolToObject(net, "up", eth::up());
    cJSON_AddStringToObject(net, "ip", eth::ip());
    cJSON_AddStringToObject(r, "bleSetup", ble_setup::name());

    cJSON *pool = cJSON_AddObjectToObject(r, "pool");
    cJSON_AddBoolToObject(pool, "connected", s.pool_connected);
    cJSON_AddStringToObject(pool, "host", s.tmpl.valid ? s.pools[s.tmpl.pool < 0 ? 0 : s.tmpl.pool].host : "");
    cJSON_AddNumberToObject(pool, "diff", s.pool_diff);
    cJSON_AddNumberToObject(pool, "accepted", s.shares_accepted);
    cJSON_AddNumberToObject(pool, "rejected", s.shares_rejected);
    cJSON_AddNumberToObject(pool, "bestDiff", s.best_diff);
    cJSON_AddNumberToObject(pool, "sessionBest", s.session_best);
    cJSON_AddNumberToObject(pool, "height", s.block_height);
    cJSON_AddNumberToObject(pool, "networkDiff", work::nbits_to_difficulty(s.network_nbits));
    cJSON_AddNumberToObject(pool, "blocks", s.blocks_found);
    cJSON_AddStringToObject(pool, "mode", settings::get().pool_mode == PoolMode::Failover ? "failover" : "split");

    cJSON *pl = cJSON_AddArrayToObject(r, "pools");
    for (int i = 0; i < AF_POOLS; i++) {
        const PoolState &ps = s.pools[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", ps.enabled);
        cJSON_AddBoolToObject(o, "connected", ps.connected);
        cJSON_AddNumberToObject(o, "proto", ps.proto);
        cJSON_AddStringToObject(o, "host", ps.host);
        cJSON_AddNumberToObject(o, "diff", ps.diff);
        cJSON_AddNumberToObject(o, "accepted", ps.accepted);
        cJSON_AddNumberToObject(o, "rejected", ps.rejected);
        cJSON_AddNumberToObject(o, "best", ps.best);
        cJSON_AddNumberToObject(o, "ghs", ps.ghs);
        cJSON_AddNumberToObject(o, "workPct", ps.work_pct);
        cJSON_AddNumberToObject(o, "jobs", ps.jobs);
        cJSON_AddNumberToObject(o, "pingMs", ps.ping_ms);
        cJSON_AddNumberToObject(o, "payout", ps.payout);
        cJSON_AddNumberToObject(o, "payoutPct", ps.payout_pct);
        cJSON_AddBoolToObject(o, "payoutBlocked", ps.payout_blocked);
        cJSON_AddStringToObject(o, "status", ps.status);
        cJSON_AddItemToArray(pl, o);
    }

    const BlockTemplate &bt = s.tmpl;
    cJSON *tm = cJSON_AddObjectToObject(r, "template");
    cJSON_AddBoolToObject(tm, "valid", bt.valid);
    cJSON_AddNumberToObject(tm, "pool", bt.pool);
    cJSON_AddNumberToObject(tm, "height", bt.height);
    cJSON_AddStringToObject(tm, "prevHash", bt.prev_hash);
    cJSON_AddNumberToObject(tm, "reward", bt.reward_btc);
    cJSON_AddNumberToObject(tm, "branches", bt.branches);
    cJSON_AddStringToObject(tm, "tag", bt.tag);
    cJSON_AddNumberToObject(tm, "ntime", bt.ntime);
    // solo payout check: 0 unknown, 1 username isn't an address, 2 pays you, 3 doesn't
    cJSON_AddNumberToObject(tm, "payout", (int) bt.payout);
    cJSON_AddStringToObject(tm, "payoutAddr", bt.payout_addr);
    cJSON_AddNumberToObject(tm, "payoutSats", (double) bt.payout_sats);
    cJSON_AddNumberToObject(tm, "totalSats", (double) bt.total_sats);
    cJSON *outs = cJSON_AddArrayToObject(tm, "outputs");
    for (int i = 0; i < bt.n_outputs; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr", bt.outputs[i].addr);
        cJSON_AddNumberToObject(o, "sats", (double) bt.outputs[i].sats);
        cJSON_AddBoolToObject(o, "yours", bt.outputs[i].yours);
        cJSON_AddItemToArray(outs, o);
    }
    cJSON_AddStringToObject(r, "lastHash", s.last_hash);
    cJSON_AddNumberToObject(r, "lastHashDiff", s.last_hash_diff);
    cJSON_AddNumberToObject(r, "hashesChecked", s.hashes_checked);
    cJSON_AddNumberToObject(r, "dupResults", s.dup_results);

    const MempoolState &m = s.mempool;
    cJSON *mp = cJSON_AddObjectToObject(r, "mempool");
    cJSON_AddBoolToObject(mp, "valid", m.valid);
    cJSON_AddStringToObject(mp, "error", m.error);
    cJSON_AddNumberToObject(mp, "feeFast", m.fee_fast);
    cJSON_AddNumberToObject(mp, "feeHour", m.fee_hour);
    cJSON_AddNumberToObject(mp, "feeEco", m.fee_eco);
    cJSON *mined = cJSON_AddArrayToObject(mp, "mined");
    for (const MempoolBlock &b : m.mined) {
        if (!b.height) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "height", b.height);
        cJSON_AddNumberToObject(o, "txs", b.txs);
        cJSON_AddNumberToObject(o, "size", b.size_mb);
        cJSON_AddNumberToObject(o, "time", b.time);
        cJSON_AddNumberToObject(o, "fee", b.median_fee);
        cJSON_AddNumberToObject(o, "fees", b.fees_btc);
        cJSON_AddStringToObject(o, "pool", b.pool);
        cJSON_AddItemToArray(mined, o);
    }
    cJSON *nx = cJSON_AddArrayToObject(mp, "next");
    for (const MempoolNext &n : m.next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "txs", n.txs);
        cJSON_AddNumberToObject(o, "vsize", n.vsize_mb);
        cJSON_AddNumberToObject(o, "fee", n.median_fee);
        cJSON_AddNumberToObject(o, "min", n.min_fee);
        cJSON_AddNumberToObject(o, "max", n.max_fee);
        cJSON_AddItemToArray(nx, o);
    }

    const TunerState &t = s.tuner;
    cJSON *tu = cJSON_AddObjectToObject(r, "tuner");
    cJSON_AddStringToObject(tu, "phase", phase_name(t.phase));
    cJSON_AddStringToObject(tu, "note", t.note);
    cJSON_AddNumberToObject(tu, "progress", t.progress);
    cJSON_AddNumberToObject(tu, "steps", t.steps_done);
    cJSON_AddNumberToObject(tu, "bestFreq", t.best_freq);
    cJSON_AddNumberToObject(tu, "bestMv", t.best_mv);
    cJSON_AddNumberToObject(tu, "bestGhs", t.best_ghs);
    cJSON_AddNumberToObject(tu, "trialFreq", t.trial_freq);
    cJSON_AddNumberToObject(tu, "trialMv", t.trial_mv);
    cJSON_AddNumberToObject(tu, "goal", settings::get().tune_goal);

    cJSON *chips = cJSON_AddArrayToObject(r, "chips");
    for (int i = 0; i < AF_NUM_ASICS; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "ghs", s.chip_ghs[i]);
        cJSON_AddNumberToObject(c, "errGhs", s.chip_err_ghs[i]);
        cJSON_AddNumberToObject(c, "temp", s.chip_temp[i]);
        cJSON_AddBoolToObject(c, "ownSensor", s.chip_temp_own[i]);
        cJSON_AddNumberToObject(c, "mhz", s.freq_mhz + asic::chip_offset(i));
        cJSON_AddItemToArray(chips, c);
    }

    const WeatherState &w = s.weather;
    cJSON *wx = cJSON_AddObjectToObject(r, "weather");
    cJSON_AddBoolToObject(wx, "valid", w.valid);
    cJSON_AddStringToObject(wx, "city", w.city);
    cJSON_AddNumberToObject(wx, "temp", w.temp);
    cJSON_AddNumberToObject(wx, "code", w.code);
    cJSON_AddStringToObject(wx, "text", weather::describe(w.code));
    cJSON_AddStringToObject(wx, "error", w.error);
    return send_json(req, r);
}

static esp_err_t history_get(httpd_req_t *req)
{
    // Same shape as /api/stats, so the dashboard draws both ranges with one function:
    // {"step":30,"hashrate":[...],"temp":[...],"power":[...]}, oldest first.
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "step", 30);
    cJSON *hr = cJSON_AddArrayToObject(r, "hashrate");
    cJSON *t = cJSON_AddArrayToObject(r, "temp");
    cJSON *p = cJSON_AddArrayToObject(r, "power");
    for (int i = 0; i < AF_HISTORY_LEN; i++) {
        int k = (g_state.history_head + i) % AF_HISTORY_LEN;
        cJSON_AddItemToArray(hr, cJSON_CreateNumber((int) g_state.history[k]));
        cJSON_AddItemToArray(t, cJSON_CreateNumber((int) (g_state.history_temp[k] * 10 + 0.5f) / 10.0));
        cJSON_AddItemToArray(p, cJSON_CreateNumber((int) (g_state.history_power[k] * 10 + 0.5f) / 10.0));
    }
    return send_json(req, r);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    const Settings &c = settings::get();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "ssid", c.wifi_ssid);
    cJSON_AddStringToObject(r, "hostname", c.hostname);
    cJSON_AddStringToObject(r, "pages", c.pages);
    cJSON_AddNumberToObject(r, "disptype", c.display_type);
    cJSON_AddBoolToObject(r, "mempool", c.mempool_on);
    cJSON_AddStringToObject(r, "mpurl", c.mempool_url);
    cJSON_AddBoolToObject(r, "atune", c.autotune);
    cJSON_AddNumberToObject(r, "freq", c.freq_mhz);
    cJSON_AddNumberToObject(r, "mv", c.core_mv);
    cJSON_AddNumberToObject(r, "tgoal", c.tune_goal);
    cJSON_AddNumberToObject(r, "pcap", c.power_cap_w);
    cJSON_AddBoolToObject(r, "chiptune", c.chip_tune);
    cJSON_AddBoolToObject(r, "chipClock", asic::PER_CHIP_CLOCK);
    cJSON_AddBoolToObject(r, "otaguard", c.ota_guard);
    cJSON_AddBoolToObject(r, "priceon", c.price_on);
    cJSON_AddNumberToObject(r, "pricesrc", c.price_src);
    cJSON_AddStringToObject(r, "pricezone", c.price_zone);
    cJSON_AddBoolToObject(r, "priceurlSet", c.price_url[0] != 0);  // may hold a token: never sent back
    cJSON_AddStringToObject(r, "pricepath", c.price_path);
    cJSON_AddNumberToObject(r, "pricemax", c.price_max);
    cJSON_AddNumberToObject(r, "fantgt", c.fan_target_c);
    cJSON_AddNumberToObject(r, "fanmin", c.fan_min_pct);
    cJSON_AddNumberToObject(r, "fmode0", c.fan_mode[0]);
    cJSON_AddNumberToObject(r, "fmode1", c.fan_mode[1]);
    cJSON_AddNumberToObject(r, "fman0", c.fan_manual[0]);
    cJSON_AddNumberToObject(r, "fman1", c.fan_manual[1]);
    cJSON_AddNumberToObject(r, "theme", c.theme);
    cJSON_AddNumberToObject(r, "bright", c.brightness);
    cJSON_AddNumberToObject(r, "scroff", c.screen_off_min);
    cJSON_AddNumberToObject(r, "defpage", c.default_page);
    cJSON_AddNumberToObject(r, "rotate", c.page_rotate_s);
    cJSON_AddBoolToObject(r, "rot180", c.rotate_180);
    cJSON_AddStringToObject(r, "city", c.city);
    cJSON_AddBoolToObject(r, "fahr", c.fahrenheit);
    cJSON_AddBoolToObject(r, "h24", c.clock_24h);
    cJSON_AddNumberToObject(r, "paymode", c.payout_mode);
    cJSON_AddNumberToObject(r, "payfee", c.payout_max_fee);
    cJSON_AddNumberToObject(r, "jobms", c.job_ms);
    cJSON_AddNumberToObject(r, "jobmsDefault", AF_JOB_INTERVAL_MS);
    cJSON_AddNumberToObject(r, "vrhz", c.vr_hz);
    cJSON_AddBoolToObject(r, "vrSupported", VR_SPEED_SETTING);
    // the webhook URL and bot token are secrets: say whether they're set, never send them
    cJSON_AddBoolToObject(r, "discordSet", c.discord_url[0] != 0);
    cJSON_AddBoolToObject(r, "tgSet", c.tg_token[0] != 0);
    cJSON_AddStringToObject(r, "tgchat", c.tg_chat);
    cJSON_AddNumberToObject(r, "notify", c.notify_events);
    cJSON_AddBoolToObject(r, "bap", c.bap_on);
    cJSON_AddBoolToObject(r, "hasBap", HAS_BAP);
    cJSON_AddBoolToObject(r, "blesetup", c.ble_setup);
    cJSON_AddBoolToObject(r, "hasEth", HAS_ETH);
    cJSON *lim = cJSON_AddObjectToObject(r, "limits");
    cJSON_AddNumberToObject(lim, "maxFreq", board::limits().max_freq_mhz);
    cJSON_AddNumberToObject(lim, "minMv", CONFIG_AF_MIN_CORE_MV);
    cJSON_AddNumberToObject(lim, "maxMv", board::max_mv());
    cJSON_AddNumberToObject(lim, "chipCeil", CONFIG_AF_MAX_CHIP_TEMP);
    cJSON_AddNumberToObject(lim, "chipCrit", CONFIG_AF_CRIT_CHIP_TEMP);
    cJSON_AddNumberToObject(lim, "vrCeil", board::limits().vr_ceil_c);
    cJSON_AddNumberToObject(lim, "vrCrit", board::limits().vr_crit_c);
    cJSON_AddNumberToObject(lim, "maxPower", board::limits().max_power_w);
    return send_json(req, r);
}

// "http(s)://host[...]": the mempool client appends "/api/..." to this.
static bool valid_base_url(const char *u)
{
    const char *h = !strncmp(u, "https://", 8) ? u + 8 : !strncmp(u, "http://", 7) ? u + 7 : nullptr;
    if (!h || !*h || *h == '/' || *h == ':') return false;
    for (const char *p = u; *p; p++)
        if (*p <= ' ' || *p == '"' || *p == '<' || *p == '>' || *p == '\\') return false;
    return true;
}

static bool copy_str(cJSON *root, const char *key, char *dst, size_t len)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(v)) return false;
    bool changed = strncmp(dst, v->valuestring, len) != 0;
    strncpy(dst, v->valuestring, len - 1);
    dst[len - 1] = 0;
    return changed;
}

static int get_int(cJSON *root, const char *key, int def, int lo, int hi)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    int r = def;
    if (cJSON_IsNumber(v)) r = v->valueint;
    else if (cJSON_IsString(v) && v->valuestring[0]) r = atoi(v->valuestring);
    return r < lo ? lo : r > hi ? hi : r;
}

static bool get_bool(cJSON *root, const char *key, bool def)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

// Survives esp_restart() (not a power cut), so the next boot can say who asked.
#define RESTART_NOTE_MAGIC 0x52534e54
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    char why[80];
} s_restart_note;

// The client's address, also when it arrives as an IPv4-mapped IPv6 address.
static void peer_ip(httpd_req_t *req, char *out, size_t len)
{
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    snprintf(out, len, "?");
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *) &a, &al) != 0) return;
    if (a.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *) &a)->sin_addr, out, len);
    } else if (a.ss_family == AF_INET6) {
        const uint8_t *b = ((struct sockaddr_in6 *) &a)->sin6_addr.s6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, mapped, 12) == 0) snprintf(out, len, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        else inet_ntop(AF_INET6, b, out, len);
    }
}

static void reboot_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    board::shutdown();
    esp_restart();
}

static void schedule_reboot(httpd_req_t *req)
{
    char ip[48];
    peer_ip(req, ip, sizeof(ip));
    snprintf(s_restart_note.why, sizeof(s_restart_note.why), "%s %s from %s",
             http_method_str((enum http_method) req->method), req->uri, ip);
    s_restart_note.magic = RESTART_NOTE_MAGIC;
    ESP_LOGW(TAG, "restart requested: %s", s_restart_note.why);
    xTaskCreate(reboot_task, "reboot", 4096, NULL, 5, NULL);  // shutdown + logging needs >2 kB
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char body[1536];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    body[len] = 0;
    cJSON *j = json_parse(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");

    if (via_setup_ap(req)) {
        // from the setup network: Wi-Fi and hostname only, whatever else was sent
        static const char *const KEEP[] = {"ssid", "wpass", "hostname"};
        cJSON *it = j->child;
        while (it) {
            cJSON *next = it->next;
            bool keep = false;
            for (const char *k : KEEP) keep |= it->string && strcmp(it->string, k) == 0;
            if (!keep) cJSON_Delete(cJSON_DetachItemViaPointer(j, it));
            it = next;
        }
    }

    Settings &c = settings::get();
    bool reboot = false;
    // network / pool: applied after a reboot
    reboot |= copy_str(j, "ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
    {
        // empty means automatic; DHCP only learns a new name at boot
        char old_host[sizeof(c.hostname)];
        strcpy(old_host, c.hostname);
        if (copy_str(j, "hostname", c.hostname, sizeof(c.hostname))) {
            if (c.hostname[0] && !settings::valid_hostname(c.hostname)) {
                ESP_LOGW(TAG, "hostname '%s' ignored: use letters, digits and hyphens", c.hostname);
                strcpy(c.hostname, old_host);
            } else {
                reboot = true;
            }
        }
    }
    cJSON *wp = cJSON_GetObjectItem(j, "wpass");
    if (cJSON_IsString(wp) && wp->valuestring[0]) reboot |= copy_str(j, "wpass", c.wifi_pass, sizeof(c.wifi_pass));

    // tuning: picked up live by the tuner
    c.autotune = get_bool(j, "atune", c.autotune);
    c.freq_mhz = board::clamp_freq(get_int(j, "freq", c.freq_mhz, 0, 2000));
    c.core_mv = board::clamp_mv(get_int(j, "mv", c.core_mv, 0, 2000));
    // a new goal (or cap, or per-chip setting) means a new search
    int goal = get_int(j, "tgoal", c.tune_goal, 0, 2);
    int cap = get_int(j, "pcap", c.power_cap_w, 0, 2000);
    bool chip_tune = get_bool(j, "chiptune", c.chip_tune);
    bool retune = goal != c.tune_goal || (goal == 1 && cap != c.power_cap_w) || chip_tune != c.chip_tune;
    c.tune_goal = (uint8_t) goal;
    c.power_cap_w = (uint16_t) cap;
    c.chip_tune = chip_tune;
    c.fan_target_c = (uint8_t) get_int(j, "fantgt", c.fan_target_c, 40, 70);
    c.fan_min_pct = (uint8_t) get_int(j, "fanmin", c.fan_min_pct, 10, 100);
    c.fan_mode[0] = (uint8_t) get_int(j, "fmode0", c.fan_mode[0], 0, 1);
    c.fan_mode[1] = (uint8_t) get_int(j, "fmode1", c.fan_mode[1], 0, 1);
    c.fan_manual[0] = (uint8_t) get_int(j, "fman0", c.fan_manual[0], 20, 100);
    c.fan_manual[1] = (uint8_t) get_int(j, "fman1", c.fan_manual[1], 20, 100);
    uint8_t theme = (uint8_t) get_int(j, "theme", c.theme, 0, 2);
    if (theme != c.theme) {
        c.theme = theme;
        display::rebuild_ui();  // the screen UI is built with the theme's palette
    }

    // display: applied immediately
    c.brightness = (uint8_t) get_int(j, "bright", c.brightness, 5, 100);
    c.screen_off_min = (uint8_t) get_int(j, "scroff", c.screen_off_min, 0, 240);
    c.default_page = (uint8_t) get_int(j, "defpage", c.default_page, 0, 4);
    c.page_rotate_s = (uint8_t) get_int(j, "rotate", c.page_rotate_s, 0, 255);
    bool rot = get_bool(j, "rot180", c.rotate_180);
    if (rot != c.rotate_180) display::set_rotation(rot);
    c.rotate_180 = rot;
    display::set_brightness(c.brightness);

    // clock & weather
    bool wx_changed = copy_str(j, "city", c.city, sizeof(c.city));
    if (wx_changed) c.lat = c.lon = 0;  // re-geocode
    bool fahr = get_bool(j, "fahr", c.fahrenheit);
    wx_changed |= fahr != c.fahrenheit;
    c.fahrenheit = fahr;
    c.clock_24h = get_bool(j, "h24", c.clock_24h);
    if (copy_str(j, "pages", c.pages, sizeof(c.pages))) ui::reload_pages();
    int dt = get_int(j, "disptype", c.display_type, 0, 1);
    if (dt != c.display_type) reboot = true;  // the panel driver is chosen at boot
    c.display_type = (uint8_t) dt;
    bool mp_on = get_bool(j, "mempool", c.mempool_on);
    char old_mp[sizeof(c.mempool_url)];
    strcpy(old_mp, c.mempool_url);
    bool mp_changed = copy_str(j, "mpurl", c.mempool_url, sizeof(c.mempool_url)) || mp_on != c.mempool_on;
    if (!valid_base_url(c.mempool_url)) {
        ESP_LOGW(TAG, "mempool URL '%s' ignored: needs http:// or https:// and a host", c.mempool_url);
        strcpy(c.mempool_url, old_mp);
    }
    c.mempool_on = mp_on;
    // strip a trailing slash so "/api/..." can be appended
    size_t ml = strlen(c.mempool_url);
    if (ml && c.mempool_url[ml - 1] == '/') c.mempool_url[ml - 1] = 0;
    // solo payout check and chip timing: picked up live
    c.payout_mode = (uint8_t) get_int(j, "paymode", c.payout_mode, 0, 1);
    c.payout_max_fee = (uint8_t) get_int(j, "payfee", c.payout_max_fee, 0, 100);
    int jm = get_int(j, "jobms", c.job_ms, 0, 5000);
    c.job_ms = (uint16_t) (jm && jm < 50 ? 50 : jm);
    int vr = get_int(j, "vrhz", (int) c.vr_hz, 0, 100000);
    if (vr && vr < 5000) vr = 5000;
    if ((uint32_t) vr != c.vr_hz) {
        c.vr_hz = (uint32_t) vr;
        asic::set_vr_freq(c.vr_hz, board::is_powered());
    }
    // notifications: an empty field keeps what is saved, "notifyClear" removes all
    if (get_bool(j, "notifyClear", false)) c.discord_url[0] = c.tg_token[0] = c.tg_chat[0] = 0;
    cJSON *dv = cJSON_GetObjectItem(j, "discord");
    if (cJSON_IsString(dv) && dv->valuestring[0]) {
        if (notify::valid_discord(dv->valuestring) && strlen(dv->valuestring) < sizeof(c.discord_url))
            snprintf(c.discord_url, sizeof(c.discord_url), "%s", dv->valuestring);
        else ESP_LOGW(TAG, "Discord webhook ignored: it has to be a https://discord.com/api/webhooks/ address");
    }
    cJSON *tv = cJSON_GetObjectItem(j, "tgtoken");
    if (cJSON_IsString(tv) && tv->valuestring[0]) {
        if (notify::valid_tg_token(tv->valuestring)) snprintf(c.tg_token, sizeof(c.tg_token), "%s", tv->valuestring);
        else ESP_LOGW(TAG, "Telegram bot token ignored: it looks like 123456789:AA...");
    }
    cJSON *cv = cJSON_GetObjectItem(j, "tgchat");
    if (cJSON_IsString(cv)) {
        // a chat id (-100123...) or @channelname
        bool ok = strlen(cv->valuestring) < sizeof(c.tg_chat);
        for (const char *p = cv->valuestring; *p && ok; p++)
            ok = (*p >= '0' && *p <= '9') || *p == '-' || *p == '@' || *p == '_' || (*p >= 'a' && *p <= 'z') ||
                 (*p >= 'A' && *p <= 'Z');
        if (ok) snprintf(c.tg_chat, sizeof(c.tg_chat), "%s", cv->valuestring);
    }
    c.notify_events = (uint8_t) get_int(j, "notify", c.notify_events, 0, 0x1f);
    bool bap_on = get_bool(j, "bap", c.bap_on);
    if (bap_on != c.bap_on) reboot = true;  // the port is opened at boot
    c.bap_on = bap_on;
    c.ble_setup = get_bool(j, "blesetup", c.ble_setup);
    c.ota_guard = get_bool(j, "otaguard", c.ota_guard);

    // electricity price
    c.price_on = get_bool(j, "priceon", c.price_on);
    c.price_src = (uint8_t) get_int(j, "pricesrc", c.price_src, 0, 1);
    char zone[sizeof(c.price_zone)];
    snprintf(zone, sizeof(zone), "%s", c.price_zone);
    if (copy_str(j, "pricezone", zone, sizeof(zone))) {
        for (char *p = zone; *p; p++) *p = (char) toupper((unsigned char) *p);
        snprintf(c.price_zone, sizeof(c.price_zone), "%s", zone);
    }
    char purl[sizeof(c.price_url)];
    snprintf(purl, sizeof(purl), "%s", c.price_url);
    if (copy_str(j, "priceurl", purl, sizeof(purl)) && purl[0]) {  // empty keeps the saved one
        if (valid_base_url(purl)) snprintf(c.price_url, sizeof(c.price_url), "%s", purl);
        else ESP_LOGW(TAG, "price URL ignored: needs http:// or https:// and a host");
    }
    copy_str(j, "pricepath", c.price_path, sizeof(c.price_path));
    cJSON *pm = cJSON_GetObjectItem(j, "pricemax");
    if (cJSON_IsNumber(pm) && pm->valuedouble > -1000 && pm->valuedouble < 100000) c.price_max = (float) pm->valuedouble;

    cJSON_Delete(j);
    settings::save();
    price::refresh_now();
    if (retune && c.autotune) tuner::restart();
    if (wx_changed) weather::refresh_now();
    if (mp_changed) mempool::refresh_now();

    if (reboot) {
        httpd_resp_sendstr(req, "Saved. Rebooting to apply...");
        schedule_reboot(req);
    } else {
        httpd_resp_sendstr(req, "Saved.");
    }
    return ESP_OK;
}

static esp_err_t retune_post(httpd_req_t *req)
{
    settings::get().autotune = true;
    settings::save();
    tuner::restart();
    return httpd_resp_sendstr(req, "Auto-tune restarted. A full search takes 1-2 hours.");
}

static esp_err_t ota_keep_post(httpd_req_t *req)
{
    ota_guard::keep();
    return httpd_resp_sendstr(req, "Keeping this firmware.");
}

static esp_err_t restart_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "Rebooting...");
    schedule_reboot(req);
    return ESP_OK;
}

// This image's board tag, kept in the binary so the next upload can check it.
// External linkage and a real reference (logged in web::start) so neither the
// compiler nor --gc-sections can drop it.
extern const char af_board_tag[];
const char af_board_tag[] = AF_BOARD_TAG;

// Looks for "AXEFORGE-BOARD:<id>;" in `data`. Returns the id length (copied to
// `id`) or 0. The bare prefix used by this search sits in rodata too, followed
// by a NUL, so only a complete tag counts.
static int find_board_tag(const char *data, int len, char *id, int id_cap)
{
    const int plen = sizeof(AF_BOARD_TAG_PREFIX) - 1;
    for (int i = 0; i + plen < len; i++) {
        if (data[i] != 'A' || memcmp(data + i, AF_BOARD_TAG_PREFIX, plen) != 0) continue;
        int k = 0;
        const char *p = data + i + plen;
        while (i + plen + k < len && k < id_cap - 1 &&
               ((p[k] >= 'a' && p[k] <= 'z') || (p[k] >= '0' && p[k] <= '9')))
            k++;
        if (k >= 2 && i + plen + k < len && p[k] == ';') {
            memcpy(id, p, k);
            id[k] = 0;
            return k;
        }
    }
    return 0;
}

// Raw .bin upload (the app image, not the factory image).
static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    if (req->content_len < 0x10000 || req->content_len > part->size)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file size looks wrong");
    ESP_LOGW(TAG, "OTA upload started (%d bytes)", req->content_len);

    esp_ota_handle_t h;
    // erase sector by sector as data arrives: a one-shot 4 MB erase stalls the CPU
    // for seconds, long enough for the sensor reads to fail and trip the cut-off
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");

    // mining keeps running; only the HTTP task is busy while writing.
    // The last bytes of each chunk are kept in front of the next one, so a
    // board tag split across two chunks is still found.
    const int CHUNK = 4096, KEEP = 48;
    char *buf = (char *) malloc(CHUNK + KEEP);
    int remaining = req->content_len, carry = 0;
    bool ok = true, first = true, wrong_board = false;
    char other[24] = "";
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf + carry, remaining < CHUNK ? remaining : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        if (first) {
            first = false;
            if ((uint8_t) buf[0] != 0xE9) {  // ESP image magic
                ok = false;
                break;
            }
        }
        if (!other[0] && find_board_tag(buf, carry + n, other, sizeof(other)) && strcmp(other, AF_BOARD_ID) != 0) {
            wrong_board = true;
            ok = false;
            break;
        }
        if (esp_ota_write(h, buf + carry, n) != ESP_OK) {
            ok = false;
            break;
        }
        remaining -= n;
        int total = carry + n;
        carry = total < KEEP ? total : KEEP;
        memmove(buf, buf + total - carry, carry);
    }
    free(buf);
    if (wrong_board) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "OTA refused: image is for '%s', this is '%s'", other, AF_BOARD_ID);
        char msg[160];
        snprintf(msg, sizeof(msg), "Upload refused: that firmware is for the %s build, this miner needs the %s build.",
                 other, AF_BOARD_ID);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    if (ok && !other[0]) {
        // every AxeForge image carries its board tag; one without it isn't ours
        esp_ota_abort(h);
        ESP_LOGE(TAG, "OTA refused: no AxeForge board tag in the image");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Upload refused: that file is not an AxeForge firmware for this miner.");
    }
    if (!ok || esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        if (ok) ESP_LOGE(TAG, "OTA validation failed");
        else esp_ota_abort(h);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload rejected: not a valid AxeForge app image");
    }
    ota_guard::armed(part);
    ESP_LOGW(TAG, "OTA written to %s, rebooting", part->label);
    httpd_resp_sendstr(req, "Update installed. Rebooting...");
    schedule_reboot(req);
    return ESP_OK;
}

// ------------------------------------------------------------------ pools
static esp_err_t pools_get(httpd_req_t *req)
{
    char *pools = settings::pools_to_json();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "pools", json_parse(pools));
    free(pools);
    cJSON_AddStringToObject(r, "mode", settings::get().pool_mode == PoolMode::Failover ? "failover" : "split");
    return send_json(req, r);
}

static esp_err_t pools_post(httpd_req_t *req)
{
    char *body = recv_body(req, 4096);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    cJSON *j = json_parse(body);
    free(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *list = cJSON_GetObjectItem(j, "pools");
    if (cJSON_IsArray(list)) {
        char *s = cJSON_PrintUnformatted(list);
        settings::pools_from_json(s);
        free(s);
    }
    cJSON *mode = cJSON_GetObjectItem(j, "mode");
    if (cJSON_IsString(mode)) settings::get().pool_mode = strcmp(mode->valuestring, "failover") ? PoolMode::Split : PoolMode::Failover;
    cJSON_Delete(j);
    settings::save();
    pools::reconfigure();  // reconnects in place, no reboot
    return httpd_resp_sendstr(req, "Pools saved. Reconnecting...");
}

// ------------------------------------------------------------------ logs
static esp_err_t logs_get(httpd_req_t *req)
{
    char q[48], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);
    const size_t max = 12 * 1024;
    char *text = (char *) malloc(max);
    uint32_t next = logbuf::read(since, text, max);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "next", next);
    cJSON_AddNumberToObject(r, "ms", esp_log_timestamp());  // the log prefix clock, so the page can show wall time
    cJSON_AddStringToObject(r, "text", text);
    free(text);
    return send_json(req, r);
}

// ------------------------------------------------------------------ screen
// Live capture of the real LCD as a 16-bit BMP.
static esp_err_t screen_get(httpd_req_t *req)
{
    lv_img_dsc_t *img = display::snapshot(1500);
    if (!img) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display busy");
    const int w = img->header.w, h = img->header.h;
    const uint32_t row = w * 2, data = row * h, hdr = 14 + 40 + 12;
    uint8_t head[66] = {'B', 'M'};
    auto put32 = [&](int o, uint32_t v) { memcpy(head + o, &v, 4); };
    auto put16 = [&](int o, uint16_t v) { memcpy(head + o, &v, 2); };
    put32(2, hdr + data);
    put32(10, hdr);
    put32(14, 40);
    put32(18, w);
    put32(22, (uint32_t) -h);  // top-down rows
    put16(26, 1);
    put16(28, 16);
    put32(30, 3);              // BI_BITFIELDS
    put32(34, data);
    put32(54, 0xF800);
    put32(58, 0x07E0);
    put32(62, 0x001F);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, (const char *) head, sizeof(head));
    uint8_t *line = (uint8_t *) malloc(row * 8);
    const uint8_t *px = img->data;
    for (int y = 0; y < h; y += 8) {
        int rows = h - y < 8 ? h - y : 8;
        for (int i = 0; i < (int) row * rows; i += 2) {  // LVGL stores swapped bytes for the SPI/i80 panel
            line[i] = px[y * row + i + 1];
            line[i + 1] = px[y * row + i];
        }
        if (httpd_resp_send_chunk(req, (const char *) line, row * rows) != ESP_OK) break;
    }
    free(line);
    display::free_snapshot(img);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page_post(httpd_req_t *req)
{
    char *body = recv_body(req, 128);
    cJSON *j = body ? json_parse(body) : nullptr;
    free(body);
    cJSON *dir = cJSON_GetObjectItem(j, "dir"), *kind = cJSON_GetObjectItem(j, "kind");
    if (cJSON_IsNumber(dir)) ui::request_page(dir->valueint > 0 ? 1 : -1);
    else if (cJSON_IsString(kind) && kind->valuestring[0]) ui::request_kind(kind->valuestring[0]);
    cJSON_Delete(j);
    vTaskDelay(pdMS_TO_TICKS(600));  // let the display switch before the next screenshot
    char out[32];
    snprintf(out, sizeof(out), "{\"kind\":\"%c\"}", ui::current_kind());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// ------------------------------------------------------------------ swarm
static esp_err_t swarm_get(httpd_req_t *req)
{
    char *out = swarm::to_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return ESP_OK;
}

// {"add":"192.168.1.20"} | {"remove":"192.168.1.20"} | {"scan":true}
static esp_err_t swarm_post(httpd_req_t *req)
{
    char *body = recv_body(req, 256);
    cJSON *j = body ? json_parse(body) : nullptr;
    free(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    const char *msg = "Nothing to do.";
    cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "add")))
        msg = swarm::add(v->valuestring) ? "Miner added." : "Not added: invalid address, duplicate or list full.";
    else if (cJSON_IsString(v = cJSON_GetObjectItem(j, "remove")))
        msg = swarm::remove(v->valuestring) ? "Miner removed." : "Not in the list.";
    else if (cJSON_IsTrue(cJSON_GetObjectItem(j, "scan"))) {
        swarm::request_scan();
        msg = "Scanning the network, this takes about 20 seconds.";
    }
    cJSON_Delete(j);
    return httpd_resp_sendstr(req, msg);
}

// Captive portal: in setup mode every unknown page (the OS connectivity checks
// like /generate_204 or /hotspot-detect.html) is sent to the setup page.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t)
{
    if (wifi::in_setup_mode() && strncmp(req->uri, "/api/", 5) != 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

// ------------------------------------------------------------------ layout
static esp_err_t layout_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, layout_store::json());
}

static esp_err_t layout_post(httpd_req_t *req)
{
    char *body = recv_body(req, 6144);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    bool ok = layout_store::set_json(body);
    free(body);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid layout");
    return httpd_resp_sendstr(req, "Layout saved and applied.");
}

// ------------------------------------------------------------------ wifi scan
static esp_err_t wifi_scan_get(httpd_req_t *req) { return send_json(req, wifi::scan()); }

// ------------------------------------------------------------------ best shares, 24 h stats
static esp_err_t scoreboard_get(httpd_req_t *req) { return send_json(req, stats::scoreboard_json()); }

static esp_err_t scoreboard_post(httpd_req_t *req)
{
    char *body = recv_body(req, 64);
    cJSON *j = body ? json_parse(body) : nullptr;
    free(body);
    bool reset = cJSON_IsTrue(cJSON_GetObjectItem(j, "reset"));
    cJSON_Delete(j);
    if (!reset) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing to do");
    stats::reset();
    return httpd_resp_sendstr(req, "Best shares cleared.");
}

static esp_err_t stats_get(httpd_req_t *req) { return send_json(req, stats::day_json()); }

// ------------------------------------------------------------------ self-test
static esp_err_t selftest_get(httpd_req_t *req) { return send_json(req, selftest::json()); }

static esp_err_t selftest_post(httpd_req_t *req)
{
    return httpd_resp_sendstr(req, selftest::start() ? "Self-test started. It takes about a minute; mining keeps running."
                                                     : "A self-test is already running.");
}

// ------------------------------------------------------------------ notifications
static esp_err_t notify_post(httpd_req_t *req)
{
    return httpd_resp_sendstr(req, notify::test() ? "Test message sent. Check Discord / Telegram."
                                                  : "Set a Discord webhook or a Telegram bot first, then save.");
}

// ------------------------------------------------------------------ two-factor login
#define SESSION_HDR "X-AxeForge-Session"

static void session_of(httpd_req_t *req, char *out, size_t len)
{
    out[0] = 0;
    size_t n = httpd_req_get_hdr_value_len(req, SESSION_HDR);
    if (n && n < len) httpd_req_get_hdr_value_str(req, SESSION_HDR, out, len);
}

// The otpauth:// URI as an SVG QR code, for the authenticator app to scan.
static char *qr_svg(const char *text)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true)) return nullptr;
    int n = qrcodegen_getSize(qr);
    size_t cap = 256 + (size_t) n * n * 24;
    char *svg = (char *) malloc(cap);
    if (!svg) return nullptr;
    size_t o = snprintf(svg, cap,
                        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='-4 -4 %d %d' shape-rendering='crispEdges'>"
                        "<rect x='-4' y='-4' width='%d' height='%d' fill='#fff'/><path fill='#000' d='",
                        n + 8, n + 8, n + 8, n + 8);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y) && o + 24 < cap) o += snprintf(svg + o, cap - o, "M%d %dh1v1h-1z", x, y);
    snprintf(svg + o, cap - o, "'/></svg>");
    return svg;
}

static esp_err_t otp_get(httpd_req_t *req)
{
    char tok[40];
    session_of(req, tok, sizeof(tok));
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", otp::enabled());
    cJSON_AddBoolToObject(r, "session", otp::enabled() && otp::session_ok(tok));
    cJSON_AddBoolToObject(r, "clock", g_state.time_synced);
    return send_json(req, r);
}

// {"action":"setup"|"confirm"|"login"|"logout"|"disable", "code":"123456"}
static esp_err_t otp_post(httpd_req_t *req)
{
    char *body = recv_body(req, 128);
    cJSON *j = body ? json_parse(body) : nullptr;
    free(body);
    cJSON *a = cJSON_GetObjectItem(j, "action"), *c = cJSON_GetObjectItem(j, "code");
    char action[12] = "", code[8] = "";
    if (cJSON_IsString(a)) snprintf(action, sizeof(action), "%s", a->valuestring);
    if (cJSON_IsString(c)) snprintf(code, sizeof(code), "%s", c->valuestring);
    cJSON_Delete(j);
    char tok[40];
    session_of(req, tok, sizeof(tok));
    cJSON *r = cJSON_CreateObject();
    const char *err = nullptr;
    char token[40] = "";
    if (!strcmp(action, "setup")) {
        if (otp::enabled()) {
            err = "Two-factor login is already on.";
        } else {
            char secret[40], uri[160];
            otp::begin_setup(secret, sizeof(secret), uri, sizeof(uri));
            cJSON_AddStringToObject(r, "secret", secret);
            cJSON_AddStringToObject(r, "uri", uri);
            char *svg = qr_svg(uri);
            cJSON_AddStringToObject(r, "svg", svg ? svg : "");
            free(svg);
        }
    } else if (!strcmp(action, "confirm")) {
        err = otp::confirm(code, token, sizeof(token));
    } else if (!strcmp(action, "login")) {
        err = otp::login(code, token, sizeof(token));
    } else if (!strcmp(action, "logout")) {
        otp::logout(tok);
    } else if (!strcmp(action, "disable")) {
        // a valid session and a fresh code: a stolen session alone can't switch it off
        if (!otp::session_ok(tok)) err = "Log in first.";
        else err = otp::disable(code);
    } else {
        err = "Unknown action.";
    }
    if (err) {
        cJSON_Delete(r);
        httpd_resp_set_status(req, "400 Bad Request");
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", err);
        return send_json(req, e);
    }
    if (token[0]) cJSON_AddStringToObject(r, "token", token);
    cJSON_AddBoolToObject(r, "enabled", otp::enabled());
    return send_json(req, r);
}

// ------------------------------------------------------------------ guard
// The API has no login, so it must at least be unusable from other web sites.
// - Host has to name this device (an IP or a LAN name), which defeats DNS
//   rebinding, where a foreign name is pointed at the miner's address.
// - Anything that changes state needs the X-AxeForge header. A cross-site
//   page can only send that after a CORS preflight, which is never granted.
//   Origin and Sec-Fetch-Site, when the browser sends them, must be ours too.

// 1 present, 0 absent, -1 too long to be legitimate
static int get_hdr(httpd_req_t *req, const char *name, char *out, size_t len)
{
    size_t n = httpd_req_get_hdr_value_len(req, name);
    if (n == 0) return 0;
    if (n >= len) return -1;
    return httpd_req_get_hdr_value_str(req, name, out, len) == ESP_OK ? 1 : -1;
}

static bool ends_with(const char *s, size_t n, const char *suffix)
{
    size_t k = strlen(suffix);
    return n > k && strncasecmp(s + n - k, suffix, k) == 0;
}

static bool host_is_local(const char *host)
{
    if (host[0] == '[') return true;  // IPv6 literal
    const char *colon = strchr(host, ':');
    size_t n = colon ? (size_t) (colon - host) : strlen(host);
    if (n == 0) return false;
    bool dotted_ip = true, has_dot = false;
    for (size_t i = 0; i < n; i++) {
        char c = host[i];
        if (c == '.') has_dot = true;
        else if (c < '0' || c > '9') dotted_ip = false;
    }
    if (dotted_ip || !has_dot) return true;  // IPv4 literal or a bare LAN name
    static const char *const local_suffixes[] = {".local", ".lan", ".home", ".home.arpa", ".internal",
                                                 ".localdomain", ".fritz.box"};
    for (const char *suf : local_suffixes)
        if (ends_with(host, n, suf)) return true;
    return false;
}

// True when the request reached the miner through its own setup network (the
// soft-AP at 192.168.4.1) rather than the LAN.
static bool via_setup_ap(httpd_req_t *req)
{
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *) &a, &al) != 0) return false;
    const uint8_t *b;
    if (a.ss_family == AF_INET) {
        b = (const uint8_t *) &((struct sockaddr_in *) &a)->sin_addr;
    } else if (a.ss_family == AF_INET6) {
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        b = ((struct sockaddr_in6 *) &a)->sin6_addr.s6_addr;
        if (memcmp(b, mapped, 12) != 0) return false;
        b += 12;
    } else {
        return false;
    }
    return b[0] == 192 && b[1] == 168 && b[2] == 4 && b[3] == 1;
}

// On the setup network only what the Wi-Fi form needs: no pool credentials,
// logs or firmware upload for whoever is in radio range.
static bool setup_ap_allows(httpd_req_t *req)
{
    size_t n = strcspn(req->uri, "?");
    auto is = [&](const char *p) { return strlen(p) == n && strncmp(req->uri, p, n) == 0; };
    if (req->method == HTTP_GET) return is("/api/status") || is("/api/settings") || is("/api/wifi/scan");
    return req->method == HTTP_POST && is("/api/settings");  // settings_post keeps only the Wi-Fi keys
}

// Origin is "scheme://host[:port]" and must name the host the request went to.
static bool origin_matches(const char *origin, const char *host)
{
    const char *p = strstr(origin, "://");
    return p && strcasecmp(p + 3, host) == 0;
}

static esp_err_t deny(httpd_req_t *req, const char *why)
{
    char ip[48];
    peer_ip(req, ip, sizeof(ip));
    ESP_LOGW(TAG, "blocked %s %s from %s: %s", http_method_str((enum http_method) req->method), req->uri, ip, why);
    return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
}

typedef esp_err_t (*handler_fn)(httpd_req_t *);

static esp_err_t api_guard(httpd_req_t *req)
{
    char host[96], v[128];
    if (get_hdr(req, "Host", host, sizeof(host)) != 1 || !host_is_local(host)) return deny(req, "foreign Host");
    if (via_setup_ap(req) && !setup_ap_allows(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "Not available on the setup network: connect the miner to your Wi-Fi first.");
    }
    if (req->method != HTTP_GET) {
        if (get_hdr(req, "X-AxeForge", v, sizeof(v)) != 1) return deny(req, "missing X-AxeForge header");
        int o = get_hdr(req, "Origin", v, sizeof(v));
        if (o < 0 || (o == 1 && !origin_matches(v, host))) return deny(req, "cross-site Origin");
        int f = get_hdr(req, "Sec-Fetch-Site", v, sizeof(v));
        if (f < 0 || (f == 1 && strcmp(v, "same-origin") != 0 && strcmp(v, "none") != 0))
            return deny(req, "cross-site request");
        // with two-factor login on, changes need a session from a valid code
        if (otp::enabled() && strcmp(req->uri, "/api/otp") != 0) {
            char tok[40];
            session_of(req, tok, sizeof(tok));
            if (!otp::session_ok(tok)) {
                httpd_resp_set_status(req, "401 Unauthorized");
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, "{\"login\":true,\"error\":\"Log in with your authenticator code.\"}");
            }
        }
        // every change to the miner leaves a trace of who made it
        char ip[48];
        peer_ip(req, ip, sizeof(ip));
        ESP_LOGI(TAG, "%s %s from %s", http_method_str((enum http_method) req->method), req->uri, ip);
    }
    return ((handler_fn) req->user_ctx)(req);
}

namespace web {

const char *previous_restart()
{
    static char prev[sizeof(s_restart_note.why)];
    static bool read = false;
    if (!read) {
        read = true;
        if (s_restart_note.magic == RESTART_NOTE_MAGIC) {
            memcpy(prev, s_restart_note.why, sizeof(prev));
            prev[sizeof(prev) - 1] = 0;
        }
        s_restart_note.magic = 0;  // one restart, one note
    }
    return prev;
}

void start()
{
    ESP_LOGI(TAG, "firmware board tag %s", af_board_tag);
    httpd_handle_t h = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 36;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 10;
    cfg.max_open_sockets = 8;
    cfg.lru_purge_enable = true;  // a browser holding idle keep-alives must not lock others out
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, index_get, NULL},
        {"/api/status", HTTP_GET, status_get, NULL},
        {"/favicon.ico", HTTP_GET, favicon_get, NULL},
        {"/api/history", HTTP_GET, history_get, NULL},
        {"/api/settings", HTTP_GET, settings_get, NULL},
        {"/api/settings", HTTP_POST, settings_post, NULL},
        {"/api/retune", HTTP_POST, retune_post, NULL},
        {"/api/restart", HTTP_POST, restart_post, NULL},
        {"/api/ota", HTTP_POST, ota_post, NULL},
        {"/api/ota/keep", HTTP_POST, ota_keep_post, NULL},
        {"/api/pools", HTTP_GET, pools_get, NULL},
        {"/api/pools", HTTP_POST, pools_post, NULL},
        {"/api/logs", HTTP_GET, logs_get, NULL},
        {"/api/screen.bmp", HTTP_GET, screen_get, NULL},
        {"/api/page", HTTP_POST, page_post, NULL},
        {"/api/layout", HTTP_GET, layout_get, NULL},
        {"/api/layout", HTTP_POST, layout_post, NULL},
        {"/api/swarm", HTTP_GET, swarm_get, NULL},
        {"/api/swarm", HTTP_POST, swarm_post, NULL},
        {"/api/wifi/scan", HTTP_GET, wifi_scan_get, NULL},
        {"/api/scoreboard", HTTP_GET, scoreboard_get, NULL},
        {"/api/scoreboard", HTTP_POST, scoreboard_post, NULL},
        {"/api/stats", HTTP_GET, stats_get, NULL},
        {"/api/selftest", HTTP_GET, selftest_get, NULL},
        {"/api/selftest", HTTP_POST, selftest_post, NULL},
        {"/api/notify", HTTP_POST, notify_post, NULL},
        {"/api/otp", HTTP_GET, otp_get, NULL},
        {"/api/otp", HTTP_POST, otp_post, NULL},
    };
    for (auto u : uris) {
        if (strncmp(u.uri, "/api/", 5) == 0) {
            u.user_ctx = (void *) u.handler;
            u.handler = api_guard;
        }
        httpd_register_uri_handler(h, &u);
    }
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, not_found);
}

} // namespace web
