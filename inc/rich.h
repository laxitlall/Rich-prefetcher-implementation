#ifndef RICH_H
#define RICH_H

#include <vector>
#include <deque>
#include "bitmap.h"
#include "prefetcher.h"

using namespace std;

class RichPrefetcher : public Prefetcher
{
public:
	class FTEntry
	{
	public:
		uint64_t page;
		uint64_t pc;
		uint64_t first_addr;
		uint32_t trigger_offset;
		vector<uint32_t> offsets;

	public:
		void reset()
		{
			page = 0xdeadbeef;
			pc = 0xdeadbeef;
			first_addr = 0xdeadbeef;
			trigger_offset = 0;
			offsets.clear();
		}
		FTEntry(){reset();}
		~FTEntry(){}
	};

	class ATEntry
	{
	public:
		uint64_t page;
		uint64_t pc;
		uint64_t first_addr;
		uint32_t trigger_offset;
		vector<uint32_t> offsets;
		Bitmap pattern;
		uint32_t age;

	public:
		void reset()
		{
			page = pc = first_addr = 0xdeadbeef;
			trigger_offset = 0;
			offsets.clear();
			pattern.reset();
			age = 0;
		}
		ATEntry(){reset();}
		~ATEntry(){}
	};

	class PHTEntry
	{
	public:
		uint64_t signature;
		bool is_pc_addr;
		Bitmap pattern;
		uint32_t age;

	public:
		void reset()
		{
			signature = 0xdeadbeef;
			is_pc_addr = false;
			pattern.reset();
			age = 0;
		}
		PHTEntry(){reset();}
		~PHTEntry(){}
	};

	class LookupFIFO
	{
	public:
		uint64_t page;
		uint64_t pc;
		vector<uint32_t> offsets;
		bool already_triggered;
		uint32_t age;

	public:
		void reset()
		{
			page = 0xdeadbeef;
			pc = 0xdeadbeef;
			offsets.clear();
			already_triggered = false;
			age = 0;
		}
		LookupFIFO(){reset();}
		~LookupFIFO(){}
	};

private:
	/* 2KB Region Tables */
	deque<FTEntry*> filter_table;
	deque<ATEntry*> acc_table;
	vector<deque<PHTEntry*> > pht;
	uint32_t pht_sets;

	/* 4KB Region Tables */
	deque<FTEntry*> filter_table_4kb;
	deque<ATEntry*> acc_table_4kb;
	vector<deque<PHTEntry*> > pht_4kb;
	uint32_t pht_4kb_sets;
	deque<LookupFIFO*> lookup_fifo_4kb;

	deque<uint64_t> pref_buffer;

	struct
	{
		struct
		{
			uint64_t lookup;
			uint64_t hit;
			uint64_t insert;
			uint64_t evict;
		} ft;
		struct
		{
			uint64_t lookup;
			uint64_t hit;
			uint64_t insert;
			uint64_t evict;
		} at;
		struct
		{
			uint64_t lookup;
			uint64_t hit;
			uint64_t insert;
			uint64_t evict;
		} pht;
		struct
		{
			uint64_t called;
			uint64_t pht_miss;
			uint64_t pc_addr_hit;
			uint64_t pc_off_hit;
			uint64_t pref_generated;
		} generate_prefetch;
		struct
		{
			uint64_t lookup;
			uint64_t hit;
			uint64_t insert;
			uint64_t evict;
		} ft_4kb;
		struct
		{
			uint64_t lookup;
			uint64_t hit;
			uint64_t insert;
			uint64_t evict;
		} at_4kb;
		struct
		{
			uint64_t lookup;
			uint64_t hit;
			uint64_t insert;
			uint64_t evict;
		} pht_4kb;
		struct
		{
			uint64_t called;
			uint64_t pht_miss;
			uint64_t pht_hit;
			uint64_t pref_generated;
		} generate_prefetch_4kb;
		struct
		{
			uint64_t spilled;
			uint64_t buffered;
			uint64_t issued;
		} pref_buffer;

	} stats;

private:
	void init_knobs();
	void init_stats();

