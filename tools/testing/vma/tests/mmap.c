// SPDX-License-Identifier: GPL-2.0-or-later

static bool test_mmap_region_basic(void)
{
	const vma_flags_t vma_flags = mk_vma_flags(VMA_READ_BIT, VMA_WRITE_BIT,
			VMA_MAYREAD_BIT, VMA_MAYWRITE_BIT);
	struct mm_struct mm = {};
	unsigned long addr;
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, &mm, 0);

	current->mm = &mm;

	/* Map at 0x300000, length 0x3000. */
	addr = __mmap_region(NULL, 0x300000, 0x3000, vma_flags, 0x300, NULL);
	ASSERT_EQ(addr, 0x300000);

	/* Map at 0x250000, length 0x3000. */
	addr = __mmap_region(NULL, 0x250000, 0x3000, vma_flags, 0x250, NULL);
	ASSERT_EQ(addr, 0x250000);

	/* Map at 0x303000, merging to 0x300000 of length 0x6000. */
	addr = __mmap_region(NULL, 0x303000, 0x3000, vma_flags, 0x303, NULL);
	ASSERT_EQ(addr, 0x303000);

	/* Map at 0x24d000, merging to 0x250000 of length 0x6000. */
	addr = __mmap_region(NULL, 0x24d000, 0x3000, vma_flags, 0x24d, NULL);
	ASSERT_EQ(addr, 0x24d000);

	ASSERT_EQ(mm.map_count, 2);

	for_each_vma(vmi, vma) {
		if (vma->vm_start == 0x300000) {
			ASSERT_EQ(vma->vm_end, 0x306000);
			ASSERT_EQ(vma->vm_pgoff, 0x300);
		} else if (vma->vm_start == 0x24d000) {
			ASSERT_EQ(vma->vm_end, 0x253000);
			ASSERT_EQ(vma->vm_pgoff, 0x24d);
		} else {
			ASSERT_FALSE(true);
		}
	}

	cleanup_mm(&mm, &vmi);
	return true;
}

static bool mmap_region_fill_hole(bool merge)
{
	const vma_flags_t vma_flags = mk_vma_flags(VMA_READ_BIT, VMA_WRITE_BIT,
			VMA_MAYREAD_BIT, VMA_MAYWRITE_BIT, VMA_MAYEXEC_BIT);
	vma_flags_t hole_flags = vma_flags;
	struct mm_struct mm = {};
	struct vm_area_struct *vma;
	unsigned long addr;
	int count = 0;
	VMA_ITERATOR(vmi, &mm, 0);

	current->mm = &mm;
	if (!merge)
		vma_flags_set(&hole_flags, VMA_EXEC_BIT);

	/* Leave a hole between two otherwise mergeable mappings. */
	addr = __mmap_region(NULL, 0x300000, 0x3000, vma_flags, 0x300, NULL);
	ASSERT_EQ(addr, 0x300000);
	addr = __mmap_region(NULL, 0x306000, 0x3000, vma_flags, 0x306, NULL);
	ASSERT_EQ(addr, 0x306000);
	ASSERT_EQ(mm.map_count, 2);
	vma_iter_set(&vmi, 0x303000);
	ASSERT_EQ(vma_iter_load(&vmi), NULL);
	vma_iter_set(&vmi, 0x305fff);
	ASSERT_EQ(vma_iter_load(&vmi), NULL);

	/* A single flag difference must prevent merging with either neighbor. */
	addr = __mmap_region(NULL, 0x303000, 0x3000, hole_flags, 0x303, NULL);
	ASSERT_EQ(addr, 0x303000);
	ASSERT_EQ(mm.map_count, merge ? 1 : 3);

	vma_iter_set(&vmi, 0);
	for_each_vma(vmi, vma) {
		unsigned long start = 0x300000 + count * 0x3000;
		unsigned long end = merge ? 0x309000 : start + 0x3000;
		VMA_ITERATOR(lookup, &mm, start);

		ASSERT_EQ(vma->vm_start, start);
		ASSERT_EQ(vma->vm_end, end);
		ASSERT_EQ(vma_start_pgoff(vma), start >> PAGE_SHIFT);
		ASSERT_EQ(vma_start_anon_pgoff(vma), start >> PAGE_SHIFT);
		ASSERT_TRUE(vma_test_all(vma, VMA_READ_BIT, VMA_WRITE_BIT,
					VMA_MAYREAD_BIT, VMA_MAYWRITE_BIT,
					VMA_MAYEXEC_BIT));
		ASSERT_EQ(vma_test(vma, VMA_EXEC_BIT), !merge && count == 1);
		for (addr = start; addr < end; addr += PAGE_SIZE) {
			vma_iter_set(&lookup, addr);
			ASSERT_EQ(vma_iter_load(&lookup), vma);
			vma_iter_set(&lookup, addr + PAGE_SIZE - 1);
			ASSERT_EQ(vma_iter_load(&lookup), vma);
		}
		count++;
	}
	ASSERT_EQ(count, mm.map_count);
	vma_iter_set(&vmi, 0x2fffff);
	ASSERT_EQ(vma_iter_load(&vmi), NULL);
	vma_iter_set(&vmi, 0x309000);
	ASSERT_EQ(vma_iter_load(&vmi), NULL);

	ASSERT_EQ(cleanup_mm(&mm, &vmi), count);
	return true;
}

static bool test_mmap_region_fill_hole_merge(void)
{
	return mmap_region_fill_hole(true);
}

static bool test_mmap_region_fill_hole_flags_mismatch(void)
{
	return mmap_region_fill_hole(false);
}

static void run_mmap_tests(int *num_tests, int *num_fail)
{
	TEST(mmap_region_basic);
	TEST(mmap_region_fill_hole_merge);
	TEST(mmap_region_fill_hole_flags_mismatch);
}
