// SPDX-License-Identifier: GPL-2.0
#include "perf-libelf.h"

#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "build-id.h"
#include "debug.h"

/*
 * Align offset to 4 bytes as needed for note name and descriptor data.
 */
#define NOTE_ALIGN(n) (((n) + 3) & -4U)

Elf_Scn *elf_section_by_name(Elf *elf, GElf_Ehdr *ep,
			     GElf_Shdr *shp, const char *name, size_t *idx)
{
	Elf_Scn *sec = NULL;
	size_t cnt = 1;

	/* ELF is corrupted/truncated, avoid calling elf_strptr. */
	if (!elf_rawdata(elf_getscn(elf, ep->e_shstrndx), NULL))
		return NULL;

	while ((sec = elf_nextscn(elf, sec)) != NULL) {
		char *str;

		gelf_getshdr(sec, shp);
		str = elf_strptr(elf, ep->e_shstrndx, shp->sh_name);
		if (str && !strcmp(name, str)) {
			if (idx)
				*idx = cnt;
			return sec;
		}
		++cnt;
	}

	return NULL;
}

int __libelf__read_build_id(Elf *elf, void *bf, size_t size)
{
	int err = -1;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;
	Elf_Data *data;
	Elf_Scn *sec;
	Elf_Kind ek;
	void *ptr;

	if (size < BUILD_ID_SIZE)
		goto out;

	ek = elf_kind(elf);
	if (ek != ELF_K_ELF)
		goto out;

	if (gelf_getehdr(elf, &ehdr) == NULL) {
		pr_err("%s: cannot get elf header.\n", __func__);
		goto out;
	}

	/*
	 * Check following sections for notes:
	 *   '.note.gnu.build-id'
	 *   '.notes'
	 *   '.note' (VDSO specific)
	 */
	do {
		sec = elf_section_by_name(elf, &ehdr, &shdr,
					  ".note.gnu.build-id", NULL);
		if (sec)
			break;

		sec = elf_section_by_name(elf, &ehdr, &shdr,
					  ".notes", NULL);
		if (sec)
			break;

		sec = elf_section_by_name(elf, &ehdr, &shdr,
					  ".note", NULL);
		if (sec)
			break;

		return err;

	} while (0);

	data = elf_getdata(sec, NULL);
	if (data == NULL)
		goto out;

	ptr = data->d_buf;
	while (ptr < (data->d_buf + data->d_size)) {
		GElf_Nhdr *nhdr = ptr;
		size_t namesz = NOTE_ALIGN(nhdr->n_namesz),
		       descsz = NOTE_ALIGN(nhdr->n_descsz);
		const char *name;

		ptr += sizeof(*nhdr);
		name = ptr;
		ptr += namesz;
		if (nhdr->n_type == NT_GNU_BUILD_ID &&
		    nhdr->n_namesz == sizeof("GNU")) {
			if (memcmp(name, "GNU", sizeof("GNU")) == 0) {
				size_t sz = min(size, descsz);

				memcpy(bf, ptr, sz);
				memset(bf + sz, 0, size - sz);
				err = sz;
				break;
			}
		}
		ptr += descsz;
	}

out:
	return err;
}

int libelf__read_build_id(int _fd, const char *filename, struct build_id *bid)
{
	size_t size = sizeof(bid->data);
	int fd, err = -1;
	Elf *elf;

	if (size < BUILD_ID_SIZE)
		goto out;

	fd = dup(_fd);
	if (fd < 0)
		goto out;

	elf = elf_begin(fd, PERF_ELF_C_READ_MMAP, NULL);
	if (elf == NULL) {
		pr_debug2("%s: cannot read %s ELF file.\n", __func__, filename);
		goto out_close;
	}

	err = __libelf__read_build_id(elf, bid->data, size);
	if (err > 0)
		bid->size = err;

	elf_end(elf);
out_close:
	close(fd);
out:
	return err;
}

int libelf_sysfs__read_build_id(const char *filename, struct build_id *bid)
{
	size_t size = sizeof(bid->data);
	int fd, err = -1;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		goto out;

	while (1) {
		char bf[BUFSIZ];
		GElf_Nhdr nhdr;
		size_t namesz, descsz;

		if (read(fd, &nhdr, sizeof(nhdr)) != sizeof(nhdr))
			break;

		namesz = NOTE_ALIGN(nhdr.n_namesz);
		descsz = NOTE_ALIGN(nhdr.n_descsz);
		if (nhdr.n_type == NT_GNU_BUILD_ID &&
		    nhdr.n_namesz == sizeof("GNU")) {
			if (read(fd, bf, namesz) != (ssize_t)namesz)
				break;
			if (memcmp(bf, "GNU", sizeof("GNU")) == 0) {
				size_t sz = min(descsz, size);

				if (read(fd, bid->data, sz) == (ssize_t)sz) {
					memset(bid->data + sz, 0, size - sz);
					bid->size = sz;
					err = 0;
					break;
				}
			} else if (read(fd, bf, descsz) != (ssize_t)descsz)
				break;
		} else {
			int n = namesz + descsz;

			if (n > (int)sizeof(bf)) {
				n = sizeof(bf);
				pr_debug("%s: truncating reading of build id in sysfs file %s: n_namesz=%u, n_descsz=%u.\n",
					 __func__, filename, nhdr.n_namesz, nhdr.n_descsz);
			}
			if (read(fd, bf, n) != n)
				break;
		}
	}
	close(fd);
out:
	return err;
}

int libelf_filename__read_debuglink(const char *filename, char *debuglink, size_t size)
{
	int fd, err = -1;
	Elf *elf;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;
	Elf_Data *data;
	Elf_Scn *sec;
	Elf_Kind ek;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		goto out;

	elf = elf_begin(fd, PERF_ELF_C_READ_MMAP, NULL);
	if (elf == NULL) {
		pr_debug2("%s: cannot read %s ELF file.\n", __func__, filename);
		goto out_close;
	}

	ek = elf_kind(elf);
	if (ek != ELF_K_ELF)
		goto out_elf_end;

	if (gelf_getehdr(elf, &ehdr) == NULL) {
		pr_err("%s: cannot get elf header.\n", __func__);
		goto out_elf_end;
	}

	sec = elf_section_by_name(elf, &ehdr, &shdr,
				  ".gnu_debuglink", NULL);
	if (sec == NULL)
		goto out_elf_end;

	data = elf_getdata(sec, NULL);
	if (data == NULL)
		goto out_elf_end;

	/* the start of this section is a zero-terminated string */
	strncpy(debuglink, data->d_buf, size);

	err = 0;

out_elf_end:
	elf_end(elf);
out_close:
	close(fd);
out:
	return err;
}

enum dso_type libelf_dso__type_fd(int fd)
{
	enum dso_type dso_type = DSO__TYPE_UNKNOWN;
	GElf_Ehdr ehdr;
	Elf_Kind ek;
	Elf *elf;

	elf = elf_begin(fd, PERF_ELF_C_READ_MMAP, NULL);
	if (elf == NULL)
		goto out;

	ek = elf_kind(elf);
	if (ek != ELF_K_ELF)
		goto out_end;

	if (gelf_getclass(elf) == ELFCLASS64) {
		dso_type = DSO__TYPE_64BIT;
		goto out_end;
	}

	if (gelf_getehdr(elf, &ehdr) == NULL)
		goto out_end;

	if (ehdr.e_machine == EM_X86_64)
		dso_type = DSO__TYPE_X32BIT;
	else
		dso_type = DSO__TYPE_32BIT;
out_end:
	elf_end(elf);
out:
	return dso_type;
}
