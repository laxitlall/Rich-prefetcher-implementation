#include "rich.h"
#include <algorithm>
#include <iostream>

RichPrefetcher::RichPrefetcher(std::string _type, RichMode _mode, uint64_t _offchip_lat)
    : Prefetcher(_type),
      mode(_mode),
      sim_cycle(0),
      offchip_latency_cycles(_offchip_lat),
      offchip_threshold(30) {
    // Initialize Table Sizes according to Table 4 of the RICH paper
    ft_2kb.resize(64);
    ft_4kb.resize(64);
    ft_16kb.resize(64);

    at_2kb.resize(128);
    at_4kb.resize(128);
    at_16kb.resize(32);

    pht_2kb.resize(4096);
    pht_4kb.resize(1024);

    if (mode == RichMode::ALL_ON_CHIP || mode == RichMode::REGION_16KB_ONLY ||
        mode == RichMode::NAIVE_MULTI_REGION || mode == RichMode::MULTI_OFFSET_ONLY) {
        pht_16kb_onchip.resize(4096); // All-on-chip configurations
    } else {
        pht_16kb_onchip.resize(256);  // 256-entry on-chip PHT Cache (Table 4)
    }

    valid_map_16kb.reset();
}

// 12-bit hash combining oldest PC and collected offsets (Pseudo-code 1, Line 26)
uint32_t RichPrefetcher::hash_trigger(uint64_t pc, const std::vector<uint32_t>& offsets) const {
    uint64_t h = (pc >> 2) ^ (pc >> 14);
    for (uint32_t off : offsets) {
        h = ((h << 5) | (h >> 59)) ^ (off * 0x9e3779b97f4a7c15ULL);
    }
    return static_cast<uint32_t>((h ^ (h >> 12) ^ (h >> 24)) & 0xFFF); // 12 bits (0..4095)
}

// 12-bit hash for Bingo-style (PC, address) trigger in 2 KB region
uint32_t RichPrefetcher::hash_pc_addr(uint64_t pc, uint64_t addr) const {
    uint64_t line_addr = addr >> 6;
    uint64_t h = (pc >> 2) ^ (line_addr * 0x517cc1b727220a95ULL);
    return static_cast<uint32_t>((h ^ (h >> 12) ^ (h >> 24)) & 0xFFF);
}

// Commit finalized AT pattern to PHT (and spill to off-chip 16KB PHT if evicted)
void RichPrefetcher::commit_to_pht(const ATEntry& at_entry, std::vector<PHTEntry>& pht,
                                   bool is_2kb, bool is_16kb) {
    if (at_entry.pattern.count() <= 1) return; // Ignore empty/single-line noise

    auto insert_one = [&](uint32_t trig_id, bool pc_addr_flag) {
        // 1. Check if trigger already exists in PHT
        for (auto& entry : pht) {
            if (entry.valid && entry.trigger_id == trig_id && entry.is_pc_addr == pc_addr_flag) {
                entry.pattern = at_entry.pattern;
                entry.lru_cycle = sim_cycle;
                return;
            }
        }
        // 2. Find invalid or LRU victim
        auto victim = std::min_element(pht.begin(), pht.end(),
            [](const PHTEntry& a, const PHTEntry& b) {
                if (!a.valid && b.valid) return true;
                if (a.valid && !b.valid) return false;
                return a.lru_cycle < b.lru_cycle;
            });

        // 3. Off-chip spilling logic for 16 KB PHT Cache eviction (Section 4.1)
        if (is_16kb && mode == RichMode::FULL_RICH && victim->valid) {
            if (victim->pattern.count() >= offchip_threshold) {
                uint32_t idx = victim->trigger_id & 0xFFF;
                offchip_pht_16kb[idx] = victim->pattern;
                valid_map_16kb.set(idx);
            }
        }

        victim->valid = true;
        victim->trigger_id = trig_id;
        victim->is_pc_addr = pc_addr_flag;
        victim->pattern = at_entry.pattern;
        victim->lru_cycle = sim_cycle;
    };

    insert_one(at_entry.trigger_id, false);
    if (is_2kb) {
        insert_one(at_entry.addr_trigger_id, true);
    }
}

