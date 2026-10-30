#pragma once
// Shared runtime state. Writers own their fields; readers (UI, web, tuner)
// take snapshots. Plain floats/ints are written atomically on the S3, so a
// torn read just means one stale value for one refresh.

#include <stdint.h>
#include <stdbool.h>

#include "core/board_config.h"  // AF_NUM_ASICS
#define AF_HISTORY_LEN 120  // hashrate history points for the UI chart

enum class TunerPhase : uint8_t {
    Off,        // manual frequency/voltage
    WarmUp,     // letting temps settle before measuring
    Measuring,  // collecting a stability window
    StepUp,     // applying the next candidate
    Locked,     // found the best stable point, watching for drift
    Backoff,    // something went wrong, stepping down
};

enum class SysStatus : uint8_t {
    Booting,
    NoWifi,
    SetupAP,
    Connecting,
    Mining,
    Fault,
    Overheat,
    Paused,     // chips off on purpose (electricity price)
};

struct TunerState {
    TunerPhase phase = TunerPhase::Off;
    uint16_t best_freq = 0;
    uint16_t best_mv = 0;
    float best_ghs = 0;
    uint16_t trial_freq = 0;
    uint16_t trial_mv = 0;
    uint8_t progress = 0;        // 0..100 of current window
    uint16_t steps_done = 0;
    char note[64] = "";
};

struct WeatherDay {
    int8_t code = -1;      // WMO weather code
    float tmin = 0, tmax = 0;
};

struct WeatherState {
    bool valid = false;
    char city[48] = "";
    float temp = 0;
    float feels = 0;
    uint8_t humidity = 0;
    float wind = 0;
    int8_t code = -1;
    bool is_day = true;
    WeatherDay days[3];
    int32_t utc_offset_s = 0;
    uint32_t updated_s = 0;   // uptime of last successful fetch
    char error[48] = "";
};

#define AF_POOLS 4

struct PoolState {
    bool enabled = false;
    bool connected = false;
    uint8_t proto = 0;           // PoolProto
    char host[64] = "";
    uint32_t diff = 0;
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    double best = 0;
    double accepted_work = 0;    // sum of accepted share difficulty (for hashrate)
    float ghs = 0;               // estimate from accepted work over the last ~10 min
    uint8_t work_pct = 0;        // actual share of jobs sent here
    uint32_t jobs = 0;
    uint32_t last_share_s = 0;
    uint32_t ping_ms = 0;        // submit -> response latency
    uint8_t payout = 0;          // Payout of this pool's latest template
    uint8_t payout_pct = 0;      // share of the coinbase paid to your address
    bool payout_blocked = false; // payout check enforced: this pool gets no work
    char status[48] = "";        // last error / state text
};

// Solo payout check: does the coinbase pay the address in the pool username?
enum class Payout : uint8_t {
    Unknown,     // no template yet
    NotAddress,  // the username isn't a Bitcoin address (pooled account)
    PaysYou,     // at least one output pays your address
    NotYou,      // your address is in the username but not in the coinbase
};

struct CoinbaseOut {
    uint64_t sats = 0;
    char addr[91] = "";  // address, "OP_RETURN" or "" for a non-standard script
    bool yours = false;
};

#define AF_SHOWN_OUTPUTS 4

// What the chips are hashing right now (from the active job).
struct BlockTemplate {
    bool valid = false;
    int pool = -1;
    uint32_t height = 0;
    char prev_hash[65] = "";     // display (big endian) order
    uint32_t ntime = 0;
    uint32_t nbits = 0;
    uint32_t version = 0;
    uint8_t branches = 0;        // merkle depth -> ~2^n transactions
    double reward_btc = 0;       // sum of coinbase outputs (subsidy + fees)
    char tag[40] = "";           // printable coinbase text (pool tag)
    // coinbase outputs (the first AF_SHOWN_OUTPUTS) and the payout check
    uint8_t n_outputs = 0;
    CoinbaseOut outputs[AF_SHOWN_OUTPUTS];
    Payout payout = Payout::Unknown;
    uint64_t payout_sats = 0;    // to your address
    uint64_t total_sats = 0;
    char payout_addr[91] = "";   // your address, from the pool username
};

