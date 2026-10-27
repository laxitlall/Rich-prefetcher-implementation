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
using LookupFIFO = RichPrefetcher::LookupFIFO;

const unsigned long long magic_constant = 0x517cc1b727220a95ULL;

namespace knob
{
	/* Initialized directly from Table 4 of the RICH paper (2KB & 4KB regions) */
	uint32_t rich_ft_size = 64;
	uint32_t rich_at_size = 128;
	uint32_t rich_pht_size = 4096;
	uint32_t rich_pht_assoc = 16;
	uint32_t rich_region_size = 2048;
	uint32_t rich_region_size_log = 11;

	/* 4KB Region Parameters (Table 4 of RICH paper) */
	uint32_t rich_ft_4kb_size = 64;
	uint32_t rich_at_4kb_size = 128;
	uint32_t rich_pht_4kb_size = 1024;
	uint32_t rich_pht_4kb_assoc = 16;
	uint32_t rich_region_4kb_size = 4096;
	uint32_t rich_region_4kb_size_log = 12;
	uint32_t rich_4kb_num_offsets = 3;
	uint32_t rich_lookup_fifo_4kb_size = 2;

	uint32_t rich_pref_degree = 4;
	bool     rich_enable_pref_buffer = true;
	uint32_t rich_pref_buffer_size = 320;
}

void RichPrefetcher::init_knobs()
{
	pht_sets = knob::rich_pht_size / knob::rich_pht_assoc;
	pht_4kb_sets = knob::rich_pht_4kb_size / knob::rich_pht_4kb_assoc;
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
		<< "rich_region_size " << knob::rich_region_size << endl
		<< "rich_region_size_log " << knob::rich_region_size_log << endl
		<< "rich_at_4kb_size " << knob::rich_at_4kb_size << endl
		<< "rich_ft_4kb_size " << knob::rich_ft_4kb_size << endl
		<< "rich_pht_4kb_size " << knob::rich_pht_4kb_size << endl
		<< "rich_pht_4kb_assoc " << knob::rich_pht_4kb_assoc << endl
		<< "rich_region_4kb_size " << knob::rich_region_4kb_size << endl
		<< "rich_region_4kb_size_log " << knob::rich_region_4kb_size_log << endl
		<< "rich_4kb_num_offsets " << knob::rich_4kb_num_offsets << endl
		<< "rich_pref_degree " << knob::rich_pref_degree << endl
		<< "rich_enable_pref_buffer " << knob::rich_enable_pref_buffer << endl
		<< "rich_pref_buffer_size " << knob::rich_pref_buffer_size << endl
		<< endl;
}

RichPrefetcher::RichPrefetcher(string type) : Prefetcher(type)
{
	init_knobs();
	init_stats();
	print_config();

	/* init PHTs */
	deque<PHTEntry*> d;
	pht.resize(pht_sets, d);
	pht_4kb.resize(pht_4kb_sets, d);
}

RichPrefetcher::~RichPrefetcher()
{

}

