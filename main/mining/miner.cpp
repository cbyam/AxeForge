#include "miner.h"
#include <math.h>

#include <algorithm>

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "asic/bm13xx.h"
#include "core/board.h"
#include "core/state.h"
#include "mining/work.h"
#include "core/settings.h"
#include "stratum/pools.h"
#include "mining/stats.h"
#include "net/notify.h"

static const char *TAG = "miner";

#define JOB_INTERVAL_MS AF_JOB_INTERVAL_MS  // per board, see core/board_config.h
// A chip below this share of the median chip (or wildly above its expected rate) for
// STALL_PERIODS counter periods in a row is reported as stalled.
#define STALL_LOW 0.30f
#define STALL_HIGH 3.0f
#define STALL_PERIODS 3
#define NUM_SLOTS 128
#define ASIC_DIFF_MIN AF_ASIC_DIFF_MIN
#define ASIC_DIFF_MAX AF_ASIC_DIFF_MAX
#define COUNTER_PERIOD_MS 5000
#define HISTORY_EVERY 6  // counter periods per chart point (30 s)
#define STATS_EVERY 12   // counter periods per 24-hour sample (60 s)
#define BEST_NOTIFY_GAP_S 600  // at most one "new best" message per 10 minutes

static SemaphoreHandle_t s_work_lock;
static Work s_work[NUM_SLOTS];
static volatile bool s_paused = false;
static TaskHandle_t s_job_task;
static uint32_t s_job_counter = 0;

// --- hashrate counters (written only by the result task) ---
static int64_t s_prev_t[AF_NUM_ASICS];
static uint32_t s_prev_cnt[AF_NUM_ASICS];
static int64_t s_prev_err_t[AF_NUM_ASICS];
static uint32_t s_prev_err[AF_NUM_ASICS];
// measurement window (reset by tuner)
static portMUX_TYPE s_win_mux = portMUX_INITIALIZER_UNLOCKED;
static double s_win_valid[AF_NUM_ASICS];
static double s_win_err[AF_NUM_ASICS];
static int64_t s_win_us[AF_NUM_ASICS];
static int64_t s_win_start;

static void invalidate_all()
{
    xSemaphoreTake(s_work_lock, portMAX_DELAY);
    for (auto &w : s_work) w.valid = false;
    xSemaphoreGive(s_work_lock);
}

static void invalidate_pool(int pool)
{
    xSemaphoreTake(s_work_lock, portMAX_DELAY);
    for (auto &w : s_work)
        if (w.pool == pool) w.valid = false;
    xSemaphoreGive(s_work_lock);
}

// The chips only take a whole difficulty: round down, so nothing that could be a
// share is filtered out on the chip.
static uint32_t asic_difficulty(double pool_diff)
{
    if (pool_diff < ASIC_DIFF_MIN) return ASIC_DIFF_MIN;
    if (pool_diff > ASIC_DIFF_MAX) return ASIC_DIFF_MAX;
    return (uint32_t) pool_diff;
}

// Keeps the per-pool payout check and logs when it changes, so a solo pool that
// stops paying your address (or never did) shows up in the log.
static void note_payout(int p, const BlockTemplate &t)
{
    PoolState &ps = g_state.pools[p];
    uint8_t pct = t.total_sats ? (uint8_t) (100.0 * t.payout_sats / t.total_sats + 0.5) : 0;
    // with the payout check set to enforce, a pool that doesn't pay you (or keeps
    // more than the fee limit) gets no work until its next template does
    const Settings &cfg = settings::get();
    bool blocked = cfg.payout_mode == 1 &&
                   (t.payout == Payout::NotYou || (t.payout == Payout::PaysYou && pct + cfg.payout_max_fee < 100));
    if (blocked != ps.payout_blocked) {
        ps.payout_blocked = blocked;
        if (blocked)
            ESP_LOGW(TAG, "pool %d gets no work: its coinbase %s", p + 1,
                     t.payout == Payout::NotYou ? "doesn't pay your address" : "keeps more than your fee limit");
        else
            ESP_LOGI(TAG, "pool %d pays you again, mining on it", p + 1);
    }
    if ((uint8_t) t.payout == ps.payout && pct == ps.payout_pct) return;
    ps.payout = (uint8_t) t.payout;
    ps.payout_pct = pct;
    if (t.payout == Payout::PaysYou)
        ESP_LOGI(TAG, "pool %d: a block would pay %s %.8f BTC (%u%% of the coinbase)", p + 1, t.payout_addr,
                 t.payout_sats / 1e8, pct);
    else if (t.payout == Payout::NotYou)
        ESP_LOGW(TAG, "pool %d: the coinbase does NOT pay %s (fine for pooled mining, wrong for solo)", p + 1,
                 t.payout_addr);
}

