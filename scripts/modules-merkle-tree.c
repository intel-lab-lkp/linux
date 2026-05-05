// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Compute hashes for modules files and build a merkle tree.
 *
 * Copyright (C) 2025 Sebastian Andrzej Siewior <sebastian@breakpoint.cc>
 * Copyright (C) 2025 Thomas Weißschuh <linux@weissschuh.net>
 *
 * Structure of the Merkle tree:
 *
 * The full built modules are leaf nodes. They are hashed pairwise in the order
 * of modules.order to create internal nodes. These in turn are also hashed
 * pairwise to create the next higher level of internal nodes. This is repeated
 * up to a single root node. In case of an uneven amount of node on a level, the
 * last node is paired with itself.
 *
 * The single root node can then be embedded into vmlinux to validate all modules.
 */

#define _GNU_SOURCE 1
#include <endian.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
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

#include <linux/module_signature.h>
#include <xalloc.h>

static int hash_size;
static EVP_MD_CTX *ctx;

struct hash {
	uint8_t h[32]; /* For sha256 */
};

struct file_entry {
	char *name;
	unsigned int pos;
	struct hash hash;
};

static struct file_entry *fh_list;
static size_t num_files;

struct mtree {
	struct hash **level_hashes;
	unsigned int *entries;
	unsigned int num_levels;
};

static unsigned int log2_roundup(uint32_t val)
{
	if (val <= 1)
		return 1;
	return 32 - __builtin_clz(val - 1);
}

static void hash_data(unsigned int pos, unsigned char *data, size_t size, struct hash *ret_hash)
{
	uint8_t magic = 0x01; /* domain separation prefix */
	uint32_t pos_be;

	pos_be = htobe32(pos);

	ERR(EVP_DigestInit_ex(ctx, NULL, NULL) != 1, "EVP_DigestInit_ex()");
	ERR(EVP_DigestUpdate(ctx, &magic, sizeof(magic)) != 1, "EVP_DigestUpdate(magic)");
	ERR(EVP_DigestUpdate(ctx, &pos_be, sizeof(pos_be)) != 1, "EVP_DigestUpdate(pos)");
	ERR(EVP_DigestUpdate(ctx, data, size) != 1, "EVP_DigestUpdate(data)");
	ERR(EVP_DigestFinal_ex(ctx, ret_hash->h, NULL) != 1, "EVP_DigestFinal_ex()");
}

static void hash_entry(const struct hash *left, const struct hash *right, struct hash *ret_hash)
{
	uint8_t magic = 0x02; /* domain separation prefix */

	ERR(EVP_DigestInit_ex(ctx, NULL, NULL) != 1, "EVP_DigestInit_ex()");
	ERR(EVP_DigestUpdate(ctx, &magic, sizeof(magic)) != 1, "EVP_DigestUpdate(magic)");
	ERR(EVP_DigestUpdate(ctx, left, hash_size) != 1, "EVP_DigestUpdate(left)");
	ERR(EVP_DigestUpdate(ctx, right, hash_size) != 1, "EVP_DigestUpdate(right)");
	ERR(EVP_DigestFinal_ex(ctx, ret_hash->h, NULL) != 1, "EVP_DigestFinal_ex()");
}

static void hash_file(struct file_entry *fe)
{
	unsigned char *mem;
	struct stat sb;
	int fd, ret;

	fd = open(fe->name, O_RDONLY);
	if (fd < 0)
		err(1, "Failed to open %s", fe->name);

	ret = fstat(fd, &sb);
	if (ret)
		err(1, "Failed to stat %s", fe->name);

	mem = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED)
		err(1, "Failed to mmap %s", fe->name);

	hash_data(fe->pos, mem, sb.st_size, &fe->hash);

	munmap(mem, sb.st_size);
	close(fd);
}

static struct mtree *build_merkle(struct file_entry *files, size_t num_files)
{
	unsigned int num_cur_le, num_prev_le;
	struct mtree *mt;

	if (!num_files)
		return NULL;

	mt = xmalloc(sizeof(*mt));
	mt->num_levels = log2_roundup(num_files);