struct MempoolBlock {
    uint32_t height = 0;
    uint32_t txs = 0;
    float size_mb = 0;
    uint32_t time = 0;
    float median_fee = 0;
    float fees_btc = 0;
    char pool[24] = "";
};

struct MempoolNext {
    uint32_t txs = 0;
    float vsize_mb = 0;
    float median_fee = 0;
    float min_fee = 0, max_fee = 0;
};

struct MempoolState {
    bool valid = false;
    MempoolBlock mined[6];        // newest first
    MempoolNext next[3];          // projected next blocks
    float fee_fast = 0, fee_hour = 0, fee_eco = 0;
    uint32_t updated_s = 0;
    char error[48] = "";
};

struct MinerState {
    SysStatus status = SysStatus::Booting;
    char fault[64] = "";

    // ASIC
    int chips_detected = 0;
    uint16_t freq_mhz = 0;
    uint16_t core_mv = 0;
    float chip_ghs[AF_NUM_ASICS] = {};
    float chip_err_ghs[AF_NUM_ASICS] = {};
    float hashrate_ghs = 0;       // smoothed total
    float expected_ghs = 0;       // freq * cores
    float error_pct = 0;          // hw errors / total
    float history[AF_HISTORY_LEN] = {};        // hashrate, GH/s
    float history_temp[AF_HISTORY_LEN] = {};   // chip_temp_max, same cadence
    float history_power[AF_HISTORY_LEN] = {};  // pin, W
    int history_head = 0;

    // Power / thermal
    float vin = 0, iin = 0, pin = 0;
    float vout = 0, iout = 0;
    float chip_temp[AF_NUM_ASICS] = {};
    bool chip_temp_own[AF_NUM_ASICS] = {};  // false = borrowed from the nearest board sensor
    float chip_temp_max = 0;
    float vr_temp = 0;
    bool temp_ok = false;     // every ASIC-side sensor answered this second
    bool vr_temp_ok = false;
    bool board_rev7 = false;  // TPS546 regulator
    uint8_t chip_stall = 0;   // 1-based chip that stopped hashing; the monitor acts on it
    bool no_hash = false;     // powered and fed work, but no chip returns anything
    uint16_t fan_rpm[2] = {};
    uint8_t fan_pct = 0;
    uint8_t fan_duty[2] = {};
    bool fan_present[2] = {};  // seen spinning since boot (an empty header reads 0 rpm)

    // Pool
    bool pool_connected = false;
    char pool_host[64] = "";
    uint32_t pool_diff = 0;
    uint32_t shares_accepted = 0;
    uint32_t shares_rejected = 0;
    double best_diff = 0;
    double session_best = 0;
    uint32_t block_height = 0;
    uint32_t network_nbits = 0;
    uint32_t blocks_found = 0;

    // Network
    char ip[16] = "";
    char ssid[33] = "";
    char hostname[33] = "";      // the name this miner gives DHCP
    int8_t rssi = 0;
    char ap_ssid[33] = "";
    char ap_pass[16] = "";       // setup network password, "" = open (first setup)

    uint32_t uptime_s = 0;
    char reset_reason[16] = "";
    TunerState tuner;
    WeatherState weather;
    bool time_synced = false;

    PoolState pools[AF_POOLS];
    BlockTemplate tmpl;
    MempoolState mempool;
    char last_hash[65] = "";      // most recent share hash, display order
    double last_hash_diff = 0;
    uint32_t hashes_checked = 0;  // nonces verified on the ESP32
    uint32_t dup_results = 0;     // results the chips reported twice (dropped, never submitted)
};

extern MinerState g_state;