// Section 4.1 & Figure 7: Two-stage FT -> AT Training Process
void RichPrefetcher::train_region(uint64_t pc, uint64_t address, uint64_t region_size,
                                  size_t req_offsets, std::vector<FTEntry>& ft,
                                  std::vector<ATEntry>& at, std::vector<PHTEntry>& pht,
                                  bool is_2kb, bool is_16kb) {
    uint64_t region_tag = address / region_size;
    uint32_t offset = (address % region_size) >> 6;

    // 1. Check if region is already accumulating in AT (Figure 7, Step 4)
    for (auto& at_entry : at) {
        if (at_entry.valid && at_entry.region_tag == region_tag) {
            at_entry.pattern.set(offset);
            at_entry.lru_cycle = sim_cycle;
            return;
        }
    }

    // 2. Check if region is being filtered in FT (Figure 7, Steps 1-3)
    for (auto& ft_entry : ft) {
        if (ft_entry.valid && ft_entry.region_tag == region_tag) {
            ft_entry.lru_cycle = sim_cycle;
            if (std::find(ft_entry.offsets.begin(), ft_entry.offsets.end(), offset) ==
                ft_entry.offsets.end()) {
                if (ft_entry.offsets.size() < req_offsets) {
                    ft_entry.offsets.push_back(offset);
                } else {
                    // Observed (req_offsets + 1)-th distinct offset -> Promote to AT!
                    auto at_victim = std::min_element(at.begin(), at.end(),
                        [](const ATEntry& a, const ATEntry& b) {
                            if (!a.valid && b.valid) return true;
                            if (a.valid && !b.valid) return false;
                            return a.lru_cycle < b.lru_cycle;
                        });

                    if (at_victim->valid) {
                        commit_to_pht(*at_victim, pht, is_2kb, is_16kb);
                    }

                    at_victim->valid = true;
                    at_victim->region_tag = region_tag;
                    at_victim->trigger_id = hash_trigger(ft_entry.pc, ft_entry.offsets);
                    at_victim->addr_trigger_id = hash_pc_addr(ft_entry.pc, ft_entry.first_addr);
                    at_victim->pattern.reset();
                    for (uint32_t o : ft_entry.offsets) {
                        at_victim->pattern.set(o);
                    }
                    at_victim->pattern.set(offset);
                    at_victim->lru_cycle = sim_cycle;

                    ft_entry.valid = false; // Clear FT entry after promotion
                }
            }
            return;
        }
    }

    // 3. New untracked region -> Insert into FT (Figure 7, Step 1)
    auto ft_victim = std::min_element(ft.begin(), ft.end(),
        [](const FTEntry& a, const FTEntry& b) {
            if (!a.valid && b.valid) return true;
            if (a.valid && !b.valid) return false;
            return a.lru_cycle < b.lru_cycle;
        });

    ft_victim->valid = true;
    ft_victim->region_tag = region_tag;
    ft_victim->pc = pc;
    ft_victim->first_addr = address;
    ft_victim->offsets.clear();
    ft_victim->offsets.push_back(offset);
    ft_victim->lru_cycle = sim_cycle;
}