struct PoolSlot {
    StratumJob tmpl;
    pools::JobInfo info;
    bool have = false;
    uint32_t gen = 0;
    uint32_t en2 = 0;
    int32_t credit = 0;      // split-mode scheduler
    uint32_t shown_gen = 0;  // template last decoded for the UI
    BlockTemplate decoded;
};

// Weighted round robin (split) or first-healthy (failover).
static int pick_pool(PoolSlot *slots)
{
    const Settings &cfg = settings::get();
    bool ok[AF_MAX_POOLS];
    int32_t total = 0;
    for (int i = 0; i < AF_MAX_POOLS; i++) {
        ok[i] = slots[i].have && pools::ready(i) && cfg.pools[i].enabled;
        if (cfg.pool_mode == PoolMode::Split && cfg.pools[i].weight == 0) ok[i] = false;
        if (g_state.pools[i].payout_blocked) ok[i] = false;
        if (ok[i]) total += cfg.pools[i].weight;
    }
    if (cfg.pool_mode == PoolMode::Failover) {
        for (int i = 0; i < AF_MAX_POOLS; i++)
            if (ok[i]) return i;
        return -1;
    }
    int best = -1;
    for (int i = 0; i < AF_MAX_POOLS; i++) {
        if (!ok[i]) continue;
        slots[i].credit += cfg.pools[i].weight;
        if (best < 0 || slots[i].credit > slots[best].credit) best = i;
    }
    if (best >= 0) slots[best].credit -= total;
    return best;
}

