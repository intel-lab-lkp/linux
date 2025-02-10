// SPDX-License-Identifier: GPL-2.0

#include <linux/slab.h>
#include <linux/memory.h>
#include <linux/func_meta.h>
#include <linux/text-patching.h>

#define FUNC_META_INSN_SIZE	5

#ifdef CONFIG_CFI_CLANG
  #ifdef CONFIG_CALL_THUNKS
    /* use the extra 5-bytes that we reserve */
    #define FUNC_META_INSN_OFFSET	(CONFIG_FUNCTION_PADDING_BYTES + 5)
    #define FUNC_META_DATA_OFFSET	(CONFIG_FUNCTION_PADDING_BYTES + 4)
  #else
    /* use the space that CALL_THUNKS suppose to use */
    #define FUNC_META_INSN_OFFSET	(5)
    #define FUNC_META_DATA_OFFSET	(4)
  #endif
#else
  /* use the space that CFI_CLANG support to use */
  #define FUNC_META_INSN_OFFSET	(CONFIG_FUNCTION_PADDING_BYTES)
  #define FUNC_META_DATA_OFFSET	(CONFIG_FUNCTION_PADDING_BYTES - 1)
#endif

static u32 func_meta_count = 32, func_meta_used;
struct func_meta *func_metas;
EXPORT_SYMBOL_GPL(func_metas);

static DEFINE_MUTEX(func_meta_lock);

static u32 func_meta_get_index(void *ip)
{
	return *(u32 *)(ip - FUNC_META_DATA_OFFSET);
}

static bool func_meta_exist(void *ip)
{
	return *(u8 *)(ip - FUNC_META_INSN_OFFSET) == 0xB8;
}

static void func_meta_init(struct func_meta *metas, u32 start, u32 end)
{
	u32 i;

	for (i = start; i < end; i++)
		metas[i].users = 0;
}

/* get next usable function metadata */
static struct func_meta *func_meta_get_next(u32 *index)
{
	struct func_meta *tmp;
	u32 i;

	if (func_metas == NULL) {
		func_metas = kmalloc_array(func_meta_count, sizeof(*tmp),
					   GFP_KERNEL);
		if (!func_metas)
			return NULL;
		func_meta_init(func_metas, 0, func_meta_count);
	}

	/* maybe we can manage the used function metadata entry with a bit
	 * map ?
	 */
	for (i = 0; i < func_meta_count; i++) {
		if (!func_metas[i].users) {
			func_meta_used++;
			*index = i;
			func_metas[i].users++;
			return func_metas + i;
		}
	}

	tmp = kmalloc_array(func_meta_count * 2, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return NULL;

	memcpy(tmp, func_metas, func_meta_count * sizeof(*tmp));
	kfree(func_metas);

	/* TODO: we need a way to update func_metas synchronously, RCU ? */
	func_metas = tmp;
	func_meta_init(func_metas, func_meta_count, func_meta_count * 2);

	tmp += func_meta_count;
	*index = func_meta_count;
	func_meta_count <<= 1;
	func_meta_used++;
	tmp->users++;

	return tmp;
}

static int func_meta_text_poke(void *ip, u32 index, bool nop)
{
	const u8 nop_insn[FUNC_META_INSN_SIZE] = { BYTES_NOP1, BYTES_NOP1,
						   BYTES_NOP1, BYTES_NOP1,
						   BYTES_NOP1 };
	u8 insn[FUNC_META_INSN_SIZE] = { 0xB8, };
	const u8 *prog;
	void *target;
	int ret = 0;

	target = ip - FUNC_META_INSN_OFFSET;
	mutex_lock(&text_mutex);
	if (nop) {
		if (!memcmp(target, nop_insn, FUNC_META_INSN_SIZE))
			goto out;
		prog = nop_insn;
	} else {
		*(u32 *)(insn + 1) = index;
		if (!memcmp(target, insn, FUNC_META_INSN_SIZE))
			goto out;

		if (memcmp(target, nop_insn, FUNC_META_INSN_SIZE)) {
			ret = -EBUSY;
			goto out;
		}
		prog = insn;
	}
	text_poke(target, prog, FUNC_META_INSN_SIZE);
	text_poke_sync();
out:
	mutex_unlock(&text_mutex);
	return ret;
}

static void __func_meta_put(void *ip, struct func_meta *meta)
{
	if (WARN_ON_ONCE(meta->users <= 0))
		return;

	meta->users--;
	if (meta->users > 0)
		return;

	/* TODO: we need a way to shrink the array "func_metas" */
	func_meta_used--;
	if (!func_meta_exist(ip))
		return;

	func_meta_text_poke(ip, 0, true);
}

void func_meta_put(void *ip, struct func_meta *meta)
{
	mutex_lock(&func_meta_lock);
	__func_meta_put(ip, meta);
	mutex_unlock(&func_meta_lock);
}
EXPORT_SYMBOL_GPL(func_meta_put);

struct func_meta *func_meta_get(void *ip)
{
	struct func_meta *tmp = NULL;
	u32 index;

	mutex_lock(&func_meta_lock);
	if (func_meta_exist(ip)) {
		index = func_meta_get_index(ip);
		if (WARN_ON_ONCE(index >= func_meta_count))
			goto out;

		tmp = &func_metas[index];
		tmp->users++;
		goto out;
	}

	tmp = func_meta_get_next(&index);
	if (!tmp)
		goto out;

	if (func_meta_text_poke(ip, index, false)) {
		__func_meta_put(ip, tmp);
		goto out;
	}
	tmp->func = ip;
out:
	mutex_unlock(&func_meta_lock);
	return tmp;
}
EXPORT_SYMBOL_GPL(func_meta_get);
