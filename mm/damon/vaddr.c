// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Primitives for Virtual Address Spaces
 *
 * Author: SeongJae Park <sjpark@amazon.de>
 */

#define pr_fmt(fmt) "damon-va: " fmt

#include <asm-generic/mman-common.h>

#include "prmtv-common.h"

#ifdef CONFIG_DAMON_VADDR_KUNIT_TEST
#undef DAMON_MIN_REGION
#define DAMON_MIN_REGION 1
#endif

/*
 * 't->id' should be the pointer to the relevant 'struct pid' having reference
 * count.  Caller must put the returned task, unless it is NULL.
 */
#define damon_get_task_struct(t) \
	(get_pid_task((struct pid *)t->id, PIDTYPE_PID))

/*
 * Get the mm_struct of the given target
 *
 * Caller _must_ put the mm_struct after use, unless it is NULL.
 *
 * Returns the mm_struct of the target on success, NULL on failure
 */
static struct mm_struct *damon_get_mm(struct damon_target *t)
{
	struct task_struct *task;
	struct mm_struct *mm;

	task = damon_get_task_struct(t);
	if (!task)
		return NULL;

	mm = get_task_mm(task);
	put_task_struct(task);
	return mm;
}

/*
 * Functions for the initial monitoring target regions construction
 */

/*
 * Size-evenly split a region into 'nr_pieces' small regions
 *
 * Returns 0 on success, or negative error code otherwise.
 */
static int damon_va_evenly_split_region(struct damon_ctx *ctx,
		struct damon_region *r, unsigned int nr_pieces)
{
	unsigned long sz_orig, sz_piece, orig_end;
	struct damon_region *n = NULL, *next;
	unsigned long start;

	if (!r || !nr_pieces)
		return -EINVAL;

	orig_end = r->ar.end;
	sz_orig = r->ar.end - r->ar.start;
	sz_piece = ALIGN_DOWN(sz_orig / nr_pieces, DAMON_MIN_REGION);

	if (!sz_piece)
		return -EINVAL;

	r->ar.end = r->ar.start + sz_piece;
	next = damon_next_region(r);
	for (start = r->ar.end; start + sz_piece <= orig_end;
			start += sz_piece) {
		n = damon_new_region(start, start + sz_piece);
		if (!n)
			return -ENOMEM;
		damon_insert_region(n, r, next);
		r = n;
	}
	/* complement last region for possible rounding error */
	if (n)
		n->ar.end = orig_end;

	return 0;
}

static unsigned long sz_range(struct damon_addr_range *r)
{
	return r->end - r->start;
}

static void swap_ranges(struct damon_addr_range *r1,
			struct damon_addr_range *r2)
{
	struct damon_addr_range tmp;

	tmp = *r1;
	*r1 = *r2;
	*r2 = tmp;
}

/*
 * Find three regions separated by two biggest unmapped regions
 *
 * vma		the head vma of the target address space
 * regions	an array of three address ranges that results will be saved
 *
 * This function receives an address space and finds three regions in it which
 * separated by the two biggest unmapped regions in the space.  Please refer to
 * below comments of '__damon_va_init_regions()' function to know why this is
 * necessary.
 *
 * Returns 0 if success, or negative error code otherwise.
 */
static int __damon_va_three_regions(struct vm_area_struct *vma,
				       struct damon_addr_range regions[3])
{
	struct damon_addr_range gap = {0}, first_gap = {0}, second_gap = {0};
	struct vm_area_struct *last_vma = NULL;
	unsigned long start = 0;
	struct rb_root rbroot;

	/* Find two biggest gaps so that first_gap > second_gap > others */
	for (; vma; vma = vma->vm_next) {
		if (!last_vma) {
			start = vma->vm_start;
			goto next;
		}

		if (vma->rb_subtree_gap <= sz_range(&second_gap)) {
			rbroot.rb_node = &vma->vm_rb;
			vma = rb_entry(rb_last(&rbroot),
					struct vm_area_struct, vm_rb);
			goto next;
		}

		gap.start = last_vma->vm_end;
		gap.end = vma->vm_start;
		if (sz_range(&gap) > sz_range(&second_gap)) {
			swap_ranges(&gap, &second_gap);
			if (sz_range(&second_gap) > sz_range(&first_gap))
				swap_ranges(&second_gap, &first_gap);
		}
next:
		last_vma = vma;
	}

