/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM swap

#if !defined(_TRACE_SWAP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SWAP_H

#include <linux/tracepoint.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>

#define	PAGEMAP_MAPPED		0x0001u
#define PAGEMAP_ANONYMOUS	0x0002u
#define PAGEMAP_FILE		0x0004u
#define PAGEMAP_SWAPCACHE	0x0008u
#define PAGEMAP_SWAPBACKED	0x0010u
#define PAGEMAP_MAPPEDDISK	0x0020u
#define PAGEMAP_BUFFERS		0x0040u

#define trace_pagemap_flags(folio) ( \
	(folio_test_anon(folio)		? PAGEMAP_ANONYMOUS  : PAGEMAP_FILE) | \
	(folio_mapped(folio)		? PAGEMAP_MAPPED     : 0) | \
	(folio_test_swapcache(folio)	? PAGEMAP_SWAPCACHE  : 0) | \
	(folio_test_swapbacked(folio)	? PAGEMAP_SWAPBACKED : 0) | \
	(folio_test_mappedtodisk(folio)	? PAGEMAP_MAPPEDDISK : 0) | \
	(folio_test_private(folio)	? PAGEMAP_BUFFERS    : 0) \
	)


TRACE_EVENT(folio_add_to_swap,

	TP_PROTO(struct folio *folio),

	TP_ARGS(folio),

	TP_STRUCT__entry(
		__field(struct folio *,	folio	)
		__field(unsigned long,	entry	)
		__field(unsigned long,	flags	)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->entry	= page_private(folio_page(folio, 0));
		__entry->flags	= trace_pagemap_flags(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio=%p[%s] entry=0x%lx flags=%s%s%s%s%s%s",
			__entry->folio,
			folio_test_transhuge(__entry->folio)? "T": "N",
			__entry->entry,
			__entry->flags & PAGEMAP_MAPPED		? "M" : " ",
			__entry->flags & PAGEMAP_ANONYMOUS	? "a" : "f",
			__entry->flags & PAGEMAP_SWAPCACHE	? "s" : " ",
			__entry->flags & PAGEMAP_SWAPBACKED	? "b" : " ",
			__entry->flags & PAGEMAP_MAPPEDDISK	? "d" : " ",
			__entry->flags & PAGEMAP_BUFFERS	? "B" : " ")
);
/*DJL ADD BEGIN*/
TRACE_EVENT(get_swap_pages_noswap,

	TP_PROTO(int ckpt, int n_ret, int n_goal, int avail_pgs),

	TP_ARGS(ckpt, n_ret, n_goal, avail_pgs),

	TP_STRUCT__entry(
		__field(int, ckpt)
		__field(int, n_ret)
		__field(int, n_goal)
		__field(int, avail_pgs)
	),

	TP_fast_assign(
		__entry->ckpt   = ckpt;
		__entry->n_ret	= n_ret;
		__entry->n_goal	= n_goal;
		__entry->avail_pgs	= avail_pgs;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("[ckpt%d] try to get %d pages but got %d, avail_pgs is %d",
			__entry->ckpt, __entry->n_goal, __entry->n_ret, __entry->avail_pgs)
);

TRACE_EVENT(swap_alloc_cluster,

	TP_PROTO(struct swap_info_struct *si, bool empty),

	TP_ARGS(si, empty),

	TP_STRUCT__entry(
		__field(struct swap_info_struct *, si)
		__field(bool , empty)
	),

	TP_fast_assign(
		__entry->si	= si;
		__entry->empty = empty;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("swap_alloc_cluster from si[%p] %s",
			__entry->si, __entry->empty ? "empty" : "not empty")
);

TRACE_EVENT(scan_swap_map_slots,

	TP_PROTO(int ckpt),

	TP_ARGS(ckpt),

	TP_STRUCT__entry(
		__field(int, ckpt)
	),

	TP_fast_assign(
		__entry->ckpt	= ckpt;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("scan_swap_map_slots ckpt %d",
			__entry->ckpt)
);

TRACE_EVENT(new_swap_ra_info,

	TP_PROTO(pte_t *pte, unsigned long nr_pte, unsigned long offset, unsigned int win),

	TP_ARGS(pte, nr_pte, offset, win),

	TP_STRUCT__entry(
		__field(pte_t*, pte)
		__field(unsigned long, nr_pte)
		__field(unsigned long, offset)
		__field(unsigned int ,  win)
	),

	TP_fast_assign(
		__entry->pte	= pte;
		__entry->nr_pte	= nr_pte;
		__entry->offset	= offset;
		__entry->win	= win;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("new_swap_ra_info ptes[0]=%lu; nr_pte=%lu, offset=%lu, win=%u",
			(unsigned long)__entry->pte, __entry->nr_pte, 
			 __entry->offset, __entry->win)
);

TRACE_EVENT(do_swap_page,

	TP_PROTO(int ckpt, struct folio* folio),

	TP_ARGS(ckpt, folio),

	TP_STRUCT__entry(
		__field(int, ckpt)
		__field(struct folio*, folio)
	),

	TP_fast_assign(
		__entry->ckpt	= ckpt;
		__entry->folio	= folio;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("ckpt %d folio@[%p]",
			__entry->ckpt, __entry->folio)
);

TRACE_EVENT(folio_add_lru,

	TP_PROTO(struct folio* folio, bool ra),

	TP_ARGS(folio, ra),

	TP_STRUCT__entry(
		__field(struct folio*, folio)
		__field(bool, ra)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->ra	= ra;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio@[%p], %s;%s;%s;%s gen[%d], ra[%d]",
			__entry->folio, 
			folio_test_reclaim(__entry->folio) ? "[Reclm]" : "",
			folio_test_dirty(__entry->folio) ? "[Dirty]" : "",
			folio_test_writeback(__entry->folio) ? "[wb]" : "",
			__entry->ra ? "[ra-noactv]":"[actv]", 
			folio_lru_gen(__entry->folio), 
			folio_test_readahead(__entry->folio))
);

TRACE_EVENT(readahead_swap_readpage,

	TP_PROTO(struct folio* folio, struct swap_info_struct * si),

	TP_ARGS(folio, si),

	TP_STRUCT__entry(
		__field(struct folio* ,folio)
		__field(struct swap_info_struct *,  si)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->si	= si;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio[%p]low[%d]high[%d] ra[%d] from-prio[%d]", 
	          __entry->folio, 
			  folio_test_swappriolow(__entry->folio), folio_test_swappriohigh(__entry->folio),
			  folio_test_readahead(__entry->folio), 
			  __entry->si->prio)
);

TRACE_EVENT(swapin_readahead_hit,

	TP_PROTO(struct folio* folio),

	TP_ARGS(folio),

	TP_STRUCT__entry(
		__field(struct folio* ,folio)
	),

	TP_fast_assign(
		__entry->folio	= folio;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio[%p] ",__entry->folio)
);

TRACE_EVENT(swapin_force_wake_kswapd,

	TP_PROTO(int order),

	TP_ARGS(order),

	TP_STRUCT__entry(
		__field(int, order)
	),

	TP_fast_assign(
		__entry->order	= order;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("order %d ",__entry->order)
);

TRACE_EVENT(folio_inc_refs,

	TP_PROTO(struct folio *folio, int ckpt),

	TP_ARGS(folio, ckpt),

	TP_STRUCT__entry(
		__field(struct folio *, folio)
		__field(int, ckpt)
		__field(int, ref)
	),

	TP_fast_assign(
		__entry->folio  = folio;
		__entry->ckpt	= ckpt;
		__entry->ref    = folio_lru_refs(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio[%p] [%s] ref[%d]",
	__entry->folio, 
	__entry->ckpt == 0 ? "unevivt" : 
   (__entry->ckpt == 1 ? "set ref" : 
   (__entry->ckpt == 2 ? "set work" :
   "new_flags")), 
   __entry->ref)
);

TRACE_EVENT(should_try_to_free_swap,

	TP_PROTO(struct folio *folio, int low, int high, int fault_flags, int ksm),

	TP_ARGS(folio, low, high, fault_flags, ksm),

	TP_STRUCT__entry(
		__field(struct folio *, folio)
		__field(int, low)
		__field(int, high)
		__field(int, fault_flags)
		__field(int, ksm)
	),

	TP_fast_assign(
		__entry->folio  = folio;
		__entry->low  = low;
		__entry->high  = high;
		__entry->fault_flags	= fault_flags;
		__entry->ksm    = ksm;
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio[%p]low[%d]high[%d] fault_flags[%d] ksm[%d]", __entry->folio,
	__entry->low, __entry->high,
	__entry->fault_flags, __entry->ksm)
);
/*DJL ADD END*/
#endif /* _TRACE_SWAP_H */
#include <trace/define_trace.h>