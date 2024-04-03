// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/pagevec.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/swap_slots.h>
#include <linux/swap_scan_slot.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
#include "internal.h"
#include "swap.h"
/*DJL ADD START*/
#include <linux/memcontrol.h>
#define CREATE_TRACE_POINTS
#include <trace/events/swap.h>
/*DJL ADD END*/
/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_page_list.
 */
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.dirty_folio	= noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= migrate_folio,
#endif
};

struct address_space *swapper_spaces[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces[MAX_SWAPFILES] __read_mostly;

struct address_space *swapper_spaces_remap[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces_remap[MAX_SWAPFILES] __read_mostly;

static bool enable_vma_readahead __read_mostly = true;
/*DJL ADD BEGIN*/
static bool enable_vma_readahead_boost __read_mostly = true;//controller of boost
static bool enable_ra_fast_evict __read_mostly = false;//controller of boost
bool swap_scan_enabled_sysfs __read_mostly = true;
/*DJL ADD END*/

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);
/*DJL ADD BEGIN*/
#ifdef CONFIG_LRU_GEN_CGROUP_KSWAPD_BOOST
static int force_wake_up_delay = 10;
static int force_wake_up_delay_now = 0;
#endif
/*DJL ADD END*/
void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Free swap  = %ldkB\n",
		get_nr_swap_pages() << (PAGE_SHIFT - 10));
	printk("Total swap = %lukB\n", total_swap_pages << (PAGE_SHIFT - 10));
}

void *get_shadow_from_swap_cache(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	struct page *page;

	page = xa_load(&address_space->i_pages, idx);
	if (!page)
		return page;
	if (xa_is_value(page))
		return page;
	else if (entry_is_entry_ext(page)== 1)
		return page;
	else if (entry_is_entry_ext(page) == -1){
		pr_err("get_shadow_from_swap_cache entry[%lx] a freed page[%p]", entry.val, page);
	}
	return NULL;
}

void *get_shadow_from_swap_cache_erase(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, 1);
	unsigned long i;
	int entry_state = 0;
	void *old;
	// xas_set_update(&xas, workingset_update_node);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < 1; i++) {
			old = xas_load(&xas);
//			pr_err("addr_space[%p]ind[%lx] = [%lx]", address_space, idx, old);
			if (xa_is_value(old)) { //files
				xas_store(&xas, NULL);
				pr_err("get shadow clean shadow entry[%lx]->shadow[%p]", entry.val, old);
			}
			else if (old){
				entry_state = entry_is_entry_ext(old);
				if (entry_state > 0){ //shadow, erased now
					xas_store(&xas, NULL);
					// pr_err("get shadow clean shadow_ext entry[%lx]->shadow_ext[%p]", entry.val, old);
				}
				else if (-1 == entry_state){ //might be still in swapcache ?
					pr_err("return a freed entry[%lx]->shadow[%p]", entry.val, old);
					BUG();
					old = NULL;
				}
				else if (0 == entry_state){
					// pr_err("get_shadow_from_s$ delete origin folio entry[%lx]->folio[%p]", entry.val, old);
					old = NULL;
				}
				else{
					BUG();
				}
			}
			xas_next(&xas);
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, GFP_KERNEL));
	if (!xas_error(&xas))
		return old;
	BUG();
	return NULL;
}

extern atomic_t ext_count;
/*
 * add_to_swap_cache resembles filemap_add_folio on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
int add_to_swap_cache(struct folio *folio, swp_entry_t entry,
			gfp_t gfp, void **shadowp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));
	unsigned long i, nr = folio_nr_pages(folio);
	int entry_state = 0;
	void *old;

	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr);
	folio_set_swapcache(folio);
	if (entry_is_entry_ext(folio) != 0){
		pr_err("add_to_swap_cache bad folio[%p]", folio);
		BUG();
	}
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			old = xas_load(&xas);
			if (old){
				if (xa_is_value(old)) { //files
					if (shadowp)
						*shadowp = old;
				}
				else{
					entry_state = entry_is_entry_ext_debug(old);
					if (entry_state == 1){ //swap
						if (shadowp){
							*shadowp = old;
						}else{ //
							pr_info("[FREE]folio[%p] add to sw$ , not freed entry[%lx] ext[%p] addr_spc[%p]idx[%ld]", 
										folio, entry.val, old, address_space, idx);
							shadow_entry_free(old);
							atomic_dec(&ext_count);
							// BUG();
						}
					}
					else if (old && 0 == entry_state){
						pr_err("return a fucked entry[%lx]->shadow[%p], folio[%p] failed add $", entry.val, old, folio);
						BUG();
					}
					else if (-1 == entry_state){
						pr_err("add_to_swap_cache invalid entry[%lx]->shadow[%p], folio[%p] failed add $", entry.val, old, folio);
						BUG();
					}
					else{
						pr_err("add_to_swap_cache NULL ext entry[%lx]->shadow[%p], folio[%p] failed add $", entry.val, old, folio);
						BUG(); // NULL
					}
				}
			}

			// set_page_private_debug(folio_page(folio, i), entry.val + i, 1);
			set_page_private(folio_page(folio, i), entry.val + i);
			xas_store(&xas, folio);

			xas_next(&xas);
		}
		address_space->nrpages += nr;
		__node_stat_mod_folio(folio, NR_FILE_PAGES, nr);
		__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr);
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));
	// pr_err("set entry[%lx]->folio[%p]", entry.val, folio);
	check_private_debug(folio);
	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	folio_ref_sub(folio, nr);
	return xas_error(&xas);
}

/*THIS ONLY MASK FOR DIFFERENT MAPS, DOESN"T DEAL WITH EXT BITS*/
int prepare_swp_entry(swp_entry_t from_entry, swp_entry_t* masked_entry)
{
	struct address_space *address_space = swap_address_space_remap(from_entry);
	swp_entry_t mig_swap, from_entry_2;
	int current_offset = 0;

	mig_swap.val = 0;
	if (!address_space) 
		return -1;	
	
	xa_lock_irq(&address_space->i_pages);
	while (!(mig_swap.val)){
		pgoff_t idx;
		from_entry_2.val = from_entry.val;
		swp_entry_set_special(&from_entry_2, current_offset); //set offset
		idx = swp_offset(from_entry_2);
		XA_STATE(xas, &address_space->i_pages, idx);
		void *entry = xas_load(&xas);
		if (!entry){
			break;
		}
		mig_swap.val = xa_to_value(entry);
		if (mig_swap.val) { //conflict
			pr_err("from_swap conflict entry[%lx], cur[%d]", 
						from_entry_2.val,  current_offset);
			current_offset++;
			if (current_offset > SWP_MIG_MAX_MAP){
				pr_err("prepare_swp_entry ecceed error");
				xa_unlock_irq(&address_space->i_pages);
				return -2;
			}
			mig_swap.val = 0; //continue check
		}
	}	
	xa_unlock_irq(&address_space->i_pages);
	masked_entry->val = from_entry_2.val;
	pr_err("entry[%lx]=>[%lx] was saved [%d] time"
			, from_entry.val, from_entry_2.val, current_offset);

	return 0;
}

int enable_swp_entry_remap(struct folio* folio, swp_entry_t from_entry, swp_entry_t* p_to_entry)
{
	struct address_space *address_space = swap_address_space_remap(from_entry);
	swp_entry_t to_entry, to_entry_enabled;
	long nr, i;
	void* entry;
	bool locked = false;
	pgoff_t idx = swp_offset(from_entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));

	VM_WARN_ON_FOLIO(folio_test_writeback(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);
	
	nr = folio_nr_pages(folio);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			entry = xas_load(&xas);
			if (!entry || !folio_test_stalesaved(folio)){
				pr_err("enable_swp_entry_remap fail [%p] entry[%lx]stalesaved[%d]", 
							folio, from_entry.val ,folio_test_stalesaved(folio));
				goto unlock;
			}
			to_entry.val = xa_to_value(entry);
			if (to_entry.val) {
				if (unlikely(non_swap_entry(to_entry))){
					pr_err("enable_swp_entry_remap fail [%p] entry[%lx]->entry[%lx]", 
								folio, from_entry.val, to_entry.val);
					goto unlock;
				}
				to_entry_enabled.val = to_entry.val;
				swp_entry_clear_ext(&to_entry_enabled, 0x1); //we cannot use 0x3 to test if locked
				if (swp_entry_test_ext(to_entry_enabled) & 0x2){ // 0x2 locket this should be returned
					pr_err("enable_swp_entry_remap fail locked [%p] $[%d] entry[%lx]->entry[%lx]", 
							folio, folio_test_swapcache(folio), 
							from_entry.val, to_entry_enabled.val);
					locked = true; 
					//message has been passed to this place
					//we should unlock it here, and let do_swap_page do the rest			
					swp_entry_clear_ext(&to_entry_enabled, 0x2); //we cannot use 0x3 to test if locked
					pr_err("enable_swp_entry_remap clear locked [%p]ref[%d] $[%d] entry[%lx]->entry[%lx] ", 
							folio, folio_ref_count(folio), folio_test_swapcache(folio), 
							from_entry.val, to_entry_enabled.val);	
					xas_store(&xas, xa_mk_value(to_entry_enabled.val + i));
					goto unlock;
				}
				else{
					xas_store(&xas, xa_mk_value(to_entry_enabled.val + i));
					pr_err("enable_swp_entry_remap success folio[%p]ref[%d] $[%d] entry[%lx]->entry[%lx] ", 
							folio, folio_ref_count(folio), folio_test_swapcache(folio), 
							from_entry.val, to_entry_enabled.val);					
				}
			}
			else{
				pr_err("enable_swp_entry_remap fail [%p] entry[%lx]", folio, from_entry.val);
				goto unlock;
			}
			xas_next(&xas);
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN)));
	check_private_debug(folio);
	if (locked){
		p_to_entry->val = 0;
		return 1;
	}
	else{
		p_to_entry->val = to_entry_enabled.val;
	}
	if (!xas_error(&xas))
		return 0;

	return xas_error(&xas);
}