	if (!sz_range(&second_gap) || !sz_range(&first_gap))
		return -EINVAL;

	/* Sort the two biggest gaps by address */
	if (first_gap.start > second_gap.start)
		swap_ranges(&first_gap, &second_gap);

	/* Store the result */
	regions[0].start = ALIGN(start, DAMON_MIN_REGION);
	regions[0].end = ALIGN(first_gap.start, DAMON_MIN_REGION);
	regions[1].start = ALIGN(first_gap.end, DAMON_MIN_REGION);
	regions[1].end = ALIGN(second_gap.start, DAMON_MIN_REGION);
	regions[2].start = ALIGN(second_gap.end, DAMON_MIN_REGION);
	regions[2].end = ALIGN(last_vma->vm_end, DAMON_MIN_REGION);

	return 0;
}

/*
 * Get the three regions in the given target (task)
 *
 * Returns 0 on success, negative error code otherwise.
 */
static int damon_va_three_regions(struct damon_target *t,
				struct damon_addr_range regions[3])
{
	struct mm_struct *mm;
	int rc;

	mm = damon_get_mm(t);
	if (!mm)
		return -EINVAL;

	mmap_read_lock(mm);
	rc = __damon_va_three_regions(mm->mmap, regions);
	mmap_read_unlock(mm);

	mmput(mm);
	return rc;
}

/*
 * Initialize the monitoring target regions for the given target (task)
 *
 * t	the given target
 *
 * Because only a number of small portions of the entire address space
 * is actually mapped to the memory and accessed, monitoring the unmapped
 * regions is wasteful.  That said, because we can deal with small noises,
 * tracking every mapping is not strictly required but could even incur a high
 * overhead if the mapping frequently changes or the number of mappings is
 * high.  The adaptive regions adjustment mechanism will further help to deal
 * with the noise by simply identifying the unmapped areas as a region that
 * has no access.  Moreover, applying the real mappings that would have many
 * unmapped areas inside will make the adaptive mechanism quite complex.  That
 * said, too huge unmapped areas inside the monitoring target should be removed
 * to not take the time for the adaptive mechanism.
 *
 * For the reason, we convert the complex mappings to three distinct regions
 * that cover every mapped area of the address space.  Also the two gaps
 * between the three regions are the two biggest unmapped areas in the given
 * address space.  In detail, this function first identifies the start and the
 * end of the mappings and the two biggest unmapped areas of the address space.
 * Then, it constructs the three regions as below:
 *
 *     [mappings[0]->start, big_two_unmapped_areas[0]->start)
 *     [big_two_unmapped_areas[0]->end, big_two_unmapped_areas[1]->start)
 *     [big_two_unmapped_areas[1]->end, mappings[nr_mappings - 1]->end)
 *
 * As usual memory map of processes is as below, the gap between the heap and
 * the uppermost mmap()-ed region, and the gap between the lowermost mmap()-ed
 * region and the stack will be two biggest unmapped regions.  Because these
 * gaps are exceptionally huge areas in usual address space, excluding these
 * two biggest unmapped regions will be sufficient to make a trade-off.
 *
 *   <heap>
 *   <BIG UNMAPPED REGION 1>
 *   <uppermost mmap()-ed region>
 *   (other mmap()-ed regions and small unmapped regions)
 *   <lowermost mmap()-ed region>
 *   <BIG UNMAPPED REGION 2>
 *   <stack>
 */
static void __damon_va_init_regions(struct damon_ctx *c,
				     struct damon_target *t)
{
	struct damon_region *r;
	struct damon_addr_range regions[3];
	unsigned long sz = 0, nr_pieces;
	int i;

