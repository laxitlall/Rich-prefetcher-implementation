#include <iostream>
#include <math.h>
#include <algorithm>
#include <iomanip>
#include <assert.h>
#include <strings.h>
#include "rich.h"
#include "champsim.h"

using namespace std;

using FTEntry = RichPrefetcher::FTEntry;
using ATEntry = RichPrefetcher::ATEntry;
using PHTEntry = RichPrefetcher::PHTEntry;

const unsigned long long magic_constant = 0x517cc1b727220a95ULL;

namespace knob
{
	/* Initialized directly from Table 4 of the RICH paper (2KB region baseline) */
	uint32_t rich_ft_size = 64;
	uint32_t rich_at_size = 128;
	uint32_t rich_pht_size = 4096;
	uint32_t rich_pht_assoc = 16;
	uint32_t rich_pref_degree = 4;
	uint32_t rich_region_size = 2048;
	uint32_t rich_region_size_log = 11;
	bool     rich_enable_pref_buffer = true;
	uint32_t rich_pref_buffer_size = 320;
}

void RichPrefetcher::init_knobs()
{
	pht_sets = knob::rich_pht_size / knob::rich_pht_assoc;
}

void RichPrefetcher::init_stats()
{
	bzero(&stats, sizeof(stats));
}

void RichPrefetcher::print_config()
{
	cout << "rich_at_size " << knob::rich_at_size << endl
		<< "rich_ft_size " << knob::rich_ft_size << endl
		<< "rich_pht_size " << knob::rich_pht_size << endl
		<< "rich_pht_assoc " << knob::rich_pht_assoc << endl
		<< "rich_pref_degree " << knob::rich_pref_degree << endl
		<< "rich_region_size " << knob::rich_region_size << endl
		<< "rich_region_size_log " << knob::rich_region_size_log << endl
		<< "rich_enable_pref_buffer " << knob::rich_enable_pref_buffer << endl
		<< "rich_pref_buffer_size " << knob::rich_pref_buffer_size << endl
		<< endl;
}

RichPrefetcher::RichPrefetcher(string type) : Prefetcher(type)
{
	init_knobs();
	init_stats();
	print_config();

	/* init PHT */
	deque<PHTEntry*> d;
	pht.resize(pht_sets, d);
}

RichPrefetcher::~RichPrefetcher()
{

}

void RichPrefetcher::invoke_prefetcher(uint64_t pc, uint64_t address, uint8_t cache_hit, uint8_t type, vector<uint64_t> &pref_addr)
{
	uint64_t page = address >> knob::rich_region_size_log;
	uint32_t offset = (address >> LOG2_BLOCK_SIZE) & ((1ull << (knob::rich_region_size_log - LOG2_BLOCK_SIZE)) - 1);

	auto at_index = search_acc_table(page);
	stats.at.lookup++;
	if(at_index != acc_table.end())
	{
		/* accumulation table hit */
		stats.at.hit++;
		(*at_index)->pattern[offset] = 1;
		update_age_acc_table(at_index);
	}
	else
	{
		/* search filter table */
		auto ft_index = search_filter_table(page);
		stats.ft.lookup++;
		if(ft_index != filter_table.end())
		{
			/* filter table hit */
			stats.ft.hit++;
            //need to check for differrent offsets for 4kb and others
			insert_acc_table((*ft_index), offset);
			evict_filter_table(ft_index);
		}
		else
		{
			/* filter table miss. Beginning of new generation. Issue prefetch */
			insert_filter_table(pc, address, page, offset);
            // even for ft hit, we need to generate prefetches for 4kb and otehrs
			generate_prefetch(pc, address, page, offset, pref_addr);
			if(knob::rich_enable_pref_buffer)
			{
				buffer_prefetch(pref_addr);
				pref_addr.clear();
			}
		}
	}

	/* slowly inject prefetches at every demand access, if buffer is turned on */
	if(knob::rich_enable_pref_buffer)
	{
		issue_prefetch(pref_addr);
	}
}

/* Functions for Filter table */
deque<FTEntry*>::iterator RichPrefetcher::search_filter_table(uint64_t page)
{
	return find_if(filter_table.begin(), filter_table.end(), [page](FTEntry *ftentry){return (ftentry->page == page);});
}

void RichPrefetcher::insert_filter_table(uint64_t pc, uint64_t address, uint64_t page, uint32_t offset)
{
	stats.ft.insert++;
	FTEntry *ftentry = NULL;
	if(filter_table.size() >= knob::rich_ft_size)
	{
		auto victim = search_victim_filter_table();
		evict_filter_table(victim);
	}

	ftentry = new FTEntry();
	ftentry->page = page;
	ftentry->pc = pc;
	ftentry->first_addr = address;
	ftentry->trigger_offset = offset;
	filter_table.push_back(ftentry);
}

deque<FTEntry*>::iterator RichPrefetcher::search_victim_filter_table()
{
	return filter_table.begin();
}

