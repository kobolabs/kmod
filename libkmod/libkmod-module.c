/*
 * libkmod - interface to kernel module operations
 *
 * Copyright (C) 2011  ProFUSION embedded systems
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation version 2.1.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>

#include "libkmod.h"
#include "libkmod-private.h"

/**
 * kmod_module:
 *
 * Opaque object representing a module.
 */
struct kmod_module {
	struct kmod_ctx *ctx;
	const char *path;
	const char *name;
	struct kmod_list *dep;
	int refcount;
	struct {
		bool dep : 1;
	} init;
};

static char *path_to_modname(const char *path, bool alloc)
{
	char *modname;
	char *c;

	modname = basename(path);
	if (modname == NULL || modname[0] == '\0')
		return NULL;

	if (alloc)
		modname = strdup(modname);

	for (c = modname; *c != '\0' && *c != '.'; c++) {
		if (*c == '-')
			*c = '_';
	}

	*c = '\0';
	return modname;
}

static const char *get_modname(struct kmod_module *mod)
{
	if (mod->name == NULL)
		mod->name = path_to_modname(mod->path, true);

	return mod->name;
}

int kmod_module_parse_dep(struct kmod_module *mod, char *line)
{
	struct kmod_ctx *ctx = mod->ctx;
	struct kmod_list *list = NULL;
	char *p, *saveptr;
	int err, n = 0;

	assert(!mod->init.dep && mod->dep == NULL);
	mod->init.dep = true;

	p = strchr(line, ':');
	if (p == NULL)
		return 0;

	p++;

	for (p = strtok_r(p, " \t", &saveptr); p != NULL;
					p = strtok_r(NULL, " \t", &saveptr)) {
		const char *modname = path_to_modname(p, false);
		struct kmod_module *depmod;

		err = kmod_module_new_from_name(ctx, modname, &depmod);
		if (err < 0) {
			ERR(ctx, "ctx=%p modname=%s error=%s\n",
						ctx, modname, strerror(-err));
			goto fail;
		}

		DBG(ctx, "add dep: %s\n", modname);

		list = kmod_list_append(list, depmod);
		n++;
	}

	DBG(ctx, "%d dependencies for %s\n", n, mod->name);

	mod->dep = list;
	return n;

fail:
	kmod_module_unref_list(list);
	mod->init.dep = false;
	return err;
}

KMOD_EXPORT int kmod_module_new_from_name(struct kmod_ctx *ctx,
						const char *name,
						struct kmod_module **mod)
{
	struct kmod_module *m;

	if (ctx == NULL || name == NULL)
		return -ENOENT;

	m = calloc(1, sizeof(*m));
	if (m == NULL) {
		free(m);
		return -ENOMEM;
	}

	m->ctx = kmod_ref(ctx);
	m->name = strdup(name);
	m->refcount = 1;

	*mod = m;

	return 0;
}

KMOD_EXPORT int kmod_module_new_from_path(struct kmod_ctx *ctx,
						const char *path,
						struct kmod_module **mod)
{
	struct kmod_module *m;
	int err;
	struct stat st;

	if (ctx == NULL || path == NULL)
		return -ENOENT;

	err = stat(path, &st);
	if (err < 0)
		return -errno;

	m = calloc(1, sizeof(*m));
	if (m == NULL) {
		free(m);
		return -ENOMEM;
	}

	m->ctx = kmod_ref(ctx);
	m->path = strdup(path);
	m->refcount = 1;

	*mod = m;

	return 0;
}

KMOD_EXPORT struct kmod_module *kmod_module_unref(struct kmod_module *mod)
{
	if (mod == NULL)
		return NULL;

	if (--mod->refcount > 0)
		return mod;

	DBG(mod->ctx, "kmod_module %p released\n", mod);

	kmod_module_unref_list(mod->dep);
	kmod_unref(mod->ctx);
	free((char *) mod->path);
	free((char *) mod->name);
	free(mod);
	return NULL;
}

KMOD_EXPORT struct kmod_module *kmod_module_ref(struct kmod_module *mod)
{
	if (mod == NULL)
		return NULL;

	mod->refcount++;

	return mod;
}

#define CHECK_ERR_AND_FINISH(_err, _label_err, _list, label_finish)	\
	do {								\
		if ((_err) < 0)						\
			goto _label_err;				\
		if (*(_list) != NULL)					\
			goto finish;					\
	} while (0)

KMOD_EXPORT int kmod_module_new_from_lookup(struct kmod_ctx *ctx,
						const char *alias,
						struct kmod_list **list)
{
	int err;

	if (ctx == NULL || alias == NULL)
		return -ENOENT;

	if (list == NULL || *list != NULL) {
		ERR(ctx, "An empty list is needed to create lookup\n");
		return -ENOSYS;
	}