	if (damon_va_three_regions(t, regions)) {
		pr_err("Failed to get three regions of target %lu\n", t->id);
		return;
	}

	for (i = 0; i < 3; i++)
		sz += regions[i].end - regions[i].start;
	if (c->min_nr_regions)
		sz /= c->min_nr_regions;
	if (sz < DAMON_MIN_REGION)
		sz = DAMON_MIN_REGION;

	/* Set the initial three regions of the target */
	for (i = 0; i < 3; i++) {
		r = damon_new_region(regions[i].start, regions[i].end);
		if (!r) {
			pr_err("%d'th init region creation failed\n", i);
			return;
		}
		damon_add_region(r, t);

		nr_pieces = (regions[i].end - regions[i].start) / sz;
		damon_va_evenly_split_region(c, r, nr_pieces);
	}
}

/* Initialize '->regions_list' of every target (task) */
void damon_va_init(struct damon_ctx *ctx)
{
	struct damon_target *t;

	damon_for_each_target(t, ctx) {
		/* the user may set the target regions as they want */
		if (!damon_nr_regions(t))
			__damon_va_init_regions(ctx, t);
	}
}

/*
 * Functions for the dynamic monitoring target regions update
 */

/*
 * Check whether a region is intersecting an address range
 *
 * Returns true if it is.
 */
static bool damon_intersect(struct damon_region *r, struct damon_addr_range *re)
{
	return !(r->ar.end <= re->start || re->end <= r->ar.start);
}

/*
 * Update damon regions for the three big regions of the given target
 *
 * t		the given target
 * bregions	the three big regions of the target
 */
static void damon_va_apply_three_regions(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_addr_range bregions[3])
{
	struct damon_region *r, *next;
	unsigned int i = 0;

	/* Remove regions which are not in the three big regions now */
	damon_for_each_region_safe(r, next, t) {
		for (i = 0; i < 3; i++) {
			if (damon_intersect(r, &bregions[i]))
				break;
		}
		if (i == 3)
			damon_destroy_region(r);
	}

	/* Adjust intersecting regions to fit with the three big regions */
	for (i = 0; i < 3; i++) {
		struct damon_region *first = NULL, *last;
		struct damon_region *newr;
		struct damon_addr_range *br;

		br = &bregions[i];
		/* Get the first and last regions which intersects with br */
		damon_for_each_region(r, t) {
			if (damon_intersect(r, br)) {
				if (!first)
					first = r;
				last = r;
			}
			if (r->ar.start >= br->end)
				break;
		}
		if (!first) {
			/* no damon_region intersects with this big region */
			newr = damon_new_region(
					ALIGN_DOWN(br->start,
						DAMON_MIN_REGION),
					ALIGN(br->end, DAMON_MIN_REGION));
			if (!newr)
				continue;
			damon_insert_region(newr, damon_prev_region(r), r);
		} else {
			first->ar.start = ALIGN_DOWN(br->start,
					DAMON_MIN_REGION);
			last->ar.end = ALIGN(br->end, DAMON_MIN_REGION);
		}
	}
}

/*
 * Update regions for current memory mappings
 */
void damon_va_update(struct damon_ctx *ctx)
{
	struct damon_addr_range three_regions[3];
	struct damon_target *t;

	damon_for_each_target(t, ctx) {
		if (damon_va_three_regions(t, three_regions))
			continue;
		damon_va_apply_three_regions(ctx, t, three_regions);
	}
}

/*
 * Functions for the access checking of the regions
 */

static void damon_va_prepare_access_check(struct damon_ctx *ctx,
			struct mm_struct *mm, struct damon_region *r)
{
	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);

	damon_va_mkold(mm, r->sampling_addr);
}

void damon_va_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct mm_struct *mm;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		mm = damon_get_mm(t);
		if (!mm)
			continue;
		damon_for_each_region(r, t)
			damon_va_prepare_access_check(ctx, mm, r);
		mmput(mm);
	}
}