static void job_task(void *)
{
    static PoolSlot slots[AF_MAX_POOLS];
    uint32_t sent_window = 0;
    uint32_t sent_per_pool[AF_MAX_POOLS] = {};
    int last_pool = -1;

    while (true) {
        if (s_paused || !board::is_powered()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        double min_diff = UINT32_MAX;
        uint32_t roll_mask = 0xffffffff;  // only bits every pool in use allows
        for (int i = 0; i < AF_MAX_POOLS; i++) {
            PoolSlot &s = slots[i];
            StratumJob next;
            if (pools::fetch_job(i, next, s.info, &s.gen)) {
                if (s.have) {
                    free(s.tmpl.coinb1);
                    free(s.tmpl.coinb2);
                }
                s.tmpl = next;
                s.have = true;
                if (pools::take_clean_flag(i)) invalidate_pool(i);
            }
            if (s.have && s.shown_gen != s.gen) {
                // decode each pool's template once per new job: the payout check
                // has to see it before the pool is picked
                s.decoded = BlockTemplate();
                char user[sizeof(settings::get().pools[i].user)];
                settings::pool_user(i, user, sizeof(user));
                work::describe_template(s.tmpl, s.info.en1, s.info.en2_len, s.decoded, user);
                s.decoded.pool = i;
                s.shown_gen = s.gen;
                note_payout(i, s.decoded);
            }
            if (s.have && pools::ready(i)) {
                if (s.info.difficulty < min_diff) min_diff = s.info.difficulty;
                roll_mask &= s.info.version_mask;
            }
        }

        int p = pick_pool(slots);
        g_state.pool_connected = p >= 0;
        if (p < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        PoolSlot &s = slots[p];
        if (p != last_pool || g_state.tmpl.height != s.decoded.height) {
            g_state.tmpl = s.decoded;
            g_state.block_height = s.decoded.height;
            g_state.network_nbits = s.decoded.nbits;
            last_pool = p;
        }

        Work w;
        AsicJob cj;
        work::build(s.tmpl, s.info.en1, s.en2++, s.info.en2_len, s.info.version_mask, s.info.difficulty, w, cj);
        w.pool = p;
        asic::set_difficulty_mask(asic_difficulty(min_diff));
        asic::set_version_mask(roll_mask);
        uint8_t slot = asic::send_job(s_job_counter++, cj);
        xSemaphoreTake(s_work_lock, portMAX_DELAY);
        s_work[slot] = w;
        xSemaphoreGive(s_work_lock);

        g_state.pools[p].jobs++;
        sent_per_pool[p]++;
        if (++sent_window >= 40) {  // refresh the real split every ~20 s
            for (int i = 0; i < AF_MAX_POOLS; i++) {
                g_state.pools[i].work_pct = (uint8_t) (100 * sent_per_pool[i] / sent_window);
                sent_per_pool[i] = 0;
            }
            sent_window = 0;
        }
        g_state.pool_diff = (uint32_t) ceil(s.info.difficulty);

        // a new block wakes us early so stale work is replaced immediately
        uint16_t job_ms = settings::get().job_ms;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(job_ms ? job_ms : JOB_INTERVAL_MS));
    }
}

static void on_counter(int chip, uint32_t cnt, bool is_error)
{
    if (chip < 0 || chip >= AF_NUM_ASICS) return;
    int64_t now = esp_timer_get_time();
    int64_t *pt = is_error ? s_prev_err_t : s_prev_t;
    uint32_t *pc = is_error ? s_prev_err : s_prev_cnt;
    if (!pt[chip]) {
        pt[chip] = now;
        pc[chip] = cnt;
        return;
    }
    int64_t dt = now - pt[chip];
    uint32_t dc = cnt - pc[chip];  // wraps correctly
    pt[chip] = now;
    pc[chip] = cnt;
    if (dt <= 0) return;
    // each count is 2^32 hashes
    float ghs = (float) ((double) dc * 4294967296.0 / (double) dt / 1000.0);
    portENTER_CRITICAL(&s_win_mux);
    if (is_error) {
        g_state.chip_err_ghs[chip] = ghs;
        s_win_err[chip] += dc;
    } else {
        g_state.chip_ghs[chip] = ghs;
        s_win_valid[chip] += dc;
        s_win_us[chip] += dt;
    }
    portEXIT_CRITICAL(&s_win_mux);
}

// The chain occasionally reports the same nonce twice (a repeated UART frame, for
// one). Submitting both gets the second rejected as a duplicate, so remember the
// last few results and drop exact repeats.
#define RECENT_RESULTS 32
struct RecentResult {
    uint8_t slot;
    uint32_t nonce, version;
};

static bool seen_before(uint8_t slot, uint32_t nonce, uint32_t version)
{
    static RecentResult recent[RECENT_RESULTS];
    static int next = 0;
    for (const RecentResult &x : recent)
        if (x.slot == slot && x.nonce == nonce && x.version == version) return true;
    recent[next] = {slot, nonce, version};
    next = (next + 1) % RECENT_RESULTS;
    return false;
}

static void result_task(void *)
{
    AsicResult r;
    uint8_t hash[32];
    while (true) {
        if (!board::is_powered()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!asic::receive(r, 1000)) continue;

        if (r.is_register) {
            if (r.reg == asic::REG_NONCE_COUNT) on_counter(r.chip, r.data, false);
            else if (r.reg == asic::REG_HW_ERROR_COUNT) on_counter(r.chip, r.data, true);
            continue;
        }

        Work w;
        xSemaphoreTake(s_work_lock, portMAX_DELAY);
        w = s_work[r.job_slot & 0x7f];
        xSemaphoreGive(s_work_lock);
        if (!w.valid) continue;

        // the chip reports the new state of the rolled bits: substitute them, so a
        // template with bits already set inside the mask is rebuilt correctly
        uint32_t version = (w.version & ~w.version_mask) | (r.version_bits & w.version_mask);
        if (seen_before(r.job_slot, r.nonce, version)) {
            g_state.dup_results++;
            ESP_LOGW(TAG, "chip %d repeated nonce %08lx (job slot %u), dropped (%lu so far)", r.chip,
                     (unsigned long) r.nonce, r.job_slot, (unsigned long) g_state.dup_results);
            continue;
        }
        double diff = work::nonce_difficulty(w, r.nonce, version, hash);
        g_state.hashes_checked++;
        if (diff > g_state.session_best) g_state.session_best = diff;
        if (diff > g_state.best_diff) {
            static uint32_t last_best_msg = 0;
            double prev = g_state.best_diff;
            g_state.best_diff = diff;
            // not for the first shares after a reset, every one of those is a "best"
            if (prev >= 1e6 && (!last_best_msg || g_state.uptime_s - last_best_msg >= BEST_NOTIFY_GAP_S)) {
                last_best_msg = g_state.uptime_s;
                notify::post(NOTIFY_BEST, "new best share: difficulty %.0f (was %.0f)", diff, prev);
            }
        }
        stats::offer(diff, w.pool, r.chip);
        PoolState &ps = g_state.pools[w.pool];
        if (diff > ps.best) ps.best = diff;

        // live hash for the UI, in the usual big-endian display order
        uint8_t be[32];
        for (int i = 0; i < 32; i++) be[i] = hash[31 - i];
        work::bin2hex(be, 32, g_state.last_hash);
        g_state.last_hash_diff = diff;

        if (diff >= w.pool_diff) {
            // only notable shares at info level, otherwise the console is just shares
            if (diff >= 8.0 * w.pool_diff)
                ESP_LOGI(TAG, "nice share: pool %d chip %d diff %.0f (pool %lu)", w.pool + 1, r.chip, diff,
                         (unsigned long) w.pool_diff);
            else
                ESP_LOGD(TAG, "share pool %d chip %d diff %.0f", w.pool + 1, r.chip, diff);
            pools::submit(w.pool, w, r.nonce, version);
            if (diff >= work::nbits_to_difficulty(w.nbits)) {
                g_state.blocks_found++;
                ESP_LOGW(TAG, "!!! BLOCK FOUND !!! diff %.0f on pool %d", diff, w.pool);
                notify::post(NOTIFY_BLOCK, "BLOCK FOUND at height %lu, difficulty %.0f, pool %d",
                             (unsigned long) g_state.block_height, diff, w.pool + 1);
            }
        }
    }
}

static void counter_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(4000));
    int n = 0, periods = 0, stall = 0, dead = 0;
    while (true) {
        if (!board::is_powered()) {
            memset(s_prev_t, 0, sizeof(s_prev_t));
            memset(s_prev_err_t, 0, sizeof(s_prev_err_t));
            g_state.hashrate_ghs = 0;
            memset(g_state.chip_ghs, 0, sizeof(g_state.chip_ghs));
            memset(g_state.chip_err_ghs, 0, sizeof(g_state.chip_err_ghs));
            periods = stall = dead = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_paused) {
            // power-up or a clock change in progress: keep off the UART, and take a
            // fresh baseline afterwards (the counters restart with the chips)
            memset(s_prev_t, 0, sizeof(s_prev_t));
            memset(s_prev_err_t, 0, sizeof(s_prev_err_t));
            dead = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // two READ_ALL bursts must not overlap, so space them out
        asic::read_register_all(asic::REG_NONCE_COUNT);
        vTaskDelay(pdMS_TO_TICKS(500));
        asic::read_register_all(asic::REG_HW_ERROR_COUNT);
        vTaskDelay(pdMS_TO_TICKS(500));

        float total = 0, err = 0;
        for (int i = 0; i < AF_NUM_ASICS; i++) {
            total += g_state.chip_ghs[i];
            err += g_state.chip_err_ghs[i];
        }
        g_state.hashrate_ghs = g_state.hashrate_ghs ? 0.7f * g_state.hashrate_ghs + 0.3f * total : total;
        g_state.error_pct = (total + err) > 0 ? 100.0f * err / (total + err) : 0;

        // Chips in series voltage domains share one current: a chip that stops
        // hashing lets its domain's voltage climb. Report it so the monitor can
        // cut power before that hurts anything.
        // Compared against the median chip, so an idle board (no pool work yet)
        // or one that is throttled as a whole never counts as a stall.
        int chips = g_state.chips_detected < AF_NUM_ASICS ? g_state.chips_detected : AF_NUM_ASICS;
        // The median comparison below can't see a board where every chip went
        // quiet at once (a tripped regulator, say): catch that on its own, while
        // work is actually being sent.
        bool fed = g_state.pool_connected && !s_paused && g_state.expected_ghs > 0;
        dead = periods > 3 && fed && total < 0.05f * g_state.expected_ghs ? dead + 1 : 0;
        if (dead >= STALL_PERIODS) {
            ESP_LOGE(TAG, "no chip is hashing (%.0f GH/s of %.0f expected)", total, g_state.expected_ghs);
            g_state.no_hash = true;
            dead = 0;
        }
        if (++periods > 3 && chips > 1 && g_state.expected_ghs > 0) {
            float per = g_state.expected_ghs / chips;
            float sorted[AF_NUM_ASICS];
            memcpy(sorted, g_state.chip_ghs, sizeof(float) * chips);
            std::sort(sorted, sorted + chips);
            float med = sorted[chips / 2];
            int bad = -1;
            if (med >= 0.5f * per) {
                for (int i = 0; i < chips; i++) {
                    float v = g_state.chip_ghs[i];
                    if (v < STALL_LOW * med || v > STALL_HIGH * per) bad = i;
                }
            }
            stall = bad >= 0 ? stall + 1 : 0;
            if (stall >= STALL_PERIODS) {
                ESP_LOGE(TAG, "ASIC %d at %.0f GH/s while the others run ~%.0f", bad + 1, g_state.chip_ghs[bad], med);
                g_state.chip_stall = (uint8_t) (bad + 1);
                stall = 0;
            }
        }
        if (periods % 2 == 0) {
            char line[128];
            int o = 0;
            for (int i = 0; i < chips && i < AF_NUM_ASICS && o < (int) sizeof(line) - 8; i++)
                o += snprintf(line + o, sizeof(line) - o, " %.0f", g_state.chip_ghs[i]);
            ESP_LOGI(TAG, "chips GH/s:%s | %.1fW %.3fV %.1fA", line, g_state.pin, g_state.vout, g_state.iout);
        }

        if (n % STATS_EVERY == STATS_EVERY - 1) stats::tick();
        if (++n % HISTORY_EVERY == 0) {
            g_state.history[g_state.history_head] = g_state.hashrate_ghs;
            g_state.history_temp[g_state.history_head] = g_state.chip_temp_max;
            g_state.history_power[g_state.history_head] = g_state.pin;
            g_state.history_head = (g_state.history_head + 1) % AF_HISTORY_LEN;
        }
        vTaskDelay(pdMS_TO_TICKS(COUNTER_PERIOD_MS - 1000));
    }
}