	mt->level_hashes = xcalloc(sizeof(*mt->level_hashes), mt->num_levels);

	mt->entries = xcalloc(sizeof(*mt->entries), mt->num_levels);
	num_cur_le = (num_files + 1) / 2;
	mt->entries[0] = num_cur_le;
	mt->level_hashes[0] = xcalloc(sizeof(**mt->level_hashes), num_cur_le);

	/* First level of pairs */
	for (size_t i = 0; i < num_files; i += 2) {
		/* Hash the pair, or the last file with itself if it's odd. */
		const struct hash *right = i + 1 < num_files ? &files[i + 1].hash : &files[i].hash;

		hash_entry(&files[i].hash, right, &mt->level_hashes[0][i / 2]);
	}

	for (unsigned int i = 1; i < mt->num_levels; i++) {
		num_prev_le = num_cur_le;

		num_cur_le = (num_prev_le + 1) / 2;
		mt->entries[i] = num_cur_le;
		mt->level_hashes[i] = xcalloc(sizeof(**mt->level_hashes), num_cur_le);

		for (unsigned int n = 0; n < num_prev_le; n += 2) {
			/* Hash the pair, or the last with itself if it's odd. */
			const struct hash *right = n + 1 < num_prev_le ?
						   &mt->level_hashes[i - 1][n + 1] :
						   &mt->level_hashes[i - 1][n];
			hash_entry(&mt->level_hashes[i - 1][n], right,
				   &mt->level_hashes[i][n / 2]);
		}
	}

	/* FIXME assert single hash in root */

	return mt;
}

static void free_mtree(struct mtree *mt)
{
	if (!mt)
		return;

	for (unsigned int i = 0; i < mt->num_levels; i++)
		free(mt->level_hashes[i]);

	free(mt->level_hashes);
	free(mt->entries);
	free(mt);
}

static void write_be_int(int fd, unsigned int v)
{
	unsigned int be_val = htobe32(v);

	if (write(fd, &be_val, sizeof(be_val)) != sizeof(be_val))
		err(1, "Failed writing to file");
}

static void write_hash(int fd, const struct hash *hash)
{
	if (write(fd, hash->h, hash_size) != hash_size)
		err(1, "Failed writing to file");
}

static void build_proof(struct mtree *mt, unsigned int n, int fd)
{
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

	/* Next is the sibling hash, followed by hashes in the tree */
	write_hash(fd, &fe_sib->hash);

	for (unsigned int i = 0; i < mt->num_levels - 1; i++) {
		n >>= 1;
		if ((n & 1) == 0) {
			const struct hash *h;

			/* No pair, hash with itself */
			if (n + 1 == mt->entries[i])
				h = &mt->level_hashes[i][n];
			else
				h = &mt->level_hashes[i][n + 1];

			write_hash(fd, h);
		} else {
			write_hash(fd, &mt->level_hashes[i][n - 1]);
		}
	}
}

static void append_module_signature_magic(int fd, unsigned int sig_len)
{
	const struct module_signature sig_info = {
		.id_type	= MODULE_SIGNATURE_TYPE_MERKLE,
		.sig_len	= htobe32(sig_len),
	};
	const size_t sig_str_len = sizeof(MODULE_SIGNATURE_MARKER) - 1;
	const char *sig_str = MODULE_SIGNATURE_MARKER;

	if (write(fd, &sig_info, sizeof(sig_info)) != sizeof(sig_info))
		err(1, "write(sig_info) failed");

	if (write(fd, sig_str, sig_str_len) != sig_str_len)
		err(1, "write(magic_number) failed");
}

