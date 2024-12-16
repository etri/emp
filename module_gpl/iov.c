#include <linux/uio.h>
#include "config.h"
#include "gpa.h"
#include "vm.h"
#include "hva.h"
#include "block-flag.h"

/**
 * mark_io_blocks - Mark contiguous blocks as io_write
 * @param bvma bvma data structure
 * @param gpas gpas for blocks
 * @param len the length of the blocks
 * @param write is it I/O write operation?
 * @param heads heads of blocks
 * @param heads_len the number of heads
 *
 * @return the number of heads for I/O
 */
static int COMPILER_DEBUG
mark_io_blocks(struct emp_mm *emm, struct emp_vmr *vmr,
		unsigned long start_idx, unsigned long len,
		bool write, unsigned long *heads, int heads_len)
{
#if defined(GPA_IO_READ_MASK) && defined(GPA_IO_WRITE_MASK)
	unsigned long gpa_idx;
	unsigned long head_idx;
	unsigned long end_idx = start_idx + len;
	struct emp_gpa *gpa, *head;
	int io_heads_count;

	// if gpa == NULL, it is an initial subblock and does not incur remote
	// fetch. Do not mark io blocks for them.
	if (!write) {
		while ((gpa = raw_get_gpadesc(vmr, start_idx)) == NULL) {
			// TODO: use MAX_ORDER for each region
			start_idx++;
			if (unlikely(start_idx >= end_idx))
				return 0;
		}

		head_idx = emp_get_block_head_index(vmr, start_idx);
		debug_BUG_ON(head_idx == WRONG_GPA_IDX); // we had gpa
		if (start_idx != head_idx)
			start_idx = head_idx + num_subblock_in_block(gpa);

		while ((gpa = raw_get_gpadesc(vmr, end_idx)) == NULL) {
			// TODO: use MAX_ORDER for each region
			end_idx--;
			if (unlikely(start_idx >= end_idx))
				return 0;
		}

		head_idx = emp_get_block_head_index(vmr, end_idx);
		debug_BUG_ON(head_idx == WRONG_GPA_IDX); // we had gpa
		if (end_idx != head_idx)
			end_idx = head_idx;
	}

	io_heads_count = 0;
	raw_for_all_gpa_heads_range(vmr, gpa_idx, gpa, start_idx, end_idx) {
		if (!is_gpa_flags_set(gpa, GPA_REMOTE_MASK))
			continue;

		/* Don't use __emp_trylock_block(). head can be changed during runtime. */
		if ((head = emp_trylock_block(vmr, &gpa, gpa_idx)) == NULL)
			continue;

		if ((head->r_state == GPA_INIT) &&
				is_gpa_flags_set(head, GPA_REMOTE_MASK)) {
			set_gpa_flags_if_unset(head,
					write ? GPA_IO_WRITE_MASK
					      : GPA_IO_READ_MASK);
			debug_BUG_ON(gpa_idx !=
					emp_get_block_head_index(vmr, gpa_idx));
			heads[io_heads_count++] = gpa_idx;
		}
		emp_unlock_block(head);

		if (io_heads_count >= heads_len)
			break;
	}

	return io_heads_count;
#else
	return 0;
#endif
}

/**
 * map_io_blocks - Map the pages in I/O blocks in host page table
 * @param emm emm data structure
 * @param heads heads of blocks
 * @param heads_len the number of heads
 *
 * @return 0 without error, negative value on error.
 */
static int
map_io_blocks(struct emp_vmr *vmr, unsigned long *head_idx_arr, int head_len)
{
	struct emp_gpa *h, *g;
	unsigned long g_idx;
	struct emp_gpa *fs, *fe; // fetch_start, fetch_end
	struct emp_mm *emm = vmr->emm;
	int i;

	for (i = 0; i < head_len; i++) {
		g_idx = head_idx_arr[i];
		{
			struct vm_fault vmf = {
				.pgoff = g_idx << bvma_subblock_order(emm),
				.address = GPN_TO_HVA(emm, vmr, vmf.pgoff)
			};
			vmf.flags = FAULT_FLAG_WRITE;

			g = get_gpadesc(vmr, g_idx);
			if (unlikely(!g))
				return -ENOMEM;
			h = emp_lock_block(vmr, &g, g_idx);
			debug_BUG_ON(!h); // we already have @g
			fs = h;
			fe = h + num_subblock_in_block(h);

			if (is_gpa_flags_set(h, GPA_IO_IP_MASK)) {
				printk("%s vmf->address: %llx vmf->pgoff: %lx "
						"vmf->flags: %x\n", 
						__func__,
						(u64)vmf.address,
						vmf.pgoff, vmf.flags);
				clear_gpa_flags_if_set(h, GPA_IO_IP_MASK);
				emp_unlock_subblock(h);

				emp_page_fault_hptes_map(emm, vmr, h, g, g_idx,
						fs, fe, true, &vmf, false);
			}
			emp_unlock_block(h);
		}
	}

	return 0;
}

/**
 * fetch_io_blocks - Fetch pages in I/O blocks
 * @param io_base base address of IOvector
 *
 * @return the number of pages for I/O
 */
