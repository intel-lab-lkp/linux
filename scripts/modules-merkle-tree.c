// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Compute hashes for modules files and build a merkle tree.
 *
 * Copyright (C) 2025 Sebastian Andrzej Siewior <sebastian@breakpoint.cc>
 * Copyright (C) 2025 Thomas Weißschuh <linux@weissschuh.net>
 *
 */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <sys/mman.h>

#include <openssl/evp.h>
#include <openssl/err.h>

#include "ssl-common.h"

static int hash_size;
static EVP_MD_CTX *ctx;

struct module_signature {
	uint8_t		algo;		/* Public-key crypto algorithm [0] */
	uint8_t		hash;		/* Digest algorithm [0] */
	uint8_t		id_type;	/* Key identifier type [PKEY_ID_PKCS7] */
	uint8_t		signer_len;	/* Length of signer's name [0] */
	uint8_t		key_id_len;	/* Length of key identifier [0] */
	uint8_t		__pad[3];
	uint32_t	sig_len;	/* Length of signature data */
};

#define PKEY_ID_MERKLE 3

static const char magic_number[] = "~Module signature appended~\n";

struct file_entry {
	char *name;
	unsigned int pos;
	unsigned char hash[EVP_MAX_MD_SIZE];
};

static struct file_entry *fh_list;
static size_t num_files;

struct leaf_hash {
	unsigned char hash[EVP_MAX_MD_SIZE];
};

struct mtree {
	struct leaf_hash **l;
	unsigned int *entries;
	unsigned int levels;
};

static inline void *xcalloc(size_t n, size_t size)
{
	void *p;

	p = calloc(n, size);
	if (!p)
		errx(1, "Memory allocation failed");

	return p;
}

static void *xmalloc(size_t size)
{
	void *p;

	p = malloc(size);
	if (!p)
		errx(1, "Memory allocation failed");

	return p;
}

static inline void *xreallocarray(void *oldp, size_t n, size_t size)
{
	void *p;

	p = reallocarray(oldp, n, size);
	if (!p)
		errx(1, "Memory allocation failed");

	return p;
}

static inline char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	char *strp;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&strp, fmt, ap);
	va_end(ap);
	if (ret == -1)
		err(1, "Memory allocation failed");

	return strp;
}

static unsigned int get_pow2(unsigned int val)
{
	return 31 - __builtin_clz(val);
}

static unsigned int roundup_pow2(unsigned int val)
{
	return 1 << (get_pow2(val - 1) + 1);
}

static unsigned int log2_roundup(unsigned int val)
{
	return get_pow2(roundup_pow2(val));
}

static void hash_data(void *p, unsigned int pos, size_t size, void *ret_hash)
{
	unsigned char magic = 0x01;
	unsigned int pos_be;

	pos_be = htonl(pos);

	ERR(EVP_DigestInit_ex(ctx, NULL, NULL) != 1, "EVP_DigestInit_ex()");
	ERR(EVP_DigestUpdate(ctx, &magic, sizeof(magic)) != 1, "EVP_DigestUpdate(magic)");
	ERR(EVP_DigestUpdate(ctx, &pos_be, sizeof(pos_be)) != 1, "EVP_DigestUpdate(pos)");
	ERR(EVP_DigestUpdate(ctx, p, size) != 1, "EVP_DigestUpdate(data)");
	ERR(EVP_DigestFinal_ex(ctx, ret_hash, NULL) != 1, "EVP_DigestFinal_ex()");
}

static void hash_entry(void *left, void *right, void *ret_hash)
{
	int hash_size = EVP_MD_CTX_get_size_ex(ctx);
	unsigned char magic = 0x02;

	ERR(EVP_DigestInit_ex(ctx, NULL, NULL) != 1, "EVP_DigestInit_ex()");
	ERR(EVP_DigestUpdate(ctx, &magic, sizeof(magic)) != 1, "EVP_DigestUpdate(magic)");
	ERR(EVP_DigestUpdate(ctx, left, hash_size) != 1, "EVP_DigestUpdate(left)");
	ERR(EVP_DigestUpdate(ctx, right, hash_size) != 1, "EVP_DigestUpdate(right)");
	ERR(EVP_DigestFinal_ex(ctx, ret_hash, NULL) != 1, "EVP_DigestFinal_ex()");
}