// 2 KB Lookup: Checks (PC, address) first, then (PC, offset) (Section 3.1 & Figure 8)
PrefetchCandidate RichPrefetcher::lookup_2kb(uint64_t pc, uint64_t address) {
    PrefetchCandidate cand;
    uint64_t region_tag = address / 2048;
    uint32_t offset = (address % 2048) >> 6;

    // Filter out if AT is currently learning this region (Pseudo-code 1, Line 24)
    for (const auto& at_e : at_2kb) {
        if (at_e.valid && at_e.region_tag == region_tag) return cand;
    }

    // 1. Check (PC, address) trigger first (Highest Priority)
    if (mode != RichMode::REGION_4KB_ONLY && mode != RichMode::REGION_16KB_ONLY) {
        uint32_t addr_trig = hash_pc_addr(pc, address);
        for (auto& entry : pht_2kb) {
            if (entry.valid && entry.is_pc_addr && entry.trigger_id == addr_trig) {
                entry.lru_cycle = sim_cycle;
                cand.valid = true;
                cand.region_base_addr = region_tag * 2048;
                cand.region_size_bytes = 2048;
                cand.priority = RegionPriority::PRIO_2KB_PC_ADDR;
                cand.pattern = entry.pattern;
                cand.pattern.reset(offset);
                cand.num_lines = 32;
                stat_pred_2kb_addr++;
                return cand;
            }
        }
    }

    // 2. Fallback to (PC, 1 offset) trigger
    uint32_t off_trig = hash_trigger(pc, {offset});
    for (auto& entry : pht_2kb) {
        if (entry.valid && !entry.is_pc_addr && entry.trigger_id == off_trig) {
            entry.lru_cycle = sim_cycle;
            cand.valid = true;
            cand.region_base_addr = region_tag * 2048;
            cand.region_size_bytes = 2048;
            cand.priority = RegionPriority::PRIO_2KB_PC_OFF;
            cand.pattern = entry.pattern;
            cand.pattern.reset(offset);
            cand.num_lines = 32;
            stat_pred_2kb_off++;
            return cand;
        }
    }
    return cand;
}

// 4 KB & 16 KB Multi-Offset Lookup (Pseudo-code 1, Lines 12-36)
PrefetchCandidate RichPrefetcher::lookup_multi_offset(
    uint64_t pc, uint64_t address, uint64_t region_size, size_t fifo_cap,
    std::array<LookupFIFO, 2>& fifos, const std::vector<ATEntry>& at,
    std::vector<PHTEntry>& pht, RegionPriority prio, bool is_16kb) {

    PrefetchCandidate cand;
    uint64_t region_tag = address / region_size;
    uint32_t offset = (address % region_size) >> 6;

    // Step P1: Accumulate observations in circular FIFO buffer (Lines 16-23)
    LookupFIFO* active_fifo = nullptr;
    for (auto& f : fifos) {
        if (f.valid && f.region_tag == region_tag) {
            active_fifo = &f;
            break;
        }
    }

    if (!active_fifo) {
        // Select FIFO slot to reset and track new region_tag (Lines 19-20)
        active_fifo = (fifos[0].last_touch <= fifos[1].last_touch) ? &fifos[0] : &fifos[1];
        active_fifo->valid = true;
        active_fifo->region_tag = region_tag;
        active_fifo->oldest_pc = pc;
        active_fifo->already_triggered = false;
        active_fifo->offsets.clear();
    }

    active_fifo->last_touch = sim_cycle;
    if (active_fifo->already_triggered) return cand;

    if (std::find(active_fifo->offsets.begin(), active_fifo->offsets.end(), offset) ==
        active_fifo->offsets.end()) {
        if (active_fifo->offsets.empty()) active_fifo->oldest_pc = pc;
        active_fifo->offsets.push_back(offset);
    }

    // Wait until FIFO collects all required offsets (Line 23)
    if (active_fifo->offsets.size() < fifo_cap) return cand;
    active_fifo->already_triggered = true;

    // Do not trigger if Training Unit is currently learning region_tag (Line 24)
    for (const auto& at_e : at) {
        if (at_e.valid && at_e.region_tag == region_tag) return cand;
    }

    // Step P2: Hash collected offsets and query on-chip PHT (Lines 25-29)
    uint32_t trigger_id = hash_trigger(active_fifo->oldest_pc, active_fifo->offsets);
    for (auto& entry : pht) {
        if (entry.valid && entry.trigger_id == trigger_id) {
            entry.lru_cycle = sim_cycle;
            cand.valid = true;
            cand.region_base_addr = region_tag * region_size;
            cand.region_size_bytes = region_size;
            cand.priority = prio;
            cand.pattern = entry.pattern;
            for (uint32_t o : active_fifo->offsets) cand.pattern.reset(o);
            cand.num_lines = region_size >> 6;
            if (is_16kb) stat_pred_16kb_on++;
            else stat_pred_4kb++;
            return cand;
        }
    }

    // Step M: On-chip miss -> Check Off-Chip 16 KB PHT (Lines 30-35)
    if (is_16kb && mode == RichMode::FULL_RICH) {
        uint32_t offchip_idx = trigger_id & 0xFFF;
        if (valid_map_16kb.test(offchip_idx)) {
            bool already_pending = false;
            for (const auto& req : pending_offchip_reads) {
                if (req.region_tag == region_tag) {
                    already_pending = true;
                    break;
                }
            }
            if (!already_pending) {
                PendingOffchipRead req;
                req.trigger_id = offchip_idx;
                req.region_tag = region_tag;
                req.ready_cycle = sim_cycle + offchip_latency_cycles;
                req.late_mask_2kb = 0;
                pending_offchip_reads.push_back(req);
            }
        }
    }

    return cand;
}

