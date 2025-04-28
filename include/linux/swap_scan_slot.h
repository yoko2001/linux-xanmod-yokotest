/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_SCAN_SLOT_H
#define _LINUX_SWAP_SCAN_SLOT_H

#include <linux/swap.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

/* 
 * SWAP_SCAN_SLOT_SIZE
 * migrate target batch size. When scanning batch is full, 
 * stops scanning and start migration process
 */
#define SWAP_SCAN_SLOT_SIZE			  SWAP_BATCH	

/* 
 * SWAP_SLOTS_SCAN_MIN
 * Minimum slot scan number at one attempt.
 * If scanning batch hasn't fill up, scan will
 * be triggered again at next kswapd awake.
 */
#define SWAP_SLOTS_SCAN_MIN            SWAP_SCAN_SLOT_SIZE * 16

/*
 * SWAP_SLOTS_SCAN_SAVE_ONCE
 * When swap_vma_readahead is triggerd, 
 * At most this much swap migration process will
 * be triggerd at once. 
 */
#define SWAP_SLOTS_SCAN_SAVE_ONCE		16 


/*
 * SEQ_DIFF_THRESHOLD
 * When swap_vma_readahead is triggerd, 
 * At most this much swap migration process will
 * be triggerd at once. 
 */
#define SEQ_DIFF_THRESHOLD             2

/*
 * swap scanning watermark
 * start scanning when fast swap is under 1/ACTIVATE
 * stop scanning when fast swap is over 1/DEACTIVATE
 */
#define THRESHOLD_ACTIVATE_SWAP_SCAN_SLOT  32
#define THRESHOLD_DEACTIVATE_SWAP_SCAN_SLOT 16

struct swap_scan_slot {
	bool		lock_initialized;
	spinlock_t	scan_lock; /* protects slots, nr, cur */
	bool	scan_stop; /* protects slots, nr, cur */
	int		nr;
	int 	cur;
    struct swap_info_struct * si;
	swp_entry_t	*slots; //store scanned
};

void enable_swap_scan_slot(void);
void disable_swap_scan_slot(void);
swp_entry_t get_next_saved_entry(bool* finished);
void putback_last_saved_entry(swp_entry_t last);
void reenable_scan_cpu(void);

#endif /* _LINUX_SWAP_SLOTS_H */