static long fetch_io_blocks(unsigned long io_base)
{
	long nr_pages_pinned;
	struct page *page;

	nr_pages_pinned = get_user_pages(io_base, 1, FOLL_WRITE, &page, NULL);
	if (nr_pages_pinned) {
		put_page(page);
		debug_page_ref_mark_page(-100, page, -1);
	}

	return nr_pages_pinned;
}

#define GUP_MAX 92

/**
 * handle_qiov_write - Handle WRITE I/O request from QEMU
 * @param emm emm data structure
 * @param iov iovector
 * @param iov_len the length of iov
 */
void handle_qiov_write(struct emp_mm *emm, struct iovec *iov, int iov_len)
{
	struct emp_vmr *vmr;
	unsigned int sb_order;
	int i, j;
	int count, io_head_count;
	int io_head_arr_len = GUP_MAX;
	u64 io_base;
	size_t io_len;
	unsigned long io_gpa_idx;
	unsigned long *io_head;
	unsigned long io_head_arr[GUP_MAX];

	sb_order = bvma_subblock_order(emm);

	io_head_count = 0;
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0))
	down_read(&current->mm->mmap_sem);
#else
	down_read(&current->mm->mmap_lock);
#endif

	for (i = 0; i < iov_len; i++) {
		io_len = iov[i].iov_len >> (sb_order + PAGE_SHIFT);
		if (io_len == 0)
			continue;

		io_base = (u64)iov[i].iov_base;
		vmr = emp_vmr_lookup_hva(emm, io_base);
		if (vmr == NULL)
			continue;

		io_gpa_idx = HVA_TO_GPN(emm, vmr, io_base) >> sb_order;
		count = mark_io_blocks(emm, vmr, io_gpa_idx, io_len, true,
					io_head_arr + io_head_count,
					io_head_arr_len - io_head_count);
		if (unlikely(count < 0))
			goto out;

		if (count == 0)
			continue;

		io_head = io_head_arr + io_head_count;
		for (j = 0; j < count; j++) {
			io_base = GPN_TO_HVA(emm, vmr, (io_head[j])<<sb_order);
			fetch_io_blocks(io_base);
		}

		io_head_count += count;

		if (map_io_blocks(vmr, io_head, count) < 0)
			goto out;

		if (io_head_count == GUP_MAX)
			break;
	}

out:
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0))
	up_read(&current->mm->mmap_sem);
#else
	up_read(&current->mm->mmap_lock);
#endif

}
 
/**
 * handle_qiov_read - Handle READ I/O request from QEMU
 * @param emm emm data structure
 * @param iov iovector
 * @param iov_len the length of iov
 */
void handle_qiov_read(struct emp_mm *emm, struct iovec *iov, int iov_len)
{
	struct emp_vmr *vmr;
	pid_t pid;
	unsigned int sb_order;
	int i, j;
	int count, io_head_count;
	int io_head_arr_len = GUP_MAX;
	u64 io_base;
	size_t io_len;
	struct emp_gpa *io_gpa;
	unsigned long io_gpa_idx;
	unsigned long *io_head;
	unsigned long io_head_arr[GUP_MAX];

	sb_order = bvma_subblock_order(emm);

	io_head_count = 0;
	pid = current->pid;
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0))
	down_read(&current->mm->mmap_sem);
#else
	down_read(&current->mm->mmap_lock);
#endif

	for (i = 0; i < iov_len; i++) {
		io_len = iov[i].iov_len >> (sb_order + PAGE_SHIFT);
		if (io_len == 0)
			continue;

		io_base = (u64)iov[i].iov_base;
		vmr = emp_vmr_lookup_hva(emm, io_base);
		if (vmr == NULL)
			continue;

		io_gpa_idx = HVA_TO_GPN(emm, vmr, io_base) >> sb_order;
		count = mark_io_blocks(emm, vmr, io_gpa_idx, io_len, false,
					io_head_arr + io_head_count,
					io_head_arr_len - io_head_count);
		if (count == 0)
			continue;

		io_head = io_head_arr + io_head_count;
		for (j = 0; j < count; j++) {
			io_base = GPN_TO_HVA(emm, vmr, io_head[j] << sb_order);
			fetch_io_blocks(io_base);
			io_gpa = get_gpadesc(vmr, io_head[j]);
			if (unlikely(!io_gpa))
				goto out;
			io_gpa->pid = pid;
		}

		io_head_count += count;

		if (io_head_count == GUP_MAX)
			break;
	}

out:
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0))
	up_read(&current->mm->mmap_sem);
#else
	up_read(&current->mm->mmap_lock);
#endif
}

/**
 * handle_qiov - Handle I/O request from QEMU
 * @param bvma bvma data structure
 * @param is_write indicate write operation
 * @param iov iovector
 * @param iov_len the length of iov
 *
 * This function calls when the QEMU request I/O operation. \n
 * It hooks the requests and checks the page is currently used for I/O
 */
void handle_qiov(struct emp_mm *bvma, bool is_write, struct iovec *iov,
			int iov_len)
{
	return is_write?
		handle_qiov_write(bvma, iov, iov_len):
		handle_qiov_read(bvma, iov, iov_len);
}