	/* Aliases from config file override all the others */
	err = kmod_lookup_alias_from_config(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

	err = kmod_lookup_alias_from_moddep_file(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

	err = kmod_lookup_alias_from_symbols_file(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

	err = kmod_lookup_alias_from_aliases_file(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

finish:

	return err;
fail:
	kmod_module_unref_list(*list);
	*list = NULL;
	return err;
}
#undef CHECK_ERR_AND_FINISH


KMOD_EXPORT int kmod_module_unref_list(struct kmod_list *list)
{
	for (; list != NULL; list = kmod_list_remove(list))
		kmod_module_unref(list->data);

	return 0;
}

/*
 * We don't increase the refcount. Maybe we should.
 */
KMOD_EXPORT struct kmod_list *kmod_module_get_dependency(const struct kmod_module *mod)
{
	// FIXME calculate dependency if it's not initialized
	return mod->dep;
}

KMOD_EXPORT struct kmod_module *kmod_module_get_module(const struct kmod_list *entry)
{
	if (entry == NULL)
		return NULL;
	return kmod_module_ref(entry->data);
}

KMOD_EXPORT const char *kmod_module_get_name(const struct kmod_module *mod)
{
	// FIXME calculate name if name == NULL
	return mod->name;
}

KMOD_EXPORT const char *kmod_module_get_path(const struct kmod_module *mod)
{
	// FIXME calculate path if path == NULL
	return mod->path;
}


extern long delete_module(const char *name, unsigned int flags);

KMOD_EXPORT int kmod_module_remove_module(struct kmod_module *mod,
							unsigned int flags)
{
	int err;
	const char *modname;

	if (mod == NULL)
		return -ENOENT;

	/* Filter out other flags */
	flags &= (KMOD_REMOVE_FORCE | KMOD_REMOVE_NOWAIT);

	modname = get_modname(mod);
	err = delete_module(modname, flags);
	if (err != 0) {
		ERR(mod->ctx, "Removing '%s': %s\n", modname,
							strerror(-err));
		return err;
	}

	return 0;
}

extern long init_module(void *mem, unsigned long len, const char *args);

KMOD_EXPORT int kmod_module_insert_module(struct kmod_module *mod,
							unsigned int flags)
{
	int err;
	void *mmaped_file;
	struct stat st;
	int fd;
	const char *args = "";

	if (mod == NULL)
		return -ENOENT;

	if (mod->path == NULL) {
		ERR(mod->ctx, "Not supported to load a module by name yet\n");
		return -ENOSYS;
	}

	if (flags != 0)
		INFO(mod->ctx, "Flags are not implemented yet\n");

	if ((fd = open(mod->path, O_RDONLY)) < 0) {
		err = -errno;
		return err;
	}

	fstat(fd, &st);

	if ((mmaped_file = mmap(0, st.st_size, PROT_READ,
					MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
		close(fd);
		return -errno;
	}

	err = init_module(mmaped_file, st.st_size, args);
	if (err < 0)
		ERR(mod->ctx, "Failed to insert module '%s'\n", mod->path);

	munmap(mmaped_file, st.st_size);
	close(fd);

	return err;
}

KMOD_EXPORT const char *kmod_module_initstate_str(enum kmod_module_initstate state)
{
    switch (state) {
    case KMOD_MODULE_BUILTIN:
	return "builtin";
    case KMOD_MODULE_LIVE:
	return "live";
    case KMOD_MODULE_COMING:
	return "coming";
    case KMOD_MODULE_GOING:
	return "going";
    default:
	return NULL;
    }
}

KMOD_EXPORT int kmod_module_get_initstate(const struct kmod_module *mod)
{
	char path[PATH_MAX], buf[32];
	int fd, err, pathlen;

	pathlen = snprintf(path, sizeof(path),
				"/sys/module/%s/initstate", mod->name);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		err = -errno;

		if (pathlen > (int)sizeof("/initstate") - 1) {
			struct stat st;
			path[pathlen - (sizeof("/initstate") - 1)] = '\0';
			if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				return KMOD_MODULE_BUILTIN;
		}

		ERR(mod->ctx, "could not open '%s': %s\n",
			path, strerror(-err));
		return err;
	}

	err = read_str_safe(fd, buf, sizeof(buf));
	close(fd);
	if (err < 0) {
		ERR(mod->ctx, "could not read from '%s': %s\n",
			path, strerror(-err));
		return err;
	}

	if (strcmp(buf, "live\n") == 0)
		return KMOD_MODULE_LIVE;
	else if (strcmp(buf, "coming\n") == 0)
		return KMOD_MODULE_COMING;
	else if (strcmp(buf, "going\n") == 0)
		return KMOD_MODULE_GOING;

	ERR(mod->ctx, "unknown %s: '%s'\n", path, buf);
	return -EINVAL;
}

KMOD_EXPORT int kmod_module_get_refcnt(const struct kmod_module *mod)
{
	char path[PATH_MAX];
	long refcnt;
	int fd, err;

	snprintf(path, sizeof(path), "/sys/module/%s/refcnt", mod->name);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		err = -errno;
		ERR(mod->ctx, "could not open '%s': %s\n",
			path, strerror(errno));
		return err;
	}

	err = read_str_long(fd, &refcnt, 10);
	close(fd);
	if (err < 0) {
		ERR(mod->ctx, "could not read integer from '%s': '%s'\n",
			path, strerror(-err));
		return err;
	}

	return (int)refcnt;
}

KMOD_EXPORT struct kmod_list *kmod_module_get_holders(const struct kmod_module *mod)
{
	char dname[PATH_MAX];
	struct kmod_list *list = NULL;
	DIR *d;
	struct dirent *de;

	if (mod == NULL)
		return NULL;
	snprintf(dname, sizeof(dname), "/sys/module/%s/holders", mod->name);

	d = opendir(dname);
	if (d == NULL) {
		ERR(mod->ctx, "could not open '%s': %s\n",
			dname, strerror(errno));
		return NULL;
	}

	while ((de = readdir(d)) != NULL) {
		struct kmod_list *node;
		struct kmod_module *holder;
		int err;

		if (de->d_name[0] == '.') {
			if (de->d_name[1] == '\0' ||
			    (de->d_name[1] == '.' && de->d_name[2] == '\0'))
				continue;
		}

		err = kmod_module_new_from_name(mod->ctx, de->d_name, &holder);
		if (err < 0) {
			ERR(mod->ctx, "could not create module for '%s': %s\n",
				de->d_name, strerror(-err));
			continue;
		}

		node = kmod_list_append(list, holder);
		if (node)
			list = node;
		else {
			ERR(mod->ctx, "out of memory\n");
			kmod_module_unref(holder);
		}
	}

	closedir(d);
	return list;
}

struct kmod_module_section {
	unsigned long address;
	char name[];
};

static void kmod_module_section_free(struct kmod_module_section *section)
{
	free(section);
}

KMOD_EXPORT struct kmod_list *kmod_module_get_sections(const struct kmod_module *mod)
{
	char dname[PATH_MAX];
	struct kmod_list *list = NULL;
	DIR *d;
	int dfd;
	struct dirent *de;

	if (mod == NULL)
		return NULL;
	snprintf(dname, sizeof(dname), "/sys/module/%s/sections", mod->name);

	d = opendir(dname);
	if (d == NULL) {
		ERR(mod->ctx, "could not open '%s': %s\n",
			dname, strerror(errno));
		return NULL;
	}

	dfd = dirfd(d);
	while ((de = readdir(d)) != NULL) {
		struct kmod_module_section *section;
		struct kmod_list *node;
		unsigned long address;
		size_t namesz;
		int fd, err;

		if (de->d_name[0] == '.') {
			if (de->d_name[1] == '\0' ||
			    (de->d_name[1] == '.' && de->d_name[2] == '\0'))
				continue;
		}

		fd = openat(dfd, de->d_name, O_RDONLY);
		if (fd < 0) {
			ERR(mod->ctx, "could not open '%s/%s': %s\n",
				dname, de->d_name, strerror(errno));
			continue;
		}

		err = read_str_ulong(fd, &address, 16);
		if (err < 0) {
			ERR(mod->ctx, "could not read long from '%s/%s': %s\n",
				dname, de->d_name, strerror(-err));
			close(fd);
			continue;
		}

		namesz = strlen(de->d_name) + 1;
		section = malloc(sizeof(struct kmod_module_section) + namesz);
		section->address = address;
		memcpy(section->name, de->d_name, namesz);

		node = kmod_list_append(list, section);
		if (node)
			list = node;
		else {
			ERR(mod->ctx, "out of memory\n");
			free(section);
		}
		close(fd);
	}

	closedir(d);
	return list;
}

KMOD_EXPORT const char *kmod_module_section_get_name(const struct kmod_list *entry)
{
	struct kmod_module_section *section;
	if (entry == NULL)
		return NULL;
	section = entry->data;
	return section->name;
}

KMOD_EXPORT unsigned long kmod_module_section_get_address(const struct kmod_list *entry)
{
	struct kmod_module_section *section;
	if (entry == NULL)
		return (unsigned long)-1;
	section = entry->data;
	return section->address;
}

KMOD_EXPORT void kmod_module_section_free_list(struct kmod_list *list)
{
	while (list) {
		kmod_module_section_free(list->data);
		list = kmod_list_remove(list);
	}
}