	/* 2KB Region Helper Functions */
	deque<FTEntry*>::iterator search_filter_table(uint64_t page);
	deque<FTEntry*>::iterator search_victim_filter_table();
	void evict_filter_table(deque<FTEntry*>::iterator victim);
	void insert_filter_table(uint64_t pc, uint64_t address, uint64_t page, uint32_t offset);

	deque<ATEntry*>::iterator search_acc_table(uint64_t page);
	deque<ATEntry*>::iterator search_victim_acc_table();
	void evict_acc_table(deque<ATEntry*>::iterator victim);
	void update_age_acc_table(deque<ATEntry*>::iterator current);
	void insert_acc_table(FTEntry *ftentry, uint32_t offset);
	
	deque<PHTEntry*>::iterator search_pht(uint64_t signature, bool is_pc_addr, int32_t *set);
	deque<PHTEntry*>::iterator search_victim_pht(int32_t set);
	void evcit_pht(int32_t set, deque<PHTEntry*>::iterator victim);
	void update_age_pht(int32_t set, deque<PHTEntry*>::iterator current);
	void insert_pht_entry(uint64_t signature, bool is_pc_addr, const Bitmap &pattern);
	void insert_pht_table(ATEntry *atentry);

	uint64_t create_signature(uint64_t pc, uint32_t offset);
	uint64_t create_pc_addr_signature(uint64_t pc, uint64_t address);
	int generate_prefetch(uint64_t pc, uint64_t address, uint64_t page, uint32_t offset, vector<uint64_t> &pref_addr);

	/* 4KB Region Helper Functions */
	deque<LookupFIFO*>::iterator search_lookup_fifo_4kb(uint64_t page);
	deque<LookupFIFO*>::iterator search_victim_lookup_fifo_4kb();
	void evict_lookup_fifo_4kb(deque<LookupFIFO*>::iterator victim);
	deque<LookupFIFO*>::iterator insert_lookup_fifo_4kb(uint64_t pc, uint64_t page, uint32_t offset);
	void update_age_lookup_fifo_4kb(deque<LookupFIFO*>::iterator current);

	deque<FTEntry*>::iterator search_filter_table_4kb(uint64_t page);
	deque<FTEntry*>::iterator search_victim_filter_table_4kb();
	void evict_filter_table_4kb(deque<FTEntry*>::iterator victim);
	void insert_filter_table_4kb(uint64_t pc, uint64_t page, uint32_t offset);

	deque<ATEntry*>::iterator search_acc_table_4kb(uint64_t page);
	deque<ATEntry*>::iterator search_victim_acc_table_4kb();
	void evict_acc_table_4kb(deque<ATEntry*>::iterator victim);
	void update_age_acc_table_4kb(deque<ATEntry*>::iterator current);
	void insert_acc_table_4kb(FTEntry *ftentry, uint32_t offset);

	deque<PHTEntry*>::iterator search_pht_4kb(uint64_t signature, int32_t *set);
	deque<PHTEntry*>::iterator search_victim_pht_4kb(int32_t set);
	void evcit_pht_4kb(int32_t set, deque<PHTEntry*>::iterator victim);
	void update_age_pht_4kb(int32_t set, deque<PHTEntry*>::iterator current);
	void insert_pht_table_4kb(ATEntry *atentry);

	uint64_t create_multi_offset_signature(uint64_t pc, const vector<uint32_t> &offsets);
	int generate_prefetch_4kb(uint64_t pc, uint64_t page, const vector<uint32_t> &offsets, vector<uint64_t> &pref_addr);

	void buffer_prefetch(vector<uint64_t> pref_addr);
	void issue_prefetch(vector<uint64_t> &pref_addr);

public:
	RichPrefetcher(string type);
	~RichPrefetcher();
	void invoke_prefetcher(uint64_t pc, uint64_t address, uint8_t cache_hit, uint8_t type, vector<uint64_t> &pref_addr);
	void dump_stats();
	void print_config();
};

#endif /* RICH_H */