void RichPrefetcher::evict_filter_table(deque<FTEntry*>::iterator victim)
{
	stats.ft.evict++;
	FTEntry *ftentry = (*victim);
	filter_table.erase(victim);
	delete ftentry;
}

/* Functions for Accumulation Table */
deque<ATEntry*>::iterator RichPrefetcher::search_acc_table(uint64_t page)
{
	return find_if(acc_table.begin(), acc_table.end(), [page](ATEntry *atentry){return (atentry->page == page);});	
}

void RichPrefetcher::insert_acc_table(FTEntry *ftentry, uint32_t offset)
{
	stats.at.insert++;
	ATEntry *atentry = NULL;
	if(acc_table.size() >= knob::rich_at_size)
	{
		auto victim = search_victim_acc_table();
		evict_acc_table(victim);
	}

	atentry = new ATEntry();
	atentry->pc = ftentry->pc;
	atentry->first_addr = ftentry->first_addr;
	atentry->page = ftentry->page;
	atentry->trigger_offset = ftentry->trigger_offset;
	atentry->pattern[ftentry->trigger_offset] = 1;
	atentry->pattern[offset] = 1;
	atentry->age = 0;
	for(uint32_t index = 0; index < acc_table.size(); ++index) acc_table[index]->age++;
	acc_table.push_back(atentry);
}

deque<ATEntry*>::iterator RichPrefetcher::search_victim_acc_table()
{
	uint32_t max_age = 0;
	deque<ATEntry*>::iterator it, victim;
	for(it = acc_table.begin(); it != acc_table.end(); ++it)
	{
		if((*it)->age >= max_age)
		{
			max_age = (*it)->age;
			victim = it;
		}
	}
	return victim;
}

void RichPrefetcher::evict_acc_table(deque<ATEntry*>::iterator victim)
{
	stats.at.evict++;
	ATEntry *atentry = (*victim);
	insert_pht_table(atentry);
	acc_table.erase(victim);
	delete(atentry);
}

void RichPrefetcher::update_age_acc_table(deque<ATEntry*>::iterator current)
{
	for(auto it = acc_table.begin(); it != acc_table.end(); ++it)
	{
		(*it)->age++;
	}
	(*current)->age = 0;
}

/* Functions for Pattern History Table */
void RichPrefetcher::insert_pht_entry(uint64_t signature, bool is_pc_addr, const Bitmap &pattern)
{
	stats.pht.lookup++;
	int32_t set = -1;
	auto pht_index = search_pht(signature, is_pc_addr, &set);
	assert(set != -1);
	if(pht_index != pht[set].end())
	{
		/* PHT hit */
		stats.pht.hit++;
		(*pht_index)->pattern = pattern;
		update_age_pht(set, pht_index);
	}
	else
	{
		/* PHT miss */
        assert(set != -1);
		if(pht[set].size() >= knob::rich_pht_assoc)
		{
			auto victim = search_victim_pht(set);
			evcit_pht(set, victim);
		}

		stats.pht.insert++;
		PHTEntry *phtentry = new PHTEntry();
		phtentry->signature = signature;
		phtentry->is_pc_addr = is_pc_addr;
		phtentry->pattern = pattern;
		phtentry->age = 0;
		for(uint32_t index = 0; index < pht[set].size(); ++index) pht[set][index]->age++;
		pht[set].push_back(phtentry);
	}
}

void RichPrefetcher::insert_pht_table(ATEntry *atentry)
{
	uint64_t pc_addr_sig = create_pc_addr_signature(atentry->pc, atentry->first_addr);
	insert_pht_entry(pc_addr_sig, true, atentry->pattern);

	uint64_t pc_off_sig = create_signature(atentry->pc, atentry->trigger_offset);
	insert_pht_entry(pc_off_sig, false, atentry->pattern);
}

deque<PHTEntry*>::iterator RichPrefetcher::search_pht(uint64_t signature, bool is_pc_addr, int32_t *set)
{
	(*set) = signature % pht_sets;
	return find_if(pht[(*set)].begin(), pht[(*set)].end(), [signature, is_pc_addr](PHTEntry *phtentry){
		return (phtentry->signature == signature && phtentry->is_pc_addr == is_pc_addr);
	});	
}

deque<PHTEntry*>::iterator RichPrefetcher::search_victim_pht(int32_t set)
{
	uint32_t max_age = 0;
	deque<PHTEntry*>::iterator it, victim;
	for(it = pht[set].begin(); it != pht[set].end(); ++it)
	{
		if((*it)->age >= max_age)
		{
			max_age = (*it)->age;
			victim = it;
		}
	}
	return victim;
}

void RichPrefetcher::update_age_pht(int32_t set, deque<PHTEntry*>::iterator current)
{
	for(auto it = pht[set].begin(); it != pht[set].end(); ++it)
	{
		(*it)->age++;
	}
	(*current)->age = 0;
}

void RichPrefetcher::evcit_pht(int32_t set, deque<PHTEntry*>::iterator victim)
{
	stats.pht.evict++;
	PHTEntry *phtentry = (*victim);
	pht[set].erase(victim);
	delete phtentry;
}

