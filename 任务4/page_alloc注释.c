/*
 * linux/mm/page_alloc.c
 *
 * 管理空闲列表，系统在这里分配空闲页面。
 * 注意kmalloc()存在于slab.c中。
 *
 * Copyright (C) 1991, 1992, 1993, 1994 Linus Torvalds
 * Swap重新组织了29.12.95, Stephen Tweedie
 * 对BIGMEM的支持，由Gerhard Wichert, Siemens AG, 1999年7月加入
 * 重塑它成为一个分区分配器，Ingo Molnar，Red Hat，1999年
 * 支持不连续的内存，Kanoj Sarcar，SGI，1999年11月
 * 区域平衡，Kanoj Sarcar，SGI，2000年1月
 * 每个CPU的热/冷页面列表，批量分配，Martin J. Bligh，2002年9月
 *（很多地方是从Ingo Molnar和Andrew Morton那里借用的）。
 */

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/jiffies.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/kmemcheck.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/oom.h>
#include <linux/notifier.h>
#include <linux/topology.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memory_hotplug.h>
#include <linux/nodemask.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/mempolicy.h>
#include <linux/stop_machine.h>
#include <linux/sort.h>
#include <linux/pfn.h>
#include <linux/backing-dev.h>
#include <linux/fault-inject.h>
#include <linux/page-isolation.h>
#include <linux/page_cgroup.h>
#include <linux/debugobjects.h>
#include <linux/kmemleak.h>
#include <linux/memory.h>
#include <linux/compaction.h>
#include <trace/events/kmem.h>
#include <linux/ftrace_event.h>
#include <linux/memcontrol.h>
#include <linux/prefetch.h>
#include <linux/page-debug-flags.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>
#include "internal.h"

#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
DEFINE_PER_CPU(int, numa_node);
EXPORT_PER_CPU_SYMBOL(numa_node);
#endif

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * 注意，不要直接引用"_numa_mem_"每cpu变量。
 * 当CONFIG_HAVE_MEMORYLESS_NODES未被定义时，它将不会被定义。
 * 使用accessor函数set_numa_mem(), numa_mem_id() 和 cpu_to_mem()
 * 定义在<linux/topology.h>中。
 */
DEFINE_PER_CPU(int, _numa_mem_);		/* 内核 "本地内存 "节点 */
EXPORT_PER_CPU_SYMBOL(_numa_mem_);
#endif

// 节点状态的数组。
nodemask_t node_states[NR_NODE_STATES] __read_mostly = {
	[N_POSSIBLE] = NODE_MASK_ALL,
	[N_ONLINE] = { { [0] = 1UL } },
#ifndef CONFIG_NUMA
	[N_NORMAL_MEMORY] = { { [0] = 1UL } },
#ifdef CONFIG_HIGHMEM
	[N_HIGH_MEMORY] = { { [0] = 1UL } },
#endif
	[N_CPU] = { { [0] = 1UL } },
#endif	/* NUMA */
};
EXPORT_SYMBOL(node_states);

unsigned long totalram_pages __read_mostly;
unsigned long totalreserve_pages __read_mostly;
/*
 * 当计算全局允许的脏页数量时，
 * 有一定数量的每个区的储备不应该被认为是
 * 视为可脏的内存。 这就是这些储备的总和
 * 在所有现有的区中，贡献可脏的内存。
 */
unsigned long dirty_balance_reserve __read_mostly;

int percpu_pagelist_fraction;
gfp_t gfp_allowed_mask __read_mostly = GFP_BOOT_MASK;

#ifdef CONFIG_PM_SLEEP
/*
 * 以下函数被暂停/休眠代码用来暂时
 * 改变gfp_allowed_mask，以避免在内存分配时使用I/O。
 * 当设备被暂停时。 为了避免与暂停/休眠代码的竞赛。
 * 它们应该总是在pm_mutex保持的情况下被调用（gfp_allowed_mask也是
 * 只有在持有pm_mutex的情况下才能修改，除非暂停/休眠代码
 * 保证不与该修改平行运行）。
 */

static gfp_t saved_gfp_mask;

void pm_restore_gfp_mask(void)
{
	WARN_ON(!mutex_is_locked(&pm_mutex));
	if (saved_gfp_mask) {
		gfp_allowed_mask = saved_gfp_mask;
		saved_gfp_mask = 0;
	}
}

void pm_restrict_gfp_mask(void)
{
	WARN_ON(!mutex_is_locked(&pm_mutex));
	WARN_ON(saved_gfp_mask);
	saved_gfp_mask = gfp_allowed_mask;
	gfp_allowed_mask &= ~GFP_IOFS;
}

bool pm_suspended_storage(void)
{
	if ((gfp_allowed_mask & GFP_IOFS) == GFP_IOFS)
		return false;
	return true;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE
int pageblock_order __read_mostly;
#endif

static void __free_pages_ok(struct page *page, unsigned int order);

/*
 * 在lowmem_reserve sysctl中使用256，32的结果。
 * 1G机器 -> (16M dma, 800M-16M normal, 1G-800M high)
 * 1G机器 -> (16M dma, 784M normal, 224M high)
 * NORMAL分配将在ZONE_DMA中保留784M/256的RAM。
 * HIGHMEM分配将在ZONE_NORMAL中保留224M/32的RAM。
 * HIGHMEM分配将(224M+784M)/256的RAM保留在ZONE_DMA中
 *
 * TBD：在这里应该对ZONE_DMA32机器进行特殊处理--在这些机器中我们通常
 * 不需要保留任何ZONE_NORMAL。
 */
int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES-1] = {
#ifdef CONFIG_ZONE_DMA
	 256,
#endif
#ifdef CONFIG_ZONE_DMA32
	 256,
#endif
#ifdef CONFIG_HIGHMEM
	 32,
#endif
	 32,
};

EXPORT_SYMBOL(totalram_pages);

static char * const zone_names[MAX_NR_ZONES] = {
#ifdef CONFIG_ZONE_DMA
	 "DMA",
#endif
#ifdef CONFIG_ZONE_DMA32
	 "DMA32",
#endif
	 "Normal",
#ifdef CONFIG_HIGHMEM
	 "HighMem",
#endif
	 "Movable",
};

int min_free_kbytes = 1024;

static unsigned long __meminitdata nr_kernel_pages;
static unsigned long __meminitdata nr_all_pages;
static unsigned long __meminitdata dma_reserve;

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
static unsigned long __meminitdata arch_zone_lowest_possible_pfn[MAX_NR_ZONES];
static unsigned long __meminitdata arch_zone_highest_possible_pfn[MAX_NR_ZONES];
static unsigned long __initdata required_kernelcore;
static unsigned long __initdata required_movablecore;
static unsigned long __meminitdata zone_movable_pfn[MAX_NUMNODES];

/* movable_zone是ZONE_MOVABLE中的 "真正"区域的页面。*/
int movable_zone;
EXPORT_SYMBOL(movable_zone);
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

#if MAX_NUMNODES > 1
int nr_node_ids __read_mostly = MAX_NUMNODES;
int nr_online_nodes __read_mostly = 1;
EXPORT_SYMBOL(nr_node_ids);
EXPORT_SYMBOL(nr_online_nodes);
#endif

int page_group_by_mobility_disabled __read_mostly;

static void set_pageblock_migratetype(struct page *page, int migratetype)
{
	if (unlikely(page_group_by_mobility_disabled &&
		     migratetype < MIGRATE_PCPTYPES))
		migratetype = MIGRATE_UNMOVABLE;

	set_pageblock_flags_group(page, (unsigned long)migratetype,
					PB_migrate, PB_migrate_end);
}

bool oom_killer_disabled __read_mostly;

#ifdef CONFIG_DEBUG_VM
static int page_outside_zone_boundaries(struct zone *zone, struct page *page)
{
	int ret = 0;
	unsigned seq;
	unsigned long pfn = page_to_pfn(page);

	do {
		seq = zone_span_seqbegin(zone);
		if (pfn >= zone->zone_start_pfn + zone->spanned_pages)
			ret = 1;
		else if (pfn < zone->zone_start_pfn)
			ret = 1;
	} while (zone_span_seqretry(zone, seq));

	return ret;
}

static int page_is_consistent(struct zone *zone, struct page *page)
{
	if (!pfn_valid_within(page_to_pfn(page)))
		return 0;
	if (zone != page_zone(page))
		return 0;

	return 1;
}
/*
 * 对不在指定区域内的页面进行临时调试检查。
 */
static int bad_range(struct zone *zone, struct page *page)
{
	if (page_outside_zone_boundaries(zone, page))
		return 1;
	if (!page_is_consistent(zone, page))
		return 1;

	return 0;
}
#else
static inline int bad_range(struct zone *zone, struct page *page)
{
	return 0;
}
#endif

static void bad_page(struct page *page)
{
	static unsigned long resume;
	static unsigned long nr_shown;
	static unsigned long nr_unshown;

	if (PageHWPoison(page)) {
		reset_page_mapcount(page); /* 移除PageBuddy */
		return;
	}

	/*
	 *允许突发60个报告，然后在这一分钟内保持安静。
	 *或者允许每秒钟稳定地滴下一个报告。
	 */
	if (nr_shown == 60) {
		if (time_before(jiffies, resume)) {
			nr_unshown++;
			goto out;
		}
		if (nr_unshown) {
			printk(KERN_ALERT
			      "BUG: Bad page state: %lu messages suppressed\n",
				nr_unshown);
			nr_unshown = 0;
		}
		nr_shown = 0;
	}
	if (nr_shown++ == 0)
		resume = jiffies + 60 * HZ;

	printk(KERN_ALERT "BUG: Bad page state in process %s  pfn:%05lx\n",
		current->comm, page_to_pfn(page));
	dump_page(page);

	print_modules();
	dump_stack();
out:
	/* 为调试留下坏字段，除了PageBuddy可能会制造麻烦 */
	reset_page_mapcount(page); /* 移除 PageBuddy */
	add_taint(TAINT_BAD_PAGE);
}

/*
 * 高阶页面被称为 "复合页面"。 它们的结构是这样的。
 *
 * 第一个PAGE_SIZE页被称为 "头页"。
 *
 * 其余的PAGE_SIZE页被称为 "尾页"。
 *
 * 所有的页面都有PG_compound设置。 所有的尾部页面都有它们的->first_page
 * 指向头部页面。
 *
 * 第一个尾页的->lru.next持有复合页的地址
 * put_page()函数。 它的->lru.prev持有分配的顺序。
 * 这种用法意味着零序页可能不会被复合。
 */

static void free_compound_page(struct page *page)
{
	__free_pages_ok(page, compound_order(page));
}

void prep_compound_page(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;

	set_compound_page_dtor(page, free_compound_page);
	set_compound_order(page, order);
	__SetPageHead(page);
	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;
		__SetPageTail(p);
		set_page_count(p, 0);
		p->first_page = page;
	}
}

/* 如果改变了这个函数，更新 __split_huge_page_refcount */
static int destroy_compound_page(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;
	int bad = 0;

	if (unlikely(compound_order(page) != order) ||
	    unlikely(!PageHead(page))) {
		bad_page(page);
		bad++;
	}

	__ClearPageHead(page);

	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;

		if (unlikely(!PageTail(p) || (p->first_page != page))) {
			bad_page(page);
			bad++;
		}
		__ClearPageTail(p);
	}

	return bad;
}

static inline void prep_zero_page(struct page *page, int order, gfp_t gfp_flags)
{
	int i;

	/*
	 * clear_highpage()将使用KM_USER0，所以使用__GFP_ZERO是错误的。
	 *__GFP_HIGHMEM从硬中断或软中断上下文。
	 */
	VM_BUG_ON((gfp_flags & __GFP_HIGHMEM) && in_interrupt());
	for (i = 0; i < (1 << order); i++)
		clear_highpage(page + i);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
unsigned int _debug_guardpage_minorder;

static int __init debug_guardpage_minorder_setup(char *buf)
{
	unsigned long res;

	if (kstrtoul(buf, 10, &res) < 0 ||  res > MAX_ORDER / 2) {
		printk(KERN_ERR "Bad debug_guardpage_minorder value\n");
		return 0;
	}
	_debug_guardpage_minorder = res;
	printk(KERN_INFO "Setting debug_guardpage_minorder to %lu\n", res);
	return 0;
}
__setup("debug_guardpage_minorder=", debug_guardpage_minorder_setup);

static inline void set_page_guard_flag(struct page *page)
{
	__set_bit(PAGE_DEBUG_FLAG_GUARD, &page->debug_flags);
}

static inline void clear_page_guard_flag(struct page *page)
{
	__clear_bit(PAGE_DEBUG_FLAG_GUARD, &page->debug_flags);
}
#else
static inline void set_page_guard_flag(struct page *page) { }
static inline void clear_page_guard_flag(struct page *page) { }
#endif

static inline void set_page_order(struct page *page, int order)
{
	set_page_private(page, order);
	__SetPageBuddy(page);
}

static inline void rmv_page_order(struct page *page)
{
	__ClearPageBuddy(page);
	set_page_private(page, 0);
}

/*
 * 在我们的结构中找到匹配的伙伴（buddy1）和它们组成的组合（page）的结构页。
 * 匹配的好友(buddy1)和他们组成的O(n+1)页的组合(page)。
 *
 * 1）任何好友B1都会有一个满足以下条件的O型双胞胎B2的顺序
 * 以下的方程式。
 * b2 = b1 ^ (1 << o)
 * 例如，如果开始的伙伴（buddy2）是#8，它的顺序为
 * 1个好友是#10。
 * B2 = 8 ^ (1 << 1) = 8 ^ 2 = 10
 *
 * 2）任何好友B将有一个顺序为O+1的父P，它
 * 满足以下公式。
 * P = B & ~(1 << O)
 *
 * 假设。*_mem_map至少在MAX_ORDER之前是连续的。
 */
static inline unsigned long
__find_buddy_index(unsigned long page_idx, unsigned int order)
{
	return page_idx ^ (1 << order);
}

/*
 * 这个函数检查一个页面是否是空的&&是好友的
 * 我们可以做到凝聚一个页面和它的好友，如果
 * (a) 伙伴不在洞中&&。
 * (b) 该伙伴是在伙伴系统中&&。
 * (c) 一个页面和它的伙伴有相同的顺序&&。
 * (d)一个页面和它的伙伴在同一个区域。
 *
 * 为了记录一个页面是否在好友系统中，我们设置->_mapcount -2。
 * 设置、清除和测试_mapcount -2是通过zone->lock进行序列化。
 *
 * 对于记录页面的顺序，我们使用page_private(page)。
 */
static inline int page_is_buddy(struct page *page, struct page *buddy,
								int order)
{
	if (!pfn_valid_within(page_to_pfn(buddy)))
		return 0;

	if (page_zone_id(page) != page_zone_id(buddy))
		return 0;

	if (page_is_guard(buddy) && page_order(buddy) == order) {
		VM_BUG_ON(page_count(buddy) != 0);
		return 1;
	}

	if (PageBuddy(buddy) && page_order(buddy) == order) {
		VM_BUG_ON(page_count(buddy) != 0);
		return 1;
	}
	return 0;
}

/*
 * 释放好友系统分配器的函数。
 *
 * 伙伴系统的概念是维护直接映射的表
 * (包含比特值)，用于不同 "等级 "的内存块。
 * 最底层的表包含了最小的可分配内存的映射。
 * 内存的最小单位（这里是指页），上面的每一级都描述了
 * 下面几级的单元对，因此是 "伙伴"。
 * 在高层次上，这里发生的所有事情都是标记表的条目
 * 在最底层的可用表项，并根据需要向上传播变化。
 * 必要时，再加上一些必要的核算，以便与虚拟机系统的其他部分很好地配合。
 * 虚拟机系统的其他部分。
 * 在每一级，我们都有一个页面列表，这些页面是连续的头。
 * 长度为(1 << 顺序)的自由页，并以_mapcount -2标记。页面的
 * 页的顺序被记录在page_private(page)字段中。
 * 因此，当我们分配或释放一个时，我们可以得出另一个的状态。
 * 另一个。 也就是说，如果我们分配了一个小块，而两个都被   
 * 释放了，那么该区域的剩余部分必须被分割成块。  
 * 如果一个块被释放了，而它的伙伴也被释放了，那么这
 * 触发了凝聚成一个更大尺寸的块。           
 *
 * --wli
 */

static inline void __free_one_page(struct page *page,
		struct zone *zone, unsigned int order,
		int migratetype)
{
	unsigned long page_idx;
	unsigned long combined_idx;
	unsigned long uninitialized_var(buddy_idx);
	struct page *buddy;

	if (unlikely(PageCompound(page)))
		if (unlikely(destroy_compound_page(page, order)))
			return;

	VM_BUG_ON(migratetype == -1);

	page_idx = page_to_pfn(page) & ((1 << MAX_ORDER) - 1);

	VM_BUG_ON(page_idx & ((1 << order) - 1));
	VM_BUG_ON(bad_range(zone, page));

	while (order < MAX_ORDER-1) {
		buddy_idx = __find_buddy_index(page_idx, order);
		buddy = page + (buddy_idx - page_idx);
		if (!page_is_buddy(page, buddy, order))
			break;
		/*
		 *我们的伙伴是免费的，或者它是CONFIG_DEBUG_PAGEALLOC的守护页。
		 *与它合并，并向上移动一个顺序。
		 */
		if (page_is_guard(buddy)) {
			clear_page_guard_flag(buddy);
			set_page_private(page, 0);
			__mod_zone_page_state(zone, NR_FREE_PAGES, 1 << order);
		} else {
			list_del(&buddy->lru);
			zone->free_area[order].nr_free--;
			rmv_page_order(buddy);
		}
		combined_idx = buddy_idx & page_idx;
		page = page + (combined_idx - page_idx);
		page_idx = combined_idx;
		order++;
	}
	set_page_order(page, order);

	/*
	 * 如果这不是最大的可能的页面，检查好友
	 * 次高的顺序是否有空。如果是的话，可能是
	 * 正在释放的页面很快就会聚集起来。在这种情况下。
	 * 把空闲的页面添加到列表的尾部
	 * 这样它就不太可能很快被使用，而更有可能被合并起来
	 * 作为一个更高阶的页面
	 */
	if ((order < MAX_ORDER-2) && pfn_valid_within(page_to_pfn(buddy))) {
		struct page *higher_page, *higher_buddy;
		combined_idx = buddy_idx & page_idx;
		higher_page = page + (combined_idx - page_idx);
		buddy_idx = __find_buddy_index(combined_idx, order + 1);
		higher_buddy = higher_page + (buddy_idx - combined_idx);
		if (page_is_buddy(higher_page, higher_buddy, order + 1)) {
			list_add_tail(&page->lru,
				&zone->free_area[order].free_list[migratetype]);
			goto out;
		}
	}

	list_add(&page->lru, &zone->free_area[order].free_list[migratetype]);
out:
	zone->free_area[order].nr_free++;
}

/*
 * free_page_mlock() -- 清理试图释放和mlocked()页面的尝试。
 * 页面不应该在lru上，所以不需要修复它。
 * free_pages_check() 将验证...
 */
static inline void free_page_mlock(struct page *page)
{
	__dec_zone_page_state(page, NR_MLOCK);
	__count_vm_event(UNEVICTABLE_MLOCKFREED);
}

static inline int free_pages_check(struct page *page)
{
	if (unlikely(page_mapcount(page) |
		(page->mapping != NULL)  |
		(atomic_read(&page->_count) != 0) |
		(page->flags & PAGE_FLAGS_CHECK_AT_FREE) |
		(mem_cgroup_bad_page_check(page)))) {
		bad_page(page);
		return 1;
	}
	if (page->flags & PAGE_FLAGS_CHECK_AT_PREP)
		page->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
	return 0;
}

/*
 * 从PCP列表中释放一定数量的页面
 * 假设列表中的所有页面都在同一区域，且顺序相同。
 * Count是要释放的页数。
 *
 * 如果该区以前处于 "所有页面被钉住 "的状态，则查看
 * 这次释放是否清除了该状态。
 *
 * 并清除该区的pages_scanned计数器，以阻止 "所有页面被钉住 "的检测逻辑。
 * pinned "的检测逻辑。
 */
static void free_pcppages_bulk(struct zone *zone, int count,
					struct per_cpu_pages *pcp)
{
	int migratetype = 0;
	int batch_free = 0;
	int to_free = count;

	spin_lock(&zone->lock);
	zone->all_unreclaimable = 0;
	zone->pages_scanned = 0;

	while (to_free) {
		struct page *page;
		struct list_head *list;

		/*
		 * 以轮流的方式从列表中删除页面。
		 * batch_free计数被维护，当遇到一个
		 * 空的列表时，这个计数会被增加。 这是为了让更多的页面被释放出来
		 * 从较满的列表中释放，而不是在空列表中过度地旋转列表
		 */
		do {
			batch_free++;
			if (++migratetype == MIGRATE_PCPTYPES)
				migratetype = 0;
			list = &pcp->lists[migratetype];
		} while (list_empty(list));

		/* 这是唯一的非空列表。把它们全部释放出来。*/
		if (batch_free == MIGRATE_PCPTYPES)
			batch_free = to_free;

		do {
			page = list_entry(list->prev, struct page, lru);
			/* 必须在__free_one_page列表操作时删除 */
			list_del(&page->lru);
			/* MIGRATE_MOVABLE列表可能包括MIGRATE_RESERVEs */
			__free_one_page(page, zone, 0, page_private(page));
			trace_mm_page_pcpu_drain(page, 0, page_private(page));
		} while (--to_free && --batch_free && !list_empty(list));
	}
	__mod_zone_page_state(zone, NR_FREE_PAGES, count);
	spin_unlock(&zone->lock);
}

static void free_one_page(struct zone *zone, struct page *page, int order,
				int migratetype)
{
	spin_lock(&zone->lock);
	zone->all_unreclaimable = 0;
	zone->pages_scanned = 0;

	__free_one_page(page, zone, order, migratetype);
	__mod_zone_page_state(zone, NR_FREE_PAGES, 1 << order);
	spin_unlock(&zone->lock);
}

static bool free_pages_prepare(struct page *page, unsigned int order)
{
	int i;
	int bad = 0;

	trace_mm_page_free(page, order);
	kmemcheck_free_shadow(page, order);

	if (PageAnon(page))
		page->mapping = NULL;
	for (i = 0; i < (1 << order); i++)
		bad += free_pages_check(page + i);
	if (bad)
		return false;

	if (!PageHighMem(page)) {
		debug_check_no_locks_freed(page_address(page),PAGE_SIZE<<order);
		debug_check_no_obj_freed(page_address(page),
					   PAGE_SIZE << order);
	}
	arch_free_page(page, order);
	kernel_map_pages(page, 1 << order, 0);

	return true;
}

static void __free_pages_ok(struct page *page, unsigned int order)
{
	unsigned long flags;
	int wasMlocked = __TestClearPageMlocked(page);

	if (!free_pages_prepare(page, order))
		return;

	local_irq_save(flags);
	if (unlikely(wasMlocked))
		free_page_mlock(page);
	__count_vm_events(PGFREE, 1 << order);
	free_one_page(page_zone(page), page, order,
					get_pageblock_migratetype(page));
	local_irq_restore(flags);
}

void __meminit __free_pages_bootmem(struct page *page, unsigned int order)
{
	unsigned int nr_pages = 1 << order;
	unsigned int loop;

	prefetchw(page);
	for (loop = 0; loop < nr_pages; loop++) {
		struct page *p = &page[loop];

		if (loop + 1 < nr_pages)
			prefetchw(p + 1);
		__ClearPageReserved(p);
		set_page_count(p, 0);
	}

	set_page_refcounted(page);
	__free_pages(page, order);
}


/*
 * 这里的细分顺序对于IO子系统来说是至关重要的。
 * 如果没有充分的理由和回归测试，不能改变这个顺序。
 * 测试具体来说，当大的内存块被细分的时候
 * 较小的块被交付的顺序取决于
 * 它们在这个函数中被分割的顺序。这是最主要的因素
 * 影响页面被交付给IO的顺序
 * 根据经验测试，这是影响页面交付给IO子系统的顺序的主要因素，而且这也是合理的。
 * 通过考虑一个包含单一的
 * 考虑到一个伙伴系统的行为，其中包含一个由一系列小的分配所作用的大的内存块。
 * 这种行为是sglist合并成功的一个关键因素。
 *
 * --wli
 */
static inline void expand(struct zone *zone, struct page *page,
	int low, int high, struct free_area *area,
	int migratetype)
{
	unsigned long size = 1 << high;

	while (high > low) {
		area--;
		high--;
		size >>= 1;
		VM_BUG_ON(bad_range(zone, &page[size]));

#ifdef CONFIG_DEBUG_PAGEALLOC
		if (high < debug_guardpage_minorder()) {
			/*
			 * 标记为保护页（或页面），这将使
			 * 当伙伴将被释放时，合并回分配器。
			 * 对应的页表项将不会被触动。
			 * 页面将保持不存在于虚拟地址空间中
			 */
			INIT_LIST_HEAD(&page[size].lru);
			set_page_guard_flag(&page[size]);
			set_page_private(&page[size], high);
			/* 守护页面不能用于任何用途 */
			__mod_zone_page_state(zone, NR_FREE_PAGES, -(1 << high));
			continue;
		}
#endif
		list_add(&page[size].lru, &area->free_list[migratetype]);
		area->nr_free++;
		set_page_order(&page[size], high);
	}
}

/*
 * 这个页面即将从页面分配器返回
 */
static inline int check_new_page(struct page *page)
{
	if (unlikely(page_mapcount(page) |
		(page->mapping != NULL)  |
		(atomic_read(&page->_count) != 0)  |
		(page->flags & PAGE_FLAGS_CHECK_AT_PREP) |
		(mem_cgroup_bad_page_check(page)))) {
		bad_page(page);
		return 1;
	}
	return 0;
}

static int prep_new_page(struct page *page, int order, gfp_t gfp_flags)
{
	int i;

	for (i = 0; i < (1 << order); i++) {
		struct page *p = page + i;
		if (unlikely(check_new_page(p)))
			return 1;
	}

	set_page_private(page, 0);
	set_page_refcounted(page);

	arch_alloc_page(page, order);
	kernel_map_pages(page, 1 << order, 1);

	if (gfp_flags & __GFP_ZERO)
		prep_zero_page(page, order, gfp_flags);

	if (order && (gfp_flags & __GFP_COMP))
		prep_compound_page(page, order);

	return 0;
}

/*
 * 浏览给定migratetype的自由列表，并从自由列表中删除
 * 从自由列表中删除最小的可用页面
 */
static inline
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
						int migratetype)
{
	unsigned int current_order;
	struct free_area * area;
	struct page *page;

	/* 在首选列表中找到合适尺寸的页面 */
	for (current_order = order; current_order < MAX_ORDER; ++current_order) {
		area = &(zone->free_area[current_order]);
		if (list_empty(&area->free_list[migratetype]))
			continue;

		page = list_entry(area->free_list[migratetype].next,
							struct page, lru);
		list_del(&page->lru);
		rmv_page_order(page);
		area->nr_free--;
		expand(zone, page, order, current_order, area, migratetype);
		return page;
	}

	return NULL;
}