static void hash_file(struct file_entry *fe)
{
	struct stat sb;
	int fd, ret;
	void *mem;

	fd = open(fe->name, O_RDONLY);
	if (fd < 0)
		err(1, "Failed to open %s", fe->name);

	ret = fstat(fd, &sb);
	if (ret)
		err(1, "Failed to stat %s", fe->name);

	mem = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);

	if (mem == MAP_FAILED)
		err(1, "Failed to mmap %s", fe->name);

	hash_data(mem, fe->pos, sb.st_size, fe->hash);

	munmap(mem, sb.st_size);
}

static struct mtree *build_merkle(struct file_entry *fh, size_t num)
{
	struct mtree *mt;
	unsigned int le;

	if (!num)
		return NULL;

	mt = xmalloc(sizeof(*mt));
	mt->levels = log2_roundup(num);

	mt->l = xcalloc(sizeof(*mt->l), mt->levels);

	mt->entries = xcalloc(sizeof(*mt->entries), mt->levels);
	le = num / 2;
	if (num & 1)
		le++;
	mt->entries[0] = le;
	mt->l[0] = xcalloc(sizeof(**mt->l), le);

	/* First level of pairs */
	for (unsigned int i = 0; i < num; i += 2) {
		if (i == num - 1) {
			/* Odd number of files, no pair. Hash with itself */
			hash_entry(fh[i].hash, fh[i].hash, mt->l[0][i / 2].hash);
		} else {
			hash_entry(fh[i].hash, fh[i + 1].hash, mt->l[0][i / 2].hash);
		}
	}
	for (unsigned int i = 1; i < mt->levels; i++) {
		int odd = 0;

		if (le & 1) {
			le++;
			odd++;
		}

		mt->entries[i] = le / 2;
		mt->l[i] = xcalloc(sizeof(**mt->l), le);

		for (unsigned int n = 0; n < le; n += 2) {
			if (n == le - 2 && odd) {
				/* Odd number of pairs, no pair. Hash with itself */
				hash_entry(mt->l[i - 1][n].hash, mt->l[i - 1][n].hash,
					   mt->l[i][n / 2].hash);
			} else {
				hash_entry(mt->l[i - 1][n].hash, mt->l[i - 1][n + 1].hash,
					   mt->l[i][n / 2].hash);
			}
		}
		le =  mt->entries[i];
	}
	return mt;
}

static void free_mtree(struct mtree *mt)
{
	if (!mt)
		return;

	for (unsigned int i = 0; i < mt->levels; i++)
		free(mt->l[i]);

	free(mt->l);
	free(mt->entries);
	free(mt);
}

static void write_be_int(int fd, unsigned int v)
{
	unsigned int be_val = htonl(v);

	if (write(fd, &be_val, sizeof(be_val)) != sizeof(be_val))
		err(1, "Failed writing to file");
}

static void write_hash(int fd, const void *h)
{
	ssize_t wr;

	wr = write(fd, h, hash_size);
	if (wr != hash_size)
		err(1, "Failed writing to file");
}

static void build_proof(struct mtree *mt, unsigned int n, int fd)
{
	unsigned char cur[EVP_MAX_MD_SIZE];
	unsigned char tmp[EVP_MAX_MD_SIZE];
	struct file_entry *fe, *fe_sib;

	fe = &fh_list[n];

	if ((n & 1) == 0) {
		/* No pair, hash with itself */
		if (n + 1 == num_files)
			fe_sib = fe;
		else
			fe_sib = &fh_list[n + 1];
	} else {
		fe_sib = &fh_list[n - 1];
	}
	/* First comes the node position into the file */
	write_be_int(fd, n);

	if ((n & 1) == 0)
		hash_entry(fe->hash, fe_sib->hash, cur);
	else
		hash_entry(fe_sib->hash, fe->hash, cur);

	/* Next is the sibling hash, followed by hashes in the tree */
	write_hash(fd, fe_sib->hash);

	for (unsigned int i = 0; i < mt->levels - 1; i++) {
		n >>= 1;
		if ((n & 1) == 0) {
			void *h;

			/* No pair, hash with itself */
			if (n + 1 == mt->entries[i])
				h = cur;
			else
				h = mt->l[i][n + 1].hash;

			hash_entry(cur, h, tmp);
			write_hash(fd, h);
		} else {
			hash_entry(mt->l[i][n - 1].hash, cur, tmp);
			write_hash(fd, mt->l[i][n - 1].hash);
		}
		memcpy(cur, tmp, hash_size);
	}

	 /* After all that, the end hash should match the root hash */
	if (memcmp(cur, mt->l[mt->levels - 1][0].hash, hash_size))
		errx(1, "hash mismatch");
}