void RichPrefetcher::invoke_prefetcher(uint64_t pc, uint64_t address, uint8_t cache_hit, uint8_t type, vector<uint64_t> &pref_addr)
{
	/* ==================== 1. 2KB REGION ==================== */
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
			if((*ft_index)->trigger_offset != offset)
			{
				insert_acc_table((*ft_index), offset);
				evict_filter_table(ft_index);
			}
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

	/* ==================== 2. 4KB REGION ==================== */
	uint64_t page_4kb = address >> knob::rich_region_4kb_size_log;
	uint32_t offset_4kb = (address >> LOG2_BLOCK_SIZE) & ((1ull << (knob::rich_region_4kb_size_log - LOG2_BLOCK_SIZE)) - 1);

	/* 2a. 4KB Lookup FIFO: collect 3 distinct offsets and trigger PHT lookup on the 3rd offset */
	auto fifo_it = search_lookup_fifo_4kb(page_4kb);
	if(fifo_it == lookup_fifo_4kb.end())
	{
		fifo_it = insert_lookup_fifo_4kb(pc, page_4kb, offset_4kb);
	}
	else
	{
		update_age_lookup_fifo_4kb(fifo_it);
		if(!(*fifo_it)->already_triggered)
		{
			auto off_it = find((*fifo_it)->offsets.begin(), (*fifo_it)->offsets.end(), offset_4kb);
			if(off_it == (*fifo_it)->offsets.end())
			{
				(*fifo_it)->offsets.push_back(offset_4kb);
			}
		}
	}

	if(!(*fifo_it)->already_triggered && (*fifo_it)->offsets.size() == knob::rich_4kb_num_offsets)
	{
		(*fifo_it)->already_triggered = true;
		/* Only trigger if AT_4KB is not currently learning this 4KB region */
		if(search_acc_table_4kb(page_4kb) == acc_table_4kb.end())
		{
			generate_prefetch_4kb((*fifo_it)->pc, page_4kb, (*fifo_it)->offsets, pref_addr);
			if(knob::rich_enable_pref_buffer)
			{
				buffer_prefetch(pref_addr);
				pref_addr.clear();
			}
		}
	}

	/* 2b. 4KB Training: AT_4KB -> FT_4KB (collects 3 offsets, promotes to AT_4KB on 4th distinct offset) */
	auto at_4kb_index = search_acc_table_4kb(page_4kb);
	stats.at_4kb.lookup++;
	if(at_4kb_index != acc_table_4kb.end())
	{
		/* 4KB accumulation table hit */
		stats.at_4kb.hit++;
		(*at_4kb_index)->pattern[offset_4kb] = 1;
		update_age_acc_table_4kb(at_4kb_index);
	}
	else
	{
		/* search 4KB filter table */
		auto ft_4kb_index = search_filter_table_4kb(page_4kb);
		stats.ft_4kb.lookup++;
		if(ft_4kb_index != filter_table_4kb.end())
		{
			/* 4KB filter table hit */
			stats.ft_4kb.hit++;
			auto off_it = find((*ft_4kb_index)->offsets.begin(), (*ft_4kb_index)->offsets.end(), offset_4kb);
			if(off_it == (*ft_4kb_index)->offsets.end())
			{
				if((*ft_4kb_index)->offsets.size() < knob::rich_4kb_num_offsets)
				{
					/* Still collecting the 3 trigger offsets */
					(*ft_4kb_index)->offsets.push_back(offset_4kb);
				}
				else
				{
					/* 4th distinct offset observed -> promote to AT_4KB */
					insert_acc_table_4kb((*ft_4kb_index), offset_4kb);
					evict_filter_table_4kb(ft_4kb_index);
				}
			}
		}
		else
		{
			/* 4KB filter table miss -> insert 1st offset */
			insert_filter_table_4kb(pc, page_4kb, offset_4kb);
		}
	}

	/* slowly inject prefetches at every demand access, if buffer is turned on */
	if(knob::rich_enable_pref_buffer)
	{
		issue_prefetch(pref_addr);
	}
}

/* ==================== Functions for 2KB Filter Table ==================== */
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

/* ==================== Functions for 2KB Accumulation Table ==================== */
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

/* ==================== Functions for 2KB Pattern History Table ==================== */
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

/* ==================== Functions for 4KB Lookup FIFO ==================== */
deque<LookupFIFO*>::iterator RichPrefetcher::search_lookup_fifo_4kb(uint64_t page)
{
	return find_if(lookup_fifo_4kb.begin(), lookup_fifo_4kb.end(), [page](LookupFIFO *fifo){return (fifo->page == page);});
}