/*only supports single page remap*/
int add_swp_entry_remap(struct folio* folio, swp_entry_t from_entry, swp_entry_t to_entry, 
			gfp_t gfp)
{
	struct address_space *address_space = swap_address_space_remap(from_entry);
	long nr = folio_nr_pages(folio);
	int i;
	void* old;
	pgoff_t idx = swp_offset(from_entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));

	// xas_set_update(&xas, workingset_update_node);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_stalesaved(folio), folio);

	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			old = xas_load(&xas);
			if (xa_is_value(old)) {
				xas_unlock_irq(&xas);
				return -EEXIST;
			}
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			VM_BUG_ON_FOLIO(from_entry.val + i != page_private(folio_page(folio, i)), folio);
			xas_store(&xas, xa_mk_value(to_entry.val + i));
			xas_next(&xas);
		}
		address_space->nrpages += nr;
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	return xas_error(&xas);
}

static int add_to_swap_cache_save_check(struct folio *folio, swp_entry_t entry,
			gfp_t gfp, bool saved)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	void* _entry;
	unsigned long message;

	if (swp_entry_test_ext(entry)){
		pr_err("add_to_swap_cache_save can't deal ext entry[%lx]", entry.val);
		BUG();
		return -1;
	}
	if (entry_is_entry_ext(folio)){
		pr_err("a buggy folio[%p] entry[%lx]", folio, entry);
		BUG();
	}
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));
	unsigned long i, nr = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr);
	folio_set_swapcache(folio);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			_entry = xas_load(&xas);
			if (_entry){ //normal
				if (xa_is_value(_entry)){
					message = xa_to_value(_entry);
					pr_err("got message[%lx]", message);
					xas_unlock_irq(&xas);
					return -2;
				} 
				else if (entry_is_entry_ext(_entry) > 0){
					if (saved){
						pr_err("got shadow entry [%lx], transfer to folio[%p] ", (unsigned long)_entry, folio);
						folio->shadow_ext = _entry;						
					}
					else{
						pr_err("got shadow entry [%lx], in mig[%lx]", (unsigned long)_entry, entry.val);
						BUG();
					}
				}
				else if (entry_is_entry_ext(_entry) == 0){
					pr_err("add_to_swap_cache_save_check [%lx]->[%p] got folio value ?",
								entry,  _entry);
					xas_unlock_irq(&xas);
					return -3;
				}
				else{
					pr_err("add_to_swap_cache_save_check [%lx]->[%p] got freed entry_ext",
								entry,  _entry);
				}
			}

			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			// set_page_private_debug(folio_page(folio, i), entry.val + i, 2);
			xas_store(&xas, folio);
			if (entry_is_entry_ext((void*)folio))
			{
				pr_err("folio misunderstand err[%lx]", folio);
				BUG();	
			}
			xas_next(&xas);
		}
		address_space->nrpages += nr;
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));
	check_private_debug(folio);

	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	pr_err("add_to_swap_cache_save");
	folio_ref_sub(folio, nr);
	BUG();

	return xas_error(&xas);
}
/*
 * add_to_swap_cache resembles filemap_add_folio on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
static int add_to_swap_cache_save(struct folio *folio, swp_entry_t entry,
			gfp_t gfp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	void* _entry;
	if (swp_entry_test_ext(entry)){
		pr_err("add_to_swap_cache_save can't deal ext entry[%lx]", entry.val);
		BUG();
		return -1;
	}
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));
	unsigned long i, nr = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr);
	folio_set_swapcache(folio);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			// set_page_private_debug(folio_page(folio, i), entry.val + i, 2);
			_entry = xas_store(&xas, folio);
			if (_entry){
				pr_err("add_to_swap_cache_save got entry?[%lx]", (unsigned long)_entry);
				BUG();
			}
			xas_next(&xas);
		}
		address_space->nrpages += nr;
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	pr_err("add_to_swap_cache_save");
	BUG();
	folio_ref_sub(folio, nr);
	return xas_error(&xas);
}


swp_entry_t entry_get_migentry_lock(swp_entry_t ori_swap)
{
	swp_entry_t mig_swap, locked_mig;
	struct address_space *address_space_remap;
	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	long nr = 1, i;
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		if (xa_is_value(entry)){
			mig_swap.val = xa_to_value(entry);
			if (mig_swap.val && non_swap_entry(mig_swap)){
				pr_err("remap err [%lx]->[%lx]", ori_swap.val, mig_swap.val);
				BUG();
			}
			locked_mig.val = mig_swap.val;
			swp_entry_set_ext(&locked_mig, swp_entry_test_ext(locked_mig)| 0x2);
			xas_store(&xas, xa_mk_value(locked_mig.val));
		}
		else if (entry){
			pr_err("remap inner bug [%lx]->[%p]", ori_swap.val, entry);
			BUG();
		}
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);

	return mig_swap;
}

// swp_entry_t entry_get_migentry_unlock(swp_entry_t ori_swap, swp_entry_t mig_swap)
// {
// 	swp_entry_t mig_swap, locked_mig;
// 	struct address_space *address_space_remap;
// 	mig_swap.val = 0;
// 	address_space_remap = swap_address_space_remap(ori_swap);
// 	if (!address_space_remap) 
// 		return mig_swap;
// 	long nr = 1, i;
// 	pgoff_t idx = swp_offset(ori_swap);
// 	XA_STATE(xas, &address_space_remap->i_pages, idx);

// 	xa_lock_irq(&address_space_remap->i_pages);
// 	for (i = 0; i < nr; i++) {
// 		void *entry = xas_load(&xas);
// 		if (xa_is_value(entry)){
// 			mig_swap.val = xa_to_value(entry);
// 			if (mig_swap.val && non_swap_entry(mig_swap)){
// 				pr_err("remap err [%lx]->[%lx]", ori_swap.val, mig_swap.val);
// 				BUG();
// 			}
// 			locked_mig.val = mig_swap.val;
// 			swp_entry_set_ext(&locked_mig, swp_entry_test_ext(locked_mig)| 0x2);
// 			xas_store(&xas, xa_mk_value(locked_mig.val));
// 		}
// 		xas_next(&xas);
// 	}
// 	xa_unlock_irq(&address_space_remap->i_pages);

// 	return mig_swap;
// }

swp_entry_t entry_get_migentry(swp_entry_t ori_swap)
{
	swp_entry_t mig_swap;
	struct address_space *address_space_remap;
	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	long nr = 1, i;
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		if (xa_is_value(entry))
			mig_swap.val = xa_to_value(entry);
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);

	return mig_swap;
}

static bool entry_remap_empty(swp_entry_t entry)
{
	swp_entry_t mig_swap;
	void* xavalue;
	bool ret;
	pgoff_t idx;
	struct address_space *address_space_remap;
	ret = false;
	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(entry);
	if (!address_space_remap) 
		return false;
	idx = swp_offset(entry);
	XA_STATE(xas, &address_space_remap->i_pages, idx);
	
	xa_lock_irq(&address_space_remap->i_pages);
	xavalue = xas_load(&xas);
	if (xa_is_value(xavalue))
		ret = false;
	else
		ret = true;
	xa_unlock_irq(&address_space_remap->i_pages);

	return ret;
}

/*
 * check if this entry already has some remaps
 * find a sutable version for it if manage to find one, 
 * return version number
 * else, return -1 indicates cannot hold a remap anymore,
 * unless some one fall back and returns a version back
 */