static void append_module_signature_magic(int fd, unsigned int sig_len)
{
	struct module_signature sig_info = {
		.id_type	= PKEY_ID_MERKLE,
		.sig_len	= htonl(sig_len),
	};

	if (write(fd, &sig_info, sizeof(sig_info)) < 0)
		err(1, "write(sig_info) failed");

	if (write(fd, &magic_number, sizeof(magic_number) - 1) < 0)
		err(1, "write(magic_number) failed");
}

static void write_merkle_root(struct mtree *mt, const char *fp)
{
	char buf[1024];
	unsigned int levels;
	unsigned char *h;
	FILE *f;

	if (mt) {
		levels = mt->levels;
		h = mt->l[mt->levels - 1][0].hash;
	} else {
		levels = 0;
		h = xcalloc(1, hash_size);
	}

	f = fopen(fp, "w");
	if (!f)
		err(1, "Failed to create %s", buf);

	fprintf(f, "#include <linux/module_hashes.h>\n\n");
	fprintf(f, "const struct module_hashes_root module_hashes_root __module_hashes_section = {\n");

	fprintf(f, "\t.levels = %u,\n", levels);
	fprintf(f, "\t.hash = {");
	for (unsigned int i = 0; i < hash_size; i++) {
		char *space = "";

		if (!(i % 8))
			fprintf(f, "\n\t\t");

		if ((i + 1) % 8)
			space = " ";

		fprintf(f, "0x%02x,%s", h[i], space);
	}
	fprintf(f, "\n\t},");

	fprintf(f, "\n};\n");
	fclose(f);

	if (!mt)
		free(h);
}

static char *xstrdup_replace_suffix(const char *str, const char *new_suffix)
{
	const char *current_suffix;
	size_t base_len;

	current_suffix = strchr(str, '.');
	if (!current_suffix)
		errx(1, "No existing suffix in '%s'", str);

	base_len = current_suffix - str;

	return xasprintf("%.*s%s", (int)base_len, str, new_suffix);
}

static void read_modules_order(const char *fname, const char *suffix)
{
	char line[PATH_MAX];
	FILE *in;

	in = fopen(fname, "r");
	if (!in)
		err(1, "fopen(%s)", fname);

	while (fgets(line, PATH_MAX, in)) {
		struct file_entry *entry;

		fh_list = xreallocarray(fh_list, num_files + 1, sizeof(*fh_list));
		entry = &fh_list[num_files];

		entry->pos = num_files;
		entry->name = xstrdup_replace_suffix(line, suffix);
		hash_file(entry);

		num_files++;
	}

	fclose(in);
}

static __attribute__((noreturn))
void format(void)
{
	fprintf(stderr,
		"Usage: scripts/modules-merkle-tree <root definition>\n");
	exit(2);
}

int main(int argc, char *argv[])
{
	const EVP_MD *hash_evp;
	struct mtree *mt;

	if (argc != 3)
		format();

	hash_evp = EVP_get_digestbyname("sha256");
	ERR(!hash_evp, "EVP_get_digestbyname");

	ctx = EVP_MD_CTX_new();
	ERR(!ctx, "EVP_MD_CTX_new()");

	hash_size = EVP_MD_get_size(hash_evp);
	ERR(hash_size <= 0, "EVP_get_digestbyname");

	if (EVP_DigestInit_ex(ctx, hash_evp, NULL) != 1)
		ERR(1, "EVP_DigestInit_ex()");

	read_modules_order("modules.order", argv[2]);

	mt = build_merkle(fh_list, num_files);
	write_merkle_root(mt, argv[1]);
	for (unsigned int i = 0; i < num_files; i++) {
		char *signame;
		int fd;

		signame = xstrdup_replace_suffix(fh_list[i].name, ".merkle");

		fd = open(signame, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			err(1, "Can't create %s", signame);

		build_proof(mt, i, fd);
		append_module_signature_magic(fd, lseek(fd, 0, SEEK_CUR));
		close(fd);
	}

	free_mtree(mt);
	for (unsigned int i = 0; i < num_files; i++)
		free(fh_list[i].name);
	free(fh_list);

	EVP_MD_CTX_free(ctx);
	return 0;
}