// Level 2 Arbitration: Check overlap against 6-entry Inflight Prefetch Table (Figure 8)
bool RichPrefetcher::has_higher_or_equal_overlap(const PrefetchCandidate& win_req) const {
    uint64_t req_start = win_req.region_base_addr;
    uint64_t req_end = req_start + win_req.region_size_bytes;

    for (const auto& entry : inflight_table) {
        if (entry.valid && entry.prefetches_left > 0) {
            bool overlaps = (std::max(req_start, entry.start_addr) <
                             std::min(req_end, entry.end_addr));
            // Lower enum integer = higher priority
            if (overlaps && static_cast<uint8_t>(entry.priority) <=
                            static_cast<uint8_t>(win_req.priority)) {
                return true;
            }
        }
    }
    return false;
}

// Insert winning request into Inflight Prefetch Table & Prefetch Buffer
void RichPrefetcher::insert_inflight_and_enqueue(const PrefetchCandidate& win_req) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < win_req.num_lines; i++) {
        if (win_req.pattern.test(i)) {
            uint64_t pf_addr = win_req.region_base_addr + (static_cast<uint64_t>(i) << 6);
            if (prefetch_buffer.size() < 320) {
                prefetch_buffer.push_back(pf_addr);
                count++;
            }
        }
    }
    if (count == 0) return;

    // Allocate slot in 6-entry Inflight Prefetch Table
    InflightEntry* slot = &inflight_table[0];
    for (auto& entry : inflight_table) {
        if (!entry.valid || entry.prefetches_left <= 0) {
            slot = &entry;
            break;
        }
        if (entry.prefetches_left < slot->prefetches_left) {
            slot = &entry;
        }
    }

    slot->valid = true;
    slot->start_addr = win_req.region_base_addr;
    slot->end_addr = win_req.region_base_addr + win_req.region_size_bytes;
    slot->prefetches_left = static_cast<int32_t>(count);
    slot->priority = win_req.priority;
}