int entry_remap_usable_version(swp_entry_t entry)
{
	swp_entry_t check_entry;
	int i;
	struct address_space *address_space_remap;
	pgoff_t idx;
	void* xavalue;

	address_space_remap = swap_address_space_remap(entry);

	VM_BUG_ON(swp_entry_test_ext(entry));
	i = 0;
	xa_lock_irq(&address_space_remap->i_pages); //lock
	while (i <= SWP_ENTRY_ALIVE_VERSION_SPEC){
		check_entry.val = entry.val;
		swp_entry_set_special(&check_entry, i);
		swp_entry_clear_ext(&check_entry, 0x3); //clear all ext
		idx = swp_offset(check_entry);
		XA_STATE(xas, &address_space_remap->i_pages, idx);

		xavalue = xas_load(&xas);
		if (!xa_is_value(xavalue)) //not occupied
		{
			xa_unlock_irq(&address_space_remap->i_pages);//unlock
			return i;
		}
check_next_version:
		i++;
	}
	xa_unlock_irq(&address_space_remap->i_pages);//unlock

	pr_err("entry_remap_usable_version all occupied [0x%lx]", entry.val);
	return -1;//didn't found

// 	while (i <= SWP_ENTRY_MAX_SPEC){
// 		check_entry.val = entry.val;
// 		swp_entry_set_special(&check_entry, i);
// 		swp_entry_clear_ext(&check_entry, 0x3); //clear all ext
// 		if (entry_remap_empty(check_entry)){
// 			if (i)
// 				pr_err("entry_remap_usable_version first [0x%lx]v[%d]", check_entry.val, i);
// 			return i;
// 		} else {
// 			// pr_err("entry_remap_usable_version $ed [0x%lx]v[%d]", check_entry.val, i);
// 		}
// check_next_version:
// 		i++;
// 	}
}
swp_entry_t folio_get_migentry(struct folio* folio, swp_entry_t ori_swap)
{
	swp_entry_t mig_swap;
	int i;
	long nr;
	struct address_space *address_space_remap;
	VM_BUG_ON_FOLIO((page_private(folio_page(folio, 0)) == 0), folio);

	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	// xas_set_update(&xas, workingset_update_node);
	VM_BUG_ON_FOLIO((nr > 1), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	
	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		VM_BUG_ON_FOLIO(!entry, folio);
		mig_swap.val = xa_to_value(entry);
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);

	return mig_swap;
}
/*
 * This must be called only on folios that have
 * been verified to be in the swap cache.
 */
void __delete_from_swap_cache(struct folio *folio,
			swp_entry_t entry, void *shadow)
{
	struct address_space *address_space = swap_address_space(entry);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(entry);
	XA_STATE(xas, &address_space->i_pages, idx);

	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);
	if (unlikely(folio_test_stalesaved(folio))){
		pr_err("folio[%p] entry[%lx] cnt:%d origin swapcache clearing", 
					folio, entry.val, __swp_swapcount(entry));
	}
	if (unlikely(shadow && !xa_is_value(shadow) && entry_is_entry_ext(shadow)!=1)){
		pr_err("bad shadow $ folio[%p] entry[%lx] shadow[%lx]", folio, entry, shadow);
		shadow = NULL;
		BUG();
	}
	for (i = 0; i < nr; i++) {
		void *entry_ = xas_store(&xas, shadow);
		if (shadow)
			pr_info("[TRANSFER]__delete_s$ folio[%p]->entry[%lx] shadow[%p]", folio, entry, shadow);
		VM_BUG_ON_PAGE(entry_ != folio, entry_);

		if (unlikely(entry_ != folio)) {
			pr_err("__delete_sc_mig mismatch entry[%lx]->[%p]<>folio[%p]", 
					entry.val, entry_, folio);
			BUG();
		}
		if (unlikely(page_private(folio_page(folio, i)) != entry.val + i)){
			pr_err("try delete from s$2 folio[%p] [%lx]<>[%lx]", folio, page_private(folio_page(folio, i)), entry.val + i);
			BUG();
		}
		// set_page_private_debug(folio_page(folio, i), 0, 3);
		set_page_private(folio_page(folio, i), 0);
		if (unlikely(swp_entry_test_special(entry) > 1))
			pr_err("__delete_from_swap_cache entry[%lx] folio[%p]->shadow[%lx]", 
				entry.val, folio, shadow);
		xas_next(&xas);
	}
	folio_clear_swapcache(folio);
	check_private_debug(folio);
	// if (shadow)
	// 	pr_err("delete $ entry[%lx]->folio[%p]=>shadow[%p]", entry.val, folio, shadow);
	address_space->nrpages -= nr;
	__node_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr);
}

/*make sure entry has no ext set*/
void __delete_from_swap_cache_mig(struct folio *folio,
			swp_entry_t entry, bool shadow_transfer)
{
	struct address_space *address_space = swap_address_space(entry);
	int i;
	pgoff_t idx;
	long nr = folio_nr_pages(folio);
	void *shadow = NULL;

	idx = swp_offset(entry);	
	XA_STATE(xas, &address_space->i_pages, idx);
	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_stalesaved(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry_ = xas_load(&xas);
		if (entry_ != folio) {
			pr_err("__delete_sc_mig mismatch entry[%lx]->folio[%p] [%p]", 
					swp_offset(entry), folio, entry_);
			BUG();
		}
		// if (page_private(folio_page(folio, i)) != 0){
		// 	pr_err("try delete from s$ folio[%p] pri[%lx]<>[0]", 
		// 					folio, page_private(folio_page(folio, i)));
			// set_page_private_debug(folio_page(folio, i), 0, 4);
		// }
		if (shadow_transfer){
			if (folio->shadow_ext){ //may transfer, check
				if (entry_is_entry_ext(folio->shadow_ext) == 1){
					shadow = folio->shadow_ext;
					xas_store(&xas, shadow);
					xas_next(&xas);
					folio->shadow_ext = NULL;
					pr_info("[TRANSFER]__delete_sc_mig folio[%p]->entry[%lx] ext[%lx]", 
								folio, entry.val, (unsigned long)folio->shadow_ext);
					continue;
				}
				else{
					pr_err("__delete_sc_mig fail transfer folio[%p]->shadow[%lx]", folio, (unsigned long)folio->shadow_ext);
					BUG();
				}
			}
		}
		//no transfer, store a NULL
		xas_store(&xas, NULL);
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
	pr_err("delete mig $ entry[%lx]->folio[%p] shadow[%lx] ref[%d]", 
				entry.val, folio, (unsigned long)shadow, folio_ref_count(folio));

	__node_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr);
}