deque<LookupFIFO*>::iterator RichPrefetcher::search_victim_lookup_fifo_4kb()
{
	uint32_t max_age = 0;
	deque<LookupFIFO*>::iterator it, victim = lookup_fifo_4kb.begin();
	for(it = lookup_fifo_4kb.begin(); it != lookup_fifo_4kb.end(); ++it)
	{
		if((*it)->age >= max_age)
		{
			max_age = (*it)->age;
			victim = it;
		}
	}
	return victim;
}

void RichPrefetcher::evict_lookup_fifo_4kb(deque<LookupFIFO*>::iterator victim)
{
	LookupFIFO *fifo = (*victim);
	lookup_fifo_4kb.erase(victim);
	delete fifo;
}

deque<LookupFIFO*>::iterator RichPrefetcher::insert_lookup_fifo_4kb(uint64_t pc, uint64_t page, uint32_t offset)
{
	if(lookup_fifo_4kb.size() >= knob::rich_lookup_fifo_4kb_size)
	{
		auto victim = search_victim_lookup_fifo_4kb();
		evict_lookup_fifo_4kb(victim);
	}

	LookupFIFO *fifo = new LookupFIFO();
	fifo->page = page;
	fifo->pc = pc;
	fifo->offsets.push_back(offset);
	fifo->already_triggered = false;
	fifo->age = 0;
	for(uint32_t index = 0; index < lookup_fifo_4kb.size(); ++index) lookup_fifo_4kb[index]->age++;
	lookup_fifo_4kb.push_back(fifo);
	return lookup_fifo_4kb.end() - 1;
}

void RichPrefetcher::update_age_lookup_fifo_4kb(deque<LookupFIFO*>::iterator current)
{
	for(auto it = lookup_fifo_4kb.begin(); it != lookup_fifo_4kb.end(); ++it)
	{
		(*it)->age++;
	}
	(*current)->age = 0;
}

/* ==================== Functions for 4KB Filter Table ==================== */
deque<FTEntry*>::iterator RichPrefetcher::search_filter_table_4kb(uint64_t page)
{
	return find_if(filter_table_4kb.begin(), filter_table_4kb.end(), [page](FTEntry *ftentry){return (ftentry->page == page);});
}

void RichPrefetcher::insert_filter_table_4kb(uint64_t pc, uint64_t page, uint32_t offset)
{
	stats.ft_4kb.insert++;
	if(filter_table_4kb.size() >= knob::rich_ft_4kb_size)
	{
		auto victim = search_victim_filter_table_4kb();
		evict_filter_table_4kb(victim);
	}

	FTEntry *ftentry = new FTEntry();
	ftentry->page = page;
	ftentry->pc = pc;
	ftentry->offsets.push_back(offset);
	filter_table_4kb.push_back(ftentry);
}

deque<FTEntry*>::iterator RichPrefetcher::search_victim_filter_table_4kb()
{
	return filter_table_4kb.begin();
}

void RichPrefetcher::evict_filter_table_4kb(deque<FTEntry*>::iterator victim)
{
	stats.ft_4kb.evict++;
	FTEntry *ftentry = (*victim);
	filter_table_4kb.erase(victim);
	delete ftentry;
}

/* ==================== Functions for 4KB Accumulation Table ==================== */
deque<ATEntry*>::iterator RichPrefetcher::search_acc_table_4kb(uint64_t page)
{
	return find_if(acc_table_4kb.begin(), acc_table_4kb.end(), [page](ATEntry *atentry){return (atentry->page == page);});
}

void RichPrefetcher::insert_acc_table_4kb(FTEntry *ftentry, uint32_t offset)
{
	stats.at_4kb.insert++;
	if(acc_table_4kb.size() >= knob::rich_at_4kb_size)
	{
		auto victim = search_victim_acc_table_4kb();
		evict_acc_table_4kb(victim);
	}

	ATEntry *atentry = new ATEntry();
	atentry->pc = ftentry->pc;
	atentry->page = ftentry->page;
	atentry->offsets = ftentry->offsets;
	for(uint32_t i = 0; i < ftentry->offsets.size(); ++i)
	{
		atentry->pattern[ftentry->offsets[i]] = 1;
	}
	atentry->pattern[offset] = 1;
	atentry->age = 0;
	for(uint32_t index = 0; index < acc_table_4kb.size(); ++index) acc_table_4kb[index]->age++;
	acc_table_4kb.push_back(atentry);
}