/*
 * 这个数组描述的是，当理想的迁移类型的空闲列表被耗尽时，列表会被退回到哪种顺序。
 * 所需的迁移类型的空闲列表被耗尽时，将返回到该列表。
 */
static int fallbacks[MIGRATE_TYPES][MIGRATE_TYPES-1] = {
	[MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE,   MIGRATE_RESERVE },
	[MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE,   MIGRATE_RESERVE },
	[MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE, MIGRATE_RESERVE },
	[MIGRATE_RESERVE]     = { MIGRATE_RESERVE,     MIGRATE_RESERVE,   MIGRATE_RESERVE }, /* Never used */
};

/*
 * 将一个范围内的空闲页移到所要求的类型的空闲列表中。
 * 注意start_page和end_pages不是在页块上对齐的
 * 如果需要对齐，使用move_freepages_block()
 */
static int move_freepages(struct zone *zone,
			  struct page *start_page, struct page *end_page,
			  int migratetype)
{
	struct page *page;
	unsigned long order;
	int pages_moved = 0;

#ifndef CONFIG_HOLES_IN_ZONE
	/*
	 * page_zone在这种情况下调用是不安全的，当
	 * CONFIG_HOLES_IN_ZONE被设置。这个错误检查可能是多余的
	 * 因为我们在move_freepages_block()中检查区域边界。
	 * 在以后没有与以下内容有关的错误报告时删除
	 * 通过移动性对页面进行分组
	 */
	BUG_ON(page_zone(start_page) != page_zone(end_page));
#endif

	for (page = start_page; page <= end_page;) {
		/* 确保我们没有在无意中改变节点 */
		VM_BUG_ON(page_to_nid(page) != zone_to_nid(zone));

		if (!pfn_valid_within(page_to_pfn(page))) {
			page++;
			continue;
		}

		if (!PageBuddy(page)) {
			page++;
			continue;
		}

		order = page_order(page);
		list_move(&page->lru,
			  &zone->free_area[order].free_list[migratetype]);
		page += 1 << order;
		pages_moved += 1 << order;
	}

	return pages_moved;
}

static int move_freepages_block(struct zone *zone, struct page *page,
				int migratetype)
{
	unsigned long start_pfn, end_pfn;
	struct page *start_page, *end_page;

	start_pfn = page_to_pfn(page);
	start_pfn = start_pfn & ~(pageblock_nr_pages-1);
	start_page = pfn_to_page(start_pfn);
	end_page = start_page + pageblock_nr_pages - 1;
	end_pfn = start_pfn + pageblock_nr_pages - 1;

	/* 不要跨越区域边界 */
	if (start_pfn < zone->zone_start_pfn)
		start_page = page;
	if (end_pfn >= zone->zone_start_pfn + zone->spanned_pages)
		return 0;

	return move_freepages(zone, start_page, end_page, migratetype);
}

static void change_pageblock_range(struct page *pageblock_page,
					int start_order, int migratetype)
{
	int nr_pageblocks = 1 << (start_order - pageblock_order);

	while (nr_pageblocks--) {
		set_pageblock_migratetype(pageblock_page, migratetype);
		pageblock_page += pageblock_nr_pages;
	}
}

/* 从回退列表中删除好友分配器中的一个元素 */
static inline struct page *
__rmqueue_fallback(struct zone *zone, int order, int start_migratetype)
{
	struct free_area * area;
	int current_order;
	struct page *page;
	int migratetype, i;

	/* 找到另一个列表中最大的可能的页面块 */
	for (current_order = MAX_ORDER-1; current_order >= order;
						--current_order) {
		for (i = 0; i < MIGRATE_TYPES - 1; i++) {
			migratetype = fallbacks[start_migratetype][i];

			/* 必要时稍后处理MIGRATE_RESERVE */
			if (migratetype == MIGRATE_RESERVE)
				continue;

			area = &(zone->free_area[current_order]);
			if (list_empty(&area->free_list[migratetype]))
				continue;

			page = list_entry(area->free_list[migratetype].next,
					struct page, lru);
			area->nr_free--;

			/*
			 * 如果打破一个大的页面块，将所有空闲的
			 * 页到首选分配列表中。如果为了一个可回收的内核分配而回落
			 * 如果是为了可回收的内核分配，要更积极地采取
			 * 更积极地获取空闲页的所有权
			 */
			if (unlikely(current_order >= (pageblock_order >> 1)) ||
					start_migratetype == MIGRATE_RECLAIMABLE ||
					page_group_by_mobility_disabled) {
				unsigned long pages;
				pages = move_freepages_block(zone, page,
								start_migratetype);

				/* 如果有一半以上的区块是空闲的，就把整个区块领走 */
				if (pages >= (1 << (pageblock_order-1)) ||
						page_group_by_mobility_disabled)
					set_pageblock_migratetype(page,
								start_migratetype);

				migratetype = start_migratetype;
			}

			/* 从自由列表中删除该页 */
			list_del(&page->lru);
			rmv_page_order(page);

			/* 对 >= pageblock_order 的订单拥有所有权 */
			if (current_order >= pageblock_order)
				change_pageblock_range(page, current_order,
							start_migratetype);

			expand(zone, page, order, current_order, area, migratetype);

			trace_mm_page_alloc_extfrag(page, order, current_order,
				start_migratetype, migratetype);

			return page;
		}
	}

	return NULL;
}

/*
 * 从好友分配器中删除一个元素。
 * 调用我已经持有的zone->lock。
 */
static struct page *__rmqueue(struct zone *zone, unsigned int order,
						int migratetype)
{
	struct page *page;

retry_reserve:
	page = __rmqueue_smallest(zone, order, migratetype);

	if (unlikely(!page) && migratetype != MIGRATE_RESERVE) {
		page = __rmqueue_fallback(zone, order, migratetype);

		/*
		 * 使用MIGRATE_RESERVE，而不是分配失败。 goto
		 * 因为 __rmqueue_smallest 是一个内联函数。
		 * 而我们只想调用一个站点
		 */
		if (!page) {
			migratetype = MIGRATE_RESERVE;
			goto retry_reserve;
		}
	}

	trace_mm_page_alloc_zone_locked(page, order, migratetype);
	return page;
}

/* 
 * 从好友分配器中获取指定数量的元素，所有这些元素都在一个锁的控制下。
 * 为了提高效率，只需要一个锁。 将它们添加到提供的列表中。
 * 返回放置在*列表中的新页面的数量。
 */
static int rmqueue_bulk(struct zone *zone, unsigned int order, 
			unsigned long count, struct list_head *list,
			int migratetype, int cold)
{
	int i;
	
	spin_lock(&zone->lock);
	for (i = 0; i < count; ++i) {
		struct page *page = __rmqueue(zone, order, migratetype);
		if (unlikely(page == NULL))
			break;

		/*
		 * 由expand()返回的分裂伙伴页面在这里被接收。
		 * 按物理页面顺序。该页被添加到调用者和
		 * 列表，然后列表头向前移动。从调用者
		 * 的角度来看，链接列表在某些情况下是按页数排序的。
		 * 某些情况下。这对IO设备来说是很有用的，因为如果物理页被排序，这些设备可以
		 * 的物理页被正确排序的话，可以合并IO请求。
		 */
		if (likely(cold == 0))
			list_add(&page->lru, list);
		else
			list_add_tail(&page->lru, list);
		set_page_private(page, migratetype);
		list = &page->lru;
	}
	__mod_zone_page_state(zone, NR_FREE_PAGES, -(i << order));
	spin_unlock(&zone->lock);
	return i;
}

#ifdef CONFIG_NUMA
/*
 * 从vmstat计数器更新器中调用，以耗尽该节点的页组。
 * 当前在远程节点上执行的处理器的页面集在它们过期后被调用。
 * 过期。
 *
 * 请注意，这个函数必须在线程被钉住的情况下调用。
 * 单个处理器。
 */
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp)
{
	unsigned long flags;
	int to_drain;

	local_irq_save(flags);
	if (pcp->count >= pcp->batch)
		to_drain = pcp->batch;
	else
		to_drain = pcp->count;
	free_pcppages_bulk(zone, to_drain, pcp);
	pcp->count -= to_drain;
	local_irq_restore(flags);
}
#endif

/*
 * 排空指定处理器的页面。
 *
 * 该处理器必须是当前的处理器和
 * 线程被钉在当前处理器上，或者是一个不在线的处理器。
 * 不在线的处理器。
 */
static void drain_pages(unsigned int cpu)
{
	unsigned long flags;
	struct zone *zone;

	for_each_populated_zone(zone) {
		struct per_cpu_pageset *pset;
		struct per_cpu_pages *pcp;

		local_irq_save(flags);
		pset = per_cpu_ptr(zone->pageset, cpu);

		pcp = &pset->pcp;
		if (pcp->count) {
			free_pcppages_bulk(zone, pcp->count, pcp);
			pcp->count = 0;
		}
		local_irq_restore(flags);
	}
}

/*
 * 将这个CPU的所有按CPU计算的页面溢出到好友分配器中。
 */
void drain_local_pages(void *arg)
{
	drain_pages(smp_processor_id());
}

/*
 * 将所有CPU的每个页面都溢出到好友分配器中。
 *
 * 注意，这段代码可以防止向离线的CPU发送IPI。
 * 但并不保证向新的热插拔CPU发送IPI。
 * on_each_cpu_mask()阻止了热插拔，不会与离线的CPU对话，但是
 * 在我们填充了cpumask之后，在调用on_each_mask()之前，没有任何东西可以阻止CPU显示出来。
 * 在调用on_each_cpu_mask()之前。
 */
void drain_all_pages(void)
{
	int cpu;
	struct per_cpu_pageset *pcp;
	struct zone *zone;

	/*
	 * 在BSS中进行分配，这样我们就不需要在BSS中分配。
	 * 直接取回路径，因为 CONFIG_CPUMASK_OFFSTACK=y
	 */
	static cpumask_t cpus_with_pcps;

	/*
	 * 我们不关心与CPU热插拔事件有关的东西。
	 * 因为离线通知会导致被通知的
	 * cpu耗尽该CPU的pcps和on_each_cpu_mask
	 * 作为其处理的一部分，禁用了抢占。
	 */
	for_each_online_cpu(cpu) {
		bool has_pcps = false;
		for_each_populated_zone(zone) {
			pcp = per_cpu_ptr(zone->pageset, cpu);
			if (pcp->pcp.count) {
				has_pcps = true;
				break;
			}
		}
		if (has_pcps)
			cpumask_set_cpu(cpu, &cpus_with_pcps);
		else
			cpumask_clear_cpu(cpu, &cpus_with_pcps);
	}
	on_each_cpu_mask(&cpus_with_pcps, drain_local_pages, NULL, 1);
}

#ifdef CONFIG_HIBERNATION

void mark_free_pages(struct zone *zone)
{
	unsigned long pfn, max_zone_pfn;
	unsigned long flags;
	int order, t;
	struct list_head *curr;

	if (!zone->spanned_pages)
		return;

	spin_lock_irqsave(&zone->lock, flags);

	max_zone_pfn = zone->zone_start_pfn + zone->spanned_pages;
	for (pfn = zone->zone_start_pfn; pfn < max_zone_pfn; pfn++)
		if (pfn_valid(pfn)) {
			struct page *page = pfn_to_page(pfn);

			if (!swsusp_page_is_forbidden(page))
				swsusp_unset_page_free(page);
		}

	for_each_migratetype_order(order, t) {
		list_for_each(curr, &zone->free_area[order].free_list[t]) {
			unsigned long i;

			pfn = page_to_pfn(list_entry(curr, struct page, lru));
			for (i = 0; i < (1UL << order); i++)
				swsusp_set_page_free(pfn_to_page(pfn + i));
		}
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif /* CONFIG_PM */

/*
 * 释放一个0阶的页面
 * cold == 1 ? 释放一个冷页 : 释放一个热页
 */
void free_hot_cold_page(struct page *page, int cold)
{
	struct zone *zone = page_zone(page);
	struct per_cpu_pages *pcp;
	unsigned long flags;
	int migratetype;
	int wasMlocked = __TestClearPageMlocked(page);

	if (!free_pages_prepare(page, 0))
		return;

	migratetype = get_pageblock_migratetype(page);
	set_page_private(page, migratetype);
	local_irq_save(flags);
	if (unlikely(wasMlocked))
		free_page_mlock(page);
	__count_vm_event(PGFREE);

	/*
	 * 我们只跟踪pcp列表中的不可移动、可回收和可移动的页面。
	 * 释放ISOLATE页到分配器中，因为它们正在被删除。
	 * 但把RESERVE当作可移动的页面，这样我们就可以在必要时把这些
	 * 如果有必要，我们可以把这些区域拿回来。否则，我们可能不得不释放
	 * 过多地进入页面分配器
	 */
	if (migratetype >= MIGRATE_PCPTYPES) {
		if (unlikely(migratetype == MIGRATE_ISOLATE)) {
			free_one_page(zone, page, 0, migratetype);
			goto out;
		}
		migratetype = MIGRATE_MOVABLE;
	}

	pcp = &this_cpu_ptr(zone->pageset)->pcp;
	if (cold)
		list_add_tail(&page->lru, &pcp->lists[migratetype]);
	else
		list_add(&page->lru, &pcp->lists[migratetype]);
	pcp->count++;
	if (pcp->count >= pcp->high) {
		free_pcppages_bulk(zone, pcp->batch, pcp);
		pcp->count -= pcp->batch;
	}

out:
	local_irq_restore(flags);
}

/*
 * 免费提供一个0阶的页面列表
 */
void free_hot_cold_page_list(struct list_head *list, int cold)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, list, lru) {
		trace_mm_page_free_batched(page, cold);
		free_hot_cold_page(page, cold);
	}
}

/*
 * split_page接收一个非复合高阶页面，并将其分割成
 * n (1<<顺序)个子页：page[0...n] 。
 * 每个子页必须被单独释放。
 *
 * 注意：这可能是一个太低级的操作，在驱动程序中使用。
 * 在你的驱动程序中使用这个之前，请咨询lkml。
 */
void split_page(struct page *page, unsigned int order)
{
	int i;

	VM_BUG_ON(PageCompound(page));
	VM_BUG_ON(!page_count(page));

#ifdef CONFIG_KMEMCHECK
	/*
	 * 分割影子页，因为free(page[0])也会导致
	 * 否则会释放整个影子。
	 */
	if (kmemcheck_page_is_tracked(page))
		split_page(virt_to_page(page[0].shadow), order);
#endif

	for (i = 1; i < (1 << order); i++)
		set_page_refcounted(page + i);
}

/*
 * 类似于split_page，只不过这个页面已经被释放了。由于这只是
 * 被用于迁移，块的migratetype也会改变。
 * 由于调用这个函数时禁用了中断，所以调用者要负责
 * 调用arch_alloc_page()和kernel_map_page()后，中断被激活。
 * 启用。
 *
 * 注意：这可能是一个太低级的操作，不适合在驱动中使用。
 * 在你的驱动程序中使用这个之前，请咨询lkml。
 */
int split_free_page(struct page *page)
{
	unsigned int order;
	unsigned long watermark;
	struct zone *zone;

	BUG_ON(!PageBuddy(page));

	zone = page_zone(page);
	order = page_order(page);

	/* 服从水印，就像正在分配页面一样 */
	watermark = low_wmark_pages(zone) + (1 << order);
	if (!zone_watermark_ok(zone, 0, watermark, 0, 0))
		return 0;

	/* 从自由列表中删除页面 */
	list_del(&page->lru);
	zone->free_area[order].nr_free--;
	rmv_page_order(page);
	__mod_zone_page_state(zone, NR_FREE_PAGES, -(1UL << order));

	/* 分割成单个页面 */
	set_page_refcounted(page);
	split_page(page, order);

	if (order >= pageblock_order - 1) {
		struct page *endpage = page + (1 << order) - 1;
		for (; page < endpage; page += pageblock_nr_pages)
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);
	}

	return 1 << order;
}

/*
 * 实际上，prep_compound_page()应该由__rmqueue_bulk()调用。 但是
 * 我们通过从这里调用它来作弊，在顺序>0的路径中。 省去了一个分支
 * 或两个。
 */
static inline
struct page *buffered_rmqueue(struct zone *preferred_zone,
			struct zone *zone, int order, gfp_t gfp_flags,
			int migratetype)
{
	unsigned long flags;
	struct page *page;
	int cold = !!(gfp_flags & __GFP_COLD);

again:
	if (likely(order == 0)) {
		struct per_cpu_pages *pcp;
		struct list_head *list;

		local_irq_save(flags);
		pcp = &this_cpu_ptr(zone->pageset)->pcp;
		list = &pcp->lists[migratetype];
		if (list_empty(list)) {
			pcp->count += rmqueue_bulk(zone, 0,
					pcp->batch, list,
					migratetype, cold);
			if (unlikely(list_empty(list)))
				goto failed;
		}

		if (cold)
			page = list_entry(list->prev, struct page, lru);
		else
			page = list_entry(list->next, struct page, lru);

		list_del(&page->lru);
		pcp->count--;
	} else {
		if (unlikely(gfp_flags & __GFP_NOFAIL)) {
			/*
			 * __GFP_NOFAIL不能用在新代码中。
			 *
			 * 所有 __GFP_NOFAIL 调用者都应该被修复，以便它们能够
			 * 正确地检测和处理分配失败。
			 *
			 * 我们绝对不希望调用者试图
			 * 分配大于1阶的页面单位。
			 * __gfp_nofail.
			 */
			WARN_ON_ONCE(order > 1);
		}
		spin_lock_irqsave(&zone->lock, flags);
		page = __rmqueue(zone, order, migratetype);
		spin_unlock(&zone->lock);
		if (!page)
			goto failed;
		__mod_zone_page_state(zone, NR_FREE_PAGES, -(1 << order));
	}

	__count_zone_vm_events(PGALLOC, zone, 1 << order);
	zone_statistics(preferred_zone, zone, gfp_flags);
	local_irq_restore(flags);

	VM_BUG_ON(bad_range(zone, page));
	if (prep_new_page(page, order, gfp_flags))
		goto again;
	return page;

failed:
	local_irq_restore(flags);
	return NULL;
}

/* ALLOC_WMARK位被用作分区->水印的索引 */
#define ALLOC_WMARK_MIN		WMARK_MIN
#define ALLOC_WMARK_LOW		WMARK_LOW
#define ALLOC_WMARK_HIGH	WMARK_HIGH
#define ALLOC_NO_WATERMARKS	0x04 /*完全不检查水印 */

/* 获得水印位的掩码 */
#define ALLOC_WMARK_MASK	(ALLOC_NO_WATERMARKS-1)

#define ALLOC_HARDER		0x10 /* 尝试更努力地分配 */
#define ALLOC_HIGH		0x20 /* __GFP_HIGH设置 */
#define ALLOC_CPUSET		0x40 /* 检查正确的cpuset */

#ifdef CONFIG_FAIL_PAGE_ALLOC

static struct {
	struct fault_attr attr;

	u32 ignore_gfp_highmem;
	u32 ignore_gfp_wait;
	u32 min_order;
} fail_page_alloc = {
	.attr = FAULT_ATTR_INITIALIZER,
	.ignore_gfp_wait = 1,
	.ignore_gfp_highmem = 1,
	.min_order = 1,
};

static int __init setup_fail_page_alloc(char *str)
{
	return setup_fault_attr(&fail_page_alloc.attr, str);
}
__setup("fail_page_alloc=", setup_fail_page_alloc);

static int should_fail_alloc_page(gfp_t gfp_mask, unsigned int order)
{
	if (order < fail_page_alloc.min_order)
		return 0;
	if (gfp_mask & __GFP_NOFAIL)
		return 0;
	if (fail_page_alloc.ignore_gfp_highmem && (gfp_mask & __GFP_HIGHMEM))
		return 0;
	if (fail_page_alloc.ignore_gfp_wait && (gfp_mask & __GFP_WAIT))
		return 0;

	return should_fail(&fail_page_alloc.attr, 1 << order);
}

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS

static int __init fail_page_alloc_debugfs(void)
{
	umode_t mode = S_IFREG | S_IRUSR | S_IWUSR;
	struct dentry *dir;

	dir = fault_create_debugfs_attr("fail_page_alloc", NULL,
					&fail_page_alloc.attr);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	if (!debugfs_create_bool("ignore-gfp-wait", mode, dir,
				&fail_page_alloc.ignore_gfp_wait))
		goto fail;
	if (!debugfs_create_bool("ignore-gfp-highmem", mode, dir,
				&fail_page_alloc.ignore_gfp_highmem))
		goto fail;
	if (!debugfs_create_u32("min-order", mode, dir,
				&fail_page_alloc.min_order))
		goto fail;

	return 0;
fail:
	debugfs_remove_recursive(dir);

	return -ENOMEM;
}

late_initcall(fail_page_alloc_debugfs);

#endif /* CONFIG_FAULT_INJECTION_DEBUG_FS */

#else /* CONFIG_FAIL_PAGE_ALLOC */

static inline int should_fail_alloc_page(gfp_t gfp_mask, unsigned int order)
{
	return 0;
}

#endif /* CONFIG_FAIL_PAGE_ALLOC */

/*
 * 如果空闲页面在'mark'之上，则返回true。这考虑到了
 * 分配的顺序。
 */
static bool __zone_watermark_ok(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags, long free_pages)
{
	/* free_pages可能会出现负数 -- 这没关系 */
	long min = mark;
	int o;

	free_pages -= (1 << order) - 1;
	if (alloc_flags & ALLOC_HIGH)
		min -= min / 2;
	if (alloc_flags & ALLOC_HARDER)
		min -= min / 4;

	if (free_pages <= min + z->lowmem_reserve[classzone_idx])
		return false;
	for (o = 0; o < order; o++) {
		/* 在下一个命令，这个命令的页面变得不可用 */
		free_pages -= z->free_area[o].nr_free << o;

		/* 要求更少的高阶页面是免费的 */
		min >>= 1;

		if (free_pages <= min)
			return false;
	}
	return true;
}

bool zone_watermark_ok(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags)
{
	return __zone_watermark_ok(z, order, mark, classzone_idx, alloc_flags,
					zone_page_state(z, NR_FREE_PAGES));
}

bool zone_watermark_ok_safe(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags)
{
	long free_pages = zone_page_state(z, NR_FREE_PAGES);

	if (z->percpu_drift_mark && free_pages < z->percpu_drift_mark)
		free_pages = zone_page_state_snapshot(z, NR_FREE_PAGES);

	return __zone_watermark_ok(z, order, mark, classzone_idx, alloc_flags,
								free_pages);
}

#ifdef CONFIG_NUMA
/*
 * zlc_setup - 设置 "分区列表缓存"。 使用缓存的区域数据来
 * 跳过cpuset不允许的区域，或者最近（上一秒）发现几乎满了的区域。
 * 最近（上一秒）被发现几乎已满的区域。 请进一步参阅
 * mmzone.h中的注释。 减少分区列表扫描的缓存占用率
 * 必须跳过大量的满区或不允许的区。
 *
 * 如果分区列表缓存存在于传递的分区列表中，那么
 * 返回一个指向允许的节点掩码的指针（无论是当前的
 * 任务 mems_allowed, 或者 node_states[N_HIGH_MEMORY])。
 *
 * 如果分区列表缓存对这个分区列表不可用，则不做任何事情并返回NULL。
 * 什么也不做，并返回NULL。
 *
 * 如果分区列表缓存中的fullzones BITMAP是陈旧的（距离上一次zap的时间超过
 * a second since last zap'd），那么我们就把它删除（清除它的比特。）
 *
 * 我们甚至暂不调用zlc_setup，直到我们检查完
 * 在分区列表中的第一个区域，理论上来说，大多数的分配都会在第一个区域得到满足。
 * 从第一个区中得到满足，所以最好尽快检查该区。
 * 尽可能快地检查该区。
 */
static nodemask_t *zlc_setup(struct zonelist *zonelist, int alloc_flags)
{
	struct zonelist_cache *zlc;	/* 缓存的分区列表加速信息 */
	nodemask_t *allowednodes;	/* zonelist_cache的近似值 */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return NULL;

	if (time_after(jiffies, zlc->last_full_zap + HZ)) {
		bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
		zlc->last_full_zap = jiffies;
	}

	allowednodes = !in_interrupt() && (alloc_flags & ALLOC_CPUSET) ?
					&cpuset_current_mems_allowed :
					&node_states[N_HIGH_MEMORY];
	return allowednodes;
}

/*
 * 给定'z'扫描一个分区列表，运行几个快速检查，看看
 * 是否值得进一步查看空闲内存。
 * 1) 检查该区是否被认为是满的（在zonelist_cache fullzones BIT中没有设置其
 * 在zonelist_cache fullzones BITMAP中设置的位）。
 * 2) 检查zone节点（从zonelist_cache中获得的
 * z_to_n[] 映射）在传入的allowednodes掩码中是允许的。
 * 如果zone值得进一步查看，则返回true（非零），或者
 * 否则返回false（零），如果它不值得。
 
 * 这个检查忽略了各种水印之间的区别。
 * 如GFP_HIGH, GFP_ATOMIC, PF_MEMALLOC, ...  如果一个区被
 * 发现这些水印的任何变化都是满的，它将被认为是满的。
 * 所有的请求都将被认为是满的，最长一秒钟，除非
 * 我们在所有允许的节点上的内存都很低，以至于我们被迫
 * 进入分区列表的第二次扫描。
 *
 * 在第二次扫描中，我们忽略了这个分区列表的缓存，并准确地
 * 将水印应用于所有区域，即使这样做会比较慢。
 * 在第二次扫描中，我们的内存不足，因此应该不遗余力地寻找
 * 寻找一个空闲的页面。
 */
static int zlc_zone_worth_trying(struct zonelist *zonelist, struct zoneref *z,
						nodemask_t *allowednodes)
{
	struct zonelist_cache *zlc;	/* 缓存的分区列表加速信息 */
	int i;				/* Zonelist区域中*z的索引 */
	int n;				/* 区域*z所在的节点 */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return 1;

	i = z - zonelist->_zonerefs;
	n = zlc->z_to_n[i];

	/* 这个区域如果允许但没有满员，就值得一试 */
	return node_isset(n, *allowednodes) && !test_bit(i, zlc->fullzones);
}

/*
 * 给定'z'扫描一个分区列表，在zlc->fullzones中设置相应的位。
 * zlc->fullzones中的相应位，这样，以后从该区分配页面的尝试就不会在重新检查它时浪费时间。
 * 从该区域分配一个页面时，就不会浪费时间重新检查它。
 */
static void zlc_mark_zone_full(struct zonelist *zonelist, struct zoneref *z)
{
	struct zonelist_cache *zlc;	/* 缓存的分区列表加速信息 */
	int i;				/* Zonelist区域中*z的索引 */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return;

	i = z - zonelist->_zonerefs;

	set_bit(i, zlc->fullzones);
}

/*
 * 清除所有已满的区域，在直接回收取得进展后调用，以便
 *最近被填满的区域不会被跳过，最长时间为一秒钟
 */
static void zlc_clear_zones_full(struct zonelist *zonelist)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return;

	bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
}

#else	/* CONFIG_NUMA */

static nodemask_t *zlc_setup(struct zonelist *zonelist, int alloc_flags)
{
	return NULL;
}

static int zlc_zone_worth_trying(struct zonelist *zonelist, struct zoneref *z,
				nodemask_t *allowednodes)
{
	return 1;
}

static void zlc_mark_zone_full(struct zonelist *zonelist, struct zoneref *z)
{
}

static void zlc_clear_zones_full(struct zonelist *zonelist)
{
}
#endif	/* CONFIG_NUMA */

/*
 * get_page_from_freelist 遍历分区列表，尝试分配
 * 一个页面。
 */
static struct page *
get_page_from_freelist(gfp_t gfp_mask, nodemask_t *nodemask, unsigned int order,
		struct zonelist *zonelist, int high_zoneidx, int alloc_flags,
		struct zone *preferred_zone, int migratetype)
{
	struct zoneref *z;
	struct page *page = NULL;
	int classzone_idx;
	struct zone *zone;
	nodemask_t *allowednodes = NULL;/* zonelist_cache的近似值 */
	int zlc_active = 0;		/* 如果使用zonelist_cache，则进行设置 */
	int did_zlc_setup = 0;		/*只需调用zlc_setup()一次 */

	classzone_idx = zone_idx(preferred_zone);
zonelist_scan:
	/*
	 * 扫描zonelist，寻找有足够空闲的区域。
	 * 参见kernel/cpuset.c中的cpuset_zone_allowed()注释。
	 */
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
						high_zoneidx, nodemask) {
		if (NUMA_BUILD && zlc_active &&
			!zlc_zone_worth_trying(zonelist, z, allowednodes))
				continue;
		if ((alloc_flags & ALLOC_CPUSET) &&
			!cpuset_zone_allowed_softwall(zone, gfp_mask))
				continue;
		/*
		 * 当分配一个页面缓存页进行写入时，我们
		 * 我们希望从一个区域中获取它，而这个区域是在它的 "脏 "的限度之内。
		 * 限制的区域获取，这样就不会有任何一个区域持有超过其
		 * 全局允许的脏页的比例份额。
		 * 脏限制考虑到了区域的
		 * 低内存储备和高水印，因此 kswapd
		 * 应该能够平衡它而不需要
		 * 从它的LRU列表中写入页面。
		 *
		 * 这看起来可能会增加下层区域的压力。
		 * 在较高区域的分配失败后，会增加较低区域的压力。
		 * 在它们被填满之前。 但是，那些溢出的页面
		 * 溢出是有限的，因为较低的区域受到这个机制的保护
		 * 这也是一种机制。 它不应该成为
		 * 对他们来说是一个实际的负担。
		 *
		 * XXX: 目前，允许分配的数据有可能
		 * 在进入回收之前，允许分配有可能超过慢速路径中每个区的肮脏限制
		 * (ALLOC_WMARK_LOW unset)才进入回收。
		 * 当在NUMA设置中，允许的分区不够大时，这一点很重要。
		 * 区不够大，无法达到
		 * 全局限制。 对这些情况的正确修复
		 * 将需要意识到分区在
		 * dirty-throttling和flushher线程。
		 */
		if ((alloc_flags & ALLOC_WMARK_LOW) &&
		    (gfp_mask & __GFP_WRITE) && !zone_dirty_ok(zone))
			goto this_zone_full;

		BUILD_BUG_ON(ALLOC_NO_WATERMARKS < NR_WMARK);
		if (!(alloc_flags & ALLOC_NO_WATERMARKS)) {
			unsigned long mark;
			int ret;

			mark = zone->watermark[alloc_flags & ALLOC_WMARK_MASK];
			if (zone_watermark_ok(zone, order, mark,
				    classzone_idx, alloc_flags))
				goto try_this_zone;

			if (NUMA_BUILD && !did_zlc_setup && nr_online_nodes > 1) {
				/*
				 *如果有多个节点，我们会进行zlc_setup的操作
				 * 并且在考虑第一个允许的区域之前
				 *由cpuset允许的第一个区域。
				 */
				allowednodes = zlc_setup(zonelist, alloc_flags);
				zlc_active = 1;
				did_zlc_setup = 1;
			}

			if (zone_reclaim_mode == 0)
				goto this_zone_full;

			/*
			 * 由于我们可能刚刚激活了ZLC，请检查第一个
			 *符合条件的区域最近没有通过zone_reclaim。
			 */
			if (NUMA_BUILD && zlc_active &&
				!zlc_zone_worth_trying(zonelist, z, allowednodes))
				continue;

			ret = zone_reclaim(zone, gfp_mask, order);
			switch (ret) {
			case ZONE_RECLAIM_NOSCAN:
				/* 没有扫描 */
				continue;
			case ZONE_RECLAIM_FULL:
				/*已扫描但不可回收 */
				continue;
			default:
				/* 我们是否收回了足够的 */
				if (!zone_watermark_ok(zone, order, mark,
						classzone_idx, alloc_flags))
					goto this_zone_full;
			}
		}

try_this_zone:
		page = buffered_rmqueue(preferred_zone, zone, order,
						gfp_mask, migratetype);
		if (page)
			break;
this_zone_full:
		if (NUMA_BUILD)
			zlc_mark_zone_full(zonelist, z);
	}

	if (unlikely(NUMA_BUILD && page == NULL && zlc_active)) {
		/* 禁用zlc缓存进行第二次分区列表扫描 */
		zlc_active = 0;
		goto zonelist_scan;
	}
	return page;
}

/*
 * 有许多可能的节点的大型机器不应该总是转储每个节点的信息。
 * meminfo在irq上下文中。
 */
static inline bool should_suppress_show_mem(void)
{
	bool ret = false;

#if NODES_SHIFT > 8
	ret = in_interrupt();
#endif
	return ret;
}

static DEFINE_RATELIMIT_STATE(nopage_rs,
		DEFAULT_RATELIMIT_INTERVAL,
		DEFAULT_RATELIMIT_BURST);

void warn_alloc_failed(gfp_t gfp_mask, int order, const char *fmt, ...)
{
	unsigned int filter = SHOW_MEM_FILTER_NODES;

	if ((gfp_mask & __GFP_NOWARN) || !__ratelimit(&nopage_rs) ||
	    debug_guardpage_minorder() > 0)
		return;

	/*
	 * 走动所有的内存来计算页面类型是非常昂贵的，应该在非阻塞的情况下抑制这种做法。
	 * 在非阻塞的情况下，应该抑制这种做法。
	 */
	if (!(gfp_mask & __GFP_WAIT))
		filter |= SHOW_MEM_FILTER_PAGE_COUNT;

	/*
	 * 这记录了在某些情况下分配的例外情况。
	 * 的情况下，允许在当前允许的节点集之外进行分配。
	 *允许的节点。
	 */
	if (!(gfp_mask & __GFP_NOMEMALLOC))
		if (test_thread_flag(TIF_MEMDIE) ||
		    (current->flags & (PF_MEMALLOC | PF_EXITING)))
			filter &= ~SHOW_MEM_FILTER_NODES;
	if (in_interrupt() || !(gfp_mask & __GFP_WAIT))
		filter &= ~SHOW_MEM_FILTER_NODES;

	if (fmt) {
		struct va_format vaf;
		va_list args;

		va_start(args, fmt);

		vaf.fmt = fmt;
		vaf.va = &args;

		pr_warn("%pV", &vaf);

		va_end(args);
	}

	pr_warn("%s: page allocation failure: order:%d, mode:0x%x\n",
		current->comm, order, gfp_mask);

	dump_stack();
	if (!should_suppress_show_mem())
		show_mem(filter);
}

static inline int
should_alloc_retry(gfp_t gfp_mask, unsigned int order,
				unsigned long did_some_progress,
				unsigned long pages_reclaimed)
{
	/* 如果有特别要求，不要循环 */
	if (gfp_mask & __GFP_NORETRY)
		return 0;

	/* 如果有特别要求，总是重试 */
	if (gfp_mask & __GFP_NOFAIL)
		return 1;

	/*
	 * Suspend将GFP_KERNEL转换为__GFP_WAIT，这可能会阻止回收。
	 * 在不调用OOM的情况下向前推进。Suspend也禁用了
	 * 存储设备，所以kswapd将无能为力。如果我们正在暂停，则保释。
	 */
	if (!did_some_progress && pm_suspended_storage())
		return 0;

	/*
	 *在这个实现中，order <= PAGE_ALLOC_COSTLY_ORDER
	 * 意味着__GFP_NOFAIL，但在其他实现中可能不是这样。
	 * 实现中可能不是这样。
	 */
	if (order <= PAGE_ALLOC_COSTLY_ORDER)
		return 1;

	/*
	 *对于>PAGE_ALLOC_COSTLY_ORDER的顺序，如果__GFP_REPEAT被指定为
	 * 指定，那么我们就重试，直到我们不再回收任何页面
	 * (如上所述)，或者我们已经回收了至少与分配顺序相同的页数
	 * 大于分配的顺序。在这两种情况下，如果
	 * 分配仍然失败，我们就停止重试。
	 */
	if (gfp_mask & __GFP_REPEAT && pages_reclaimed < (1 << order))
		return 1;

	return 0;
}

static inline struct page *
__alloc_pages_may_oom(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	struct page *page;

	/* 为zonelist中的区域获取OOM杀手锁 */
	if (!try_set_zonelist_oom(zonelist, gfp_mask)) {
		schedule_timeout_uninterruptible(1);
		return NULL;
	}

	/*
	 *应该通知PM-freezer，可能有一个OOM杀手在
	 *它的方式来杀死和唤醒某人。这太早了，我们可能
	 *最终没有杀死任何东西，但假阳性是可以接受的。
	 * 见freeze_processes。
	 */
	note_oom_kill();

	/*
	 *再过一遍分区名单，保持很高的水位。
	 *在这里，这只是为了抓住一个平行的杀戮，如果我们仍然处于重压之下，我们必须失败。
	 *我们仍然处于重压之下。
	 */
	page = get_page_from_freelist(gfp_mask|__GFP_HARDWALL, nodemask,
		order, zonelist, high_zoneidx,
		ALLOC_WMARK_HIGH|ALLOC_CPUSET,
		preferred_zone, migratetype);
	if (page)
		goto out;

	if (!(gfp_mask & __GFP_NOFAIL)) {
		/* OOM杀手不会帮助高阶分配 */
		if (order > PAGE_ALLOC_COSTLY_ORDER)
			goto out;
		/* OOM杀手不会因为内存不足而无谓地杀死任务 */
		if (high_zoneidx < ZONE_NORMAL)
			goto out;
		/*
		 * GFP_THISNODE包含__GFP_NORETRY，我们从来没有打过这个。
		 * 对__GFP_THISNODE的裸调用进行理智检查，不是真正的OOM。
		 * 调用者应该自己处理页面分配失败，如果
		 * 它指定了 __GFP_THISNODE。
		 * 注意：Hugepage使用它，但会碰到PAGE_ALLOC_COSTLY_ORDER。
		 */
		if (gfp_mask & __GFP_THISNODE)
			goto out;
	}
	/* 已经用尽了可以做的事情，所以现在是blamo时间 */
	out_of_memory(zonelist, gfp_mask, order, nodemask, false);

out:
	clear_zonelist_oom(zonelist, gfp_mask);
	return page;
}

#ifdef CONFIG_COMPACTION
/* 在回收前尝试对高阶分配的内存进行压缩 */
static struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, bool sync_migration,
	bool *deferred_compaction,
	unsigned long *did_some_progress)
{
	struct page *page;

	if (!order)
		return NULL;

	if (compaction_deferred(preferred_zone, order)) {
		*deferred_compaction = true;
		return NULL;
	}

	current->flags |= PF_MEMALLOC;
	*did_some_progress = try_to_compact_pages(zonelist, order, gfp_mask,
						nodemask, sync_migration);
	current->flags &= ~PF_MEMALLOC;
	if (*did_some_progress != COMPACT_SKIPPED) {

		/* 页面迁移释放到PCP列表，但我们想要合并 */
		drain_pages(get_cpu());
		put_cpu();

		page = get_page_from_freelist(gfp_mask, nodemask,
				order, zonelist, high_zoneidx,
				alloc_flags, preferred_zone,
				migratetype);
		if (page) {
			preferred_zone->compact_considered = 0;
			preferred_zone->compact_defer_shift = 0;
			if (order >= preferred_zone->compact_order_failed)
				preferred_zone->compact_order_failed = order + 1;
			count_vm_event(COMPACTSUCCESS);
			return page;
		}

		/*
		 * 如果压缩运行发生并失败，那就糟糕了。
		 * 最有可能的原因是，页面存在。
		 *但不足以满足水印。
		 */
		count_vm_event(COMPACTFAIL);

		/*
		 * 由于同步压实考虑的是分页锁的一个子集，所以只有在同步压实失败的情况下才会推迟。
		 * 如果失败是同步压实的失败，就会推迟。
		 */
		if (sync_migration)
			defer_compaction(preferred_zone, order);

		cond_resched();
	}

	return NULL;
}
#else
static inline struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, bool sync_migration,
	bool *deferred_compaction,
	unsigned long *did_some_progress)
{
	return NULL;
}
#endif /* CONFIG_COMPACTION */