void __delete_from_swap_remap(struct folio *folio, swp_entry_t entry_from, swp_entry_t entry_to, bool delete_unpepared)
{
	struct address_space *address_space = swap_address_space_remap(entry_from);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(entry_from);
	XA_STATE(xas, &address_space->i_pages, idx);
	swp_entry_t tmp;

	VM_BUG_ON_FOLIO(!folio_test_stalesaved(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, NULL); //clear
		tmp.val = xa_to_value(entry);
		swp_entry_clear_ext(&tmp, 0x3);
		if (delete_unpepared){
			swp_entry_clear_ext(&entry_to, 0x3);
		}
		if (entry_to.val && tmp.val != entry_to.val){
			pr_err("[%lx]->[%lx]<>[%lx] mismatch",   //means we don't care about it
					entry_from.val, entry_to.val, tmp.val);
			BUG();		
		} //entry_to.val == 0 
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
	pr_err("delete_from_swap_remap folio[%p] [%lx]->[%lx]", 
			folio, entry_from.val, entry_to.val);
}

static void __delete_from_swap_remap_get_mig(struct folio *folio, swp_entry_t entry_from, swp_entry_t* entry_to)
{
	struct address_space *address_space = swap_address_space_remap(entry_from);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(entry_from);
	XA_STATE(xas, &address_space->i_pages, idx);
	swp_entry_t tmp;

	VM_BUG_ON_FOLIO(!folio_test_stalesaved(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, NULL); //clear
		tmp.val = xa_to_value(entry);
		if (non_swap_entry(tmp)){ //entry_to.val == 0 
			pr_err("2[%lx]-<>[%lx] mismatch",   //means we don't care about it
					entry_from.val,  tmp.val);
			BUG();			
		}
		entry_to->val = tmp.val;
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
	pr_err("__delete_from_swap_remap_get_mig folio[%p] [%lx]->[%lx]", 
			folio, entry_from.val, entry_to->val);
}
/**
 * add_to_swap - allocate swap space for a folio
 * @folio: folio we want to move to swap
 *
 * Allocate swap space for the folio and add the folio to the
 * swap cache.
 *
 * Context: Caller needs to hold the folio lock.
 * Return: Whether the folio was added to the swap cache.
 */
bool add_to_swap(struct folio *folio, long* left_space)
{
	swp_entry_t entry;
	int err;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_uptodate(folio), folio);

	entry = folio_alloc_swap(folio, left_space);
	if (!entry.val)
		return false;
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR
	if (swp_entry_test_special(entry) > 1){
		count_memcg_folio_events(folio, SWAP_STALE_SAVE_REUSE, folio_nr_pages(folio));
		pr_info("folio_alloc_swap normal entry[%lx] v[%d] for folio[%p] cnt:%d",
				 entry.val, swp_entry_test_special(entry), folio, __swap_count(entry));		
	}

#endif
	
	/*
	 * XArray node allocations from PF_MEMALLOC contexts could
	 * completely exhaust the page allocator. __GFP_NOMEMALLOC
	 * stops emergency reserves from being allocated.
	 *
	 * TODO: this could cause a theoretical memory reclaim
	 * deadlock in the swap out path.
	 */
	/*
	 * Add it to the swap cache.
	 */
	err = add_to_swap_cache(folio, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN, NULL);
	if (err)
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	// pr_err("ckpt2 folio[%p]<-entry[%lx]", folio, entry.val);
	/*
	 * Normally the folio will be dirtied in unmap because its
	 * pte should be dirty. A special case is MADV_FREE page. The
	 * page's pte could have dirty bit cleared but the folio's
	 * SwapBacked flag is still set because clearing the dirty bit
	 * and SwapBacked flag has no lock protected. For such folio,
	 * unmap will not set dirty bit for it, so folio reclaim will
	 * not write the folio out. This can cause data corruption when
	 * the folio is swapped in later. Always setting the dirty flag
	 * for the folio solves the problem.
	 */
	folio_mark_dirty(folio);

	return true;

fail:
	pr_err("add_to_swap failling folio[%p], entry[%lx]", folio, entry.val);
	put_swap_folio(folio, entry);
	return false;
}

/*
 * the caller has a reference on the folio.
 */
void delete_from_swap_remap(struct folio *folio, swp_entry_t entry_from, swp_entry_t entry_to, bool delete_unpepared){
	struct address_space *address_space = swap_address_space_remap(entry_from);
	if (unlikely(!address_space)){
		pr_err("delete_from_swap_remap address space BUG");
		return;
	}
	if (!folio){
		pr_err("delete_from_swap_remap address no folio");
		return;
	}
	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_remap(folio, entry_from, entry_to, delete_unpepared);
	xa_unlock_irq(&address_space->i_pages);
	// folio_ref_sub(folio, folio_nr_pages(folio));
}

void swap_remap_unlock(struct folio *folio, swp_entry_t ori_swap, swp_entry_t mig_swap){
	swp_entry_t _locked_mig_swap, locked_mig;
	struct address_space *address_space_remap;
	long nr = 1, i;

	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap){
		BUG();
	}
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		if (xa_is_value(entry)){
			_locked_mig_swap.val = xa_to_value(entry);
			swp_entry_clear_ext(&_locked_mig_swap, 0x3);
			if (_locked_mig_swap.val && non_swap_entry(_locked_mig_swap)){
				pr_err("remap err [%lx]->[%lx]", ori_swap.val, mig_swap.val);
				BUG();
			}
			if (_locked_mig_swap.val != mig_swap.val){
				pr_err("remap mismatch [%lx]->[%lx]", _locked_mig_swap.val, mig_swap.val);
				BUG();
			}
			xas_store(&xas, xa_mk_value(mig_swap.val));
			pr_err("remap unlock[%lx]->[%lx]", ori_swap.val, mig_swap.val);
		}
		else if (entry){
			pr_err("remap inner bug [%lx]->[%p]",  ori_swap.val, entry);
			BUG();
		}
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);
}

void delete_from_swap_remap_get_mig(struct folio* folio, swp_entry_t entry_from, swp_entry_t* entry_to)
{
	struct address_space *address_space = swap_address_space_remap(entry_from);
	if (!address_space){
		pr_err("delete_from_swap_remap address space BUG");
		return;
	}
	if (!folio){
		pr_err("delete_from_swap_remap address no folio");
		return;
	}
	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_remap_get_mig(folio, entry_from, entry_to);
	xa_unlock_irq(&address_space->i_pages);
	swp_entry_clear_ext(entry_to, 0x3); 
	// folio_ref_sub(folio, folio_nr_pages(folio));
}
/*
 * This must be called only on folios that have
 * been verified to be in the swap cache and locked.
 * It will never put the folio into the free list,
 * the caller has a reference on the folio.
 */
void delete_from_swap_cache(struct folio *folio)
{
	swp_entry_t entry = folio_swap_entry(folio);
	struct address_space *address_space = swap_address_space(entry);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(folio, entry, NULL);
	xa_unlock_irq(&address_space->i_pages);

	put_swap_folio(folio, entry);
	folio_ref_sub(folio, folio_nr_pages(folio));
}

/*needs put_swap_folio after it*/
void delete_from_swap_cache_mig(struct folio* folio, swp_entry_t entry, bool dec_count, bool shadow_transfer)
{
	swp_entry_t pgentry = folio_swap_entry(folio), entry_;
	struct address_space *address_space = swap_address_space(entry);
	entry_.val = entry.val;
	swp_entry_clear_ext(&entry_, 0x3); 
		//mig cache only maps mig_entry to folio
		//it is not responsible for validation
	VM_BUG_ON_FOLIO(!(entry.val == pgentry.val), folio);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache_mig(folio, entry_, shadow_transfer);
	xa_unlock_irq(&address_space->i_pages);

	if (dec_count)
		put_swap_folio(folio, entry_);
	if (!dec_count && __swap_count(entry_) != 1){
		pr_err("folio[%p] ref[%d]entry get out of cache mig[%lx]cnt[%d]", 
				folio, folio_ref_count(folio), entry_.val, __swap_count(entry_));	
		BUG();	
	}
	folio_ref_sub(folio, folio_nr_pages(folio));
}


void clear_shadow_from_swap_cache(int type, unsigned long begin,
				unsigned long end, int free)
{
	unsigned long curr = begin;
	void *old, *_entry;
	for (;;) {
		swp_entry_t entry = swp_entry(type, curr);
		struct address_space *address_space = swap_address_space(entry);
		XA_STATE(xas, &address_space->i_pages, curr);

		xas_set_update(&xas, workingset_update_node);

		xa_lock_irq(&address_space->i_pages);
		xas_for_each(&xas, old, end) {
			if (!xa_is_value(old) && entry_is_entry_ext(old) < 1)
				continue;
			if (free && old && entry_is_entry_ext(old) == 1){
				pr_info("clear_shadow_from_s [%p] skipped", old);
				_entry = xas_store(&xas, NULL);
				if (old != _entry)
					BUG();
				shadow_entry_free(old);
				continue;
			}
			else if (entry_is_entry_ext(old) == 1){
				pr_info("entry[%lx]clear_shadow_from_s [%p] lost control", entry.val, old);
			}
		}
		xa_unlock_irq(&address_space->i_pages);

		/* search the next swapcache until we meet end */
		curr >>= SWAP_ADDRESS_SPACE_SHIFT;
		curr++;
		curr <<= SWAP_ADDRESS_SPACE_SHIFT;
		if (curr > end)
			break;
	}
}

/* 
 * If we are the only user, then try to free up the swap cache. 
 * 
 * Its ok to check the swapcache flag without the folio lock
 * here because we are going to recheck again inside
 * folio_free_swap() _with_ the lock.
 * 					- Marcelo
 */
void free_swap_cache(struct page *page)
{
	struct folio *folio = page_folio(page);

	if (folio_test_swapcache(folio) && !folio_mapped(folio) &&
	    folio_trylock(folio)) {
		folio_free_swap(folio);
		folio_unlock(folio);
	}
}

/* 
 * Perform a free_page(), also freeing any swap cache associated with
 * this page if it is the last user of the page.
 */
void free_page_and_swap_cache(struct page *page)
{
	free_swap_cache(page);
	if (!is_huge_zero_page(page))
		put_page(page);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct encoded_page **pages, int nr)
{
	lru_add_drain();
	for (int i = 0; i < nr; i++)
		free_swap_cache(encoded_page_ptr(pages[i]));
	release_pages(pages, nr);
}

