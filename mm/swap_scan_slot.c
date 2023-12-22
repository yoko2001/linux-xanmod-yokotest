#include <linux/swap_scan_slot.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <trace/events/lru_gen.h>

// static DEFINE_PER_CPU(struct swap_scan_slot, swp_scan_slots);
static DEFINE_MUTEX(swap_scan_slot_mutex);
static DEFINE_MUTEX(swap_scan_slot_enable_mutex);
static bool	swap_scan_slot_initialized;
static bool swap_scan_slot_enabled;
static bool	swap_scan_slot_active;

#define use_swap_scan_slot (swap_scan_slot_enabled && swap_scan_slot_active)

struct swap_scan_slot global_swp_scan_slot;

static void deactivate_swap_scan_slot(void)
{
	if (!swap_scan_slot_active)
		return;
	mutex_lock(&swap_scan_slot_mutex);
	swap_scan_slot_active = false;
	mutex_unlock(&swap_scan_slot_mutex);
}

static void reactivate_swap_scan_slot(void)
{
	if (swap_scan_slot_active)
		return;
	mutex_lock(&swap_scan_slot_mutex);
	swap_scan_slot_active = true;
	mutex_unlock(&swap_scan_slot_mutex);
}

void check_swap_scan_active(struct swap_info_struct *si, long left, long total)
{
	if (!swap_scan_slot_enabled)
		return;

	//check if fast swap low, use scan
	if (left * THRESHOLD_ACTIVATE_SWAP_SCAN_SLOT < total){
		reactivate_swap_scan_slot();
		trace_swap_scan_change_state(1, left, total);
	}
	//check if slow swap high, cancel scan
	if (left * THRESHOLD_DEACTIVATE_SWAP_SCAN_SLOT > total){
		deactivate_swap_scan_slot();	 
		trace_swap_scan_change_state(0, left, total);
	}
}

static int alloc_swap_scan_slot(unsigned int cpu)
{
	struct swap_scan_slot *cache;
    swp_entry_t *slots;

	slots = kvcalloc(SWAP_SCAN_SLOT_SIZE, sizeof(swp_entry_t),
			 GFP_KERNEL);
	if (!slots){
		return -ENOMEM;
	}
	cache = &global_swp_scan_slot;
	if (cache->slots){
		kvfree(slots);
		pr_err("already alloced ?");
		return 0;
	}
	if (!cache->lock_initialized) {
		spin_lock_init(&cache->scan_lock);
		cache->lock_initialized = true;
	}
	cache->nr = 0;
	cache->cur = 0;
	cache->scan_stop = false;
	mb();
	cache->slots = slots;
    return 0;
}

// static void drain_swap_scan_slot_cpu(unsigned int cpu, bool free_slots)
// {
// 	struct swap_scan_slot *cache;
	
//     cache = &per_cpu(swp_scan_slots, cpu);
// 	spin_lock_irq(&cache->scan_lock);

//     cache->nr = 0;
// 	if (free_slots && cache->slots) {
//         kvfree(cache->slots);
//         cache->slots = NULL;
//     }
//     spin_unlock_irq(&cache->scan_lock);
// }

static void __drain_swap_scan_slot(void){
	// unsigned int cpu;
	// for_each_online_cpu(cpu)
	// 	drain_swap_scan_slot_cpu(cpu, false);
	struct swap_scan_slot *cache;
	cache = &global_swp_scan_slot;
	spin_lock_irq(&cache->scan_lock);
	cache->nr = 0;
    spin_unlock_irq(&cache->scan_lock);
}

static int free_swap_scan_slot(unsigned int cpu)
{
	mutex_lock(&swap_scan_slot_mutex);
    // drain_swap_scan_slot_cpu(cpu, true);
	__drain_swap_scan_slot();
    mutex_unlock(&swap_scan_slot_mutex);
	return 0;
}

static void __reenable_swap_scan_slot(void)
{
	swap_scan_slot_enabled = has_usable_swap();
	if (!swap_scan_slot_enabled)
		pr_err("there's no usable swap enable[%d]", swap_scan_slot_enabled);
}