/* 真正的慢速分配器路径，我们进入直接回收 */
static inline struct page *
__alloc_pages_direct_reclaim(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, unsigned long *did_some_progress)
{
	struct page *page = NULL;
	struct reclaim_state reclaim_state;
	bool drained = false;

	cond_resched();

	/* 我们现在进入同步回收阶段 */
	cpuset_memory_pressure_bump();
	current->flags |= PF_MEMALLOC;
	lockdep_set_current_reclaim_state(gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	current->reclaim_state = &reclaim_state;

	*did_some_progress = try_to_free_pages(zonelist, order, gfp_mask, nodemask);

	current->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();
	current->flags &= ~PF_MEMALLOC;

	cond_resched();

	if (unlikely(!(*did_some_progress)))
		return NULL;

	/* 回收成功后，重新考虑所有区域的分配 */
	if (NUMA_BUILD)
		zlc_clear_zones_full(zonelist);

retry:
	page = get_page_from_freelist(gfp_mask, nodemask, order,
					zonelist, high_zoneidx,
					alloc_flags, preferred_zone,
					migratetype);

	/*
	 * 如果一个分配在直接回收后失败，可能是因为
	 * 页面被钉在每个cpu的列表上。排除它们并再次尝试
	 */
	if (!page && !drained) {
		drain_all_pages();
		drained = true;
		goto retry;
	}

	return page;
}

/*
 * 如果分配请求具有足够的紧迫性，以至于可以忽略水印和其他绝望的措施，则在分配器的慢速路径中调用此功能。
 * 有足够的紧迫性来忽略水印和采取其他绝望的措施
 */
static inline struct page *
__alloc_pages_high_priority(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	struct page *page;

	do {
		page = get_page_from_freelist(gfp_mask, nodemask, order,
			zonelist, high_zoneidx, ALLOC_NO_WATERMARKS,
			preferred_zone, migratetype);

		if (!page && gfp_mask & __GFP_NOFAIL)
			wait_iff_congested(preferred_zone, BLK_RW_ASYNC, HZ/50);
	} while (!page && (gfp_mask & __GFP_NOFAIL));

	return page;
}

static inline
void wake_all_kswapd(unsigned int order, struct zonelist *zonelist,
						enum zone_type high_zoneidx,
						enum zone_type classzone_idx)
{
	struct zoneref *z;
	struct zone *zone;

	for_each_zone_zonelist(zone, z, zonelist, high_zoneidx)
		wakeup_kswapd(zone, order, classzone_idx);
}

static inline int
gfp_to_alloc_flags(gfp_t gfp_mask)
{
	int alloc_flags = ALLOC_WMARK_MIN | ALLOC_CPUSET;
	const bool atomic = !(gfp_mask & (__GFP_WAIT | __GFP_NO_KSWAPD));

	/* __GFP_HIGH被假定为与ALLOC_HIGH相同，以保存一个分支。*/
	BUILD_BUG_ON(__GFP_HIGH != (__force gfp_t) ALLOC_HIGH);

	/*
	 * 如果调用者不能运行直接回收，或者调用者有实时调度，那么调用者可能会更多地使用页面储备。
	 * 不能直接回收，或者如果调用者有实时调度
	 * 政策，或者要求获得__GFP_HIGH内存。 GFP_ATOMIC请求将
	 *同时设置ALLOC_HARDER(atomic == true)和ALLOC_HIGH(__GFP_HIGH)。
	 */
	alloc_flags |= (__force int) (gfp_mask & __GFP_HIGH);

	if (atomic) {
		/*
		 *不值得为__GFP_NOMEMALLOC努力分配，即使它不能安排。
		 *如果它不能安排。
		 */
		if (!(gfp_mask & __GFP_NOMEMALLOC))
			alloc_flags |= ALLOC_HARDER;
		/*
		 * 忽略cpuset mems的GFP_ATOMIC而不是失败，见
		 * __cpuset_node_allowed_softwall()的注释。
		 */
		alloc_flags &= ~ALLOC_CPUSET;
	} else if (unlikely(rt_task(current)) && !in_interrupt())
		alloc_flags |= ALLOC_HARDER;

	if (likely(!(gfp_mask & __GFP_NOMEMALLOC))) {
		if (!in_interrupt() &&
		    ((current->flags & PF_MEMALLOC) ||
		     unlikely(test_thread_flag(TIF_MEMDIE))))
			alloc_flags |= ALLOC_NO_WATERMARKS;
	}

	return alloc_flags;
}

static inline struct page *
__alloc_pages_slowpath(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	const gfp_t wait = gfp_mask & __GFP_WAIT;
	struct page *page = NULL;
	int alloc_flags;
	unsigned long pages_reclaimed = 0;
	unsigned long did_some_progress;
	bool sync_migration = false;
	bool deferred_compaction = false;

	/*
	 * 在慢速路径中，我们进行了理智的检查，以避免曾经试图
	 *回收>= MAX_ORDER区域，这将永远不会成功。调用者可能
	 * 使用分配器的顺序，以获取一个太大的区域。
	 * 太大。
	 */
	if (order >= MAX_ORDER) {
		WARN_ON_ONCE(!(gfp_mask & __GFP_NOWARN));
		return NULL;
	}

	/*
	 * GFP_THISNODE（指__GFP_THISNODE、__GFP_NORETRY和
	 * __GFP_NOWARN设置）不应该导致回收，因为使用GFP_THISNODE的子系统
	 * (f.e. slab) 使用GFP_THISNODE可能会选择使用更大的节点集来触发回收。
	 * 在确定了每个节点允许的队列数量后，使用更大的节点集来触发回收。
	 * 每个节点允许的队列是空的，并且节点是
	 * 过度分配。
	 */
	if (NUMA_BUILD && (gfp_mask & GFP_THISNODE) == GFP_THISNODE)
		goto nopage;

restart:
	if (!(gfp_mask & __GFP_NO_KSWAPD))
		wake_all_kswapd(order, zonelist, high_zoneidx,
						zone_idx(preferred_zone));

	/*
	 * 好了，我们在kswapd的水印下面，已经踢到了背景
	 * 回收。现在事情变得更复杂了，所以要根据我们想要的方式来设置 alloc_flags。
	 *根据我们想要的方式进行。
	 */
	alloc_flags = gfp_to_alloc_flags(gfp_mask);

	/*
	 * 如果分配没有受到限制，找到真正的首选区。
	 * cpusets。
	 */
	if (!(alloc_flags & ALLOC_CPUSET) && !nodemask)
		first_zones_zonelist(zonelist, high_zoneidx, NULL,
					&preferred_zone);

rebalance:
	/* 这是最后的机会，一般来说，在goto nopage之前。*/
	page = get_page_from_freelist(gfp_mask, nodemask, order, zonelist,
			high_zoneidx, alloc_flags & ~ALLOC_NO_WATERMARKS,
			preferred_zone, migratetype);
	if (page)
		goto got_pg;

	/* 如果环境允许，分配时不加水印 */
	if (alloc_flags & ALLOC_NO_WATERMARKS) {
		page = __alloc_pages_high_priority(gfp_mask, order,
				zonelist, high_zoneidx, nodemask,
				preferred_zone, migratetype);
		if (page)
			goto got_pg;
	}

	/* 原子分配 - 我们不能平衡任何东西 */
	if (!wait)
		goto nopage;

	/* 避免直接回收的递归 */
	if (current->flags & PF_MEMALLOC)
		goto nopage;

	/* 避免没有水印的分配被无休止地循环 */
	if (test_thread_flag(TIF_MEMDIE) && !(gfp_mask & __GFP_NOFAIL))
		goto nopage;

	/*
	 * 尝试直接压实。第一遍是异步的。后续的
	 * 直接回收后的尝试是同步的
	 */
	page = __alloc_pages_direct_compact(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, sync_migration,
					&deferred_compaction,
					&did_some_progress);
	if (page)
		goto got_pg;
	sync_migration = true;

	/*
	 * 如果高阶分配的压实被推迟，这是因为
	 * 同步压实最近失败。在这种情况下，如果调用者
	 * 要求系统不被严重破坏，那么现在就不进行分配而直接进行回收。
	 * 分配，而不是进入直接回收
	 */
	if (deferred_compaction && (gfp_mask & __GFP_NO_KSWAPD))
		goto nopage;

	/* 尝试直接回收，然后分配 */
	page = __alloc_pages_direct_reclaim(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, &did_some_progress);
	if (page)
		goto got_pg;

	/*
	 * 如果我们未能在回收方面取得任何进展，那么我们就
	 * 已经没有选择了，不得不考虑进行OOM。
	 */
	if (!did_some_progress) {
		if ((gfp_mask & __GFP_FS) && !(gfp_mask & __GFP_NORETRY)) {
			if (oom_killer_disabled)
				goto nopage;
			/* Coredumps可以迅速耗尽所有的内存储备 */
			if ((current->flags & PF_DUMPCORE) &&
			    !(gfp_mask & __GFP_NOFAIL))
				goto nopage;
			page = __alloc_pages_may_oom(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask, preferred_zone,
					migratetype);
			if (page)
				goto got_pg;

			if (!(gfp_mask & __GFP_NOFAIL)) {
				/*
				 * 对于可能失败的高阶分配，不会调用oom。
				 * 可能会失败的分配，所以如果没有进展
				 * 没有进展，就没有其他选择，重试也不可能有帮助。
				 * 重试是不可能有帮助的。
				 */
				if (order > PAGE_ALLOC_COSTLY_ORDER)
					goto nopage;
				/*
				 * 低内存时不调用oom杀手。
				 * 分配，以防止无谓地杀害
				 * 无辜的任务。
				 */
				if (high_zoneidx < ZONE_NORMAL)
					goto nopage;
			}

			goto restart;
		}
	}

	/* 检查我们是否应该重试分配 */
	pages_reclaimed += did_some_progress;
	if (should_alloc_retry(gfp_mask, order, did_some_progress,
						pages_reclaimed)) {
		/* 等待一些写请求完成后重试 */
		wait_iff_congested(preferred_zone, BLK_RW_ASYNC, HZ/50);
		goto rebalance;
	} else {
		/*
		 * 高阶分配后不一定会循环。
		 * 直接回收和回收/压实取决于压实。
		 * 在回收后被调用，所以必要时直接调用
		 */
		page = __alloc_pages_direct_compact(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, sync_migration,
					&deferred_compaction,
					&did_some_progress);
		if (page)
			goto got_pg;
	}

nopage:
	warn_alloc_failed(gfp_mask, order, NULL);
	return page;
got_pg:
	if (kmemcheck_enabled)
		kmemcheck_pagealloc_alloc(page, order, gfp_mask);
	return page;

}

/*
 *这是分区好友分配器的 "核心"。
 */
struct page *
__alloc_pages_nodemask(gfp_t gfp_mask, unsigned int order,
			struct zonelist *zonelist, nodemask_t *nodemask)
{
	enum zone_type high_zoneidx = gfp_zone(gfp_mask);
	struct zone *preferred_zone;
	struct page *page = NULL;
	int migratetype = allocflags_to_migratetype(gfp_mask);
	unsigned int cpuset_mems_cookie;

	gfp_mask &= gfp_allowed_mask;

	lockdep_trace_alloc(gfp_mask);

	might_sleep_if(gfp_mask & __GFP_WAIT);

	if (should_fail_alloc_page(gfp_mask, order))
		return NULL;

	/*
	 * 检查适合gfp_mask的区域，至少包含一个
	 *有效的区域。有可能因为GFP_THISNODE和无记忆节点的结果而出现一个空的分区列表。
	 * GFP_THISNODE和一个无记忆的节点的结果。
	 */
	if (unlikely(!zonelist->_zonerefs->zone))
		return NULL;

retry_cpuset:
	cpuset_mems_cookie = get_mems_allowed();

	/* 首选区域用于以后的统计工作 */
	first_zones_zonelist(zonelist, high_zoneidx,
				nodemask ? : &cpuset_current_mems_allowed,
				&preferred_zone);
	if (!preferred_zone)
		goto out;

	/* 第一次分配尝试 */
	page = get_page_from_freelist(gfp_mask|__GFP_HARDWALL, nodemask, order,
			zonelist, high_zoneidx, ALLOC_WMARK_LOW|ALLOC_CPUSET,
			preferred_zone, migratetype);
	if (unlikely(!page))
		page = __alloc_pages_slowpath(gfp_mask, order,
				zonelist, high_zoneidx, nodemask,
				preferred_zone, migratetype);

	trace_mm_page_alloc(page, order, gfp_mask, migratetype);

out:
	/*
	 * 当更新一个任务的mems_allowed时，有可能会与并行线程发生竞争。
	 * 并行线程的方式，即在更新掩码时，分配可能失败。
	 * 掩码正在被更新。如果一个页面分配即将失败。
	 * 检查cpuset是否在分配过程中发生变化，如果是，就重试。
	 */
	if (unlikely(!put_mems_allowed(cpuset_mems_cookie) && !page))
		goto retry_cpuset;

	return page;
}
EXPORT_SYMBOL(__alloc_pages_nodemask);

/*
 * 常见的辅助功能。
 */
unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order)
{
	struct page *page;

	/*
	 * __get_free_pages() 返回一个32位的地址，它不能代表
	 *一个高内存页
	 */
	VM_BUG_ON((gfp_mask & __GFP_HIGHMEM) != 0);

	page = alloc_pages(gfp_mask, order);
	if (!page)
		return 0;
	return (unsigned long) page_address(page);
}
EXPORT_SYMBOL(__get_free_pages);

unsigned long get_zeroed_page(gfp_t gfp_mask)
{
	return __get_free_pages(gfp_mask | __GFP_ZERO, 0);
}
EXPORT_SYMBOL(get_zeroed_page);

void __free_pages(struct page *page, unsigned int order)
{
	if (put_page_testzero(page)) {
		if (order == 0)
			free_hot_cold_page(page, 0);
		else
			__free_pages_ok(page, order);
	}
}

EXPORT_SYMBOL(__free_pages);

void free_pages(unsigned long addr, unsigned int order)
{
	if (addr != 0) {
		VM_BUG_ON(!virt_addr_valid((void *)addr));
		__free_pages(virt_to_page((void *)addr), order);
	}
}

EXPORT_SYMBOL(free_pages);

static void *make_alloc_exact(unsigned long addr, unsigned order, size_t size)
{
	if (addr) {
		unsigned long alloc_end = addr + (PAGE_SIZE << order);
		unsigned long used = addr + PAGE_ALIGN(size);

		split_page(virt_to_page((void *)addr), order);
		while (used < alloc_end) {
			free_page(used);
			used += PAGE_SIZE;
		}
	}
	return (void *)addr;
}

/**
 * alloc_pages_exact - 分配一个精确数量的物理连续的页面。
 * @size: 要分配的字节数
 * @gfp_mask: 用于分配的GFP标志
 *
 * 这个函数与alloc_pages()类似，只是它分配的是
 * alloc_pages()只能以2次方的方式分配内存，以满足请求。
 * 以2次方的页面分配内存。
 *
 * 这个函数也受到MAX_ORDER的限制。
 *
 * 由这个函数分配的内存必须由free_pages_exact()释放。
 */
void *alloc_pages_exact(size_t size, gfp_t gfp_mask)
{
	unsigned int order = get_order(size);
	unsigned long addr;

	addr = __get_free_pages(gfp_mask, order);
	return make_alloc_exact(addr, order, size);
}
EXPORT_SYMBOL(alloc_pages_exact);

/**
 * alloc_pages_exact_nid - 在一个节点上分配一个精确数量的物理连续的
 * 在一个节点上分配精确数量的物理连续的页面。
 * @nid: 应该分配内存的首选节点ID
 * @size: 要分配的字节数
 * @gfp_mask: 用于分配的GFP标志
 *
 * 与alloc_pages_exact()类似，但在回退之前，先尝试在节点nid上进行分配。
 * 返回。
 * 注意这不是alloc_pages_exact_node()，它在一个特定的节点上进行分配。
 * 但并不精确。
 */
void *alloc_pages_exact_nid(int nid, size_t size, gfp_t gfp_mask)
{
	unsigned order = get_order(size);
	struct page *p = alloc_pages_node(nid, gfp_mask, order);
	if (!p)
		return NULL;
	return make_alloc_exact((unsigned long)page_address(p), order, size);
}
EXPORT_SYMBOL(alloc_pages_exact_nid);

/**
 * free_pages_exact - 释放通过 alloc_pages_exact() 分配的内存。
 * @virt：由alloc_pages_exact返回的值。
 * @size：分配的大小，与传递给 alloc_pages_exact()的值相同。
 *
 * 释放之前调用alloc_pages_exact分配的内存。
 */
void free_pages_exact(void *virt, size_t size)
{
	unsigned long addr = (unsigned long)virt;
	unsigned long end = addr + PAGE_ALIGN(size);

	while (addr < end) {
		free_page(addr);
		addr += PAGE_SIZE;
	}
}
EXPORT_SYMBOL(free_pages_exact);

static unsigned int nr_free_zone_pages(int offset)
{
	struct zoneref *z;
	struct zone *zone;

	/* 只需挑选一个节点，因为回退列表是循环的 */
	unsigned int sum = 0;

	struct zonelist *zonelist = node_zonelist(numa_node_id(), GFP_KERNEL);

	for_each_zone_zonelist(zone, z, zonelist, offset) {
		unsigned long size = zone->present_pages;
		unsigned long high = high_wmark_pages(zone);
		if (size > high)
			sum += size - high;
	}

	return sum;
}

/*
 * 在ZONE_DMA和ZONE_NORMAL内可分配的自由RAM数量
 */
unsigned int nr_free_buffer_pages(void)
{
	return nr_free_zone_pages(gfp_zone(GFP_USER));
}
EXPORT_SYMBOL_GPL(nr_free_buffer_pages);

/*
 *所有区域内可分配的自由RAM的数量
 */
unsigned int nr_free_pagecache_pages(void)
{
	return nr_free_zone_pages(gfp_zone(GFP_HIGHUSER_MOVABLE));
}

static inline void show_node(struct zone *zone)
{
	if (NUMA_BUILD)
		printk("Node %d ", zone_to_nid(zone));
}

void si_meminfo(struct sysinfo *val)
{
	val->totalram = totalram_pages;
	val->sharedram = 0;
	val->freeram = global_page_state(NR_FREE_PAGES);
	val->bufferram = nr_blockdev_pages();
	val->totalhigh = totalhigh_pages;
	val->freehigh = nr_free_highpages();
	val->mem_unit = PAGE_SIZE;
}

EXPORT_SYMBOL(si_meminfo);

#ifdef CONFIG_NUMA
void si_meminfo_node(struct sysinfo *val, int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);

	val->totalram = pgdat->node_present_pages;
	val->freeram = node_page_state(nid, NR_FREE_PAGES);
#ifdef CONFIG_HIGHMEM
	val->totalhigh = pgdat->node_zones[ZONE_HIGHMEM].present_pages;
	val->freehigh = zone_page_state(&pgdat->node_zones[ZONE_HIGHMEM],
			NR_FREE_PAGES);
#else
	val->totalhigh = 0;
	val->freehigh = 0;
#endif
	val->mem_unit = PAGE_SIZE;
}
#endif

/*
 * 确定该节点是否应该被显示，这取决于
 * SHOW_MEM_FILTER_NODES被传递给show_free_areas()。
 */
bool skip_free_areas_node(unsigned int flags, int nid)
{
	bool ret = false;
	unsigned int cpuset_mems_cookie;

	if (!(flags & SHOW_MEM_FILTER_NODES))
		goto out;

	do {
		cpuset_mems_cookie = get_mems_allowed();
		ret = !node_isset(nid, cpuset_current_mems_allowed);
	} while (!put_mems_allowed(cpuset_mems_cookie));
out:
	return ret;
}

#define K(x) ((x) << (PAGE_SHIFT-10))

/*
 * 显示空闲区域列表（在shift_scroll-lock东西里面使用）。
 * 我们还计算了碎片的百分比。我们通过计算以下内容来做到这一点
 * 每个空闲列表上的内存，列表上的第一个项目除外。
 * 抑制那些不被当前的cpuset允许的节点，如果
 * SHOW_MEM_FILTER_NODES被通过。
 */
void show_free_areas(unsigned int filter)
{
	int cpu;
	struct zone *zone;

	for_each_populated_zone(zone) {
		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s per-cpu:\n", zone->name);

		for_each_online_cpu(cpu) {
			struct per_cpu_pageset *pageset;

			pageset = per_cpu_ptr(zone->pageset, cpu);

			printk("CPU %4d: hi:%5d, btch:%4d usd:%4d\n",
			       cpu, pageset->pcp.high,
			       pageset->pcp.batch, pageset->pcp.count);
		}
	}

	printk("active_anon:%lu inactive_anon:%lu isolated_anon:%lu\n"
		" active_file:%lu inactive_file:%lu isolated_file:%lu\n"
		" unevictable:%lu"
		" dirty:%lu writeback:%lu unstable:%lu\n"
		" free:%lu slab_reclaimable:%lu slab_unreclaimable:%lu\n"
		" mapped:%lu shmem:%lu pagetables:%lu bounce:%lu\n",
		global_page_state(NR_ACTIVE_ANON),
		global_page_state(NR_INACTIVE_ANON),
		global_page_state(NR_ISOLATED_ANON),
		global_page_state(NR_ACTIVE_FILE),
		global_page_state(NR_INACTIVE_FILE),
		global_page_state(NR_ISOLATED_FILE),
		global_page_state(NR_UNEVICTABLE),
		global_page_state(NR_FILE_DIRTY),
		global_page_state(NR_WRITEBACK),
		global_page_state(NR_UNSTABLE_NFS),
		global_page_state(NR_FREE_PAGES),
		global_page_state(NR_SLAB_RECLAIMABLE),
		global_page_state(NR_SLAB_UNRECLAIMABLE),
		global_page_state(NR_FILE_MAPPED),
		global_page_state(NR_SHMEM),
		global_page_state(NR_PAGETABLE),
		global_page_state(NR_BOUNCE));

	for_each_populated_zone(zone) {
		int i;

		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s"
			" free:%lukB"
			" min:%lukB"
			" low:%lukB"
			" high:%lukB"
			" active_anon:%lukB"
			" inactive_anon:%lukB"
			" active_file:%lukB"
			" inactive_file:%lukB"
			" unevictable:%lukB"
			" isolated(anon):%lukB"
			" isolated(file):%lukB"
			" present:%lukB"
			" mlocked:%lukB"
			" dirty:%lukB"
			" writeback:%lukB"
			" mapped:%lukB"
			" shmem:%lukB"
			" slab_reclaimable:%lukB"
			" slab_unreclaimable:%lukB"
			" kernel_stack:%lukB"
			" pagetables:%lukB"
			" unstable:%lukB"
			" bounce:%lukB"
			" writeback_tmp:%lukB"
			" pages_scanned:%lu"
			" all_unreclaimable? %s"
			"\n",
			zone->name,
			K(zone_page_state(zone, NR_FREE_PAGES)),
			K(min_wmark_pages(zone)),
			K(low_wmark_pages(zone)),
			K(high_wmark_pages(zone)),
			K(zone_page_state(zone, NR_ACTIVE_ANON)),
			K(zone_page_state(zone, NR_INACTIVE_ANON)),
			K(zone_page_state(zone, NR_ACTIVE_FILE)),
			K(zone_page_state(zone, NR_INACTIVE_FILE)),
			K(zone_page_state(zone, NR_UNEVICTABLE)),
			K(zone_page_state(zone, NR_ISOLATED_ANON)),
			K(zone_page_state(zone, NR_ISOLATED_FILE)),
			K(zone->present_pages),
			K(zone_page_state(zone, NR_MLOCK)),
			K(zone_page_state(zone, NR_FILE_DIRTY)),
			K(zone_page_state(zone, NR_WRITEBACK)),
			K(zone_page_state(zone, NR_FILE_MAPPED)),
			K(zone_page_state(zone, NR_SHMEM)),
			K(zone_page_state(zone, NR_SLAB_RECLAIMABLE)),
			K(zone_page_state(zone, NR_SLAB_UNRECLAIMABLE)),
			zone_page_state(zone, NR_KERNEL_STACK) *
				THREAD_SIZE / 1024,
			K(zone_page_state(zone, NR_PAGETABLE)),
			K(zone_page_state(zone, NR_UNSTABLE_NFS)),
			K(zone_page_state(zone, NR_BOUNCE)),
			K(zone_page_state(zone, NR_WRITEBACK_TEMP)),
			zone->pages_scanned,
			(zone->all_unreclaimable ? "yes" : "no")
			);
		printk("lowmem_reserve[]:");
		for (i = 0; i < MAX_NR_ZONES; i++)
			printk(" %lu", zone->lowmem_reserve[i]);
		printk("\n");
	}

	for_each_populated_zone(zone) {
 		unsigned long nr[MAX_ORDER], flags, order, total = 0;

		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s: ", zone->name);

		spin_lock_irqsave(&zone->lock, flags);
		for (order = 0; order < MAX_ORDER; order++) {
			nr[order] = zone->free_area[order].nr_free;
			total += nr[order] << order;
		}
		spin_unlock_irqrestore(&zone->lock, flags);
		for (order = 0; order < MAX_ORDER; order++)
			printk("%lu*%lukB ", nr[order], K(1UL) << order);
		printk("= %lukB\n", K(total));
	}

	printk("%ld total pagecache pages\n", global_page_state(NR_FILE_PAGES));

	show_swap_cache_info();
}

static void zoneref_set_zone(struct zone *zone, struct zoneref *zoneref)
{
	zoneref->zone = zone;
	zoneref->zone_idx = zone_idx(zone);
}

/*
 * 构建分配后备区列表。
 *
 * 将一个节点的所有填充区添加到分区列表中。
 */
static int build_zonelists_node(pg_data_t *pgdat, struct zonelist *zonelist,
				int nr_zones, enum zone_type zone_type)
{
	struct zone *zone;

	BUG_ON(zone_type >= MAX_NR_ZONES);
	zone_type++;

	do {
		zone_type--;
		zone = pgdat->node_zones + zone_type;
		if (populated_zone(zone)) {
			zoneref_set_zone(zone,
				&zonelist->_zonerefs[nr_zones++]);
			check_highest_zone(zone_type);
		}

	} while (zone_type);
	return nr_zones;
}


/*
 * zonelist_order:
 * 0 = 自动检测更好的排序。
 * 1 = 按（[节点]距离，-zonetype）排序
 * 2 = 按(-zoneetype, [node] distance)排序
 *
 * 如果不是NUMA，ZONELIST_ORDER_ZONE和ZONELIST_ORDER_NODE将创建
 * 相同的分区列表。所以只有NUMA可以配置这个参数。
 */
#define ZONELIST_ORDER_DEFAULT  0
#define ZONELIST_ORDER_NODE     1
#define ZONELIST_ORDER_ZONE     2

/* 内核中的zonelist顺序。
 * set_zonelist_order()会将其设置为NODE或ZONE。
 */
