#ifndef RICH_H
#define RICH_H

#include <array>
#include <bitset>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "prefetcher.h"

// Modes matching Figure 15 configurations for Checkpoint-I and Checkpoint-II
enum class RichMode {
    REGION_2KB_ONLY = 1,      // (1) 2 KB-region only
    REGION_4KB_ONLY = 2,      // (2) 4 KB-region only (single-offset)
    REGION_16KB_ONLY = 3,     // (3) 16 KB-region only (single-offset)
    NAIVE_MULTI_REGION = 4,   // (4) 2/4/16 KB without multi-offset or arbitration
    MULTI_OFFSET_ONLY = 5,    // (5) 2/4/16 KB with multi-offset, no arbitration
    ALL_ON_CHIP = 6,          // (6) Multi-region + multi-offset + arbitration (all on-chip)
    FULL_RICH = 7             // (7) Full RICH with off-chip 16 KB PHT & late trimming
};

// Priority levels for Region Arbitration (Section 3.1 & Figure 8)
// Lower integer value = Higher priority
enum class RegionPriority : uint8_t {
    PRIO_2KB_PC_ADDR = 1,     // Highest: 2 KB triggered by (PC, address)
    PRIO_16KB        = 2,     // Second:  16 KB triggered by (PC, 5 offsets)
    PRIO_4KB         = 3,     // Third:   4 KB triggered by (PC, 3 offsets)
    PRIO_2KB_PC_OFF  = 4,     // Lowest:  2 KB triggered by (PC, offset)
    NONE             = 255
};

struct PrefetchCandidate {
    bool valid = false;
    uint64_t region_base_addr = 0;
    uint64_t region_size_bytes = 0;
    RegionPriority priority = RegionPriority::NONE;
    std::bitset<256> pattern; // Up to 256 bits (32 bits for 2KB, 64 for 4KB, 256 for 16KB)
    uint32_t num_lines = 0;
};

class RichPrefetcher : public Prefetcher {
private:
    RichMode mode;
    uint64_t sim_cycle;
    uint64_t offchip_latency_cycles;
    uint32_t offchip_threshold; // Default 30 (Section 4.1)

    // --- 1. Training Unit Structures (Section 4.1, Table 4, Figure 7) ---
    struct FTEntry {
        bool valid = false;
        uint64_t region_tag = 0;
        uint64_t pc = 0;
        uint64_t first_addr = 0;
        std::vector<uint32_t> offsets; // Collects up to N trigger offsets
        uint64_t lru_cycle = 0;
    };

    struct ATEntry {
        bool valid = false;
        uint64_t region_tag = 0;
        uint32_t trigger_id = 0;       // 12-bit hash of (PC, N offsets)
        uint32_t addr_trigger_id = 0;  // 12-bit hash of (PC, address) for 2KB
        std::bitset<256> pattern;
        uint64_t lru_cycle = 0;
    };

    std::vector<FTEntry> ft_2kb, ft_4kb, ft_16kb;   // 64 entries each (Table 4)
    std::vector<ATEntry> at_2kb, at_4kb, at_16kb;   // 128, 128, 32 entries (Table 4)

    // --- 2. Pattern Storage (PHTs) (Table 4) ---
    struct PHTEntry {
        bool valid = false;
        uint32_t trigger_id = 0;
        bool is_pc_addr = false;       // Distinguishes (PC, address) vs (PC, offset) in 2KB
        std::bitset<256> pattern;
        uint64_t lru_cycle = 0;
    };

    std::vector<PHTEntry> pht_2kb;            // 4096 entries (Table 4)
    std::vector<PHTEntry> pht_4kb;            // 1024 entries (Table 4)
    std::vector<PHTEntry> pht_16kb_onchip;    // 256 entries (or 4096 in ALL_ON_CHIP mode)

    // Off-chip 16 KB PHT (4096 entries x 32B = 128 KB) & Valid Map (4096 bits)
    std::array<std::bitset<256>, 4096> offchip_pht_16kb;
    std::bitset<4096> valid_map_16kb;