deque<ATEntry*>::iterator RichPrefetcher::search_victim_acc_table_4kb()
{
	uint32_t max_age = 0;
	deque<ATEntry*>::iterator it, victim;
	for(it = acc_table_4kb.begin(); it != acc_table_4kb.end(); ++it)
	{
		if((*it)->age >= max_age)
		{
			max_age = (*it)->age;
			victim = it;
		}
	}
	return victim;
}

void RichPrefetcher::evict_acc_table_4kb(deque<ATEntry*>::iterator victim)
{
	stats.at_4kb.evict++;
	ATEntry *atentry = (*victim);
	insert_pht_table_4kb(atentry);
	acc_table_4kb.erase(victim);
	delete(atentry);
}

void RichPrefetcher::update_age_acc_table_4kb(deque<ATEntry*>::iterator current)
{
	for(auto it = acc_table_4kb.begin(); it != acc_table_4kb.end(); ++it)
	{
		(*it)->age++;
	}
	(*current)->age = 0;
}

/* ==================== Functions for 4KB Pattern History Table ==================== */
void RichPrefetcher::insert_pht_table_4kb(ATEntry *atentry)
{
	stats.pht_4kb.lookup++;
	uint64_t signature = create_multi_offset_signature(atentry->pc, atentry->offsets);

	int32_t set = -1;
	auto pht_index = search_pht_4kb(signature, &set);
	assert(set != -1);
	if(pht_index != pht_4kb[set].end())
	{
		/* 4KB PHT hit */
		stats.pht_4kb.hit++;
		(*pht_index)->pattern = atentry->pattern;
		update_age_pht_4kb(set, pht_index);
	}
	else
	{
		/* 4KB PHT miss */
		assert(set != -1);
		if(pht_4kb[set].size() >= knob::rich_pht_4kb_assoc)
		{
			auto victim = search_victim_pht_4kb(set);
			evcit_pht_4kb(set, victim);
		}

		stats.pht_4kb.insert++;
		PHTEntry *phtentry = new PHTEntry();
		phtentry->signature = signature;
		phtentry->is_pc_addr = false;
		phtentry->pattern = atentry->pattern;
		phtentry->age = 0;
		for(uint32_t index = 0; index < pht_4kb[set].size(); ++index) pht_4kb[set][index]->age++;
		pht_4kb[set].push_back(phtentry);
	}
}

deque<PHTEntry*>::iterator RichPrefetcher::search_pht_4kb(uint64_t signature, int32_t *set)
{
	(*set) = signature % pht_4kb_sets;
	return find_if(pht_4kb[(*set)].begin(), pht_4kb[(*set)].end(), [signature](PHTEntry *phtentry){
		return (phtentry->signature == signature);
	});
}

deque<PHTEntry*>::iterator RichPrefetcher::search_victim_pht_4kb(int32_t set)
{
	uint32_t max_age = 0;
	deque<PHTEntry*>::iterator it, victim;
	for(it = pht_4kb[set].begin(); it != pht_4kb[set].end(); ++it)
	{
		if((*it)->age >= max_age)
		{
			max_age = (*it)->age;
			victim = it;
		}
	}
	return victim;
}

void RichPrefetcher::update_age_pht_4kb(int32_t set, deque<PHTEntry*>::iterator current)
{
	for(auto it = pht_4kb[set].begin(); it != pht_4kb[set].end(); ++it)
	{
		(*it)->age++;
	}
	(*current)->age = 0;
}