static int current_zonelist_order = ZONELIST_ORDER_DEFAULT;
static char zonelist_order_name[3][8] = {"Default", "Node", "Zone"};


#ifdef CONFIG_NUMA
/* 用户指定的值 .... 由配置改变 */
static int user_zonelist_order = ZONELIST_ORDER_DEFAULT;
/* 用于sysctl的字符串 */
#define NUMA_ZONELIST_ORDER_LEN	16
char numa_zonelist_order[16] = "default";

/*
 * 配置分区名单排序的接口。
 * 命令行选项 "numa_zonelist_order"
 * = "[d]efault - 默认，自动配置。
 * = "[nN]ode - 按节点位置排序，然后按节点内的区域排序
 * = "[ZZ]one - 按区域排序，然后按区域内的位置排序
 */

static int __parse_numa_zonelist_order(char *s)
{
	if (*s == 'd' || *s == 'D') {
		user_zonelist_order = ZONELIST_ORDER_DEFAULT;
	} else if (*s == 'n' || *s == 'N') {
		user_zonelist_order = ZONELIST_ORDER_NODE;
	} else if (*s == 'z' || *s == 'Z') {
		user_zonelist_order = ZONELIST_ORDER_ZONE;
	} else {
		printk(KERN_WARNING
			"Ignoring invalid numa_zonelist_order value:  "
			"%s\n", s);
		return -EINVAL;
	}
	return 0;
}

static __init int setup_numa_zonelist_order(char *s)
{
	int ret;

	if (!s)
		return 0;

	ret = __parse_numa_zonelist_order(s);
	if (ret == 0)
		strlcpy(numa_zonelist_order, s, NUMA_ZONELIST_ORDER_LEN);

	return ret;
}
early_param("numa_zonelist_order", setup_numa_zonelist_order);

/*
 * numa_zonelist_order的sysctl处理程序
 */
int numa_zonelist_order_handler(ctl_table *table, int write,
		void __user *buffer, size_t *length,
		loff_t *ppos)
{
	char saved_string[NUMA_ZONELIST_ORDER_LEN];
	int ret;
	static DEFINE_MUTEX(zl_order_mutex);

	mutex_lock(&zl_order_mutex);
	if (write)
		strcpy(saved_string, (char*)table->data);
	ret = proc_dostring(table, write, buffer, length, ppos);
	if (ret)
		goto out;
	if (write) {
		int oldval = user_zonelist_order;
		if (__parse_numa_zonelist_order((char*)table->data)) {
			/*
			 *假的值。恢复保存的字符串
			 */
			strncpy((char*)table->data, saved_string,
				NUMA_ZONELIST_ORDER_LEN);
			user_zonelist_order = oldval;
		} else if (oldval != user_zonelist_order) {
			mutex_lock(&zonelists_mutex);
			build_all_zonelists(NULL);
			mutex_unlock(&zonelists_mutex);
		}
	}
out:
	mutex_unlock(&zl_order_mutex);
	return ret;
}


#define MAX_NODE_LOAD (nr_online_nodes)
static int node_load[MAX_NUMNODES];

/**
 * find_next_best_node - 找到应该出现在给定节点回退列表中的下一个节点
 * @node: 我们要追加其回避列表的节点
 * @used_node_mask: 已经使用的节点的nodemask_t
 *
 * 我们使用一些因素来决定哪个是应该出现在给定节点上的下一个节点。
 * 出现在一个给定节点的后备列表中。 该节点不应该出现在
 * 已经出现在@node的后备列表中，并且它应该是下一个最近的节点
 *根据距离数组（包含任意的距离值
 *从每个节点到系统中的每个节点的任意距离值），而且还应该优先选择没有CPU的节点
 * 没有CPU的节点，因为据推测它们的分配压力非常小
 * 否则，它们的分配压力会很小。
 * 如果没有找到节点，它将返回-1。
 */
static int find_next_best_node(int node, nodemask_t *used_node_mask)
{
	int n, val;
	int min_val = INT_MAX;
	int best_node = -1;
	const struct cpumask *tmp = cpumask_of_node(0);

	/* 如果我们还没有使用本地节点，则使用本地节点 */
	if (!node_isset(node, *used_node_mask)) {
		node_set(node, *used_node_mask);
		return node;
	}

	for_each_node_state(n, N_HIGH_MEMORY) {

		/* 不希望一个节点出现超过一次 */
		if (node_isset(n, *used_node_mask))
			continue;

		/* 使用距离数组来寻找距离 */
		val = node_distance(node, n);

		/* 对我们下面的节点进行惩罚（"倾向于下一个节点"） */
		val += (n < node);

		/* 给予无头和未使用的节点优先权 */
		tmp = cpumask_of_node(n);
		if (!cpumask_empty(tmp))
			val += PENALTY_FOR_NODE_WITH_CPUS;

		/* 稍微偏向于负载较少的节点 */
		val *= (MAX_NODE_LOAD*MAX_NUMNODES);
		val += node_load[n];

		if (val < min_val) {
			min_val = val;
			best_node = n;
		}
	}

	if (best_node >= 0)
		node_set(best_node, *used_node_mask);

	return best_node;
}


/*
 *按节点和节点内的区域建立分区列表。
 * 这样做的结果是最大限度的定位--正常区域会溢出到本地的
 * DMA区，如果有的话--但有可能耗尽DMA区。
 */
static void build_zonelists_in_node_order(pg_data_t *pgdat, int node)
{
	int j;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[0];
	for (j = 0; zonelist->_zonerefs[j].zone != NULL; j++)
		;
	j = build_zonelists_node(NODE_DATA(node), zonelist, j,
							MAX_NR_ZONES - 1);
	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/*
 *建立gfp_thisnode分区列表
 */
static void build_thisnode_zonelists(pg_data_t *pgdat)
{
	int j;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[1];
	j = build_zonelists_node(pgdat, zonelist, 0, MAX_NR_ZONES - 1);
	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/*
 * 建立按区域和区域内节点排序的分区列表。
 * 这样做的结果是保存DMA区域[s]，直到所有的普通内存被用完。
 *用尽，但会导致溢出到远程节点，而内存
 * 而本地DMA区可能仍然存在。
 */
static int node_order[MAX_NUMNODES];

static void build_zonelists_in_zone_order(pg_data_t *pgdat, int nr_nodes)
{
	int pos, j, node;
	int zone_type;		/* 需要签名 */
	struct zone *z;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[0];
	pos = 0;
	for (zone_type = MAX_NR_ZONES - 1; zone_type >= 0; zone_type--) {
		for (j = 0; j < nr_nodes; j++) {
			node = node_order[j];
			z = &NODE_DATA(node)->node_zones[zone_type];
			if (populated_zone(z)) {
				zoneref_set_zone(z,
					&zonelist->_zonerefs[pos++]);
				check_highest_zone(zone_type);
			}
		}
	}
	zonelist->_zonerefs[pos].zone = NULL;
	zonelist->_zonerefs[pos].zone_idx = 0;
}

static int default_zonelist_order(void)
{
	int nid, zone_type;
	unsigned long low_kmem_size,total_size;
	struct zone *z;
	int average_size;
	/*
         * ZONE_DMA和ZONE_DMA32在系统中可能是非常小的区域。
	 * 如果它们真的很小，而且使用量很大，系统就会很容易陷入
	 * 陷入OOM。
	 * 本函数检测ZONE_DMA/DMA32的大小，并配置区域顺序。
	 */
	/* 是否有ZONE_NORMAL ? (ex. ppc只有DMA区...) */*有ZONE_NORMAL吗？
	low_kmem_size = 0;
	total_size = 0;
	for_each_online_node(nid) {
		for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
			z = &NODE_DATA(nid)->node_zones[zone_type];
			if (populated_zone(z)) {
				if (zone_type < ZONE_NORMAL)
					low_kmem_size += z->present_pages;
				total_size += z->present_pages;
			} else if (zone_type == ZONE_NORMAL) {
				/*
				 * 如果任何节点只有低内存，那么节点顺序
				 * 倾向于允许内核分配
				 * 在本地，否则，当其他节点有大量内存时，它们很容易侵犯到
				 * 当有大量的
				 * 低内存可供分配。
				 */
				return ZONELIST_ORDER_NODE;
			}
		}
	}
	if (!low_kmem_size ||  /*没有DMA区域。*/
	    low_kmem_size > total_size/2) /* DMA/DMA32是大的。*/
		return ZONELIST_ORDER_NODE;
	/*
	 * 查看每个节点的配置。
  	 * 如果有一个节点的DMA/DMA32内存在本地内存上是非常大的面积
 	 *本地内存，NODE_ORDER可能是合适的。
         */
	average_size = total_size /
				(nodes_weight(node_states[N_HIGH_MEMORY]) + 1);
	for_each_online_node(nid) {
		low_kmem_size = 0;
		total_size = 0;
		for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
			z = &NODE_DATA(nid)->node_zones[zone_type];
			if (populated_zone(z)) {
				if (zone_type < ZONE_NORMAL)
					low_kmem_size += z->present_pages;
				total_size += z->present_pages;
			}
		}
		if (low_kmem_size &&
		    total_size > average_size && /* 忽略小结点 */
		    low_kmem_size > total_size * 70/100)
			return ZONELIST_ORDER_NODE;
	}
	return ZONELIST_ORDER_ZONE;
}

static void set_zonelist_order(void)
{
	if (user_zonelist_order == ZONELIST_ORDER_DEFAULT)
		current_zonelist_order = default_zonelist_order();
	else
		current_zonelist_order = user_zonelist_order;
}

static void build_zonelists(pg_data_t *pgdat)
{
	int j, node, load;
	enum zone_type i;
	nodemask_t used_mask;
	int local_node, prev_node;
	struct zonelist *zonelist;
	int order = current_zonelist_order;

	/*初始化分区列表 */
	for (i = 0; i < MAX_ZONELISTS; i++) {
		zonelist = pgdat->node_zonelists + i;
		zonelist->_zonerefs[0].zone = NULL;
		zonelist->_zonerefs[0].zone_idx = 0;
	}

	/* 节点的NUMA感知排序 */
	local_node = pgdat->node_id;
	load = nr_online_nodes;
	prev_node = local_node;
	nodes_clear(used_mask);

	memset(node_order, 0, sizeof(node_order));
	j = 0;

	while ((node = find_next_best_node(local_node, &used_mask)) >= 0) {
		int distance = node_distance(local_node, node);

		/*
		 * 如果另一个节点离得足够远，那么最好是
		 *在离开节点之前，在一个区域内回收页面。
		 */
		if (distance > RECLAIM_DISTANCE)
			zone_reclaim_mode = 1;

		/*
		 * 我们不想给某一节点施加压力。
		 * 因此，在相同的距离组中，对第一个节点增加惩罚，使之成为轮回。
		 *距离组中的第一个节点，使之成为轮回组。
		 */
		if (distance != node_distance(local_node, prev_node))
			node_load[node] = load;

		prev_node = node;
		load--;
		if (order == ZONELIST_ORDER_NODE)
			build_zonelists_in_node_order(pgdat, node);
		else
			node_order[j++] = node;	/* 记住顺序 */
	}

	if (order == ZONELIST_ORDER_ZONE) {
		/* 计算节点顺序 -- 即DMA最后一个! */
		build_zonelists_in_zone_order(pgdat, j);
	}

	build_thisnode_zonelists(pgdat);
}

/* 构建分区列表性能缓存--进一步参见mmzone.h */
static void build_zonelist_cache(pg_data_t *pgdat)
{
	struct zonelist *zonelist;
	struct zonelist_cache *zlc;
	struct zoneref *z;

	zonelist = &pgdat->node_zonelists[0];
	zonelist->zlcache_ptr = zlc = &zonelist->zlcache;
	bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
	for (z = zonelist->_zonerefs; z->zone; z++)
		zlc->z_to_n[z - zonelist->_zonerefs] = zonelist_node_idx(z);
}

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * 返回用于 "本地 "分配的节点的节点ID。
 * 即，arg节点的通用分区列表中第一个区域的第一个节点ID。
 * 用于初始化percpu的'numa_mem'，它主要用于内核分配。
 * 用于内核分配，所以使用GFP_KERNEL标志来定位分区列表。
 */
int local_memory_node(int node)
{
	struct zone *zone;

	(void)first_zones_zonelist(node_zonelist(node, GFP_KERNEL),
				   gfp_zone(GFP_KERNEL),
				   NULL,
				   &zone);
	return zone->node;
}
#endif

#else	/* CONFIG_NUMA */

static void set_zonelist_order(void)
{
	current_zonelist_order = ZONELIST_ORDER_ZONE;
}

static void build_zonelists(pg_data_t *pgdat)
{
	int node, local_node;
	enum zone_type j;
	struct zonelist *zonelist;

	local_node = pgdat->node_id;

	zonelist = &pgdat->node_zonelists[0];
	j = build_zonelists_node(pgdat, zonelist, 0, MAX_NR_ZONES - 1);

	/*
	 * 现在我们建立分区列表，使其包含所有其他节点的分区。
	 *所有其他节点的区域。
	 * 我们不想给某个特定的节点施加压力，所以在为节点N建立区域时
	 * 构建节点N的区域时，我们要确保紧随本地区域之后的
	 * 紧随本地区域之后的是那些来自
	 * 节点N+1 (modulo N)
	 */
	for (node = local_node + 1; node < MAX_NUMNODES; node++) {
		if (!node_online(node))
			continue;
		j = build_zonelists_node(NODE_DATA(node), zonelist, j,
							MAX_NR_ZONES - 1);
	}
	for (node = 0; node < local_node; node++) {
		if (!node_online(node))
			continue;
		j = build_zonelists_node(NODE_DATA(node), zonelist, j,
							MAX_NR_ZONES - 1);
	}

	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/* 非NUMA变体的zonelist性能缓存--只是NULL zlcache_ptr */
static void build_zonelist_cache(pg_data_t *pgdat)
{
	pgdat->node_zonelists[0].zlcache_ptr = NULL;
}

#endif	/* CONFIG_NUMA */

/*
 * 启动页集表。每个cpu有一个，将被用于所有的
 * 区和所有节点。参数将被设置成这样的方式
 * 列表上的一个项目将立即被移交给
 * 伙伴列表。这是安全的，因为页面集的操作是在禁止中断的情况下进行的。
 * 在禁用中断的情况下。
 *
 * 即使在启动完成后，boot_pagesets也必须被保留下来，以用于未使用的处理器和/或区域。
 * 未使用的处理器和/或区域。它们在启动时确实起到了作用
 * 热插拔的处理器。
 *
 * zoneinfo_show()和其他函数都没有检查处理器是否在线。
 * 在跟踪pageset指针之前不检查处理器是否在线。
 * 内核的其他部分可能不会检查该区是否可用。
 */
static void setup_pageset(struct per_cpu_pageset *p, unsigned long batch);
static DEFINE_PER_CPU(struct per_cpu_pageset, boot_pageset);
static void setup_zone_pageset(struct zone *zone);

/*
 * 全局突变，以保护分区列表的大小修改。
 * 以及为新填充的区域序列化pageset设置。
 */
DEFINE_MUTEX(zonelists_mutex);

/*返回值 int ....just for stop_machine() */
static __init_refok int __build_all_zonelists(void *data)
{
	int nid;
	int cpu;

#ifdef CONFIG_NUMA
	memset(node_load, 0, sizeof(node_load));
#endif
	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);

		build_zonelists(pgdat);
		build_zonelist_cache(pgdat);
	}

	/*
	 * 初始化将被用于引导处理器的boot_pagesets。
	 * 用于引导处理器。每个区的真正的页集将在以后分配给每个cpu。
	 * 每个区域的真正的页集将在以后分配，当每个cpu
	 * 分配器时分配。
	 *
	 * boot_pagesets也用于引导脱机的处理器。
	 * 如果系统已经被启动，也会使用这些页集。
	 * 也需要初始化特定cpu上的分配器。
	 * 例如，percpu分配器需要页面分配器，而页面分配器需要percpu分配器。
	 * 需要percpu分配器来分配其页集
	 *（一个鸡生蛋的困境）。
	 */
	for_each_possible_cpu(cpu) {
		setup_pageset(&per_cpu(boot_pageset, cpu), 0);

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
		/*
		 
		 * 也就是通用分区列表中的第一个分区的节点。
		 * 为在线cpu设置numa_mem percpu变量。 在
		 * 启动时，只有启动cpu应该在线；我们将启动
		 * 次级cpu的numa_mem，因为它们已经上线了。 在
		 * 节点/内存热插拔时，我们将修复所有在线cpu。
		 */
		if (cpu_online(cpu))
			set_cpu_numa_mem(cpu, local_memory_node(cpu_to_node(cpu)));
#endif
	}

	return 0;
}

/*
 *调用时始终保持zonelists_mutex。
 * Unless system_state == SYSTEM_BOOTING.
 */
void __ref build_all_zonelists(void *data)
{
	set_zonelist_order();

	if (system_state == SYSTEM_BOOTING) {
		__build_all_zonelists(NULL);
		mminit_verify_zonelist();
		cpuset_init_current_mems_allowed();
	} else {
		/* 我们必须停止所有的cpus，以保证没有用户使用分区列表
		   的用户 */
#ifdef CONFIG_MEMORY_HOTPLUG
		if (data)
			setup_zone_pageset((struct zone *)data);
#endif
		stop_machine(__build_all_zonelists, NULL, NULL);
		/* cpuset刷新程序应该在这里 */
	}
	vm_total_pages = nr_free_pagecache_pages();
	/*
	 * 如果系统中的页数太少，不允许该机制发挥作用，则禁用按移动性分组的功能。
	 * 系统中的页面数量太少，无法让该机制发挥作用。这将是
	 * 更加准确，但对每个区的检查却很昂贵。这个检查是
	 * 在内存热增上进行，所以系统可以在开始时禁用移动性
	 * 禁用，然后再启用它
	 */
#if defined(CONFIG_MOBILITY_GROUP_RAMLESS)
	if (vm_total_pages < (pageblock_nr_pages * MIGRATE_TYPES * 2))
#else
	if (vm_total_pages < (pageblock_nr_pages * MIGRATE_TYPES * 8))
#endif
		page_group_by_mobility_disabled = 1;
	else
		page_group_by_mobility_disabled = 0;

	printk(KERN_INFO "Built %i zonelists in %s order, mobility grouping %s. "
		"Total pages: %ld\n",
			nr_online_nodes,
			zonelist_order_name[current_zonelist_order],
			page_group_by_mobility_disabled ? "off" : "on",
			vm_total_pages);
#ifdef CONFIG_NUMA
	printk("Policy zone: %s\n", zone_names[policy_zone]);
#endif
}

/*
 * 用于确定waitqueue哈希表大小的辅助函数。
 * 本质上，这些函数要选择足够大的哈希表尺寸
 * 足够大，以使试图等待页面的碰撞很少发生。
 * 但事实上，在典型的系统中，活跃的页面等待队列的数量低得可笑。
 * 系统上活跃的页面等待队列的数量低得离谱，不到200个。所以这甚至是
 * 保守的，尽管它看起来很大。
 *
 * 常数PAGES_PER_WAITQUEUE指定了页面与等待队列的比例。
 * 等待队列的比例，也就是说，考虑到页面的数量，等待队列的大小。
 */
#define PAGES_PER_WAITQUEUE	256

#ifndef CONFIG_MEMORY_HOTPLUG
static inline unsigned long wait_table_hash_nr_entries(unsigned long pages)
{
	unsigned long size = 1;

	pages /= PAGES_PER_WAITQUEUE;

	while (size < pages)
		size <<= 1;

	/*
	 * 一旦我们有几十个甚至几百个线程在睡觉
	 *在IO上，我们就会遇到比等待队列碰撞更大的问题。
	 * 把等待表的大小限制在一个合理的范围内。
	 */
	size = min(size, 4096UL);

	return max(size, 4UL);
}
#else
/*
 * 一个区域的大小可能会被热添加所改变，所以不可能确定
 * 它的wait_table的合适大小。 所以我们现在使用最大的尺寸。
 *
 * 最大等待表的大小=4096 x sizeof(wait_queue_head_t). 也就是说。
 *
 * i386 (preemption config) : 4096 x 16 = 64Kbyte.
 * ia64, x86-64 (无抢占配置): 4096 x 20 = 80Kbyte。
 *ia64, x86-64 (preemption): 4096 x 24 = 96Kbyte.
 *
 * 当一个区的内存为(512K+256)页时，准备最大的条目
 * 通过传统的方式，或更多。(见上文)。 它等于。
 *
 * i386, x86-64, powerpc(4K页面大小) : = ( 2G + 1M)byte.
 *ia64(16K页面大小) : = ( 8G + 4M)字节。
 * powerpc(64K页面大小) : = ( 32G + 16M)字节。
 */
static inline unsigned long wait_table_hash_nr_entries(unsigned long pages)
{
	return 4096UL;
}
#endif

/*
 *这是一个整数对数，以便以后可以使用移位法
 * 来从乘法的高位中提取更多的随机位。
 *在取余数之前的哈希函数。
 */
static inline unsigned long wait_table_bits(unsigned long size)
{
	return ffz(~size);
}

#define LONG_ALIGN(x) (((x)+(sizeof(long))-1)&~((sizeof(long))-1))

/*
 * 检查一个分页区是否包含保留页
 */
static int pageblock_is_reserved(unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		if (!pfn_valid_within(pfn) || PageReserved(pfn_to_page(pfn)))
			return 1;
	}
	return 0;
}

/*
 * 将一些页锁标记为MIGRATE_RESERVE。这个数字
 * 保留的块数是基于min_wmark_pages(zone)。内的内存
 * 保留的内存将倾向于存储连续的空闲页。设置min_free_kbytes
 * 会导致一个更大的保留区，它将被释放为连续的
 * 块被释放。
 */
static void setup_zone_migrate_reserve(struct zone *zone)
{
	unsigned long start_pfn, pfn, end_pfn, block_end_pfn;
	struct page *page;
	unsigned long block_migratetype;
	int reserve;

	/*
	 *获得开始pfn、结束pfn和要保留的块数
	 * 我们必须注意与pageblock_nr_pages对齐，以
	 * 确保我们总是检查 pfn_valid 的第一页。
	 * 块的第一页。
	 */
	start_pfn = zone->zone_start_pfn;
	end_pfn = start_pfn + zone->spanned_pages;
	start_pfn = roundup(start_pfn, pageblock_nr_pages);
	reserve = roundup(min_wmark_pages(zone), pageblock_nr_pages) >>
							pageblock_order;

	/*
	 * 储备块通常是为了帮助高阶原子性的
	 * 的分配，这些分配是短暂的。一个min_free_kbytes的值
	 * 会导致原子分配的储备块超过2个
	 * 被认为是为了帮助反碎片化而设置的。
	 * 未来在运行时分配的hugepages。
	 */
	reserve = min(2, reserve);

	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		if (!pfn_valid(pfn))
			continue;
		page = pfn_to_page(pfn);

		/* 注意重叠的节点 */
		if (page_to_nid(page) != zone_to_nid(zone))
			continue;

		block_migratetype = get_pageblock_migratetype(page);

		/* 只测试储备金不满足时的必要内容 */
		if (reserve > 0) {
			/*
			 * 有保留页的区块将永远不会释放，跳过它们。
			 */
			block_end_pfn = min(pfn + pageblock_nr_pages, end_pfn);
			if (pageblock_is_reserved(pfn, block_end_pfn))
				continue;

			/* 如果这个区块被保留了，请将其记入 */
			if (block_migratetype == MIGRATE_RESERVE) {
				reserve--;
				continue;
			}

			/* 如果这个区块是可移动的，适合于保留 */
			if (block_migratetype == MIGRATE_MOVABLE) {
				set_pageblock_migratetype(page,
							MIGRATE_RESERVE);
				move_freepages_block(zone, page,
							MIGRATE_RESERVE);
				reserve--;
				continue;
			}
		}

		/*
		 * 如果满足保留条件，并且这是一个先前的保留区块。
		 * 把它拿回来
		 */
		if (block_migratetype == MIGRATE_RESERVE) {
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);
			move_freepages_block(zone, page, MIGRATE_MOVABLE);
		}
	}
}

