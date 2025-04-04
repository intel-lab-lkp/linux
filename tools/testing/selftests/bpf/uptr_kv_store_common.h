#ifndef _UPTR_KV_STORE_COMMON_H
#define _UPTR_KV_STORE_COMMON_H

#define PAGE_SIZE		4096
#define KVS_MAX_VAL_SIZE	PAGE_SIZE
#define KVS_MAX_VAL_ENTRIES	1024

#ifdef __BPF__
struct kv_store_page *dummy_page;
struct kv_store_metas *dummy_metas;
#else
#define __uptr
#define __kptr
#endif

struct kv_store_meta {
	__u32 page_off:12;
	__u32 size:12;
	__u32 init:1;
};

struct kv_store_metas {
	struct kv_store_meta meta[KVS_MAX_VAL_ENTRIES];
};

struct kv_store_data_map_value {
	struct kv_store_metas __uptr *metas;
	struct kv_store_page __uptr *page;
	u16 metas_cnt;
	u16 page_size;
};

struct kv_store_page {
	char data[KVS_MAX_VAL_SIZE];
};

#endif