void RichPrefetcher::evcit_pht_4kb(int32_t set, deque<PHTEntry*>::iterator victim)
{
	stats.pht_4kb.evict++;
	PHTEntry *phtentry = (*victim);
	pht_4kb[set].erase(victim);
	delete phtentry;
}

uint64_t RichPrefetcher::create_multi_offset_signature(uint64_t pc, const vector<uint32_t> &offsets)
{
	uint64_t signature = pc;
	uint32_t offset_bits = knob::rich_region_4kb_size_log - LOG2_BLOCK_SIZE; /* 6 bits for 4KB */
	for(uint32_t i = 0; i < offsets.size(); ++i)
	{
		signature = (signature << offset_bits) ^ (uint64_t)offsets[i];
	}
	return signature;
}

int RichPrefetcher::generate_prefetch_4kb(uint64_t pc, uint64_t page, const vector<uint32_t> &offsets, vector<uint64_t> &pref_addr)
{
	stats.generate_prefetch_4kb.called++;
	uint64_t signature = create_multi_offset_signature(pc, offsets);
	int32_t set = -1;
	auto pht_index = search_pht_4kb(signature, &set);
	assert(set != -1);
	if(pht_index == pht_4kb[set].end())
	{
		stats.generate_prefetch_4kb.pht_miss++;
		return 0;
	}

	stats.generate_prefetch_4kb.pht_hit++;
	PHTEntry *phtentry = (*pht_index);
	uint32_t before_size = pref_addr.size();
	for(uint32_t index = 0; index < BITMAP_MAX_SIZE; ++index)
	{
		if(phtentry->pattern[index])
		{
			/* Skip offsets that were already accessed as trigger offsets */
			if(find(offsets.begin(), offsets.end(), index) == offsets.end())
			{
				uint64_t addr = (page << knob::rich_region_4kb_size_log) + (index << LOG2_BLOCK_SIZE);
				pref_addr.push_back(addr);
			}
		}
	}
	update_age_pht_4kb(set, pht_index);
	uint32_t generated = pref_addr.size() - before_size;
	stats.generate_prefetch_4kb.pref_generated += generated;
	return generated;
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
	<< "rich.ft_4kb.lookup " << stats.ft_4kb.lookup << endl
	<< "rich.ft_4kb.hit " << stats.ft_4kb.hit << endl
	<< "rich.ft_4kb.insert " << stats.ft_4kb.insert << endl
	<< "rich.ft_4kb.evict " << stats.ft_4kb.evict << endl
	<< "rich.at_4kb.lookup " << stats.at_4kb.lookup << endl
	<< "rich.at_4kb.hit " << stats.at_4kb.hit << endl
	<< "rich.at_4kb.insert " << stats.at_4kb.insert << endl
	<< "rich.at_4kb.evict " << stats.at_4kb.evict << endl
	<< "rich.pht_4kb.lookup " << stats.pht_4kb.lookup << endl
	<< "rich.pht_4kb.hit " << stats.pht_4kb.hit << endl
	<< "rich.pht_4kb.insert " << stats.pht_4kb.insert << endl
	<< "rich.pht_4kb.evict " << stats.pht_4kb.evict << endl
	<< "rich.generate_prefetch_4kb.called " << stats.generate_prefetch_4kb.called << endl
	<< "rich.generate_prefetch_4kb.pht_miss " << stats.generate_prefetch_4kb.pht_miss << endl
	<< "rich.generate_prefetch_4kb.pht_hit " << stats.generate_prefetch_4kb.pht_hit << endl
	<< "rich.generate_prefetch_4kb.pref_generated " << stats.generate_prefetch_4kb.pref_generated << endl
	<< "rich.pref_buffer.buffered " << stats.pref_buffer.buffered << endl
	<< "rich.pref_buffer.spilled " << stats.pref_buffer.spilled << endl
	<< "rich.pref_buffer.issued " << stats.pref_buffer.issued << endl
	<< endl;
}