/*
 * 最初，所有的页面都被保留，空闲的页面被释放。
 * 一旦早期的启动过程结束，就由free_all_bootmem()释放。
 * 完成。非原子式初始化，单次通过。
 */
void __meminit memmap_init_zone(unsigned long size, int nid, unsigned long zone,
		unsigned long start_pfn, enum memmap_context context)
{
	struct page *page;
	unsigned long end_pfn = start_pfn + size;
	unsigned long pfn;
	struct zone *z;

	if (highest_memmap_pfn < end_pfn - 1)
		highest_memmap_pfn = end_pfn - 1;

	z = &NODE_DATA(nid)->node_zones[zone];
	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		/*
		 *在启动时的mem_map[]s中可能存在漏洞。
		 * 交给这个函数。 它们并不
		 * 存在于热插拔内存中。
		 */
		if (context == MEMMAP_EARLY) {
			if (!early_pfn_valid(pfn))
				continue;
			if (!early_pfn_in_nid(pfn, nid))
				continue;
		}
		page = pfn_to_page(pfn);
		set_page_links(page, zone, nid, pfn);
		mminit_verify_page_links(page, zone, nid, pfn);
		init_page_count(page);
		reset_page_mapcount(page);
		SetPageReserved(page);
		/*
		 * 标记块的可移动性，以便在启动时将块保留为
		 * 启动时可移动。这将迫使内核分配的
		 * 保留他们的区块，而不是在启动时泄漏整个
		 * 在启动期间，当许多长期存在的
		 * 内核分配。后来一些靠近
		 * 被标记为MIGRATE_RESERVE。
		 * setup_zone_migrate_reserve()
		 *
		 * 位图是为zone的有效pfn范围创建的，但是memmap
		 * 可以为无效的页面创建（为了对齐）。
		 * 检查这里，不要针对区外的pfn调用set_pageblock_migratetype()
		 * pfn在区域之外。
		 */
		if ((z->zone_start_pfn <= pfn)
		    && (pfn < z->zone_start_pfn + z->spanned_pages)
		    && !(pfn & (pageblock_nr_pages - 1)))
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);

		INIT_LIST_HEAD(&page->lru);
#ifdef WANT_PAGE_VIRTUAL
		/* 移位不会溢出，因为ZONE_NORMAL低于4G。*/
		if (!is_highmem_idx(zone))
			set_page_address(page, __va(pfn << PAGE_SHIFT));
#endif
	}
}

static void __meminit zone_init_free_lists(struct zone *zone)
{
	int order, t;
	for_each_migratetype_order(order, t) {
		INIT_LIST_HEAD(&zone->free_area[order].free_list[t]);
		zone->free_area[order].nr_free = 0;
	}
}

#ifndef __HAVE_ARCH_MEMMAP_INIT
#define memmap_init(size, nid, zone, start_pfn) \
	memmap_init_zone((size), (nid), (zone), (start_pfn), MEMMAP_EARLY)
#endif

static int zone_batchsize(struct zone *zone)
{
#ifdef CONFIG_MMU
	int batch;

	/*
	 * 每个CPU页面的池子被设置为1000个左右的
	 *区的大小。 但不超过1/2M。
	 *
	 * OK，所以我们不知道缓存有多大。 所以猜测。
	 */
	batch = zone->present_pages / 1024;
	if (batch * PAGE_SIZE > 512 * 1024)
		batch = (512 * 1024) / PAGE_SIZE;
	batch /= 4;		/* 我们有效地*=4以下 */
	if (batch < 1)
		batch = 1;

	/*
	 *将批文夹在一个2^n - 1的值中。拥有一个幂
	 * 2的值被发现更有可能产生
	 * 在某些情况下具有次优的缓存别名特性。
	 *
	 * 例如，如果2个任务交替分配
	 * 分批分配页面，一个任务最终会有很多
	 * 一半可能的页面颜色的页面
	 * 而另一个任务则拥有其他颜色的页面。
	 */
	batch = rounddown_pow_of_two(batch + batch/2) - 1;

	return batch;

#else
	/* 在NOMMU条件下，释放的延迟和批处理应该被抑制。
	 *条件下。
	 *
	 * 问题是，NOMMU需要能够分配大块的
	 * 毗连的内存，因为没有硬件页转换来
	 * 从不连续的页面组装出明显的连续的内存。
	 *
	 * 然而，为批处理而排队的大块连续的页面。
	 * 导致这些页面实际上是以较小的块被释放的。 因为有
	 * 在单个批次被回收之间可能会有很大的延迟。
	 * 回收，这导致了曾经的大块空间被
	 * 碎片化，无法用于高阶分配。
	 */
	return 0;
#endif
}

static void setup_pageset(struct per_cpu_pageset *p, unsigned long batch)
{
	struct per_cpu_pages *pcp;
	int migratetype;

	memset(p, 0, sizeof(*p));

	pcp = &p->pcp;
	pcp->count = 0;
	pcp->high = 6 * batch;
	pcp->batch = max(1UL, 1 * batch);
	for (migratetype = 0; migratetype < MIGRATE_PCPTYPES; migratetype++)
		INIT_LIST_HEAD(&pcp->lists[migratetype]);
}

/*
 * setup_pagelist_highmark()为热的per_cpu_pagelist设置高水位标记。
 * 的高水位，并将其设置为pageset的高值。
 */

static void setup_pagelist_highmark(struct per_cpu_pageset *p,
				unsigned long high)
{
	struct per_cpu_pages *pcp;

	pcp = &p->pcp;
	pcp->high = high;
	pcp->batch = max(1UL, high/4);
	if ((high/4) > (PAGE_SHIFT * 8))
		pcp->batch = PAGE_SHIFT * 8;
}

static void setup_zone_pageset(struct zone *zone)
{
	int cpu;

	zone->pageset = alloc_percpu(struct per_cpu_pageset);

	for_each_possible_cpu(cpu) {
		struct per_cpu_pageset *pcp = per_cpu_ptr(zone->pageset, cpu);

		setup_pageset(pcp, zone_batchsize(zone));

		if (percpu_pagelist_fraction)
			setup_pagelist_highmark(pcp,
				(zone->present_pages /
					percpu_pagelist_fraction));
	}
}

/*
 * 分配每个cpu页集并初始化它们。
 * 在这个调用之前，只有启动页组是可用的。
 */
void __init setup_per_cpu_pageset(void)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		setup_zone_pageset(zone);
}

static noinline __init_refok
int zone_wait_table_init(struct zone *zone, unsigned long zone_size_pages)
{
	int i;
	struct pglist_data *pgdat = zone->zone_pgdat;
	size_t alloc_size;

	/*
	 *每页的等待队列机制使用散列的等待队列
	 *每个区。
	 */
	zone->wait_table_hash_nr_entries =
		 wait_table_hash_nr_entries(zone_size_pages);
	zone->wait_table_bits =
		wait_table_bits(zone->wait_table_hash_nr_entries);
	alloc_size = zone->wait_table_hash_nr_entries
					* sizeof(wait_queue_head_t);

	if (!slab_is_available()) {
		zone->wait_table = (wait_queue_head_t *)
			alloc_bootmem_node_nopanic(pgdat, alloc_size);
	} else {
		/*
		 * 这种情况意味着，一个大小为0的区域通过内存热增获得新的内存
		 * 通过内存热添加。
		 *但也有可能是一个新的节点被热加了。 在这种情况下
		 * 这种情况下，vmalloc()将不能使用这个新节点的
		 * 内存 - 这个 wait_table 必须被初始化以使用这个新的
		 * 节点本身。
		 * 为了使用这个新节点的内存，需要进一步考虑
		 * 有必要。
		 */
		zone->wait_table = vmalloc(alloc_size);
	}
	if (!zone->wait_table)
		return -ENOMEM;

	for(i = 0; i < zone->wait_table_hash_nr_entries; ++i)
		init_waitqueue_head(zone->wait_table + i);

	return 0;
}

static int __zone_pcp_update(void *data)
{
	struct zone *zone = data;
	int cpu;
	unsigned long batch = zone_batchsize(zone), flags;

	for_each_possible_cpu(cpu) {
		struct per_cpu_pageset *pset;
		struct per_cpu_pages *pcp;

		pset = per_cpu_ptr(zone->pageset, cpu);
		pcp = &pset->pcp;

		local_irq_save(flags);
		free_pcppages_bulk(zone, pcp->count, pcp);
		setup_pageset(pset, batch);
		local_irq_restore(flags);
	}
	return 0;
}

void zone_pcp_update(struct zone *zone)
{
	stop_machine(__zone_pcp_update, zone, NULL);
}

static __meminit void zone_pcp_init(struct zone *zone)
{
	/*
	 *每一个cpu子系统在这个时候还没有起来。下面的代码
	 * 依靠链接器的能力来提供
	 * 一个（静态）每cpu变量在每cpu区域的偏移。
	 */
	zone->pageset = &boot_pageset;

	if (zone->present_pages)
		printk(KERN_DEBUG "  %s zone: %lu pages, LIFO batch:%u\n",
			zone->name, zone->present_pages,
					 zone_batchsize(zone));
}

__meminit int init_currently_empty_zone(struct zone *zone,
					unsigned long zone_start_pfn,
					unsigned long size,
					enum memmap_context context)
{
	struct pglist_data *pgdat = zone->zone_pgdat;
	int ret;
	ret = zone_wait_table_init(zone, size);
	if (ret)
		return ret;
	pgdat->nr_zones = zone_idx(zone) + 1;

	zone->zone_start_pfn = zone_start_pfn;

	mminit_dprintk(MMINIT_TRACE, "memmap_init",
			"Initialising map node %d zone %lu pfns %lu -> %lu\n",
			pgdat->node_id,
			(unsigned long)zone_idx(zone),
			zone_start_pfn, (zone_start_pfn + size));

	zone_init_free_lists(zone);

	return 0;
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
#ifndef CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID
/*
 * 由SPARSEMEM要求。给定一个PFN，返回该PFN所在的节点。
 * 架构可以实现他们自己的版本，但是如果add_active_range()
 * 使用，并且没有特殊要求，这是一个方便的
 * 替代方法
 */
int __meminit __early_pfn_to_nid(unsigned long pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid)
		if (start_pfn <= pfn && pfn < end_pfn)
			return nid;
	/* 这是一个内存孔 */
	return -1;
}
#endif /* CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID */

int __meminit early_pfn_to_nid(unsigned long pfn)
{
	int nid;

	nid = __early_pfn_to_nid(pfn);
	if (nid >= 0)
		return nid;
	/*只是返回0 */
	return 0;
}

#ifdef CONFIG_NODES_SPAN_OTHER_NODES
bool __meminit early_pfn_in_nid(unsigned long pfn, int node)
{
	int nid;

	nid = __early_pfn_to_nid(pfn);
	if (nid >= 0 && nid != node)
		return false;
	return true;
}
#endif

/**
 * free_bootmem_with_active_regions - 为每个活动范围调用free_bootmem_node
 * @nid: 要释放内存的节点。如果是MAX_NUMNODES，所有的节点都被释放。
 * @max_low_pfn: 将被传递给free_bootmem_node的最高PFN。
 *
 * 如果一个架构保证了所有用
 * add_active_ranges()注册的所有范围都不包含漏洞并且可以被释放，那么这个
 * 这个函数可以用来代替手动调用free_bootmem()。
 */
void __init free_bootmem_with_active_regions(int nid, unsigned long max_low_pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, this_nid;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, &this_nid) {
		start_pfn = min(start_pfn, max_low_pfn);
		end_pfn = min(end_pfn, max_low_pfn);

		if (start_pfn < end_pfn)
			free_bootmem_node(NODE_DATA(this_nid),
					  PFN_PHYS(start_pfn),
					  (end_pfn - start_pfn) << PAGE_SHIFT);
	}
}

/**
 * sparse_memory_present_with_active_regions - 为每个活动范围调用 memory_present。
 * @nid: 要调用memory_present的节点。如果是MAX_NUMNODES，将使用所有节点。
 *
 * 如果一个架构保证所有的范围都是用
 * add_active_ranges()注册的所有范围都不包含漏洞并且可以被释放，这个
 * 函数可以被用来代替手动调用memory_present()。
 */
void __init sparse_memory_present_with_active_regions(int nid)
{
	unsigned long start_pfn, end_pfn;
	int i, this_nid;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, &this_nid)
		memory_present(this_nid, start_pfn, end_pfn);
}

/**
 * get_pfn_range_for_nid - 返回一个节点的开始和结束页面框架
 * @nid: 要返回范围的nid。如果是MAX_NUMNODES，将返回最小和最大的PFN。
 * @start_pfn: 通过引用传递。返回时，它将拥有节点start_pfn。
 * @end_pfn: 通过引用传递。在返回时，它将拥有节点end_pfn。
 *
 * 它根据信息返回一个节点的开始和结束页框
 * 由调用add_active_range()的拱门提供。如果为一个节点调用
 * 如果为一个没有可用内存的节点调用，将打印一个警告，并且开始和结束的
 * PFNs将为0。
 */
void __meminit get_pfn_range_for_nid(unsigned int nid,
			unsigned long *start_pfn, unsigned long *end_pfn)
{
	unsigned long this_start_pfn, this_end_pfn;
	int i;

	*start_pfn = -1UL;
	*end_pfn = 0;

	for_each_mem_pfn_range(i, nid, &this_start_pfn, &this_end_pfn, NULL) {
		*start_pfn = min(*start_pfn, this_start_pfn);
		*end_pfn = max(*end_pfn, this_end_pfn);
	}

	if (*start_pfn == -1UL)
		*start_pfn = 0;
}

/*
 *这将找到一个可用于ZONE_MOVABLE页面的区域。我们的假设是
 * 假设一个节点内的区域是以单调的方式排列的
 * 递增的内存地址，以便使用 "最高 "的区域。
 */
static void __init find_usable_zone_for_movable(void)
{
	int zone_index;
	for (zone_index = MAX_NR_ZONES - 1; zone_index >= 0; zone_index--) {
		if (zone_index == ZONE_MOVABLE)
			continue;

		if (arch_zone_highest_possible_pfn[zone_index] >
				arch_zone_lowest_possible_pfn[zone_index])
			break;
	}

	VM_BUG_ON(zone_index == -1);
	movable_zone = zone_index;
}

/*
 * 架构所提供的区域范围不包括 ZONE_MOVABLE
 * 因为它的大小与架构无关。与其他区域不同。
 * ZONE_MOVABLE的起点是不固定的。它可能是不同的
 * 它在每个节点中可能是不同的，这取决于每个节点的大小和内核分布的均匀程度。
 * 的分布情况。这个辅助函数调整了区域范围
 * 架构为一个给定的节点提供的区域范围，通过使用
 * ZONE_MOVABLE的最高可用区域。这保留了以下的假设
 * 节点内的区域是按单调增加内存地址的顺序排列的。
 */
static void __meminit adjust_zone_range_for_zone_movable(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *zone_start_pfn,
					unsigned long *zone_end_pfn)
{
	/* 只有当ZONE_MOVABLE在这个节点上时才会调整 */
	if (zone_movable_pfn[nid]) {
		/* 尺寸 ZONE_MOVABLE */
		if (zone_type == ZONE_MOVABLE) {
			*zone_start_pfn = zone_movable_pfn[nid];
			*zone_end_pfn = min(node_end_pfn,
				arch_zone_highest_possible_pfn[movable_zone]);

		/* 在此范围内开始调整ZONE_MOVABLE */
		} else if (*zone_start_pfn < zone_movable_pfn[nid] &&
				*zone_end_pfn > zone_movable_pfn[nid]) {
			*zone_end_pfn = zone_movable_pfn[nid];

		/* 检查整个范围是否在ZONE_MOVABLE之内 */
		} else if (*zone_start_pfn >= zone_movable_pfn[nid])
			*zone_start_pfn = *zone_end_pfn;
	}
}

/*
 * 返回一个节点中一个区所跨越的页数，包括洞的数量
 * present_pages = zone_spanned_pages_in_node() - zone_absent_pages_in_node()
 */
static unsigned long __meminit zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long *ignored)
{
	unsigned long node_start_pfn, node_end_pfn;
	unsigned long zone_start_pfn, zone_end_pfn;

	/* 获取节点和区域的起点和终点 */
	get_pfn_range_for_nid(nid, &node_start_pfn, &node_end_pfn);
	zone_start_pfn = arch_zone_lowest_possible_pfn[zone_type];
	zone_end_pfn = arch_zone_highest_possible_pfn[zone_type];
	adjust_zone_range_for_zone_movable(nid, zone_type,
				node_start_pfn, node_end_pfn,
				&zone_start_pfn, &zone_end_pfn);

	/* 检查该节点是否有在该区规定范围内的页面 */
	if (zone_end_pfn < node_start_pfn || zone_start_pfn > node_end_pfn)
		return 0;

	/*必要时在节点内移动区域边界 */
	zone_end_pfn = min(zone_end_pfn, node_end_pfn);
	zone_start_pfn = max(zone_start_pfn, node_start_pfn);

	/* 返回横跨的页面 */
	return zone_end_pfn - zone_start_pfn;
}

/*
 * 返回一个节点上某个范围内的孔的数量。如果nid是MAX_NUMNODES。
 * 那么请求的范围内的所有孔都将被计算在内。
 */
unsigned long __meminit __absent_pages_in_range(int nid,
				unsigned long range_start_pfn,
				unsigned long range_end_pfn)
{
	unsigned long nr_absent = range_end_pfn - range_start_pfn;
	unsigned long start_pfn, end_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
		start_pfn = clamp(start_pfn, range_start_pfn, range_end_pfn);
		end_pfn = clamp(end_pfn, range_start_pfn, range_end_pfn);
		nr_absent -= end_pfn - start_pfn;
	}
	return nr_absent;
}

/**
 * absent_pages_in_range - 返回一个范围内的孔的页框数量
 * @start_pfn: 开始搜索漏洞的起始PFN
 * @end_pfn: 停止搜索漏洞的结束PFN
 *
 * 它返回在一个范围内的内存孔中的页框数量。
 */
unsigned long __init absent_pages_in_range(unsigned long start_pfn,
							unsigned long end_pfn)
{
	return __absent_pages_in_range(MAX_NUMNODES, start_pfn, end_pfn);
}

/* 返回节点上某一区域的孔中的页框数量 */
static unsigned long __meminit zone_absent_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long *ignored)
{
	unsigned long zone_low = arch_zone_lowest_possible_pfn[zone_type];
	unsigned long zone_high = arch_zone_highest_possible_pfn[zone_type];
	unsigned long node_start_pfn, node_end_pfn;
	unsigned long zone_start_pfn, zone_end_pfn;

	get_pfn_range_for_nid(nid, &node_start_pfn, &node_end_pfn);
	zone_start_pfn = clamp(node_start_pfn, zone_low, zone_high);
	zone_end_pfn = clamp(node_end_pfn, zone_low, zone_high);

	adjust_zone_range_for_zone_movable(nid, zone_type,
			node_start_pfn, node_end_pfn,
			&zone_start_pfn, &zone_end_pfn);
	return __absent_pages_in_range(nid, zone_start_pfn, zone_end_pfn);
}

#else /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */
static inline unsigned long __meminit zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long *zones_size)
{
	return zones_size[zone_type];
}

static inline unsigned long __meminit zone_absent_pages_in_node(int nid,
						unsigned long zone_type,
						unsigned long *zholes_size)
{
	if (!zholes_size)
		return 0;

	return zholes_size[zone_type];
}

#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

static void __meminit calculate_node_totalpages(struct pglist_data *pgdat,
		unsigned long *zones_size, unsigned long *zholes_size)
{
	unsigned long realtotalpages, totalpages = 0;
	enum zone_type i;

	for (i = 0; i < MAX_NR_ZONES; i++)
		totalpages += zone_spanned_pages_in_node(pgdat->node_id, i,
								zones_size);
	pgdat->node_spanned_pages = totalpages;

	realtotalpages = totalpages;
	for (i = 0; i < MAX_NR_ZONES; i++)
		realtotalpages -=
			zone_absent_pages_in_node(pgdat->node_id, i,
								zholes_size);
	pgdat->node_present_pages = realtotalpages;
	printk(KERN_DEBUG "On node %d totalpages: %lu\n", pgdat->node_id,
							realtotalpages);
}

#ifndef CONFIG_SPARSEMEM
/*
 *计算zone->blockflags的大小，四舍五入为无符号长。
 * 首先确保 zonesize 是 pageblock_order 的倍数，通过四舍五入的方式
 *向上。然后在每个分页块中使用1 NR_PAGEBLOCK_BITS价值的比特，最后
 * 将现在的比特数四舍五入到最接近的长比特数，然后以比特为单位返回。
 * 字节。
 */
static unsigned long __init usemap_size(unsigned long zone_start_pfn, unsigned long zonesize)
{
	unsigned long usemapsize;

	zonesize += zone_start_pfn & (pageblock_nr_pages-1);
	usemapsize = roundup(zonesize, pageblock_nr_pages);
	usemapsize = usemapsize >> pageblock_order;
	usemapsize *= NR_PAGEBLOCK_BITS;
	usemapsize = roundup(usemapsize, 8 * sizeof(unsigned long));

	return usemapsize / 8;
}

static void __init setup_usemap(struct pglist_data *pgdat,
				struct zone *zone,
				unsigned long zone_start_pfn,
				unsigned long zonesize)
{
	unsigned long usemapsize = usemap_size(zone_start_pfn, zonesize);
	zone->pageblock_flags = NULL;
	if (usemapsize)
		zone->pageblock_flags = alloc_bootmem_node_nopanic(pgdat,
								   usemapsize);
}
#else
static inline void setup_usemap(struct pglist_data *pgdat, struct zone *zone,
				unsigned long zone_start_pfn, unsigned long zonesize) {}
#endif /* CONFIG_SPARSEMEM */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE

/* 初始化由NR_PAGEBLOCK_BITS代表的页数 */
void __init set_pageblock_order(void)
{
	unsigned int order;

	/* 检查pageblock_nr_pages是否已经被设置 */
	if (pageblock_order)
		return;

	if (HPAGE_SHIFT > PAGE_SHIFT)
		order = HUGETLB_PAGE_ORDER;
	else
		order = MAX_ORDER - 1;

	/*
	 * 假设感兴趣的最大连续顺序是一个巨大的页面。
	 * 这个值可能是可变的，取决于IA64和Powerpc上的启动参数。
	 * powerpc。
	 */
	pageblock_order = order;
}
#else /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * 当CONFIG_HUGETLB_PAGE_SIZE_VARIABLE未被设置时，set_pageblock_order()
 * 是未使用的，因为pageblock_order是在编译时设置的。参见
 * 参见include/linux/pageblock-flags.h以了解基于pageblock_order的值。
 * 内核配置
 */
void __init set_pageblock_order(void)
{
}

#endif /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * 设置区域数据结构。
 * - 标记所有页面为保留页
 * - 标记所有内存队列为空
 * - 清除内存位图
 */