static inline bool swap_use_vma_readahead(void)
{
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/*
 * Lookup a swap entry in the swap cache. A found folio will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the folio
 * lock before returning.
 *
 * Caller must lock the swap device or hold a reference to keep it valid.
 */
struct folio *swap_cache_get_folio(struct swap_info_struct * si, swp_entry_t entry,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct folio *folio;
	unsigned long offset_v;
	unsigned long version;
	offset_v = swp_offset(entry);//swp_raw_offset(entry);
	version = swp_entry_test_special(entry);
	bool is_stale_saved_folio = false;
	// if (version){
	// 	offset_v = offset_v + si->max * version;
	// }
	if (data_race(si->flags & SWP_SYNCHRONOUS_IO) && !non_swap_entry(entry)){
		folio = syncio_swapcache_get_folio(swap_address_space(entry), offset_v ,&is_stale_saved_folio);
		// if (folio){
		// 	if (is_stale_saved_folio)
		// 	// pr_err("scgf si[%d] return entry[%lx]->[%p]", si->prio, entry.val, folio);
		// }	
		// pr_err("swap_cache_get_folio si[%d] entry[%lx] offset[%lx]return folio [%p]", 
		// 					si->prio, entry.val, offset_v, folio);
	}
	else{
		folio = filemap_get_folio(swap_address_space(entry), offset_v);
		// if (folio){
		// 	// pr_err("scgf async return entry[%lx]->[%p]", entry.val, folio);
		// }
	}
	if (folio) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;
		// if (version)
		// 	pr_err("swap_cache_get_folio offset_v[%lx], ver[%lu] entry[%lx] folio[%p]", 
		// 				offset_v, version, entry.val, folio);
		/*
		 * At the moment, we don't support PG_readahead for anon THP
		 * so let's bail out rather than confusing the readahead stat.
		 */
		if (unlikely(folio_test_large(folio)))
			return folio;

		readahead = folio_test_clear_readahead(folio);
		if (vma && vma_ra) {
			unsigned long ra_val;
			int win, hits;

			ra_val = GET_SWAP_RA_VAL(vma);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead){
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
				/*DJL ADD BEGIN*/
				trace_swapin_readahead_hit(folio);		
				/*DJL ADD END*/
			}
			atomic_long_set(&vma->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
		}

		if (readahead) {
			count_vm_event(SWAP_RA_HIT);
			if (!vma || !vma_ra)
				atomic_inc(&swapin_readahead_hits);
		}
	}

	return folio;
}

/*
 * Lookup a swap entry in the swap cache. A found folio will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the folio
 * lock before returning.
 *
 * Caller must lock the swap device or hold a reference to keep it valid.
 */
static struct folio *raw_swap_cache_get_folio(struct swap_info_struct * si, swp_entry_t entry)
{
	struct folio *folio = NULL;
	unsigned long offset_v;
	offset_v = swp_offset(entry);
	bool is_stale_saved_folio = false;

	if (data_race(si->flags & SWP_SYNCHRONOUS_IO) && !non_swap_entry(entry)){
		folio = syncio_swapcache_get_folio(swap_address_space(entry), offset_v ,&is_stale_saved_folio);
		// if (folio){
		// 	if (is_stale_saved_folio)
		// 		pr_err("raw_swap_cache_get_folio si[%d] return stale saved folio [%p]", si->prio, folio);
		// 	// pr_err("scgf si[%d] return entry[%lx]->[%p]", si->prio, entry.val, folio);
		// }	
	}
	else{
		folio = filemap_get_folio(swap_address_space(entry), offset_v);
		if (folio){
			pr_err("scgf async return entry[%lx]->[%p]", entry.val, folio);
		}
	}
	return folio;
}

/**
 * filemap_get_incore_folio - Find and get a folio from the page or swap caches.
 * @mapping: The address_space to search.
 * @index: The page cache index.
 *
 * This differs from filemap_get_folio() in that it will also look for the
 * folio in the swap cache.
 *
 * Return: The found folio or %NULL.
 */
struct folio *filemap_get_incore_folio(struct address_space *mapping,
		pgoff_t index)
{
	swp_entry_t swp;
	struct swap_info_struct *si;
	struct folio *folio = __filemap_get_folio(mapping, index, FGP_ENTRY, 0);

	if (!xa_is_value(folio))
		goto out;
	if (!shmem_mapping(mapping))
		return NULL;
	pr_err("undefined result");
	swp = radix_to_swp_entry(folio);
	/* There might be swapin error entries in shmem mapping. */
	if (non_swap_entry(swp))
		return NULL;
	/* Prevent swapoff from happening to us */
	si = get_swap_device(swp);
	if (!si)
		return NULL;
	index = swp_offset(swp);
	folio = filemap_get_folio(swap_address_space(swp), index);
	put_swap_device(si);
out:
	return folio;
}
static inline int should_try_change_swap_entry(int rf_dist, int swap_level, bool loop){
	if (loop){
		if (swap_level == 1){ //refault from fast
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist > 2) return 1;
#endif
		}else if (swap_level == -1){ //refault from slow
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist < 2) return 1;
#endif
		}
	}
	else{
		if (swap_level == 1){ //refault from fast
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist > 3) return 1;
#endif
		}
		else if (swap_level == -1){	//refault from slow
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist < 2) return 1;
#endif
		}
	}
	return 0;
} 
struct page*__read_swap_cache_async_save(swp_entry_t entry, 	
	gfp_t gfp_mask, struct vm_area_struct *vma, 
	unsigned long addr,	bool *new_page_allocated, 
	bool no_ra, int* try_free_entry)
{
	struct swap_info_struct *si;
	struct folio *folio;
	void *shadow = NULL;
	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after swap_cache_get_folio() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si)
			return NULL;
		folio = filemap_get_folio(swap_address_space(entry),
						swp_offset(entry));
		put_swap_device(si);
		if (folio)
			pr_err("found already in swap_cache[%lx] -> folio[%p] ref[%d]", entry.val, folio, folio_ref_count(folio));
		if (folio)
			return folio_file_page(folio, swp_offset(entry));

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled){
			pr_err("save swap count entry[%lx][%d] not used", entry.val, __swp_swapcount(entry));
			//we should try to clean it
			return NULL;			
		}


		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		folio = vma_alloc_folio(gfp_mask, 0, vma, addr, false);
		if (!folio){
			pr_err("__read_swap_cache_async_save alloc fail addr[%lx] entry[%lx]", addr, entry.val);
			return NULL;
		}

		// /*
		//  * Swap entry may have been freed since our caller observed it.
		//  */
		err = swapcache_prepare(entry);
		if (!err)
			break;
		pr_err("mig swapcache_prepare fail[%d] addr[%lx] entry[%lx]", 
					err, addr, entry.val);

		folio_put(folio);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		schedule_timeout_uninterruptible(1);
	}
	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */

	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);
	if (mem_cgroup_swapin_charge_folio(folio, NULL, gfp_mask, entry))
		goto fail_unlock;

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(folio, entry, gfp_mask & GFP_RECLAIM_MASK, &shadow))
		goto fail_unlock;

//	mem_cgroup_swapin_uncharge_swap(entry); //DJL doesn't count this
	//now shadow has been used
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
	if(folio && folio->shadow_ext){
		pr_err("shouldn't have folio[%p]->shadow_ext", folio);
		folio->shadow_ext = NULL;
		BUG();
	}
	if (entry_is_entry_ext(shadow) == 1){
		folio->shadow_ext = shadow;
		pr_info("[TRANSFER]read_save entry[%lx]=>folio[%p] ext[%p]", 
					entry.val, folio, folio->shadow_ext);
	}
#else
	if (entry_is_entry_ext(shadow) > 0){
		pr_err("[FREE]__read_swap_cache_async_save free entry[%p]", shadow);
		shadow_entry_free(shadow);
		atomic_dec(&ext_count);
	}
#endif
	/*DJL ADD END*/

	/* Caller will initiate read into locked folio */

	//folio_add_lru_save(folio);

	if (new_page_allocated)
		*new_page_allocated = true;
	pr_err("ckpt5 folio[%p], entry[%lx]", folio, entry.val);

	return &folio->page; 

fail_unlock:
	put_swap_folio(folio, entry);
	folio_unlock(folio);
	folio_put(folio);
	return NULL;
}
/*DJL ADD BEGIN*/
struct page *__read_swap_cache_async(swp_entry_t entry, 	
	gfp_t gfp_mask, struct vm_area_struct *vma, 
	unsigned long addr,	bool *new_page_allocated, 	bool no_ra, 
	int* try_free_entry, bool allow_null)
/*DJL ADD END*/
{
	struct swap_info_struct *si;
	struct folio *folio;
	void *shadow = NULL;
	/*DJL ADD BEGIN*/
	struct lruvec *lruvec;
	pg_data_t* pgdat;
	int swap_level = -2;
	int rf_dist_ts = -1;
	/*DJL ADD END*/
	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after swap_cache_get_folio() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si){
			pr_err("entry[%lx] got not si", entry.val);
			return NULL;
		}
		folio = filemap_get_folio(swap_address_space(entry),
						swp_offset(entry));
		put_swap_device(si);
		if (folio)
			return folio_file_page(folio, swp_offset(entry));

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled){
			if (!allow_null && !entry_get_migentry(entry).val){
				pr_info("swap count entry[%lx][%d] not used exact[%d]", 
						entry.val, __swp_swapcount(entry), allow_null);
				// BUG();
			}
			return NULL;
		}

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		folio = vma_alloc_folio(gfp_mask, 0, vma, addr, false);
		if (!folio){
			pr_err("__read_swap_cache_async alloc fail addr[%lx] entry[%lx]", addr, entry.val);
			return NULL;
		}
		if (folio->shadow_ext){
			pr_err("__read_swap_cache_async alloc folio[%p] has shadow[%p]", folio, folio->shadow_ext);
			BUG();
		}
		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;
		pr_info("swapcache_prepare fail[%d] addr[%lx] entry[%lx]",
					err, addr, entry.val);
		folio_put(folio);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		schedule_timeout_uninterruptible(1);
	}

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */

	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);

	if (mem_cgroup_swapin_charge_folio(folio, NULL, gfp_mask, entry)){
		pr_err("mem_cgroup charge fail");
		goto fail_unlock;
	}
	if (page_private(folio_page(folio, 0))){
		pr_err("before add_to_swap_cache folio[%p] private[%lx]", folio, page_private(folio_page(folio, 0)));
	}
	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(folio, entry, gfp_mask & GFP_RECLAIM_MASK, &shadow)){
		pr_err("add_to_swap_cache fail");
		goto fail_unlock;
	}

	mem_cgroup_swapin_uncharge_swap(entry);