    // --- 3. Lookup Unit Circular FIFOs (Section 4.2, Pseudo-code 1) ---
    struct LookupFIFO {
        bool valid = false;
        uint64_t region_tag = 0;
        uint64_t oldest_pc = 0;
        bool already_triggered = false;
        std::vector<uint32_t> offsets;
        uint64_t last_touch = 0;
    };

    std::array<LookupFIFO, 2> fifos_4kb;      // 2 FIFOs, capacity 3
    std::array<LookupFIFO, 2> fifos_16kb;     // 2 FIFOs, capacity 5

    // --- 4. Region Arbitration & Inflight Prefetch Table (Figure 8, Table 4) ---
    struct InflightEntry {
        bool valid = false;
        uint64_t start_addr = 0;
        uint64_t end_addr = 0;
        int32_t prefetches_left = 0;
        RegionPriority priority = RegionPriority::NONE;
    };

    std::array<InflightEntry, 6> inflight_table; // 6 entries (Table 4)
    std::deque<uint64_t> prefetch_buffer;        // 320-entry Prefetch Buffer (Table 4)

    // --- 5. Asynchronous Off-Chip Metadata Control (Section 4.3) ---
    struct PendingOffchipRead {
        uint32_t trigger_id = 0;
        uint64_t region_tag = 0;
        uint64_t ready_cycle = 0;
        uint8_t late_mask_2kb = 0; // 8-bit mask tracking 2 KB sub-regions
    };

    std::deque<PendingOffchipRead> pending_offchip_reads;

    // --- Statistics ---
    uint64_t stat_pred_2kb_addr = 0;
    uint64_t stat_pred_2kb_off  = 0;
    uint64_t stat_pred_4kb      = 0;
    uint64_t stat_pred_16kb_on  = 0;
    uint64_t stat_pred_16kb_off = 0;
    uint64_t stat_arb_dropped   = 0;

    // --- Internal Helper Methods ---
    uint32_t hash_trigger(uint64_t pc, const std::vector<uint32_t>& offsets) const;
    uint32_t hash_pc_addr(uint64_t pc, uint64_t addr) const;

    void train_region(uint64_t pc, uint64_t address, uint64_t region_size,
                      size_t req_offsets, std::vector<FTEntry>& ft,
                      std::vector<ATEntry>& at, std::vector<PHTEntry>& pht,
                      bool is_2kb, bool is_16kb);

    void commit_to_pht(const ATEntry& at_entry, std::vector<PHTEntry>& pht,
                       bool is_2kb, bool is_16kb);

    PrefetchCandidate lookup_2kb(uint64_t pc, uint64_t address);
    PrefetchCandidate lookup_multi_offset(uint64_t pc, uint64_t address,
                                          uint64_t region_size, size_t fifo_cap,
                                          std::array<LookupFIFO, 2>& fifos,
                                          const std::vector<ATEntry>& at,
                                          std::vector<PHTEntry>& pht,
                                          RegionPriority prio, bool is_16kb);

    void do_region_arbitration(const PrefetchCandidate& req16,
                               const PrefetchCandidate& req4,
                               const PrefetchCandidate& req2);

    bool has_higher_or_equal_overlap(const PrefetchCandidate& win_req) const;
    void insert_inflight_and_enqueue(const PrefetchCandidate& win_req);

    void track_late_subregions(uint64_t address);
    void process_completed_offchip_reads();

public:
    RichPrefetcher(std::string type, RichMode _mode = RichMode::FULL_RICH,
                   uint64_t _offchip_lat = 180);
    ~RichPrefetcher() = default;

    void invoke_prefetcher(uint64_t pc, uint64_t address, uint8_t cache_hit,
                           uint8_t type, std::vector<uint64_t>& pref_addr) override;
    void register_fill(uint64_t address);
    void dump_stats() override;
    void print_config() override;
};

#endif // RICH_H