void disable_swap_scan_slot_lock(void)
{
	mutex_lock(&swap_scan_slot_enable_mutex);
	swap_scan_slot_enabled = false;
	if (swap_scan_slot_initialized) {
		cpus_read_lock();
		__drain_swap_scan_slot();
		cpus_read_unlock();
	}
}

void reenable_swap_scan_slot_unlock(void){
	__reenable_swap_scan_slot();
	mutex_unlock(&swap_scan_slot_enable_mutex);
}

void reenable_scan_cpu(void){
	struct swap_scan_slot *cache;
	// cache = raw_cpu_ptr(&swp_scan_slots);
	cache = &global_swp_scan_slot;
	__drain_swap_scan_slot();
	spin_lock_irq(&cache->scan_lock);

	cache->scan_stop = false;
	spin_unlock_irq(&cache->scan_lock);
}

swp_entry_t get_next_saved_entry(bool* finished){
	struct swap_scan_slot *cache;
	swp_entry_t entry;
	entry = swp_entry(MAX_SWAPFILES, 0); //invalid entry
	cache = &global_swp_scan_slot;//raw_cpu_ptr(&swp_scan_slots);
	spin_lock_irq(&cache->scan_lock);

	if (!cache->scan_stop || !use_swap_scan_slot || !cache->slots){
		*finished = true;	
		spin_unlock_irq(&cache->scan_lock);
		return entry;
	}
	//we start read
	entry = cache->slots[cache->cur];
	cache->cur++;
	*finished = false;
	if (cache->cur == cache->nr){
		cache->cur = 0;
		cache->nr = 0;
		cache->scan_stop = false;
		*finished = true;
	}
	spin_unlock_irq(&cache->scan_lock);
	if (non_swap_entry(entry))
		pr_err("returning bad entry [%d/%d]", cache->cur, cache->nr);
	return entry;
} 

int add_to_scan_slot(swp_entry_t entry)
{
	struct swap_scan_slot *cache;
	int cpu;

	cache = &global_swp_scan_slot;//raw_cpu_ptr(&swp_scan_slots);
	cpu = smp_processor_id();

	if (likely(use_swap_scan_slot && cache->slots && ! cache->scan_stop)){
		spin_lock_irq(&cache->scan_lock);
		if (!use_swap_scan_slot || !cache->slots){
			spin_unlock_irq(&cache->scan_lock);
			goto fail_add;
		}
		if (unlikely(non_swap_entry(entry))){
			pr_err("add_to_scan_slot received bad entry [%lu]", entry.val);
			spin_unlock_irq(&cache->scan_lock);
			goto fail_add;
		}
		if (unlikely(swp_entry_test_special(entry))){
			pr_err("add_to_scan_slot entry with special ? [%lu]", entry.val);
		}
		cache->slots[cache->nr++] = entry;
		if (cache->nr >= SWAP_SCAN_SLOT_SIZE){ //FULL NOW
			// swap_scan_save_entries(cache->slots, cache->nr);
			trace_add_to_scan_slot(cpu, cache->cur, cache->nr);

			cache->cur = 0; //read from start
			cache->scan_stop = true;
			//this will block  use_swap_scan_slot
			spin_unlock_irq(&cache->scan_lock);
			return -2;
		}
		spin_unlock_irq(&cache->scan_lock);
		return 0; //success add
	}
fail_add:
	return -1;
}

void enable_swap_scan_slot(void)
{
	int ret;
	mutex_lock(&swap_scan_slot_enable_mutex);
    if (!swap_scan_slot_initialized){
		pr_err("try initialize swap scan");
		ret = alloc_swap_scan_slot(smp_processor_id());
        // ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "swap_scan_slot", 
        //             alloc_swap_scan_slot, free_swap_scan_slot);
        if (WARN_ONCE(ret < 0, "Cache allocation failed (%s), operating "
				       "without swap scan slot.\n", __func__))
		 	goto out_unlock;
		pr_err("swap scan initialized ok");
        swap_scan_slot_initialized = true;
    }
	mb();
    __reenable_swap_scan_slot();
out_unlock:
	mutex_unlock(&swap_scan_slot_enable_mutex);
	pr_err("swap scan enabled");
}