namespace miner {

void start()
{
    s_work_lock = xSemaphoreCreateMutex();
    s_win_start = esp_timer_get_time();
    xTaskCreatePinnedToCore(job_task, "jobs", 6144, NULL, 10, &s_job_task, 1);
    xTaskCreatePinnedToCore(result_task, "results", 6144, NULL, 12, NULL, 1);
    xTaskCreate(counter_task, "hashctr", 4096, NULL, 6, NULL);
}

void pause(bool p)
{
    s_paused = p;
    if (p) invalidate_all();
    else if (s_job_task) xTaskNotifyGive(s_job_task);
}

void kick()
{
    if (s_job_task) xTaskNotifyGive(s_job_task);
}

void forget_pool(int pool)
{
    if (s_work_lock) invalidate_pool(pool);
}

void reset_window()
{
    portENTER_CRITICAL(&s_win_mux);
    memset(s_win_valid, 0, sizeof(s_win_valid));
    memset(s_win_err, 0, sizeof(s_win_err));
    memset(s_win_us, 0, sizeof(s_win_us));
    s_win_start = esp_timer_get_time();
    portEXIT_CRITICAL(&s_win_mux);
}

float window_stats(float *ghs, float *err_pct)
{
    double total = 0, valid = 0, errs = 0;
    int64_t start;
    portENTER_CRITICAL(&s_win_mux);
    for (int i = 0; i < AF_NUM_ASICS; i++) {
        if (s_win_us[i] > 0) total += s_win_valid[i] * 4294967296.0 / (double) s_win_us[i] / 1000.0;
        valid += s_win_valid[i];
        errs += s_win_err[i];
    }
    start = s_win_start;
    portEXIT_CRITICAL(&s_win_mux);
    *ghs = (float) total;
    *err_pct = (valid + errs) > 0 ? (float) (100.0 * errs / (valid + errs)) : 0;
    return (float) ((esp_timer_get_time() - start) / 1000000.0);
}

float window_worst_chip_err(int *chip)
{
    double worst = 0;
    int at = -1;
    portENTER_CRITICAL(&s_win_mux);
    for (int i = 0; i < AF_NUM_ASICS; i++) {
        double n = s_win_valid[i] + s_win_err[i];
        if (n > 0 && 100.0 * s_win_err[i] / n > worst) {
            worst = 100.0 * s_win_err[i] / n;
            at = i;
        }
    }
    portEXIT_CRITICAL(&s_win_mux);
    if (chip) *chip = at;
    return (float) worst;
}

float window_chip_ghs(int chip)
{
    if (chip < 0 || chip >= AF_NUM_ASICS) return 0;
    portENTER_CRITICAL(&s_win_mux);
    double valid = s_win_valid[chip];
    int64_t us = s_win_us[chip];
    portEXIT_CRITICAL(&s_win_mux);
    return us > 0 ? (float) (valid * 4294967296.0 / (double) us / 1000.0) : 0;
}

float window_chip_err(int chip)
{
    if (chip < 0 || chip >= AF_NUM_ASICS) return 0;
    portENTER_CRITICAL(&s_win_mux);
    double n = s_win_valid[chip] + s_win_err[chip];
    double e = s_win_err[chip];
    portEXIT_CRITICAL(&s_win_mux);
    return n > 0 ? (float) (100.0 * e / n) : 0;
}

} // namespace miner