/*
 * Check whether the region was accessed after the last preparation
 *
 * mm	'mm_struct' for the given virtual address space
 * r	the region to be checked
 */
static void damon_va_check_access(struct damon_ctx *ctx,
			       struct mm_struct *mm, struct damon_region *r)
{
	static struct mm_struct *last_mm;
	static unsigned long last_addr;
	static unsigned long last_page_sz = PAGE_SIZE;
	static bool last_accessed;

	/* If the region is in the last checked page, reuse the result */
	if (mm == last_mm && (ALIGN_DOWN(last_addr, last_page_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_page_sz))) {
		if (last_accessed)
			r->nr_accesses++;
		return;
	}

	last_accessed = damon_va_young(mm, r->sampling_addr, &last_page_sz);
	if (last_accessed)
		r->nr_accesses++;

	last_mm = mm;
	last_addr = r->sampling_addr;
}

unsigned int damon_va_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct mm_struct *mm;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	damon_for_each_target(t, ctx) {
		mm = damon_get_mm(t);
		if (!mm)
			continue;
		damon_for_each_region(r, t) {
			damon_va_check_access(ctx, mm, r);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
		mmput(mm);
	}

	return max_nr_accesses;
}

/*
 * Functions for the target validity check and cleanup
 */

bool damon_va_target_valid(void *target)
{
	struct damon_target *t = target;
	struct task_struct *task;

	task = damon_get_task_struct(t);
	if (task) {
		put_task_struct(task);
		return true;
	}

	return false;
}

void damon_va_cleanup(struct damon_ctx *ctx)
{
	struct damon_target *t, *next;

	damon_for_each_target_safe(t, next, ctx) {
		put_pid((struct pid *)t->id);
		damon_destroy_target(t);
	}
}

#ifndef CONFIG_ADVISE_SYSCALLS
static int damos_madvise(struct damon_target *target, struct damon_region *r,
			int behavior)
{
	return -EINVAL;
}
#else
static int damos_madvise(struct damon_target *target, struct damon_region *r,
			int behavior)
{
	struct mm_struct *mm;
	int ret = -ENOMEM;

	mm = damon_get_mm(target);
	if (!mm)
		goto out;

	ret = do_madvise(mm, PAGE_ALIGN(r->ar.start),
			PAGE_ALIGN(r->ar.end - r->ar.start), behavior);
	mmput(mm);
out:
	return ret;
}
#endif	/* CONFIG_ADVISE_SYSCALLS */

int damon_va_apply_scheme(struct damon_ctx *ctx, struct damon_target *t,
		struct damon_region *r, struct damos *scheme)
{
	int madv_action;

	switch (scheme->action) {
	case DAMOS_WILLNEED:
		madv_action = MADV_WILLNEED;
		break;
	case DAMOS_COLD:
		madv_action = MADV_COLD;
		break;
	case DAMOS_PAGEOUT:
		madv_action = MADV_PAGEOUT;
		break;
	case DAMOS_HUGEPAGE:
		madv_action = MADV_HUGEPAGE;
		break;
	case DAMOS_NOHUGEPAGE:
		madv_action = MADV_NOHUGEPAGE;
		break;
	case DAMOS_STAT:
		return 0;
	default:
		pr_warn("Wrong action %d\n", scheme->action);
		return -EINVAL;
	}

	return damos_madvise(t, r, madv_action);
}

void damon_va_set_primitives(struct damon_ctx *ctx)
{
	ctx->primitive.init = damon_va_init;
	ctx->primitive.update = damon_va_update;
	ctx->primitive.prepare_access_checks = damon_va_prepare_access_checks;
	ctx->primitive.check_accesses = damon_va_check_accesses;
	ctx->primitive.reset_aggregated = NULL;
	ctx->primitive.target_valid = damon_va_target_valid;
	ctx->primitive.cleanup = damon_va_cleanup;
	ctx->primitive.apply_scheme = damon_va_apply_scheme;
}

#include "vaddr-test.h"
