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
	if (xa_is_value(page))
		return page;
	else if (entry_is_entry_ext(page))
		return page;
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
	void *old;

	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapcache(folio), folio);
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
			old = xas_load(&xas);
			if (xa_is_value(old)) { //files
				if (shadowp)
					*shadowp = old;
			}
			else if (entry_is_entry_ext(old)){ //swap
				if (shadowp)
					*shadowp = old;
				else{
					pr_err("add to swcache but free");
					shadow_entry_free(old);
					atomic_dec(&ext_count);
				}
			}
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
		from_entry_2.val = from_entry.val;
		swp_entry_set_special(&from_entry_2, current_offset); //set offset
		pgoff_t idx = swp_ext_spec_offset(from_entry_2);
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

/*only supports single page remap*/
int add_swp_entry_remap(struct folio* folio, swp_entry_t from_entry, swp_entry_t to_entry, 
			gfp_t gfp)
{
	struct address_space *address_space = swap_address_space_remap(from_entry);
	long nr = folio_nr_pages(folio);
	int i;
	pgoff_t idx = swp_ext_spec_offset(from_entry);
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
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
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

/*
 * add_to_swap_cache resembles filemap_add_folio on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
int add_to_swap_cache_save(struct folio *folio, swp_entry_t entry,
			gfp_t gfp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_ext_offset(entry); //swp_offset(entry);
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
			// set_page_private(folio_page(folio, i), entry.val + i);
			xas_store(&xas, folio);
			xas_next(&xas);
		}
		address_space->nrpages += nr;
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	folio_ref_sub(folio, nr);
	return xas_error(&xas);
}

extern spinlock_t shadow_ext_lock;

swp_entry_t folio_get_migentry(struct folio* folio, swp_entry_t ori_swap)
{
	swp_entry_t mig_swap;
	int i;
	struct address_space *address_space_remap;
	VM_BUG_ON_FOLIO((page_private(folio_page(folio, 0)) == 0), folio);

	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xas_set_update(&xas, workingset_update_node);
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
	if (folio_test_stalesaved(folio)){
		pr_err("folio[%pK] origin swapcache clearing", folio);
	}
	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, shadow);
		VM_BUG_ON_PAGE(entry != folio, entry);
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
		// spin_lock_irq(&shadow_ext_lock);
		// if (folio->shadow_ext && entry_is_entry_ext(folio->shadow_ext)){
		// 	pr_err("delete_from_swap_cache has folio->shadow_ext");
		// 	shadow_entry_free(folio->shadow_ext);
		// 	folio->shadow_ext = NULL;
		// 	atomic_dec(&ext_count);
		// }
		// spin_unlock_irq(&shadow_ext_lock);
#endif
		set_page_private(folio_page(folio, i), 0);
		xas_next(&xas);
	}
	folio_clear_swapcache(folio);
	address_space->nrpages -= nr;
	__node_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr);
}

void __delete_from_swap_cache_mig(struct folio *folio,
			swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	int i;
	long nr = folio_nr_pages(folio);
	void *shadow = NULL;
	pgoff_t idx = swp_ext_spec_offset(entry);
	XA_STATE(xas, &address_space->i_pages, idx);

	xas_set_update(&xas, workingset_update_node);
	
	pr_err("folio[%pK] migrate swapcache clearing", folio);

	VM_BUG_ON_FOLIO(!folio_test_stalesaved(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, shadow);
		VM_BUG_ON_PAGE(entry != folio, entry);
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
	__node_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr);
}

void __delete_from_swap_remap(struct folio *folio, swp_entry_t entry_from, swp_entry_t entry_to)
{
	struct address_space *address_space = swap_address_space_remap(entry_from);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_ext_spec_offset(entry_from);
	XA_STATE(xas, &address_space->i_pages, idx);
	swp_entry_t tmp;
	pr_err("folio[%pK] migrate remap clearing", folio);

	VM_BUG_ON_FOLIO(!folio_test_stalesaved(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, NULL); //clear
		tmp.val = xa_to_value(entry);
		VM_BUG_ON_PAGE(tmp.val != entry_to.val, folio);
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
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
	put_swap_folio(folio, entry);
	return false;
}

/*
 * the caller has a reference on the folio.
 */
void delete_from_swap_remap(struct folio *folio, swp_entry_t entry_from, swp_entry_t entry_to){
	struct address_space *address_space = swap_address_space_remap(entry_from);
	if (!address_space){
		pr_err("delete_from_swap_remap address space BUG");
		return;
	}
	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_remap(folio, entry_from, entry_to);
	xa_unlock_irq(&address_space->i_pages);
	folio_ref_sub(folio, folio_nr_pages(folio));
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
void delete_from_swap_cache_mig(struct folio* folio, swp_entry_t entry)
{
	swp_entry_t pgentry = folio_swap_entry(folio);
	struct address_space *address_space = swap_address_space(entry);

	VM_BUG_ON_FOLIO(!(entry.val == pgentry.val), folio);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache_mig(folio, entry);
	xa_unlock_irq(&address_space->i_pages);

	folio_ref_sub(folio, folio_nr_pages(folio));
}

void clear_shadow_from_swap_cache(int type, unsigned long begin,
				unsigned long end, int free)
{
	unsigned long curr = begin;
	void *old;
	for (;;) {
		swp_entry_t entry = swp_entry(type, curr);
		struct address_space *address_space = swap_address_space(entry);
		XA_STATE(xas, &address_space->i_pages, curr);

		xas_set_update(&xas, workingset_update_node);

		xa_lock_irq(&address_space->i_pages);
		xas_for_each(&xas, old, end) {
			if (!xa_is_value(old) && !entry_is_entry_ext(old))
				continue;
			if (free && old && entry_is_entry_ext(old)){
				// pr_err("clear_shadow_from_s [%lx]", old);
				// spin_lock_irq(&shadow_ext_lock);
				xas_store(&xas, NULL);
				// shadow_entry_free(old);
				// spin_unlock_irq(&shadow_ext_lock);
				continue;
			}
			xas_store(&xas, NULL);
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
struct folio *swap_cache_get_folio(swp_entry_t entry,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct folio *folio;

	folio = filemap_get_folio(swap_address_space(entry), swp_offset(entry));
	if (folio) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;

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
static struct page*__read_swap_cache_async_save(swp_entry_t entry, 	
	gfp_t gfp_mask, struct vm_area_struct *vma, 
	unsigned long addr,	bool *new_page_allocated, 	bool no_ra, 
	int* try_free_entry, unsigned long realaddr)
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
			return folio_file_page(folio, swp_offset(entry));

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			return NULL;

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		folio = vma_alloc_folio(gfp_mask, 0, vma, addr, false);
		if (!folio)
			return NULL;

		// /*
		//  * Swap entry may have been freed since our caller observed it.
		//  */
		err = swapcache_prepare(entry);
		if (!err)
			break;

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
	VM_BUG_ON_FOLIO(folio, folio->shadow_ext);
	folio->shadow_ext = NULL;
	if (entry_is_entry_ext(shadow)){
		folio->shadow_ext = shadow;
	}
#else
	if (entry_is_entry_ext(shadow)){
		shadow_entry_free(shadow);
		atomic_dec(&ext_count);
	}
#endif
	/*DJL ADD END*/

	/* Caller will initiate read into locked folio */

	//folio_add_lru_save(folio);

	*new_page_allocated = true;
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
	int* try_free_entry, unsigned long realaddr)
/*DJL ADD END*/
{
	struct swap_info_struct *si;
	struct folio *folio;
	void *shadow = NULL;
	/*DJL ADD BEGIN*/
	struct lruvec *lruvec;
	pg_data_t* pgdat;
	int swap_level = -2;
	/*DJL ADD END*/
	*new_page_allocated = false;
	int rf_dist_ts = -1;

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
			return folio_file_page(folio, swp_offset(entry));

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			return NULL;

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		folio = vma_alloc_folio(gfp_mask, 0, vma, addr, false);
		if (!folio)
			return NULL;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;

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
	// spin_lock_irq(&shadow_ext_lock);
	VM_BUG_ON_FOLIO(folio, folio->shadow_ext);
	folio->shadow_ext = NULL;
	if (entry_is_entry_ext(shadow)){
		folio->shadow_ext = shadow;
	}
	// spin_unlock_irq(&shadow_ext_lock);
#else
	// spin_lock_irq(&shadow_ext_lock);
	if (entry_is_entry_ext(shadow)){
		shadow_entry_free(shadow);
		atomic_dec(&ext_count);
	}
	// spin_unlock_irq(&shadow_ext_lock);
#endif
	/*DJL ADD END*/

	/* Caller will initiate read into locked folio */
	// folio_add_lru(folio);
	if (no_ra)
		folio_add_lru(folio);
	else
		folio_add_lru_ra(folio);		

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
			vma, addr, &page_was_allocated, true, try_free_entry, addr);

	if (page_was_allocated)
		swap_readpage(retpage, do_poll, plug);

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
	unsigned long entry_offset = swp_offset(entry);
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
			gfp_mask, vma, addr, &page_allocated, true, &try_free, 
			addr); //DJL doesn't support fast swapout
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

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	nr_remap = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_REMAP_PAGES);
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
		space_remap->a_ops = &swap_aops;
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
	swp_entry_t entry, saved_entry, mig_entry, masked_saved_entry;
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

	blk_start_plug(&plug);
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte;
	     i++, pte++) {
		pentry = *pte;
		if (!is_swap_pte(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		/*DJL ADD BEGIN*/
		page = __read_swap_cache_async(entry, gfp_mask, vma,
					       vmf->address, &page_allocated, (!enable_ra_fast_evict) || (i == ra_info.offset), 
						   &try_free, (((vmf->address >> PAGE_SHIFT) + (i - ra_info.offset))<<PAGE_SHIFT));
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

	//try do serveral stale entry save here
	save_slot_finish = true;
	entry_saved = 0;
	saved_entry = get_next_saved_entry(&save_slot_finish);
	if (non_swap_entry(saved_entry)){ // can't provide now
		if (!save_slot_finish)
			pr_err("shouldn't happen. bad save");
		//scan slot empty now
	} 
	else{
		struct folio *folio = NULL, *next = NULL;
		int num_moved = 0;
		bool reset_private = false;
		blk_start_plug(&plug_save);
		while (!save_slot_finish){ //continue
			reset_private = false;
			folio = next = NULL;
			if (unlikely(non_swap_entry(saved_entry))){
				goto skip_this_save;
			}
			//valid entry
			if (swp_entry_test_special(saved_entry)){
				pr_err("swap entry [%lx] got but special bit used !", saved_entry.val);
				goto skip_this_save;
			}

			//deal with current entry_saved
			page = __read_swap_cache_async_save(saved_entry, gfp_mask, vma,
					vmf->address, &page_allocated, false, 
					&try_free, vmf->address);
			if (!page)
				goto skip_this_save;
			folio = page_folio(page);			
			if (page_allocated) {
				swap_readpage(page, false, &splug_save);
				count_vm_event(SWAP_STALE_SAVE);
			}
			else {
				put_page(page);
				pr_err("page[%pK] stale[%d] ref[%d] already in swapcache, we don't bother it here", 
						page, TestClearPageStaleSaved(page), folio_ref_count(folio));
				goto skip_this_save;
			}
			SetPageStaleSaved(page);
			folio_set_swappriolow(folio);
			folio_clear_swappriohigh(folio);

			// //check if its a valid saved entry
			// err = prepare_swp_entry(saved_entry, &masked_saved_entry);
			// if (err){
			// 	pr_err("prepare_swp_entry failure");
			// 	goto skip_this_save;
			// }

			mig_entry = folio_alloc_swap(folio, &tmp);
			if (!mig_entry.val || (mig_entry.val > LONG_MAX)){ //have to xa_mk_value
				goto skip_this_save;
			}

			//we don't update entry, only add to swap_cache
			//first mark entry as faked for now (currently under initialization)
			swp_entry_set_ext(&mig_entry, 0x1);

			err = add_to_swap_cache_save(folio, mig_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN));
			if (err){
				put_swap_folio(folio, mig_entry);
				folio_unlock(folio);
				goto skip_this_save;
			}
			//adding a remap from saved_entry -> mig_entry
			err = add_swp_entry_remap(folio, saved_entry, mig_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN));
			if (err){
				pr_err("folio[%pK] add_swp_entry_remap fail", folio);
				folio_unlock(folio);
				goto fail_delete_mig_cache;
			}
			pr_err("add_swp_entry_remap [%lx]=>[%lx]", saved_entry.val, mig_entry.val);
			//now we add the real entry
			VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

			folio_mark_dirty(folio); //to force wb
			try_to_unmap_flush_dirty();
			/*
			 * Folio is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after I/O
			 * starts and then write it out here.
			 */

			//set private of folio & trigger 
			set_page_private(folio_page(folio, 0), saved_entry.val);
			ori_pri_entry.val = page_private(folio_page(folio, 0));
			set_page_private(folio_page(folio, 0), mig_entry.val);
			reset_private = true;
			//do async pageout
			VM_BUG_ON_FOLIO(!folio_mapping(folio), folio);
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
						pr_err("PAGE_SUCCESS dirty folio[%pK]", folio);
					}					
					if (folio_test_writeback(folio)){//sync, under wb	
						folio_list_wb[num_folio_list_wb++] = folio; 	
						//check later in vmscan
					}
					else{ //A synchronous write - probably a ramdisk.
						; //should remove mapping and clean & free it
						pr_err("not inplemented [%s:%d]", __FILE__, __LINE__);
					}
					goto scceed_pageout;
				case 3: //PAGE_CLEAN
					pr_err("pageout returns PAGE_CLEAN");
				}

fail_delete_mig_cache:
				delete_from_swap_cache_mig(folio, mig_entry); //page private is also cleared
				reset_private = true;
fail_page_out:
				put_swap_folio(folio, mig_entry);
				folio_clear_dirty(folio);
				folio_clear_stalesaved(folio);
				folio_unlock(folio);
				goto skip_this_save;
scceed_pageout:
				;
			}

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

skip_this_save:
			if (reset_private){ 
				//we can change it back , to maintain correctness befor bio complete, 
				// because bio has been submitted, doesn't need it(private = migentry) anymore
				pr_err("reset folio[%pK]lru[%d] private [%lx]", folio, 
							folio_test_lru(folio), ori_pri_entry.val);
				set_page_private(folio_page(folio, 0), ori_pri_entry.val);
			}

			//get next
			if (entry_saved >= SWAP_SLOTS_SCAN_SAVE_ONCE)
				break;			
			saved_entry = get_next_saved_entry(&save_slot_finish);
		}
		if (plug_save_wb)
			swap_write_unplug(plug_save_wb);
		
		if (non_swap_entry(saved_entry) && save_slot_finish){
			//restart load
			reenable_scan_cpu();
		}

		blk_finish_plug(&plug_save);
		swap_read_unplug(splug_save);
		//add to lru_gen
		if (lruvec && lrugen){
			spin_lock_irq(&lruvec->lru_lock);
			for (int i = 0; i < num_folio_list_wb; i++){
				num_moved++;
				if (num_moved > num_folio_list_wb){
					pr_err("num_moved >= entry_saved err");
					break;	
				}
				struct folio * folio = folio_list_wb[i];//lru_to_folio_next(&folio_list_wb);
				VM_BUG_ON_FOLIO(!folio_test_writeback(folio), folio);
				
				list_add(&folio->lru, &lrugen->saved_folios);
				trace_add_to_lruvec_saved_folios(lruvec, folio, num_moved);	
			}
			spin_unlock_irq(&lruvec->lru_lock);
		}
		else{
			struct folio* folio = NULL;
			pr_err("there's no lruvec, unlock all folio in list");
			for (int i = 0; i < num_folio_list_wb; i++){
				folio = folio_list_wb[i];
				folio_unlock(folio);
			}
		}
	}
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
static struct kobj_attribute vma_ra_enabled_attr = __ATTR_RW(vma_ra_enabled);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
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