// Step P3: Two-Level Region Arbitration (Pseudo-code 1, Lines 37-40 & Figure 8)
void RichPrefetcher::do_region_arbitration(const PrefetchCandidate& req16,
                                           const PrefetchCandidate& req4,
                                           const PrefetchCandidate& req2) {
    // If arbitration is disabled (Configs 4 & 5 in Fig. 15), enqueue all valid requests
    if (mode == RichMode::NAIVE_MULTI_REGION || mode == RichMode::MULTI_OFFSET_ONLY) {
        if (req16.valid) insert_inflight_and_enqueue(req16);
        if (req4.valid)  insert_inflight_and_enqueue(req4);
        if (req2.valid)  insert_inflight_and_enqueue(req2);
        return;
    }

    // Level 1: Fixed Priority Selection
    // Order: 2KB (PC, addr) > 16KB > 4KB > 2KB (PC, offset)
    PrefetchCandidate win_req;
    if (req2.valid && req2.priority == RegionPriority::PRIO_2KB_PC_ADDR) {
        win_req = req2;
    } else if (req16.valid) {
        win_req = req16;
    } else if (req4.valid) {
        win_req = req4;
    } else if (req2.valid) {
        win_req = req2;
    } else {
        return;
    }

    // Level 2: Inflight Prefetch Table overlap check
    if (has_higher_or_equal_overlap(win_req)) {
        stat_arb_dropped++;
        return;
    }

    insert_inflight_and_enqueue(win_req);
}

// Section 4.3: Track CPU accesses at 2 KB sub-region granularity (8 bits per 16 KB region)
void RichPrefetcher::track_late_subregions(uint64_t address) {
    uint64_t region_tag_16kb = address / 16384;
    uint32_t subregion_2kb = (address % 16384) / 2048; // 0..7
    for (auto& req : pending_offchip_reads) {
        if (req.region_tag == region_tag_16kb) {
            req.late_mask_2kb |= (1U << subregion_2kb);
        }
    }
}

// Section 4.3: Retire completed asynchronous off-chip metadata loads
void RichPrefetcher::process_completed_offchip_reads() {
    while (!pending_offchip_reads.empty() &&
           pending_offchip_reads.front().ready_cycle <= sim_cycle) {
        PendingOffchipRead req = pending_offchip_reads.front();
        pending_offchip_reads.pop_front();

        std::bitset<256> pattern = offchip_pht_16kb[req.trigger_id];

        // 1. Save fetched pattern into On-chip 16 KB PHT Cache
        ATEntry dummy_at;
        dummy_at.valid = true;
        dummy_at.region_tag = req.region_tag;
        dummy_at.trigger_id = req.trigger_id;
        dummy_at.pattern = pattern;
        commit_to_pht(dummy_at, pht_16kb_onchip, false, true);

        // 2. Trim marked 2 KB sub-regions of late prefetches (32 bits per 2 KB chunk)
        for (int sub = 0; sub < 8; sub++) {
            if (req.late_mask_2kb & (1U << sub)) {
                for (int bit = sub * 32; bit < (sub + 1) * 32; bit++) {
                    pattern.reset(bit);
                }
            }
        }

        // 3. Callback to Do_RegionArbitration(req16KB, None, None)
        PrefetchCandidate cand16;
        cand16.valid = true;
        cand16.region_base_addr = req.region_tag * 16384;
        cand16.region_size_bytes = 16384;
        cand16.priority = RegionPriority::PRIO_16KB;
        cand16.pattern = pattern;
        cand16.num_lines = 256;
        stat_pred_16kb_off++;

        PrefetchCandidate empty_cand;
        do_region_arbitration(cand16, empty_cand, empty_cand);
    }
}