static void write_merkle_root(struct mtree *mt, const char *filename)
{
	unsigned int num_levels;
	struct hash *h;
	FILE *f;

	if (mt) {
		num_levels = mt->num_levels;
		h = &mt->level_hashes[mt->num_levels - 1][0];
	} else {
		num_levels = 0;
		h = xcalloc(1, hash_size);
	}

	f = fopen(filename, "w");
	if (!f)
		err(1, "Failed to create %s", filename);

	fprintf(f, "#include <linux/module_hashes.h>\n\n");
	fprintf(f, "const struct\n");
	fprintf(f, "module_hashes_root module_hashes_root __module_hashes_section = {\n");

	fprintf(f, "\t.levels = %u,\n", num_levels);
	fprintf(f, "\t.hash = {{");
	for (unsigned int i = 0; i < hash_size; i++) {
		char *space = "";

		if (!(i % 8))
			fprintf(f, "\n\t\t");

		if ((i + 1) % 8)
			space = " ";

		fprintf(f, "0x%02x,%s", h->h[i], space);
	}
	fprintf(f, "\n\t}},");

	fprintf(f, "\n};\n");

	if (fclose(f))
		err(1, "Failed to write %s", filename);

	if (!mt)
		free(h);
}

static char *xstrdup_replace_suffix(const char *str, const char *old_suffix, const char *new_suffix)
{
	size_t str_len, old_suffix_len, base_len;

	str_len = strlen(str);
	old_suffix_len = strlen(old_suffix);
	base_len = str_len - old_suffix_len;

	if (old_suffix_len > str_len || memcmp(str + base_len, old_suffix, old_suffix_len) != 0)
		errx(1, "'%s' does not end in '%s'", str, old_suffix);

	return xasprintf("%.*s%s", (int)base_len, str, new_suffix);
}

static void trim_newline(char *line)
{
	size_t len;

	if (!line)
		return;

	len = strlen(line);
	if (!len)
		return;

	if (line[len - 1] == '\n')
		line[len - 1] = '\0';
}

static void read_modules_order(const char *fname, const char *suffix)
{
	char line[PATH_MAX];
	FILE *in;

	in = fopen(fname, "r");
	if (!in)
		err(1, "Failed to open %s", fname);

	while (fgets(line, PATH_MAX, in)) {
		struct file_entry *entry;

		trim_newline(line);

		fh_list = xreallocarray(fh_list, num_files + 1, sizeof(*fh_list));
		entry = &fh_list[num_files];

		entry->pos = num_files;
		entry->name = xstrdup_replace_suffix(line, ".o", suffix);
		hash_file(entry);

		num_files++;
	}

	if (ferror(in))
		errx(1, "Failed to read %s", fname);

	fclose(in);
}

static __attribute__((noreturn))
void usage(void)
{
	fprintf(stderr,
		"Usage: scripts/modules-merkle-tree <kmod suffix> <root definition>\n");
	exit(2);
}

int main(int argc, char *argv[])
{
	const char *kmod_suffix;
	const EVP_MD *hash_evp;
	struct mtree *mt;

	if (argc != 3)
		usage();

	kmod_suffix = argv[2];

	hash_evp = EVP_sha256();
	ERR(!hash_evp, "EVP_sha256()");

	ctx = EVP_MD_CTX_new();
	ERR(!ctx, "EVP_MD_CTX_new()");

	hash_size = EVP_MD_get_size(hash_evp);
	ERR(hash_size <= 0, "EVP_get_digestbyname");

	if (hash_size != sizeof(struct hash))
		errx(1, "Invalid hash size");

	if (EVP_DigestInit_ex(ctx, hash_evp, NULL) != 1)
		ERR(1, "EVP_DigestInit_ex()");

	read_modules_order("modules.order", kmod_suffix);

	mt = build_merkle(fh_list, num_files);
	write_merkle_root(mt, argv[1]);
	for (size_t i = 0; i < num_files; i++) {
		char *signame;
		int fd;

		signame = xstrdup_replace_suffix(fh_list[i].name, kmod_suffix, ".merkle");

		fd = open(signame, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			err(1, "Can't create %s", signame);

		build_proof(mt, i, fd);
		append_module_signature_magic(fd, lseek(fd, 0, SEEK_CUR));
		if (close(fd))
			err(1, "Can't write %s", signame);
	}

	free_mtree(mt);
	for (size_t i = 0; i < num_files; i++)
		free(fh_list[i].name);
	free(fh_list);

	EVP_MD_CTX_free(ctx);
	return 0;
}