#ifdef CONFIG_LRU_GEN_PASSIVE_SWAP_ALLOC
	folio_clear_swappriohigh(folio); //clear here
	folio_set_swappriolow(folio);  //set here so that only those got promoted again can go to fast
	if (si->prio == get_fastest_swap_prio()){
		folio_swapprio_promote(folio);
	}
#endif
	if (si->prio == get_fastest_swap_prio()){
		swap_level = 1;
	}
	else if (si->prio == get_slowest_swap_prio()){
		swap_level = -1;
	}
	else
		swap_level = 0;

	/*DJL ADD BEGIN*/
	if (shadow){
		workingset_refault(folio, shadow, &rf_dist_ts, addr, swap_level, entry);
		if (vma && vma->vm_mm){
			if (get_fastest_swap_prio() == si->prio){
				count_memcg_event_mm(vma->vm_mm, WORKINGSET_REFAULT_FAST);
			}
			else if (get_slowest_swap_prio() == si->prio){
				count_memcg_event_mm(vma->vm_mm, WORKINGSET_REFAULT_SLOW);
			}
			*try_free_entry = should_try_change_swap_entry(
						rf_dist_ts >= MAX_NR_GENS ? rf_dist_ts - 4 : rf_dist_ts, 
						swap_level, rf_dist_ts >= MAX_NR_GENS ? 0 : 1);
		}
	}

	//now shadow has been used
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
	if (folio->shadow_ext){
		if (folio->shadow_ext != shadow){
			pr_err("[FREE]folio[%p]->shadowext[%p] shadow[%p]", 
			folio, folio->shadow_ext, shadow);
			shadow_entry_free(folio->shadow_ext);
		}
	}
	folio->shadow_ext = NULL;
	if (entry_is_entry_ext(shadow) == 1){
		folio->shadow_ext = shadow;
		pr_info("[TRANSFER]read_cache entry[%lx]=>folio[%p] ext[%p]", 
					entry.val, folio, folio->shadow_ext);
	}
#else
	if (entry_is_entry_ext(shadow) > 0){
		pr_err("[FREE]__read_swap_cache_async free entry[%p]", shadow);
		shadow_entry_free(shadow);
		atomic_dec(&ext_count);
	}
#endif
	/*DJL ADD END*/
	/* Caller will initiate read into locked folio */
	// folio_add_lru(folio);
	if (no_ra)
		folio_add_lru(folio);
	else
		folio_add_lru_ra(folio);		

	if (new_page_allocated)
		*new_page_allocated = true;

//reclaim another 4kb pages space //1mb = 128*4kb pages space
	lruvec = folio_lruvec(folio);
	pgdat = lruvec_pgdat(lruvec);
	pgdat->prio_lruvec = lruvec;
#ifdef CONFIG_LRU_GEN_CGROUP_KSWAPD_BOOST
	if (force_wake_up_delay_now++ >= force_wake_up_delay){
		force_wake_up_delay_now = 0;
		swapin_force_wake_kswapd(gfp_mask, 0);
	}
#endif
	return &folio->page;

fail_unlock:
	put_swap_folio(folio, entry);
	folio_unlock(folio);
	folio_put(folio);
	return NULL;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
struct page *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
				   struct vm_area_struct *vma,
				   unsigned long addr, bool do_poll,
				   struct swap_iocb **plug, bool count, int* try_free_entry)  //DJL ADD PARA
{
	bool page_was_allocated;
	/*DJL ADD BEGIN*/
	struct swap_info_struct *si;
	/*DJL ADD END*/
	struct page *retpage = __read_swap_cache_async(entry, gfp_mask,
			vma, addr, &page_was_allocated, true, try_free_entry, false);
	
	if (page_was_allocated)
		swap_readpage(retpage, do_poll, plug);
	else if (!retpage)
		pr_info("read_swap_cache_async alloc fail entry[%lx]", entry.val);
	/*DJL ADD BEGIN*/
	if (count && page_was_allocated){
		si = get_swap_device(entry);
		if (get_fastest_swap_prio() == si->prio){
			count_memcg_event_mm(vma->vm_mm, SWAPIN_FAST);
		} else if (get_slowest_swap_prio() == si->prio){
			count_memcg_event_mm(vma->vm_mm, SWAPIN_SLOW);
		} else{
			count_memcg_event_mm(vma->vm_mm, SWAPIN_MID);
		}
		put_swap_device(si);
	}
	/*DJL ADD END*/
	return retpage;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

static unsigned int __swapin_nr_pages_boost(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 4;
	if (pages == 4) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset - prev_offset > (prev_win / 2) ||  prev_offset - offset > (prev_win / 2))
			pages = 1;//force add one page in any cases
	} else {
		unsigned int roundup = 8;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win * 3 / 4;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}
/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * This has been extended to use the NUMA policies from the mm triggering
 * the readahead.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 */
struct page *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf, int* try_free_entry)
{
	struct page *page;
	unsigned long entry_offset = swp_raw_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = swp_swap_info(entry);
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	bool do_poll = true, page_allocated;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;
	int try_free;

	mask = swapin_nr_pages(offset) - 1;
	if (!mask)
		goto skip;

	do_poll = false;
	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset ; offset++) {
		/* Ok, do the async read-ahead now */
		page = __read_swap_cache_async(
			swp_entry(swp_type(entry), offset),
			gfp_mask, vma, addr, &page_allocated, true, &try_free, false); //DJL doesn't support fast swapout
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage(page, false, &splug);
			if (offset != entry_offset) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
			}
		}
		put_page(page);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);

	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	/* The page was likely read above, so no need for plugging here */
	return read_swap_cache_async(entry, gfp_mask, vma, addr, do_poll, NULL, false, try_free_entry);
}

int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space, *spaces_remap, *space_remap;
	unsigned int i, nr, nr_remap;
	nr_pages = nr_pages; //do this to support 64 remap
	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	nr = nr << (SWP_SPECIAL_MARK);
	nr_remap = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_REMAP_PAGES);
	nr_remap = nr_remap << (SWP_SPECIAL_MARK);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	spaces_remap = kvcalloc(nr_remap, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces || !spaces_remap)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	for (i = 0; i < nr_remap; i++) {
		space_remap = spaces_remap + i;
		xa_init_flags(&space_remap->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space_remap->i_mmap_writable, 0);
		space_remap->a_ops = NULL;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space_remap);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;
	nr_swapper_spaces_remap[type] = nr_remap;
	swapper_spaces_remap[type] = spaces_remap;
	return 0;
}

void exit_swap_address_space(unsigned int type)
{
	int i;
	struct address_space *spaces = swapper_spaces[type];
	struct address_space *spaces_remap = swapper_spaces_remap[type];

	for (i = 0; i < nr_swapper_spaces[type]; i++)
		VM_WARN_ON_ONCE(!mapping_empty(&spaces[i]));
	for (i = 0; i < nr_swapper_spaces_remap[type]; i++)
		VM_WARN_ON_ONCE(!mapping_empty(&spaces_remap[i]));
	kvfree(spaces);
	kvfree(spaces_remap);
	nr_swapper_spaces[type] = 0;
	swapper_spaces[type] = NULL;
	nr_swapper_spaces_remap[type] = 0;
	swapper_spaces_remap[type] = NULL;
}

static void swap_ra_info(struct vm_fault *vmf,
			 struct vma_swap_readahead *ra_info)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	unsigned long faddr, pfn, fpfn, lpfn, rpfn;
	unsigned long start, end;
	pte_t *pte, *orig_pte;
	unsigned int max_win, hits, prev_win, win;
#ifndef CONFIG_64BIT
	pte_t *tpte;