uint64_t RichPrefetcher::create_signature(uint64_t pc, uint32_t offset)
{
	uint64_t signature = pc;
	signature = (signature << (knob::rich_region_size_log - LOG2_BLOCK_SIZE));
	signature += (uint64_t)offset;
	return signature;
}

uint64_t RichPrefetcher::create_pc_addr_signature(uint64_t pc, uint64_t address)
{
	uint64_t offset_bits = knob::rich_region_size_log - LOG2_BLOCK_SIZE; /* 5 bits for 2KB */
	uint64_t pc_width = 16;
	uint64_t max_addr_width = 16;

	uint64_t block_addr = address >> LOG2_BLOCK_SIZE;
	pc &= (1ULL << pc_width) - 1;
	block_addr &= (1ULL << max_addr_width) - 1;

	uint64_t offset = block_addr & ((1ULL << offset_bits) - 1);
	uint64_t base = block_addr >> offset_bits;
	return (base << (pc_width + offset_bits)) | (pc << offset_bits) | offset;
}

int RichPrefetcher::generate_prefetch(uint64_t pc, uint64_t address, uint64_t page, uint32_t offset, vector<uint64_t> &pref_addr)
{
	stats.generate_prefetch.called++;
	int32_t set = -1;

    //first check for pc_addr signature athen check for oc + offset
    uint64_t pc_addr_sig = create_pc_addr_signature(pc, address);
    auto pht_index = search_pht(pc_addr_sig, true, &set);
    if(pht_index == pht[set].end()){
        uint64_t pc_off_sig = create_signature(pc, offset);
        pht_index = search_pht(pc_off_sig, false, &set);
        if(pht_index == pht[set].end()){
            stats.generate_prefetch.pht_miss++;
            return 0;
        }
        else{
            stats.generate_prefetch.pc_off_hit++;
        }
    }
    else{
        stats.generate_prefetch.pc_addr_hit++;
    }

	PHTEntry *phtentry = (*pht_index);
	for(uint32_t index = 0; index < BITMAP_MAX_SIZE; ++index)
	{
		if(phtentry->pattern[index] && offset != index)
		{
			uint64_t addr = (page << knob::rich_region_size_log) + (index << LOG2_BLOCK_SIZE);
			pref_addr.push_back(addr);
		}
	}
	update_age_pht(set, pht_index);
	stats.generate_prefetch.pref_generated += pref_addr.size();
	return pref_addr.size();
}

void RichPrefetcher::buffer_prefetch(vector<uint64_t> pref_addr)
{
	uint32_t count = 0;
	for(uint32_t index = 0; index < pref_addr.size(); ++index)
	{
		if(pref_buffer.size() >= knob::rich_pref_buffer_size)
		{
			break;
		}
		pref_buffer.push_back(pref_addr[index]);
		count++;
	}
	stats.pref_buffer.buffered += count;
	stats.pref_buffer.spilled += (pref_addr.size() - count);
}

void RichPrefetcher::issue_prefetch(vector<uint64_t> &pref_addr)
{
	uint32_t count = 0;
	while(!pref_buffer.empty() && count < knob::rich_pref_degree)
	{
		pref_addr.push_back(pref_buffer.front());
		pref_buffer.pop_front();
		count++;
	}
	stats.pref_buffer.issued += pref_addr.size();
}

void RichPrefetcher::dump_stats()
{
	cout << "rich.ft.lookup " << stats.ft.lookup << endl
	<< "rich.ft.hit " << stats.ft.hit << endl
	<< "rich.ft.insert " << stats.ft.insert << endl
	<< "rich.ft.evict " << stats.ft.evict << endl
	<< "rich.at.lookup " << stats.at.lookup << endl
	<< "rich.at.hit " << stats.at.hit << endl
	<< "rich.at.insert " << stats.at.insert << endl
	<< "rich.at.evict " << stats.at.evict << endl
	<< "rich.pht.lookup " << stats.pht.lookup << endl
	<< "rich.pht.hit " << stats.pht.hit << endl
	<< "rich.pht.insert " << stats.pht.insert << endl
	<< "rich.pht.evict " << stats.pht.evict << endl
	<< "rich.generate_prefetch.called " << stats.generate_prefetch.called << endl
	<< "rich.generate_prefetch.pht_miss " << stats.generate_prefetch.pht_miss << endl
	<< "rich.generate_prefetch.pc_addr_hit " << stats.generate_prefetch.pc_addr_hit << endl
	<< "rich.generate_prefetch.pc_off_hit " << stats.generate_prefetch.pc_off_hit << endl
	<< "rich.generate_prefetch.pref_generated " << stats.generate_prefetch.pref_generated << endl
	<< "rich.pref_buffer.buffered " << stats.pref_buffer.buffered << endl
	<< "rich.pref_buffer.spilled " << stats.pref_buffer.spilled << endl
	<< "rich.pref_buffer.issued " << stats.pref_buffer.issued << endl
	<< endl;
}