static void __paginginit free_area_init_core(struct pglist_data *pgdat,
		unsigned long *zones_size, unsigned long *zholes_size)
{
	enum zone_type j;
	int nid = pgdat->node_id;
	unsigned long zone_start_pfn = pgdat->node_start_pfn;
	int ret;

	pgdat_resize_init(pgdat);
	pgdat->nr_zones = 0;
	init_waitqueue_head(&pgdat->kswapd_wait);
	pgdat->kswapd_max_order = 0;
	pgdat_page_cgroup_init(pgdat);
	
	for (j = 0; j < MAX_NR_ZONES; j++) {
		struct zone *zone = pgdat->node_zones + j;
		unsigned long size, realsize, memmap_pages;
		enum lru_list lru;

		size = zone_spanned_pages_in_node(nid, j, zones_size);
		realsize = size - zone_absent_pages_in_node(nid, j,
								zholes_size);

		/*
		 * 调整realsize，使其考虑到这个区域在memmap中使用了多少内存。
		 * 被这个区域的memmap使用。这将影响到水印
		 *和每个CPU的初始化
		 */
		memmap_pages =
			PAGE_ALIGN(size * sizeof(struct page)) >> PAGE_SHIFT;
		if (realsize >= memmap_pages) {
			realsize -= memmap_pages;
			if (memmap_pages)
				printk(KERN_DEBUG
				       "  %s zone: %lu pages used for memmap\n",
				       zone_names[j], memmap_pages);
		} else
			printk(KERN_WARNING
				"  %s zone: %lu pages exceeds realsize %lu\n",
				zone_names[j], memmap_pages, realsize);

		/* 对保留页的说明 */
		if (j == 0 && realsize > dma_reserve) {
			realsize -= dma_reserve;
			printk(KERN_DEBUG "  %s zone: %lu pages reserved\n",
					zone_names[0], dma_reserve);
		}

		if (!is_highmem_idx(j))
			nr_kernel_pages += realsize;
		nr_all_pages += realsize;

		zone->spanned_pages = size;
		zone->present_pages = realsize;
#ifdef CONFIG_NUMA
		zone->node = nid;
		zone->min_unmapped_pages = (realsize*sysctl_min_unmapped_ratio)
						/ 100;
		zone->min_slab_pages = (realsize * sysctl_min_slab_ratio) / 100;
#endif
		zone->name = zone_names[j];
		spin_lock_init(&zone->lock);
		spin_lock_init(&zone->lru_lock);
		zone_seqlock_init(zone);
		zone->zone_pgdat = pgdat;

		zone_pcp_init(zone);
		for_each_lru(lru)
			INIT_LIST_HEAD(&zone->lruvec.lists[lru]);
		zone->reclaim_stat.recent_rotated[0] = 0;
		zone->reclaim_stat.recent_rotated[1] = 0;
		zone->reclaim_stat.recent_scanned[0] = 0;
		zone->reclaim_stat.recent_scanned[1] = 0;
		zap_zone_vm_stats(zone);
		zone->flags = 0;
		if (!size)
			continue;

		set_pageblock_order();
		setup_usemap(pgdat, zone, zone_start_pfn, size);
		ret = init_currently_empty_zone(zone, zone_start_pfn,
						size, MEMMAP_EARLY);
		BUG_ON(ret);
		memmap_init(size, nid, j, zone_start_pfn);
		zone_start_pfn += size;
	}
}

static void __init_refok alloc_node_mem_map(struct pglist_data *pgdat)
{
	/* 跳过空节点 */
	if (!pgdat->node_spanned_pages)
		return;

#ifdef CONFIG_FLAT_NODE_MEM_MAP
	/* ia64得到自己的node_mem_map，在这之前，没有bootmem */
	if (!pgdat->node_mem_map) {
		unsigned long size, start, end;
		struct page *map;

		/*
		 * 该区的端点不要求是MAX_ORDER的。
		 * 但node_mem_map的端点必须是按顺序排列的。
		 * 伙伴分配器才能正常工作。
		 */
		start = pgdat->node_start_pfn & ~(MAX_ORDER_NR_PAGES - 1);
		end = pgdat->node_start_pfn + pgdat->node_spanned_pages;
		end = ALIGN(end, MAX_ORDER_NR_PAGES);
		size =  (end - start) * sizeof(struct page);
		map = alloc_remap(pgdat->node_id, size);
		if (!map)
			map = alloc_bootmem_node_nopanic(pgdat, size);
		pgdat->node_mem_map = map + (pgdat->node_start_pfn - start);
	}
#ifndef CONFIG_NEED_MULTIPLE_NODES
	/*
	 * 在没有DISCONTIG的情况下，全局的mem_map只是被设置为节点0的
	 */
	if (pgdat == NODE_DATA(0)) {
		mem_map = NODE_DATA(0)->node_mem_map;
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
		if (page_to_pfn(mem_map) != pgdat->node_start_pfn)
			mem_map -= (pgdat->node_start_pfn - ARCH_PFN_OFFSET);
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */
	}
#endif
#endif /* CONFIG_FLAT_NODE_MEM_MAP */
}

void __paginginit free_area_init_node(int nid, unsigned long *zones_size,
		unsigned long node_start_pfn, unsigned long *zholes_size)
{
	pg_data_t *pgdat = NODE_DATA(nid);

	pgdat->node_id = nid;
	pgdat->node_start_pfn = node_start_pfn;
	calculate_node_totalpages(pgdat, zones_size, zholes_size);

	alloc_node_mem_map(pgdat);
#ifdef CONFIG_FLAT_NODE_MEM_MAP
	printk(KERN_DEBUG "free_area_init_node: node %d, pgdat %08lx, node_mem_map %08lx\n",
		nid, (unsigned long)pgdat,
		(unsigned long)pgdat->node_mem_map);
#endif

	free_area_init_core(pgdat, zones_size, zholes_size);
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP

#if MAX_NUMNODES > 1
/*
 *算出可能的节点ID的数量。
 */
static void __init setup_nr_node_ids(void)
{
	unsigned int node;
	unsigned int highest = 0;

	for_each_node_mask(node, node_possible_map)
		highest = node;
	nr_node_ids = highest + 1;
}
#else
static inline void setup_nr_node_ids(void)
{
}
#endif

/**
 * node_map_pfn_alignment - 确定最大的节点间排列。
 *
 * 这个函数应该在节点图被填充和排序后调用。
 * 它计算出能区分所有节点的最大二乘法排列。
 * 所有的节点。
 *
 * 例如，如果所有的节点都是1GiB并且对齐到1GiB，那么返回值
 * 将表明1GiB的对齐方式为(1 << (30 - PAGE_SHIFT))。 如果
 * 节点被移位256MiB，则为256MiB。 注意，如果只有最后一个节点被
 * 移位，1GiB就足够了，这个函数会这样表示。
 *
 * 这个函数用来测试所选择的内存的pfn->nid映射是否有足够精细的粒度。
 * 模型是否有足够精细的颗粒度，以避免错误的映射。
 * 填充的节点地图。
 *
 * 返回以pfn为单位的确定的对齐方式。 如果没有对齐方式，则为0
 * 要求（单一节点）。
 */
unsigned long __init node_map_pfn_alignment(void)
{
	unsigned long accl_mask = 0, last_end = 0;
	unsigned long start, end, mask;
	int last_nid = -1;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		if (!start || last_nid < 0 || last_nid == nid) {
			last_nid = nid;
			last_end = end;
			continue;
		}

		/*
		 * 从一个足够细化的掩码开始，以精确到该掩码。
		 * 开始时的pfn，然后一个一个地勾选位，直到它变得
		 * 直到它变得太粗糙，无法将当前节点与最后一个节点分开。
		 */
		mask = ~((1 << __ffs(start)) - 1);
		while (mask && last_end <= (start & (mask << 1)))
			mask <<= 1;

		/* 累积所有节点间掩码 */
		accl_mask |= mask;
	}

	/* 将掩码转换为页数 */
	return ~accl_mask + 1;
}

/* 找到一个节点的最低pfn */
static unsigned long __init find_min_pfn_for_node(int nid)
{
	unsigned long min_pfn = ULONG_MAX;
	unsigned long start_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, NULL, NULL)
		min_pfn = min(min_pfn, start_pfn);

	if (min_pfn == ULONG_MAX) {
		printk(KERN_WARNING
			"Could not find start_pfn for node %d\n", nid);
		return 0;
	}

	return min_pfn;
}

/**
 * find_min_pfn_with_active_regions - 找到注册的最小PFN。
 *
 * 它基于通过以下方式提供的信息返回最小的PFN
 * add_active_range().
 */
unsigned long __init find_min_pfn_with_active_regions(void)
{
	return find_min_pfn_for_node(MAX_NUMNODES);
}

/*
 * early_calculate_totalpages()
 * 为可移动区域计算活动区域的页数之和。
 * 填充N_HIGH_MEMORY以计算可用的节点。
 */
static unsigned long __init early_calculate_totalpages(void)
{
	unsigned long totalpages = 0;
	unsigned long start_pfn, end_pfn;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		unsigned long pages = end_pfn - start_pfn;

		totalpages += pages;
		if (pages)
			node_set_state(nid, N_HIGH_MEMORY);
	}
  	return totalpages;
}

/*
 * 找到每个节点中可移动区域开始的PFN。内核内存
 * 只要节点有足够的内存，就会在节点之间平均分配
 *内存。当它们没有时，一些节点会比其他节点有更多的内核。
 *其他节点
 */
static void __init find_zone_movable_pfns_for_nodes(void)
{
	int i, nid;
	unsigned long usable_startpfn;
	unsigned long kernelcore_node, kernelcore_remaining;
	/* 在借用节点任务之前保存状态 */
	nodemask_t saved_node_state = node_states[N_HIGH_MEMORY];
	unsigned long totalpages = early_calculate_totalpages();
	int usable_nodes = nodes_weight(node_states[N_HIGH_MEMORY]);

	/*
	 * 如果指定了movablecore，则计算出与之相对应的kernelcore的大小，以便使可用的内存能够被使用。
	 * 如果指定了movablecore，那么就计算出与之对应的kernelcore的大小，以便使可用于任何分配类型的内存均匀分布。
	 * 任何分配类型的内存都是均匀分布的。如果同时指定了kernelcore
	 * 和movablecore都被指定的话，那么kernelcore的值
	 * 将被用于required_kernelcore，如果它大于
	 * movablecore所允许的。
	 */
	if (required_movablecore) {
		unsigned long corepages;

/*
		 *四舍五入，使ZONE_MOVABLE至少与用户要求的一样大。
		 *用户要求的大小
		 */
		required_movablecore =
			roundup(required_movablecore, MAX_ORDER_NR_PAGES);
		corepages = totalpages - required_movablecore;

		required_kernelcore = max(required_kernelcore, corepages);
	}

	/* 如果没有指定kernelcore，就没有ZONE_MOVABLE */
	if (!required_kernelcore)
		goto out;

	/* usable_startpfn是ZONE_MOVABLE能达到的最低Pfn */
	find_usable_zone_for_movable();
	usable_startpfn = arch_zone_lowest_possible_pfn[movable_zone];

restart:
	/* 在各节点上尽可能均匀地分布内核内存 */
	kernelcore_node = required_kernelcore / usable_nodes;
	for_each_node_state(nid, N_HIGH_MEMORY) {
		unsigned long start_pfn, end_pfn;

		/*
		 * 如果每个节点的划分超过了所需的数量，则重新计算kernelcore_node。
		 * 现在超过了满足所需的
		 *内核的内存量
		 */
		if (required_kernelcore < kernelcore_node)
			kernelcore_node = required_kernelcore / usable_nodes;

		/*
		 * 在地图运行的过程中，我们使用kernelcore_remaining来跟踪内核可用的内存数量。
		 * 使用kernelcore_remaining来跟踪内核的可用内存。当它为
		 * 0时，节点的其余部分可由ZONE_MOVABLE使用。
		 */
		kernelcore_remaining = kernelcore_node;

		/* 遍历该节点内的每个PFN范围 */
		for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
			unsigned long size_pages;

			start_pfn = max(start_pfn, zone_movable_pfn[nid]);
			if (start_pfn >= end_pfn)
				continue;

			/* 说明只对kernelcore有用的东西 */
			if (start_pfn < usable_startpfn) {
				unsigned long kernel_pages;
				kernel_pages = min(end_pfn, usable_startpfn)
								- start_pfn;

				kernelcore_remaining -= min(kernel_pages,
							kernelcore_remaining);
				required_kernelcore -= min(kernel_pages,
							required_kernelcore);

				/* 如果现在范围已经完全占满，则继续 */
				if (end_pfn <= usable_startpfn) {

					/*
					 * 把zone_movable_pfn推到最后，以便
					 * 这样，如果我们要重新平衡
					 * 在各节点上重新平衡kernelcore，我们将
					 *不会在这里重复计算
					 */
					zone_movable_pfn[nid] = end_pfn;
					continue;
				}
				start_pfn = usable_startpfn;
			}

			/*
			 * ZONE_MOVABLE的可用PFN范围为
			 * start_pfn->end_pfn。计算size_pages为
			 * 用作内核的页数
			 */
			size_pages = end_pfn - start_pfn;
			if (size_pages > kernelcore_remaining)
				size_pages = kernelcore_remaining;
			zone_movable_pfn[nid] = start_pfn + size_pages;

			/*
			 * 某些kernelcore已被满足，更新计数并
			 * 如果该节点的kernelcore已经被
			 *满足了
			 */
			required_kernelcore -= min(required_kernelcore,
								size_pages);
			kernelcore_remaining -= size_pages;
			if (!kernelcore_remaining)
				break;
		}
	}

	/*
	 * 如果仍有required_kernelcore，我们就再做一次传递，在计数中少一个
	 * 在计数中少一个节点。这将进一步推动zone_movable_pfn[nid]的发展。
	 * 在仍有内存的节点上继续推进，直到kernelcore得到满足。
	 * 得到满足
	 */
	usable_nodes--;
	if (usable_nodes && required_kernelcore > usable_nodes)
		goto restart;

	/* 将所有nids上的ZONE_MOVABLE的起始点对齐到MAX_ORDER_NR_PAGES */
	for (nid = 0; nid < MAX_NUMNODES; nid++)
		zone_movable_pfn[nid] =
			roundup(zone_movable_pfn[nid], MAX_ORDER_NR_PAGES);

out:
	/* 恢复节点_状态 */
	node_states[N_HIGH_MEMORY] = saved_node_state;
}

/* 该节点上有任何常规内存吗？*/
static void check_for_regular_memory(pg_data_t *pgdat)
{
#ifdef CONFIG_HIGHMEM
	enum zone_type zone_type;

	for (zone_type = 0; zone_type <= ZONE_NORMAL; zone_type++) {
		struct zone *zone = &pgdat->node_zones[zone_type];
		if (zone->present_pages) {
			node_set_state(zone_to_nid(zone), N_NORMAL_MEMORY);
			break;
		}
	}
#endif
}

/**
 * free_area_init_nodes - 初始化所有pg_data_t和区域数据
 * @max_zone_pfn: 每个区的最大PFN数组
 *
 * 这将为系统中的每个活动节点调用free_area_init_node()。
 * 使用add_active_range()提供的页面范围，每个节点中的每个
 * 每个节点中的区域和它们的孔被计算出来。如果两个相邻区域之间的最大PFN
 * 匹配，则假定该区是空的。
 * 例如，如果arch_max_dma_pfn == arch_max_dma32_pfn，则假定
 * arch_max_dma32_pfn没有页面。也假定一个区域
 * 开始于前一个区的结束处。例如，ZONE_DMA32开始于
 * 在arch_max_dma_pfn。
 */
void __init free_area_init_nodes(unsigned long *max_zone_pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, nid;

	/* 记录区域边界的位置 */
	memset(arch_zone_lowest_possible_pfn, 0,
				sizeof(arch_zone_lowest_possible_pfn));
	memset(arch_zone_highest_possible_pfn, 0,
				sizeof(arch_zone_highest_possible_pfn));

	start_pfn = find_min_pfn_with_active_regions();

	for (i = 0; i < MAX_NR_ZONES; i++) {
		if (i == ZONE_MOVABLE)
			continue;

		end_pfn = max(max_zone_pfn[i], start_pfn);
		arch_zone_lowest_possible_pfn[i] = start_pfn;
		arch_zone_highest_possible_pfn[i] = end_pfn;

		start_pfn = end_pfn;
	}
	arch_zone_lowest_possible_pfn[ZONE_MOVABLE] = 0;
	arch_zone_highest_possible_pfn[ZONE_MOVABLE] = 0;

	/* 找到每个节点中ZONE_MOVABLE开始的PFN */
	memset(zone_movable_pfn, 0, sizeof(zone_movable_pfn));
	find_zone_movable_pfns_for_nodes();

	/* 打印出区域范围 */
	printk(KERN_INFO "Zone PFN ranges:\n");
	for (i = 0; i < MAX_NR_ZONES; i++) {
		if (i == ZONE_MOVABLE)
			continue;
		printk(KERN_INFO "  %-8s ", zone_names[i]);
		if (arch_zone_lowest_possible_pfn[i] ==
				arch_zone_highest_possible_pfn[i])
			printk("empty\n");
		else
			printk(KERN_INFO "%0#10lx -> %0#10lx\n",
				arch_zone_lowest_possible_pfn[i],
				arch_zone_highest_possible_pfn[i]);
	}

	/* 打印出每个节点中ZONE_MOVABLE开始的PFNs */
	printk(KERN_INFO "Movable zone start PFN for each node\n");
	for (i = 0; i < MAX_NUMNODES; i++) {
		if (zone_movable_pfn[i])
			printk(KERN_INFO "  Node %d: %lu\n", i, zone_movable_pfn[i]);
	}

	/* 打印出 early_node_map[] */
	printk(KERN_INFO "Early memory PFN ranges\n");
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid)
		printk(KERN_INFO "  %3d: %0#10lx -> %0#10lx\n", nid, start_pfn, end_pfn);

	/* 初始化每个节点 */
	mminit_verify_pageflags_layout();
	setup_nr_node_ids();
	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		free_area_init_node(nid, NULL,
				find_min_pfn_for_node(nid), NULL);

		/* 该节点上的任何内存 */
		if (pgdat->node_present_pages)
			node_set_state(nid, N_HIGH_MEMORY);
		check_for_regular_memory(pgdat);
	}
}

static int __init cmdline_parse_core(char *p, unsigned long *core)
{
	unsigned long long coremem;
	if (!p)
		return -EINVAL;

	coremem = memparse(p, &p);
	*core = coremem >> PAGE_SHIFT;

	/* 偏执地检查UL是否足以满足coremem的值 */
	WARN_ON((coremem >> PAGE_SHIFT) > ULONG_MAX);

	return 0;
}

/*
 * kernelcore=size 设置用于分配的内存量，这些内存不能回收或迁移。
 *不能被回收或迁移。
 */
static int __init cmdline_parse_kernelcore(char *p)
{
	return cmdline_parse_core(p, &required_kernelcore);
}

/*
 * movablecore=size 设置用于分配的内存量，这些内存量可用于
 *可以被回收或迁移。
 */
static int __init cmdline_parse_movablecore(char *p)
{
	return cmdline_parse_core(p, &required_movablecore);
}

early_param("kernelcore", cmdline_parse_kernelcore);
early_param("movablecore", cmdline_parse_movablecore);

#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

/**
 * set_dma_reserve - 在第一区设置指定的保留页数
 * @new_dma_reserve: 要标记的保留页数
 *
 * 每个CPU的批处理量和区域水印是由present_pages决定的。
 * 在DMA区，相当大的比例可能被内核图像消耗掉了
 * 和其他不可释放的分配，这可能会使水印严重偏移。这个
 * 函数可以选择性地用于计算第一区的不可释放的页面（例如
 * 第一区（例如，ZONE_DMA）。其效果将是较低的水印和
 * 更小的每cpu batchsize。
 */
void __init set_dma_reserve(unsigned long new_dma_reserve)
{
	dma_reserve = new_dma_reserve;
}

void __init free_area_init(unsigned long *zones_size)
{
	free_area_init_node(0, zones_size,
			__pa(PAGE_OFFSET) >> PAGE_SHIFT, NULL);
}

static int page_alloc_cpu_notify(struct notifier_block *self,
				 unsigned long action, void *hcpu)
{
	int cpu = (unsigned long)hcpu;

	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
		lru_add_drain_cpu(cpu);
		drain_pages(cpu);

		/*
		 *将死亡处理器的事件计数器溢出
		 * 进入当前处理器的事件计数器。
		 * 这就人为地提高了当前处理器的计数。
		 *处理器。
		 */
		vm_events_fold_cpu(cpu);

		/*
		 * 将死亡处理器的差分计数器清零
		 * 这样，虚拟机的统计数据就会一致。
		 *
		 * 这只是因为处理器已经死了，不能与我们正在做的事情竞争。
		 * 与我们正在做的事情竞争。
		 */
		refresh_cpu_vm_stats(cpu);
	}
	return NOTIFY_OK;
}

void __init page_alloc_init(void)
{
	hotcpu_notifier(page_alloc_cpu_notify, 0);
}

/*
 * calculate_totalreserve_pages - 当sysctl_lower_zone_reserve_ratio或min_free_kbytes改变时被调用。
 * 或min_free_kbytes发生变化时调用。
 */
static void calculate_totalreserve_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long reserve_pages = 0;
	enum zone_type i, j;

	for_each_online_pgdat(pgdat) {
		for (i = 0; i < MAX_NR_ZONES; i++) {
			struct zone *zone = pgdat->node_zones + i;
			unsigned long max = 0;

			/* 找到区域内的有效和最大的lowmem_reserve */
			for (j = i; j < MAX_NR_ZONES; j++) {
				if (zone->lowmem_reserve[j] > max)
					max = zone->lowmem_reserve[j];
			}

			/* 我们将高水印视为保留页。*/
			max += high_wmark_pages(zone);

			if (max > zone->present_pages)
				max = zone->present_pages;
			reserve_pages += max;
			/*
			 * Lowmem储备不能用于
			 * GFP_HIGHUSER页面缓存分配和
			 * kswapd 试图将各区平衡到它们的高
			 *水印。 因此，两者都不应该被
			 * 被看作是可脏的内存，以防止出现
			 * 为了平衡区域，reclaim不得不清理页面的情况
			 * 以平衡这些区域。
			 */
			zone->dirty_balance_reserve = max;
		}
	}
	dirty_balance_reserve = reserve_pages;
	totalreserve_pages = reserve_pages;
}

/*
 * setup_per_zone_lowmem_reserve - 每当sysctl_lower_zone_reserve_ratio发生变化时被调用。
 * sysctl_lower_zone_reserve_ratio发生变化时调用。 确保每个区
 * 有一个正确的页面保留值，因此，有足够数量的
 * 在成功的__alloc_pages()之后，该区会有足够的页数。
 */
static void setup_per_zone_lowmem_reserve(void)
{
	struct pglist_data *pgdat;
	enum zone_type j, idx;

	for_each_online_pgdat(pgdat) {
		for (j = 0; j < MAX_NR_ZONES; j++) {
			struct zone *zone = pgdat->node_zones + j;
			unsigned long present_pages = zone->present_pages;

			zone->lowmem_reserve[j] = 0;

			idx = j;
			while (idx) {
				struct zone *lower_zone;

				idx--;

				if (sysctl_lowmem_reserve_ratio[idx] < 1)
					sysctl_lowmem_reserve_ratio[idx] = 1;

				lower_zone = pgdat->node_zones + idx;
				lower_zone->lowmem_reserve[j] = present_pages /
					sysctl_lowmem_reserve_ratio[idx];
				present_pages += lower_zone->present_pages;
			}
		}
	}

	/* 更新 totalreserve_pages */
	calculate_totalreserve_pages();
}