#endif

	/*DJL ADD BEGIN*/
	// max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster),
	// 		     SWAP_RA_ORDER_CEILING);
	max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster) + READ_ONCE(ra_boost_order),
			     SWAP_RA_ORDER_CEILING + SWAP_RA_ORDER_CEILING_BOOST);
	/*DJL ADD END*/

	if (max_win == 1) {
		ra_info->win = 1;
		trace_new_swap_ra_info(ra_info->ptes, ra_info->nr_pte, ra_info->offset, ra_info->win);
		return;
	}

	faddr = vmf->address;
	fpfn = PFN_DOWN(faddr);
	ra_val = GET_SWAP_RA_VAL(vma);
	pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	/*DJL ADD BEGIN*/
	if (!enable_vma_readahead_boost)
		ra_info->win = win = __swapin_nr_pages(pfn, fpfn, hits,
					       max_win, prev_win);
	else
		ra_info->win = win = __swapin_nr_pages_boost(pfn, fpfn, hits,
						max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info,
			SWAP_RA_VAL(faddr, win, 0));

	/*DJL ADD BEGIN*/
	if (win == 1){
		trace_new_swap_ra_info(ra_info->ptes, ra_info->nr_pte, ra_info->offset, ra_info->win);
 		return;
	}
	// if (win == 1)
	// 	return;
	/*DJL ADD END*/

	/* Copy the PTEs because the page table may be unmapped */
	orig_pte = pte = pte_offset_map(vmf->pmd, faddr);
	if (fpfn == pfn + 1) {
		lpfn = fpfn;
		rpfn = fpfn + win;
	} else if (pfn == fpfn + 1) {
		lpfn = fpfn - win + 1;
		rpfn = fpfn + 1;
	} else {
		unsigned int left = (win - 1) / 2;

		lpfn = fpfn - left;
		rpfn = fpfn + win - left;
	}
	start = max3(lpfn, PFN_DOWN(vma->vm_start),
		     PFN_DOWN(faddr & PMD_MASK));
	end = min3(rpfn, PFN_DOWN(vma->vm_end),
		   PFN_DOWN((faddr & PMD_MASK) + PMD_SIZE));

	ra_info->nr_pte = end - start;
	ra_info->offset = fpfn - start;
	pte -= ra_info->offset;
#ifdef CONFIG_64BIT
	ra_info->ptes = pte;
#else
	tpte = ra_info->ptes;
	for (pfn = start; pfn != end; pfn++)
		*tpte++ = *pte++;
#endif
	trace_new_swap_ra_info(ra_info->ptes, ra_info->nr_pte, ra_info->offset, ra_info->win);
	pte_unmap(orig_pte);
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @fentry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whose
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
static struct page *swap_vma_readahead(swp_entry_t fentry, gfp_t gfp_mask,
				       struct vm_fault *vmf, int* try_free_entry)
{
	struct blk_plug plug;
	struct blk_plug plug_save;
	struct swap_iocb *splug = NULL;
	struct swap_iocb *splug_save = NULL;
	struct vm_area_struct *vma = vmf->vma;
	struct page *page;
	struct mem_cgroup *memcg;
	struct lruvec * lruvec;
	struct lru_gen_folio *lrugen;
	/*DJL ADD BEGIN*/
	struct swap_info_struct *si;
	/*DJL ADD END*/
	pte_t *pte, pentry;
	swp_entry_t entry, saved_entry, mig_entry;
	bool entry_retry_putback;
	bool save_slot_finish;
	unsigned int i;
	long tmp;
	bool page_allocated;
	swp_entry_t ori_pri_entry;
	struct folio* folio_list_wb[SWAP_SLOTS_SCAN_SAVE_ONCE];
	int num_folio_list_wb = 0;

	int try_free, prio_ori, prio_mig, entry_saved, err;
	*try_free_entry = 1;
	struct vma_swap_readahead ra_info = {
		.win = 1,
	};
	struct swap_iocb *plug_save_wb = NULL;

	swap_ra_info(vmf, &ra_info);
	if (ra_info.win == 1)
		goto skip;
	if (swp_entry_test_special(fentry))
		goto skip;
	blk_start_plug(&plug);
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte;
	     i++, pte++) {
		pentry = *pte;
		if (!is_swap_pte(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		si = swp_swap_info(entry);
		if (si->flags & SWP_SYNCHRONOUS_IO){ // block all IOs
			continue;
		}
		/*DJL ADD BEGIN*/
		page = __read_swap_cache_async(entry, gfp_mask, vma,
					       vmf->address, &page_allocated, (!enable_ra_fast_evict) || (i == ra_info.offset), 
						   &try_free, true);//, (((vmf->address >> PAGE_SHIFT) + (i - ra_info.offset))<<PAGE_SHIFT));
		/*DJL ADD END*/
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage(page, false, &splug);
			if (i != ra_info.offset) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
			}
			else{
				memcg = vma->vm_mm->lru_gen.memcg;
				if (memcg)
					lruvec = mem_cgroup_lruvec(memcg, folio_pgdat(page_folio(page)));
				if (lruvec)
					lrugen = &lruvec->lrugen;
			}
			/*DJL ADD BEGIN*/
			si = get_swap_device(entry);
			trace_readahead_swap_readpage(page_folio(page), si);
			if (get_fastest_swap_prio() == si->prio){
				count_memcg_event_mm(vma->vm_mm, SWAPIN_FAST);
			} else if (get_slowest_swap_prio() == si->prio){
				count_memcg_event_mm(vma->vm_mm, SWAPIN_SLOW);
			} else{
				count_memcg_event_mm(vma->vm_mm, SWAPIN_MID);
			}
			put_swap_device(si);

			if (i == ra_info.offset){
				*try_free_entry = try_free;
			}
			/*DJL ADD END*/
		}
		put_page(page);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();
	// pr_err("swap_vma_readahead fentry[%lx]", fentry.val);
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR
	//try do serveral stale entry save here
	if (!lruvec || !lrugen)
		goto skip;
	save_slot_finish = true;
	entry_saved = 0;
	saved_entry = get_next_saved_entry(&save_slot_finish);
	if (!saved_entry.val || non_swap_entry(saved_entry)){ // can't provide now
		if (!save_slot_finish){
			pr_err("shouldn't happen. bad save");
			BUG();
		}
		// pr_err("swap_vma_readahead fentry[%lx], skip", fentry.val);
		goto skip;
		//scan slot empty now
	} 
	else{ //saved_entry has to be valid
		struct folio *folio = NULL, *next = NULL;
		int num_moved = 0;
		bool reset_private = false;
		struct swap_info_struct* p = NULL;
		entry_retry_putback = false;
		blk_start_plug(&plug_save);
		do { //continue
			int _err;
			reset_private = false;
			folio = next = NULL;
	
			pr_err("start dealing with saved entry[%lx]", saved_entry.val);
			if (unlikely(non_swap_entry(saved_entry))){
				goto skip_this_save;
			}
			//check if this is usable
			p = get_swap_device(saved_entry); //lock saved device
			if (!p){
				pr_err("saved_entry[%lx] is not used anymore", saved_entry.val);
				goto skip_this_save;
			}
			if (!data_race(p->flags & SWP_SYNCHRONOUS_IO) ||
		    	!(__swap_count(saved_entry) == 1)) {
				pr_err("entry[%lx] sync[%lu] cnt[%d] is not used anymore", 
							saved_entry.val, data_race(p->flags & SWP_SYNCHRONOUS_IO), __swap_count(entry));
				goto skip_this_save;
			}

			//valid entry, we can swapin
			if (swp_entry_test_special(saved_entry)){
				pr_err("swap entry [%lx] got but special bit used !", saved_entry.val);
				goto skip_this_save;
			}
			si = get_swap_device(saved_entry);
			//test if folio is already in swapcache
			folio = raw_swap_cache_get_folio(si, saved_entry);
			if (folio) {
				pr_err("saved_entry[%lx] raw_swap_cache_get_folio folio[%p] already dealed with",
						 saved_entry.val, folio);
				put_swap_device(si);
				folio_ref_dec(folio);
				goto skip_this_save;
			}
			put_swap_device(si);
			pr_err("saved_entry[%lx] dealed with",saved_entry.val);

			folio = vma_alloc_folio(GFP_NOWAIT, 0,
						vma, vmf->address, false); //we don't support derect recalim so it may fail
			if (!folio) {
				pr_err("saved_entry[%lx] fail alloc folio", saved_entry.val);
				entry_retry_putback = true;
				goto skip_this_save;
			}
			page = &folio->page;

			//folio alloc ok
			__folio_set_locked(folio);
			SetPageStaleSaved(page);
			__folio_set_swapbacked(folio);
			if (mem_cgroup_swapin_charge_folio(folio,
						vma->vm_mm, GFP_KERNEL,
						saved_entry)) {
				pr_err("mem_cgroup_swapin_charge_folio fail folio[%p]", folio);
				goto fail_page_out;
			}
			//don't mem_cgroup_swapin_uncharge_swap(entry);
			folio_set_swap_entry(folio, saved_entry);			
			swap_readpage(page, true, &splug_save);
			pr_err("swap_readpage finished page[%p]", page);
			count_memcg_event_mm(vma->vm_mm, SWAPIN_FAST_SAVE);

			//page was ok now
			folio_set_swappriolow(folio);
			folio_clear_swappriohigh(folio);

			//deal with page private
			if (!(page_private(folio_page(folio, 0)) == saved_entry.val)){
				pr_err("page pri mismatch");
				BUG();
			}

			mig_entry = folio_alloc_swap(folio, &tmp);
			if (!mig_entry.val || (mig_entry.val > LONG_MAX) 
					|| !data_race(p->flags & SWP_SYNCHRONOUS_IO)){ //have to xa_mk_value
				pr_err("invalide mig_entry[%lx] alloc swap", mig_entry.val);
				goto fail_page_out;
			}
			pr_err("folio_alloc_swap entry[%lx] v[%d] for folio[%p] cnt:%d",
				 mig_entry.val, swp_entry_test_special(mig_entry), folio, __swap_count(mig_entry));
			
			_err = add_to_swap_cache_save_check(folio, saved_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN), true);
			if (_err) {
				if (_err == -2 || _err == -3){
					pr_err("folio[%p] add_to_swap_cache_save_check interupted [%d]", folio, _err);
				}
				pr_err("folio[%p] add_to_swap_cache_save_check fail [%d]", folio, _err);
				put_swap_folio(folio, saved_entry);
				folio_unlock(folio);
				goto fail_page_out;
			}


			_err = add_to_swap_cache_save_check(folio, mig_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN), false);
			if (_err) {
				pr_err("folio[%p] add_to_swap_cache_save fail", folio);
				put_swap_folio(folio, mig_entry);
				folio_unlock(folio);
				goto fail_delete_mig_cache;
			}
			pr_err("folio[%p] saved[%lx] ref[%d] add_to_swap_cache_save_check success ", 
					folio, saved_entry.val, folio_ref_count(folio));			
			//first mark entry as faked for now (currently under initialization)
			swp_entry_set_ext(&mig_entry, 0x1);
			//adding a remap from saved_entry -> mig_entry
			//this is the first time mig_entry got into remap, and its irq locked
			//we don't care about if it is locked
			err = add_swp_entry_remap(folio, saved_entry, mig_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN));
			if (err){
				pr_err("folio[%p] add_swp_entry_remap fail", folio);
				folio_unlock(folio);
				goto fail_delete_saved_cache;
			}
			pr_err("add_swp_entry_remap [%lx]=>[%lx]", saved_entry.val, mig_entry.val);
			swp_entry_clear_ext(&mig_entry, 0x3); // 局部变量无所谓
			//now we add the real entry
			VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

			//set private of folio & trigger 
			ori_pri_entry.val = page_private(folio_page(folio, 0));
			set_page_private(folio_page(folio, 0), mig_entry.val);

			//do async pageout
			VM_BUG_ON_FOLIO(!folio_mapping(folio), folio);
			folio_mark_dirty(folio); //to force wb
			try_to_unmap_flush_dirty();
			/*
			 * Folio is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after I/O
			 * starts and then write it out here.
			 */
			if (folio_mapping(folio)){
				switch (pageout_save(folio, folio_mapping(folio), &plug_save_wb)) {
				case 0: //PAGE_KEEP
					pr_err("pageout returns PAGE_KEEP");
					goto fail_page_out;
				case 1: //PAGE_ACTIVATE
					pr_err("pageout returns PAGE_ACTIVATE");
					goto fail_page_out;
				case 2: //PAGE_SUCCESS
					//check if sync io
					if (folio_test_dirty(folio)){ //unexpected
						pr_err("PAGE_SUCCESS dirty folio[%p]", folio);
						BUG();
					}					
					if (folio_test_writeback(folio)){//sync, under wb	
						folio_list_wb[num_folio_list_wb++] = folio; 	
						//check later in vmscan
					}
					else{ //A synchronous write - probably a ramdisk.
						; //should remove mapping and clean & free it
						pr_err("not inplemented [%s:%d] folio[%p] -> pageout -> entry[%lx]",
								 __FILE__, __LINE__, folio, mig_entry.val);
						BUG();
					}
					goto scceed_pageout;
				case 3: //PAGE_CLEAN
					pr_err("pageout returns PAGE_CLEAN");
				}
fail_delete_saved_cache:
				pr_err("fail_delete_saved_cache");
				delete_from_swap_cache_mig(folio, saved_entry, true, false); //page private is also cleared
fail_delete_mig_cache:
				pr_err("fail_delete_mig_cache");
				swp_entry_clear_ext(&mig_entry, 0x3);
				delete_from_swap_cache_mig(folio, mig_entry, true, false); //page private is also cleared
				// reset_private = true;
fail_page_out:
				// put_swap_folio(folio, mig_entry);
				pr_err("fail page_out folio[%p], mig_entry[%lx]", folio, mig_entry.val);
				folio_clear_dirty(folio);
				folio_clear_stalesaved(folio);
				folio_unlock(folio);
				folio_put(folio);
				goto skip_this_save;
scceed_pageout:
				;
				reset_private = true;
			}
			//success triggered a swapout to slow for migration
			count_memcg_event_mm(vma->vm_mm, SWAPOUT_SLOW_SAVE);

			//set back private just for progressing test
			si = get_swap_device(saved_entry);
			prio_ori = si->prio;
			put_swap_device(si);
			si = get_swap_device(mig_entry);
			prio_mig = si->prio;
			put_swap_device(si);

			trace_vma_ra_save_stale(saved_entry.val, mig_entry.val, entry_saved, prio_ori, prio_mig);
			//end dealing with current entry_saved
			entry_saved ++ ;
			folio_set_stalesaved(folio);
			if (folio_test_locked(folio)){
				pr_err("page not lock bug");
				BUG();
			}
