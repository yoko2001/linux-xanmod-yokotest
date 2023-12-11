/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_SCAN_SLOT_H
#define _LINUX_SWAP_SCAN_SLOT_H

#include <linux/swap.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#define SWAP_SCAN_SLOT_SIZE			SWAP_BATCH * 4
#define SWAP_SLOTS_SCAN_MIN            SWAP_SCAN_SLOT_SIZE

#define SEQ_DIFF_THRESHOLD             8

#define THRESHOLD_ACTIVATE_SWAP_SCAN_SLOT  8  //when under 1/8
#define THRESHOLD_DEACTIVATE_SWAP_SCAN_SLOT 2  //when more than 1/2
struct swap_scan_slot {
	bool		lock_initialized;
	spinlock_t	scan_lock; /* protects slots, nr, cur */
	int		nr;
    struct swap_info_struct * si;
	swp_entry_t	*slots; //store scanned
};

void enable_swap_scan_slot(void);
void disable_swap_scan_slot(void);

#endif /* _LINUX_SWAP_SLOTS_H */