/**
 * setup_per_zone_wmarks - 当min_free_kbytes改变时被调用。
 *或当内存被热{添加|删除}时调用
 *
 * 确保每个区的水印[min,low,high]值被正确设置为
 * 确保每个区的水印[min,low,high]值与min_free_kbytes的关系正确。
 */
void setup_per_zone_wmarks(void)
{
	unsigned long pages_min = min_free_kbytes >> (PAGE_SHIFT - 10);
	unsigned long lowmem_pages = 0;
	struct zone *zone;
	unsigned long flags;

	/* 计算 !ZONE_HIGHMEM的总页数 */
	for_each_zone(zone) {
		if (!is_highmem(zone))
			lowmem_pages += zone->present_pages;
	}

	for_each_zone(zone) {
		u64 tmp;

		spin_lock_irqsave(&zone->lock, flags);
		tmp = (u64)pages_min * zone->present_pages;
		do_div(tmp, lowmem_pages);
		if (is_highmem(zone)) {
			/*
			 * __GFP_HIGH和PF_MEMALLOC的分配通常不需要高内存页。
			 * 需要高内存页面，所以在这里将pages_min设为一个小值。
			 *这里的值。
			 *
			 * WMARK_HIGH-WMARK_LOW和(WMARK_LOW-WMARK_MIN)
			 * 控制着异步的页面回收，所以不应该在高内存的情况下设置上限。
			 * 不应该为highmem设置上限。
			 */
			int min_pages;

			min_pages = zone->present_pages / 1024;
			if (min_pages < SWAP_CLUSTER_MAX)
				min_pages = SWAP_CLUSTER_MAX;
			if (min_pages > 128)
				min_pages = 128;
			zone->watermark[WMARK_MIN] = min_pages;
		} else {
			/*
			 * 如果它是一个低内存区，保留一定数量的页面
			 *与该区的大小相称。
			 */
			zone->watermark[WMARK_MIN] = tmp;
		}

		zone->watermark[WMARK_LOW]  = min_wmark_pages(zone) + (tmp >> 2);
		zone->watermark[WMARK_HIGH] = min_wmark_pages(zone) + (tmp >> 1);
		setup_zone_migrate_reserve(zone);
		spin_unlock_irqrestore(&zone->lock, flags);
	}

	/* 更新 totalreserve_pages */
	calculate_totalreserve_pages();
}

/*
 * 不活跃的匿名者名单应该足够小，以便虚拟机不必做太多工作。
 * 做太多的工作，但也要足够大，以便每个不活动的页面有机会
 * 在它被替换掉之前有机会再次被引用。
 *
 * inactive_anon比率是ACTIVE_ANON与INACTIVE_ANON页面的目标比率。
 * 在这个区域的LRU上的非活动页面与非活动页面的目标比例，由
 * pageout代码。一个zone->inactive_ratio为3意味着3：1或者25%的
 * 匿名页面被保留在非活动列表中。
 *
 * 总内存    目标比率   最大的不活跃内存
 * -------------------------------------
 *   10MB       1         5MB
 *  100MB       1        50MB
 *    1GB       3       250MB
 *   10GB      10       0.9GB
 *  100GB      31         3GB
 *    1TB     101        10GB
 *   10TB     320        32GB
 */
static void __meminit calculate_zone_inactive_ratio(struct zone *zone)
{
	unsigned int gb, ratio;

	/* 区域大小，以千兆字节为单位 */
	gb = zone->present_pages >> (30 - PAGE_SHIFT);
	if (gb)
		ratio = int_sqrt(10 * gb);
	else
		ratio = 1;

	zone->inactive_ratio = ratio;
}

static void __meminit setup_per_zone_inactive_ratio(void)
{
	struct zone *zone;

	for_each_zone(zone)
		calculate_zone_inactive_ratio(zone);
}

/*
 * 初始化min_free_kbytes。
 *
 * 对于小型机器，我们希望它很小（最小128k）。 对于大型机器
 * 我们希望它很大（最大64MB）。 但它不是线性的，因为网络
 * 带宽不会随着机器大小而线性增加。 我们使用
 *
 * min_free_kbytes = 4 * sqrt(lowmem_kbytes)，以获得更好的准确性。
 * min_free_kbytes = sqrt(lowmem_kbytes * 16)
 *
 * 这就产生了
 *
 * 16MB:	512k
 * 32MB:	724k
 * 64MB:	1024k
 * 128MB:	1448k
 * 256MB:	2048k
 * 512MB:	2896k
 * 1024MB:	4096k
 * 2048MB:	5792k
 * 4096MB:	8192k
 * 8192MB:	11584k
 * 16384MB:	16384k
 */
int __meminit init_per_zone_wmark_min(void)
{
	unsigned long lowmem_kbytes;

	lowmem_kbytes = nr_free_buffer_pages() * (PAGE_SIZE >> 10);

	min_free_kbytes = int_sqrt(lowmem_kbytes * 16);
	if (min_free_kbytes < 128)
		min_free_kbytes = 128;
	if (min_free_kbytes > 65536)
		min_free_kbytes = 65536;
	setup_per_zone_wmarks();
	refresh_zone_stat_thresholds();
	setup_per_zone_lowmem_reserve();
	setup_per_zone_inactive_ratio();
	return 0;
}
module_init(init_per_zone_wmark_min)

/*
 * min_free_kbytes_sysctl_handler - 只是一个围绕proc_dointvec()的包装器，所以 
 * 我们可以在min_free_kbytes发生变化时调用两个辅助函数。
 * 发生变化时，我们可以调用两个辅助函数。
 */
int min_free_kbytes_sysctl_handler(ctl_table *table, int write, 
	void __user *buffer, size_t *length, loff_t *ppos)
{
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	if (write)
		setup_per_zone_wmarks();
	return 0;
}

#ifdef CONFIG_NUMA
int sysctl_min_unmapped_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	for_each_zone(zone)
		zone->min_unmapped_pages = (zone->present_pages *
				sysctl_min_unmapped_ratio) / 100;
	return 0;
}

int sysctl_min_slab_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	for_each_zone(zone)
		zone->min_slab_pages = (zone->present_pages *
				sysctl_min_slab_ratio) / 100;
	return 0;
}
#endif

/*
 * lowmem_reserve_ratio_sysctl_handler - 只是一个围绕着
 * proc_dointvec()，这样我们就可以在sysctl_lowmem_reserve()发生变化时调用setup_per_zone_lowmem_reserve()
 * 只要sysctl_lowmem_reserve_ratio发生变化。
 *
 * 储备率显然与最小水印完全没有关系。
 * 最小水印。低内存储备率只有在以下情况下才有意义
 * 如果在启动时区大小的函数中。
 */
int lowmem_reserve_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	proc_dointvec_minmax(table, write, buffer, length, ppos);
	setup_per_zone_lowmem_reserve();
	return 0;
}

/*
 * percpu_pagelist_fraction - 改变每个区的pcp->high在每个
 * cpu。 它是每个cpu pagelist在被冲回伙伴分配器之前，在每个区域的总页数的一部分。
 * 在被刷回给好友分配器之前可以拥有的总页数。
 */

int percpu_pagelist_fraction_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	unsigned int cpu;
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (!write || (ret < 0))
		return ret;
	for_each_populated_zone(zone) {
		for_each_possible_cpu(cpu) {
			unsigned long  high;
			high = zone->present_pages / percpu_pagelist_fraction;
			setup_pagelist_highmark(
				per_cpu_ptr(zone->pageset, cpu), high);
		}
	}
	return 0;
}

int hashdist = HASHDIST_DEFAULT;

#ifdef CONFIG_NUMA
static int __init set_hashdist(char *str)
{
	if (!str)
		return 0;
	hashdist = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("hashdist=", set_hashdist);
#endif

/*
 * 从bootmem中分配一个大的系统哈希表
 * - 假设哈希表必须包含一个精确的2次方的
 * 的数量的条目
 * - 限制是哈希桶的数量，而不是总的分配大小
 */
void *__init alloc_large_system_hash(const char *tablename,
				     unsigned long bucketsize,
				     unsigned long numentries,
				     int scale,
				     int flags,
				     unsigned int *_hash_shift,
				     unsigned int *_hash_mask,
				     unsigned long limit)
{
	unsigned long long max = limit;
	unsigned long log2qty, size;
	void *table = NULL;

	/* 允许内核cmdline有发言权 */
	if (!numentries) {
		/* 将适用的内存大小四舍五入到最接近的兆字节 */
		numentries = nr_kernel_pages;
		numentries += (1UL << (20 - PAGE_SHIFT)) - 1;
		numentries >>= 20 - PAGE_SHIFT;
		numentries <<= 20 - PAGE_SHIFT;

		/* 限制为每2^刻度字节的低内存有一个桶 */
		if (scale > PAGE_SHIFT)
			numentries >>= (scale - PAGE_SHIFT);
		else
			numentries <<= (PAGE_SHIFT - scale);

		/* 确保我们至少有一个0阶的分配。*/
		if (unlikely(flags & HASH_SMALL)) {
			/* 没有HASH_EARLY就没有意义 */
			WARN_ON(!(flags & HASH_EARLY));
			if (!(numentries >> *_hash_shift)) {
				numentries = 1UL << *_hash_shift;
				BUG_ON(!numentries);
			}
		} else if (unlikely((numentries * bucketsize) < PAGE_SIZE))
			numentries = PAGE_SIZE / bucketsize;
	}
	numentries = roundup_pow_of_two(numentries);

	/* 默认情况下，将分配大小限制为总内存的1/16 */
	if (max == 0) {
		max = ((unsigned long long)nr_all_pages << PAGE_SHIFT) >> 4;
		do_div(max, bucketsize);
	}
	max = min(max, 0x80000000ULL);

	if (numentries > max)
		numentries = max;

	log2qty = ilog2(numentries);

	do {
		size = bucketsize << log2qty;
		if (flags & HASH_EARLY)
			table = alloc_bootmem_nopanic(size);
		else if (hashdist)
			table = __vmalloc(size, GFP_ATOMIC, PAGE_KERNEL);
		else {
			/*
			 * 如果bucketsize不是2的幂，我们可能会释放出
			 * 在哈希表的末尾释放一些页面，这时
			 * 腾出一些页面，而 alloc_pages_exact()会自动腾出这些页面。
			 */
			if (get_order(size) < MAX_ORDER) {
				table = alloc_pages_exact(size, GFP_ATOMIC);
				kmemleak_alloc(table, size, 1, GFP_ATOMIC);
			}
		}
	} while (!table && size > PAGE_SIZE && --log2qty);

	if (!table)
		panic("Failed to allocate %s hash table\n", tablename);

	printk(KERN_INFO "%s hash table entries: %ld (order: %d, %lu bytes)\n",
	       tablename,
	       (1UL << log2qty),
	       ilog2(size) - PAGE_SHIFT,
	       size);

	if (_hash_shift)
		*_hash_shift = log2qty;
	if (_hash_mask)
		*_hash_mask = (1 << log2qty) - 1;

	return table;
}

/* 返回一个指针，指向存储影响一个页面块的位图 */
static inline unsigned long *get_pageblock_bitmap(struct zone *zone,
							unsigned long pfn)
{
#ifdef CONFIG_SPARSEMEM
	return __pfn_to_section(pfn)->pageblock_flags;
#else
	return zone->pageblock_flags;
#endif /* CONFIG_SPARSEMEM */
}

static inline int pfn_to_bitidx(struct zone *zone, unsigned long pfn)
{
#ifdef CONFIG_SPARSEMEM
	pfn &= (PAGES_PER_SECTION-1);
	return (pfn >> pageblock_order) * NR_PAGEBLOCK_BITS;
#else
	pfn = pfn - round_down(zone->zone_start_pfn, pageblock_nr_pages);
	return (pfn >> pageblock_order) * NR_PAGEBLOCK_BITS;
#endif /* CONFIG_SPARSEMEM */
}

/**
 * get_pageblock_flags_group - 返回pageblock_nr_pages页面块所要求的标志组。
 * @page: 所关注的块中的页面
 * @start_bitidx: 要检索的第一个感兴趣的位
 * @end_bitidx: 最后一个感兴趣的位
 * 返回 pageblock_bits 标志
 */
unsigned long get_pageblock_flags_group(struct page *page,
					int start_bitidx, int end_bitidx)
{
	struct zone *zone;
	unsigned long *bitmap;
	unsigned long pfn, bitidx;
	unsigned long flags = 0;
	unsigned long value = 1;

	zone = page_zone(page);
	pfn = page_to_pfn(page);
	bitmap = get_pageblock_bitmap(zone, pfn);
	bitidx = pfn_to_bitidx(zone, pfn);

	for (; start_bitidx <= end_bitidx; start_bitidx++, value <<= 1)
		if (test_bit(bitidx + start_bitidx, bitmap))
			flags |= value;

	return flags;
}

/**
 * set_pageblock_flags_group - 为一个pageblock_nr_pages的页面块设置所需的标志组。
 * @page: 所关注的块中的页面
 * @start_bitidx: 第一个感兴趣的位
 * @end_bitidx: 最后一个感兴趣的位
 * @flags: 要设置的标志
 */
void set_pageblock_flags_group(struct page *page, unsigned long flags,
					int start_bitidx, int end_bitidx)
{
	struct zone *zone;
	unsigned long *bitmap;
	unsigned long pfn, bitidx;
	unsigned long value = 1;

	zone = page_zone(page);
	pfn = page_to_pfn(page);
	bitmap = get_pageblock_bitmap(zone, pfn);
	bitidx = pfn_to_bitidx(zone, pfn);
	VM_BUG_ON(pfn < zone->zone_start_pfn);
	VM_BUG_ON(pfn >= zone->zone_start_pfn + zone->spanned_pages);

	for (; start_bitidx <= end_bitidx; start_bitidx++, value <<= 1)
		if (flags & value)
			__set_bit(bitidx + start_bitidx, bitmap);
		else
			__clear_bit(bitidx + start_bitidx, bitmap);
}

/*
 * 这是个子函数...请看page_isolation.c。
 * 设置/清除页面块的类型为ISOLATE。
 * 页面分配器不会从ISOLATE块中分配内存。
 */

static int
__count_immobile_pages(struct zone *zone, struct page *page, int count)
{
	unsigned long pfn, iter, found;
	/*
	 *为了避免噪音数据，应该调用lru_add_drain_all()。
	 * 如果ZONE_MOVABLE，则该区永远不包含不动的页面
	 */
	if (zone_idx(zone) == ZONE_MOVABLE)
		return true;

	if (get_pageblock_migratetype(page) == MIGRATE_MOVABLE)
		return true;

	pfn = page_to_pfn(page);
	for (found = 0, iter = 0; iter < pageblock_nr_pages; iter++) {
		unsigned long check = pfn + iter;

		if (!pfn_valid_within(check))
			continue;

		page = pfn_to_page(check);
		if (!page_count(page)) {
			if (PageBuddy(page))
				iter += (1 << page_order(page)) - 1;
			continue;
		}
		if (!PageLRU(page))
			found++;
		/*
		 * 如果有RECLAIMABLE页面，我们需要检查它。
		 * 但是现在，内存离线本身并没有调用shrink_slab()
		 * 而这一点仍有待修正。
		 */
		/*
		 * 如果该页不是RAM，page_count()应该是0。
		 * 我们不需要更多的检查。这是一个_已使用的_不可移动的页面。
		 *
		 * 这里有问题的是PG_reserved页。PG_reserved
		 * 在启动时被设置为一个内存洞页和一个_used_ kernel
		 * 在启动时被设置为内存洞页和_使用的内核页。
		 */
		if (found > count)
			return false;
	}
	return true;
}

bool is_pageblock_removable_nolock(struct page *page)
{
	struct zone *zone;
	unsigned long pfn;

	/*
	 * 我们在这里必须小心，因为我们在内存上进行迭代
	 * 因为我们是在内存上进行迭代，而这些内存是没有区域意识的，所以我们可能会在区域之外，但仍然在区域之内。
	 * 区外，但仍在区段内。
	 * 我们还必须关注节点。如果节点是离线的
	 * 它的NODE_DATA将是NULL - 参见page_zone。
	 */
	if (!node_online(page_to_nid(page)))
		return false;

	zone = page_zone(page);
	pfn = page_to_pfn(page);
	if (zone->zone_start_pfn > pfn ||
			zone->zone_start_pfn + zone->spanned_pages <= pfn)
		return false;

	return __count_immobile_pages(zone, page, 0);
}

int set_migratetype_isolate(struct page *page)
{
	struct zone *zone;
	unsigned long flags, pfn;
	struct memory_isolate_notify arg;
	int notifier_ret;
	int ret = -EBUSY;

	zone = page_zone(page);

	spin_lock_irqsave(&zone->lock, flags);

	pfn = page_to_pfn(page);
	arg.start_pfn = pfn;
	arg.nr_pages = pageblock_nr_pages;
	arg.pages_found = 0;

	/*
	 * 即使是在Migratetype不是MIGRATE_MOVABLE的情况下，也有可能隔离出一个页块。
	 * migratetype不是MIGRATE_MOVABLE。内存隔离
	 * 通知器链被气球驱动器用来返回
	 * 在一个范围内被气球驱动持有的页面数量
	 * 驱动程序来缩减内存。如果所有的页面都被
	 * 被气球占了，是空闲的，或者在LRU上，隔离可以继续。
	 * 后来，例如，当内存热插拔通知器运行时，这些
	 * 报告为 "可以被隔离 "的页面应该被隔离（释放）。
	 * 由气球驱动器通过内存通知器链进行隔离。
	 */
	notifier_ret = memory_isolate_notify(MEM_ISOLATE_COUNT, &arg);
	notifier_ret = notifier_to_errno(notifier_ret);
	if (notifier_ret)
		goto out;
	/*
	 * 修正：现在，内存热插拔不会自己调用 shrink_slab() 。
	 * 我们只是检查MOVABLE页面。
	 */
	if (__count_immobile_pages(zone, page, arg.pages_found))
		ret = 0;

	/*
	 * 不动的意思是 "不在lru "的paes。如果不移动的页面大于
	 * removable-by-driver pages reported by notifier, we'll fail.
	 */

out:
	if (!ret) {
		set_pageblock_migratetype(page, MIGRATE_ISOLATE);
		move_freepages_block(zone, page, MIGRATE_ISOLATE);
	}

	spin_unlock_irqrestore(&zone->lock, flags);
	if (!ret)
		drain_all_pages();
	return ret;
}

void unset_migratetype_isolate(struct page *page)
{
	struct zone *zone;
	unsigned long flags;
	zone = page_zone(page);
	spin_lock_irqsave(&zone->lock, flags);
	if (get_pageblock_migratetype(page) != MIGRATE_ISOLATE)
		goto out;
	set_pageblock_migratetype(page, MIGRATE_MOVABLE);
	move_freepages_block(zone, page, MIGRATE_MOVABLE);
out:
	spin_unlock_irqrestore(&zone->lock, flags);
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * 在调用此功能之前，必须将该范围内的所有页面隔离开来。
 */
void
__offline_isolated_pages(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *page;
	struct zone *zone;
	int order, i;
	unsigned long pfn;
	unsigned long flags;
	/* 找到第一个有效的pfn */
	for (pfn = start_pfn; pfn < end_pfn; pfn++)
		if (pfn_valid(pfn))
			break;
	if (pfn == end_pfn)
		return;
	zone = page_zone(pfn_to_page(pfn));
	spin_lock_irqsave(&zone->lock, flags);
	pfn = start_pfn;
	while (pfn < end_pfn) {
		if (!pfn_valid(pfn)) {
			pfn++;
			continue;
		}
		page = pfn_to_page(pfn);
		BUG_ON(page_count(page));
		BUG_ON(!PageBuddy(page));
		order = page_order(page);
#ifdef CONFIG_DEBUG_VM
		printk(KERN_INFO "remove from free list %lx %d %lx\n",
		       pfn, 1 << order, end_pfn);
#endif
		list_del(&page->lru);
		rmv_page_order(page);
		zone->free_area[order].nr_free--;
		__mod_zone_page_state(zone, NR_FREE_PAGES,
				      - (1UL << order));
#ifdef CONFIG_HIGHMEM
		if (PageHighMem(page))
			totalhigh_pages -= 1 << order;
#endif
		for (i = 0; i < (1 << order); i++)
			SetPageReserved((page+i));
		pfn += (1 << order);
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif

#ifdef CONFIG_MEMORY_FAILURE
bool is_free_buddy_page(struct page *page)
{
	struct zone *zone = page_zone(page);
	unsigned long pfn = page_to_pfn(page);
	unsigned long flags;
	int order;

	spin_lock_irqsave(&zone->lock, flags);
	for (order = 0; order < MAX_ORDER; order++) {
		struct page *page_head = page - (pfn & ((1 << order) - 1));

		if (PageBuddy(page_head) && page_order(page_head) >= order)
			break;
	}
	spin_unlock_irqrestore(&zone->lock, flags);

	return order < MAX_ORDER;
}
#endif

static struct trace_print_flags pageflag_names[] = {
	{1UL << PG_locked,		"locked"	},
	{1UL << PG_error,		"error"		},
	{1UL << PG_referenced,		"referenced"	},
	{1UL << PG_uptodate,		"uptodate"	},
	{1UL << PG_dirty,		"dirty"		},
	{1UL << PG_lru,			"lru"		},
	{1UL << PG_active,		"active"	},
	{1UL << PG_slab,		"slab"		},
	{1UL << PG_owner_priv_1,	"owner_priv_1"	},
	{1UL << PG_arch_1,		"arch_1"	},
	{1UL << PG_reserved,		"reserved"	},
	{1UL << PG_private,		"private"	},
	{1UL << PG_private_2,		"private_2"	},
	{1UL << PG_writeback,		"writeback"	},
#ifdef CONFIG_PAGEFLAGS_EXTENDED
	{1UL << PG_head,		"head"		},
	{1UL << PG_tail,		"tail"		},
#else
	{1UL << PG_compound,		"compound"	},
#endif
	{1UL << PG_swapcache,		"swapcache"	},
	{1UL << PG_mappedtodisk,	"mappedtodisk"	},
	{1UL << PG_reclaim,		"reclaim"	},
	{1UL << PG_swapbacked,		"swapbacked"	},
	{1UL << PG_unevictable,		"unevictable"	},
#ifdef CONFIG_MMU
	{1UL << PG_mlocked,		"mlocked"	},
#endif
#ifdef CONFIG_ARCH_USES_PG_UNCACHED
	{1UL << PG_uncached,		"uncached"	},
#endif
#ifdef CONFIG_MEMORY_FAILURE
	{1UL << PG_hwpoison,		"hwpoison"	},
#endif
	{-1UL,				NULL		},
};

static void dump_page_flags(unsigned long flags)
{
	const char *delim = "";
	unsigned long mask;
	int i;

	printk(KERN_ALERT "page flags: %#lx(", flags);

	/* 删除区号 */
	flags &= (1UL << NR_PAGEFLAGS) - 1;

	for (i = 0; pageflag_names[i].name && flags; i++) {

		mask = pageflag_names[i].mask;
		if ((flags & mask) != mask)
			continue;

		flags &= ~mask;
		printk("%s%s", delim, pageflag_names[i].name);
		delim = "|";
	}

	/* 检查是否有剩余的标志 */
	if (flags)
		printk("%s%#lx", delim, flags);

	printk(")\n");
}

void dump_page(struct page *page)
{
	printk(KERN_ALERT
	       "page:%p count:%d mapcount:%d mapping:%p index:%#lx\n",
		page, atomic_read(&page->_count), page_mapcount(page),
		page->mapping, page->index);
	dump_page_flags(page->flags);
	mem_cgroup_print_bad_page(page);
}