skip_this_save:
			if (reset_private){ 
				//we can change it back , to maintain correctness befor bio complete, 
				// because bio has been submitted, doesn't need it(private = migentry) anymore
				set_page_private(folio_page(folio, 0), ori_pri_entry.val);
				// pr_err("reset folio[%p]lru[%d] private [%lx]", folio, 
				// 			folio_test_lru(folio), page_private(folio_page(folio, 0)));
			}
			if (entry_retry_putback){
				putback_last_saved_entry(saved_entry);
				pr_err("putback entry[%lx]", saved_entry.val);
			}

			if (p)
				put_swap_device(p);

			//get next
			if (entry_saved >= SWAP_SLOTS_SCAN_SAVE_ONCE)
				break;			
			saved_entry = get_next_saved_entry(&save_slot_finish);
		} while (!save_slot_finish);

		if (plug_save_wb)
			swap_write_unplug(plug_save_wb);
		
		// if (non_swap_entry(saved_entry) && save_slot_finish){
		// 	//restart load
		// 	reenable_scan_cpu();
		// }

		blk_finish_plug(&plug_save);
		swap_read_unplug(splug_save);
		//add to lru_gen
		pr_err("add pages to lru_gen");
		if (lruvec && lrugen){
			spin_lock_irq(&lruvec->lru_lock);
			for (int i = 0; i < num_folio_list_wb; i++){
				struct folio * folio;
				num_moved++;
				if (num_moved > num_folio_list_wb){
					pr_err("num_moved >= entry_saved err");
					break;	
				}
				folio = folio_list_wb[i];//lru_to_folio_next(&folio_list_wb);
				VM_BUG_ON_FOLIO(!folio_test_writeback(folio), folio);
				pr_err("folio[%p] => saved_folios", folio);
				list_add(&folio->lru, &lrugen->saved_folios);
				trace_add_to_lruvec_saved_folios(lruvec, folio, num_moved);	
			}
			spin_unlock_irq(&lruvec->lru_lock);
		}
		else{
			struct folio* folio = NULL;
			pr_err("there's no lruvec[%p], unlock all folio in list", lruvec);
			BUG();
			for (int i = 0; i < num_folio_list_wb; i++){
				folio = folio_list_wb[i];
				folio_unlock(folio);
			}
		}
	}
#endif

skip:
	/* The page was likely read above, so no need for plugging here */
	return read_swap_cache_async(fentry, gfp_mask, vma, vmf->address,
				     ra_info.win == 1, NULL, true, try_free_entry);
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct page *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf, int* try_free_entry)
{
	return swap_use_vma_readahead() ?
			swap_vma_readahead(entry, gfp_mask, vmf, try_free_entry) :
			swap_cluster_readahead(entry, gfp_mask, vmf, try_free_entry);
}

void swap_scan_save_entries(swp_entry_t *entries, int n){
	return;
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  enable_vma_readahead ? "true" : "false");
}
static ssize_t swap_scan_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  swap_scan_enabled_sysfs ? "true" : "false");
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &enable_vma_readahead);
	if (ret)
		return ret;

	return count;
}
static ssize_t swap_scan_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &swap_scan_enabled_sysfs);
	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute vma_ra_enabled_attr = __ATTR_RW(vma_ra_enabled);
static struct kobj_attribute swap_scan_enabled_attr = __ATTR_RW(swap_scan_enabled);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	&swap_scan_enabled_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init_sysfs(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init_sysfs);
#endif