// Main L2 Prefetcher Entry Point in Pythia
void RichPrefetcher::invoke_prefetcher(uint64_t pc, uint64_t address, uint8_t cache_hit,
                                       uint8_t type, std::vector<uint64_t>& pref_addr) {
    sim_cycle++;

    // Periodically clear Valid Map every 250K accesses to invalidate stale off-chip patterns
    if (sim_cycle % 250000 == 0) {
        valid_map_16kb.reset();
    }

    // 1. Process any asynchronous off-chip 16 KB metadata reads
    if (mode == RichMode::FULL_RICH) {
        track_late_subregions(address);
        process_completed_offchip_reads();
    }

    // Configure offset counts based on experiment mode (Fig. 15)
    bool use_multi_offset = (mode == RichMode::MULTI_OFFSET_ONLY ||
                             mode == RichMode::ALL_ON_CHIP ||
                             mode == RichMode::FULL_RICH);
    size_t offsets_4kb  = use_multi_offset ? 3 : 1;
    size_t offsets_16kb = use_multi_offset ? 5 : 1;

    bool enable_2kb  = (mode != RichMode::REGION_4KB_ONLY && mode != RichMode::REGION_16KB_ONLY);
    bool enable_4kb  = (mode != RichMode::REGION_2KB_ONLY && mode != RichMode::REGION_16KB_ONLY);
    bool enable_16kb = (mode != RichMode::REGION_2KB_ONLY && mode != RichMode::REGION_4KB_ONLY);

    // 2. Prediction Unit Lookups (Pseudo-code 1, Lines 3-6)
    PrefetchCandidate req16, req4, req2;
    if (enable_16kb) {
        req16 = lookup_multi_offset(pc, address, 16384, offsets_16kb, fifos_16kb,
                                    at_16kb, pht_16kb_onchip, RegionPriority::PRIO_16KB, true);
    }
    if (enable_4kb) {
        req4 = lookup_multi_offset(pc, address, 4096, offsets_4kb, fifos_4kb,
                                   at_4kb, pht_4kb, RegionPriority::PRIO_4KB, false);
    }
    if (enable_2kb) {
        req2 = lookup_2kb(pc, address);
    }

    if (req16.valid || req4.valid || req2.valid) {
        do_region_arbitration(req16, req4, req2);
    }

    // 3. Training Unit Updates (Section 4.1)
    if (enable_16kb) {
        train_region(pc, address, 16384, offsets_16kb, ft_16kb, at_16kb, pht_16kb_onchip, false, true);
    }
    if (enable_4kb) {
        train_region(pc, address, 4096, offsets_4kb, ft_4kb, at_4kb, pht_4kb, false, false);
    }
    if (enable_2kb) {
        train_region(pc, address, 2048, 1, ft_2kb, at_2kb, pht_2kb, true, false);
    }

    // 4. Drain Prefetch Buffer to L2 Cache & Update Inflight Prefetch Table Progress
    while (!prefetch_buffer.empty()) {
        uint64_t pf_line_addr = prefetch_buffer.front();
        prefetch_buffer.pop_front();
        pref_addr.push_back(pf_line_addr);
        register_fill(pf_line_addr);
    }
}

// Decrement Inflight Prefetch Table counter when prefetches are issued/filled (Figure 8)
void RichPrefetcher::register_fill(uint64_t address) {
    for (auto& entry : inflight_table) {
        if (entry.valid && address >= entry.start_addr && address < entry.end_addr) {
            entry.prefetches_left--;
            if (entry.prefetches_left <= 0) {
                entry.valid = false; // Retire entry when counter hits zero
            }
        }
    }
}

void RichPrefetcher::dump_stats() {
    std::cout << "=== RICH Prefetcher Stats ===" << std::endl;
    std::cout << "rich_pred_2kb_pc_addr: " << stat_pred_2kb_addr << std::endl;
    std::cout << "rich_pred_2kb_pc_off:  " << stat_pred_2kb_off << std::endl;
    std::cout << "rich_pred_4kb:         " << stat_pred_4kb << std::endl;
    std::cout << "rich_pred_16kb_onchip: " << stat_pred_16kb_on << std::endl;
    std::cout << "rich_pred_16kb_offchip:" << stat_pred_16kb_off << std::endl;
    std::cout << "rich_arb_dropped:      " << stat_arb_dropped << std::endl;
}

void RichPrefetcher::print_config() {
    std::cout << "=== RICH Prefetcher Config ===" << std::endl;
    std::cout << "Mode: " << static_cast<int>(mode) << std::endl;
    std::cout << "Off-chip Latency Cycles: " << offchip_latency_cycles << std::endl;
    std::cout << "Off-chip Popcount Threshold: " << offchip_threshold << std::endl;
}