// SPDX-License-Identifier: MIT
// A file mailbox for the host rig: a DroidVM guest has no serial console or network, and userland writes its
// logs and crash reports into the scratch Data volume, which lives in guest RAM. The host writes a request into
// droidvm_export_box through guest memory (tools/rig/export.py); the droidvm-monitor worker answers it here, in
// the kernel's VFS context, with a file's bytes or a directory's entries in droidvm_export_data.
//
// Files are read (FREAD, O_NOFOLLOW) anywhere and written only under /private/var/ngpu/, the owner tools'
// directory (their launchd jobs and binaries, staged by the host between boots). The one other action is a signal to a process,
// for a userland that hangs where launchd would restart it (a SpringBoard deadlocked at launch).
#include <stdbool.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/signal.h>
#include <kern/kalloc.h>

#define DROIDVM_EXPORT_BYTES (1u << 20)

// request: the host bumps it after writing kind, offset and path; done: set to request once data and status are
// final. kind 0 reads up to DROIDVM_EXPORT_BYTES of a regular file from offset (total = its size); kind 1 lists a
// directory as "<d_type> <name>\n" lines from entry offset (total = entries listed, length = bytes); kind 2 lists the
// processes as "<pid> <ppid> <name>\n" lines (path unused; total = processes); kind 3 sends signal `length` to process
// `offset` (not to launchd or the kernel); kind 4 copies `length` bytes (at most DROIDVM_EXPORT_BYTES) of the user memory of
// process <path, in decimal> from address `offset` (length = bytes copied); kind 5 writes the first `length` bytes of
// droidvm_export_data to file <path> at `offset`, creating it root-owned with mode `total` (truncated when offset is 0),
// only under /private/var/ngpu/; kind 6 lists a whole tree as kind 1 lists a directory, "<d_type> <size> <path below
// <path>>\n" lines depth-first (a regular file's size, 0 for anything else), from entry offset (total = entries listed):
// one request where kind 1 and a size read per file took thousands, for the unified log's uuidtext.
struct droidvm_export {
	volatile uint32_t request, done;
	uint32_t kind;
	int32_t status;
	uint64_t offset, length, total;
	char path[512];
};
struct droidvm_export droidvm_export_box;
char droidvm_export_data[DROIDVM_EXPORT_BYTES] __attribute__((aligned(4096)));

static int droidvm_export_file(const char *path, uint64_t offset, uint64_t *length, uint64_t *total) {
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
	if (error) return error;
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	if (vnode_vtype(vp) != VREG) error = EISDIR;
	else error = vnode_getattr(vp, &va, ctx);
	if (!error) {
		uint64_t size = va.va_data_size, count = 0;
		if (offset < size) count = MIN(size - offset, (uint64_t)DROIDVM_EXPORT_BYTES);
		int resid = 0;
		if (count) error = vn_rdwr(UIO_READ, vp, droidvm_export_data, (int)count, (off_t)offset, UIO_SYSSPACE, 0,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		*length = error ? 0 : count - (uint64_t)resid;
		*total = size;
	}
	vnode_close(vp, FREAD, ctx);
	return error;
}

static int droidvm_export_directory(const char *path, uint64_t first, uint64_t *length, uint64_t *total) {
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int error = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
	if (error) return error;
	if (vnode_vtype(vp) != VDIR) {
		vnode_put(vp);
		return ENOTDIR;
	}
	static char entries[16384];
	uint64_t used = 0, index = 0;
	int eof = 0;
	off_t cookie = 0;
	while (!eof && !error) {
		uio_t uio = uio_create(1, cookie, UIO_SYSSPACE, UIO_READ);
		if (!uio) { error = ENOMEM; break; }
		uio_addiov(uio, CAST_USER_ADDR_T(entries), sizeof(entries));
		int count = 0;
		error = VNOP_READDIR(vp, uio, 0, &eof, &count, ctx);
		size_t got = sizeof(entries) - (size_t)uio_resid(uio);
		cookie = uio_offset(uio);
		uio_free(uio);
		if (error || !got) break;
		for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
			const struct dirent *entry = (const struct dirent *)(entries + at);
			if (!entry->d_reclen) break;
			if (entry->d_ino && index++ >= first) {
				int n = snprintf(droidvm_export_data + used, DROIDVM_EXPORT_BYTES - used, "%u %.*s\n",
				    entry->d_type, (int)entry->d_namlen, entry->d_name);
				if (n < 0 || used + (uint64_t)n >= DROIDVM_EXPORT_BYTES) { eof = 1; break; }
				used += (uint64_t)n;
			}
			at += entry->d_reclen;
		}
	}
	vnode_put(vp);
	*length = used;
	*total = index > first ? index - first : 0;
	return error;
}

#define DROIDVM_EXPORT_TREE_DEPTH 4   // the unified log's diagnostics and uuidtext are two levels deep
#define DROIDVM_EXPORT_DIRENTS 16384

struct droidvm_export_tree {
	char path[1024];
	size_t root;                      // the listed directory's own length in `path`
	uint64_t first, index, listed, used;
	int full;
};

static uint64_t droidvm_export_size(const char *path, vfs_context_t ctx) {
	vnode_t vp = NULLVP;
	uint64_t size = 0;
	if (vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx)) return 0;
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	if (!vnode_getattr(vp, &va, ctx)) size = va.va_data_size;
	vnode_put(vp);
	return size;
}

// One directory of the tree: tree->path holds its path (`length` bytes), and grows by each entry's name in turn. Each
// level reads its directory into a buffer of its own, since a subdirectory is listed before the next entry is read.
static int droidvm_export_tree_level(struct droidvm_export_tree *tree, size_t length, unsigned depth) {
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int error = vnode_lookup(tree->path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
	if (error) return error;
	bool directory = vnode_vtype(vp) == VDIR;
	char *entries = directory ? kalloc_data(DROIDVM_EXPORT_DIRENTS, Z_WAITOK) : NULL;
	if (!entries) {
		vnode_put(vp);
		return directory ? ENOMEM : ENOTDIR;
	}
	int eof = 0;
	off_t cookie = 0;
	while (!eof && !error && !tree->full) {
		uio_t uio = uio_create(1, cookie, UIO_SYSSPACE, UIO_READ);
		if (!uio) { error = ENOMEM; break; }
		uio_addiov(uio, CAST_USER_ADDR_T(entries), DROIDVM_EXPORT_DIRENTS);
		int count = 0;
		error = VNOP_READDIR(vp, uio, 0, &eof, &count, ctx);
		size_t got = DROIDVM_EXPORT_DIRENTS - (size_t)uio_resid(uio);
		cookie = uio_offset(uio);
		uio_free(uio);
		if (error || !got) break;
		for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got && !tree->full;) {
			const struct dirent *entry = (const struct dirent *)(entries + at);
			if (!entry->d_reclen) break;
			at += entry->d_reclen;
			bool dot = entry->d_name[0] == '.' && (entry->d_namlen == 1 || (entry->d_namlen == 2 && entry->d_name[1] == '.'));
			int n = snprintf(tree->path + length, sizeof(tree->path) - length, "/%.*s", (int)entry->d_namlen, entry->d_name);
			if (!entry->d_ino || dot || n < 0 || length + (size_t)n >= sizeof(tree->path)) continue;
			if (tree->index++ >= tree->first) {
				uint64_t size = entry->d_type == DT_REG ? droidvm_export_size(tree->path, ctx) : 0;
				int w = snprintf(droidvm_export_data + tree->used, DROIDVM_EXPORT_BYTES - tree->used, "%u %llu %s\n",
				    entry->d_type, size, tree->path + tree->root + 1);
				if (w < 0 || tree->used + (uint64_t)w >= DROIDVM_EXPORT_BYTES) { tree->full = 1; break; }
				tree->used += (uint64_t)w;
				++tree->listed;
			}
			// A subdirectory it cannot read is listed but not descended into.
			if (entry->d_type == DT_DIR && depth + 1 < DROIDVM_EXPORT_TREE_DEPTH)
				(void)droidvm_export_tree_level(tree, length + (size_t)n, depth + 1);
		}
	}
	tree->path[length] = 0;
	kfree_data(entries, DROIDVM_EXPORT_DIRENTS);
	vnode_put(vp);
	return error;
}

static int droidvm_export_tree(const char *path, uint64_t first, uint64_t *length, uint64_t *total) {
	static struct droidvm_export_tree tree;
	memset(&tree, 0, sizeof(tree));
	tree.root = strlcpy(tree.path, path, sizeof(tree.path));
	if (tree.root >= sizeof(tree.path)) return ENAMETOOLONG;
	while (tree.root > 1 && tree.path[tree.root - 1] == '/') tree.path[--tree.root] = 0;
	tree.first = first;
	int error = droidvm_export_tree_level(&tree, tree.root, 0);
	*length = tree.used;
	*total = tree.listed;
	return error;
}

static const char droidvm_export_writable[] = "/private/var/ngpu/";

static int droidvm_export_may_write(const char *path) {
	if (strncmp(path, droidvm_export_writable, sizeof(droidvm_export_writable) - 1)) return 0;
	for (const char *c = path; *c; ++c)
		if (c[0] == '/' && c[1] == '.' && c[2] == '.' && (c[3] == '/' || !c[3])) return 0;
	return 1;
}

static int droidvm_export_write(const char *path, uint64_t offset, uint64_t length, uint64_t mode) {
	if (!droidvm_export_may_write(path) || length > DROIDVM_EXPORT_BYTES || (mode & ~(uint64_t)07777)) return EPERM;
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FWRITE | O_CREAT | O_NOFOLLOW | (offset ? 0 : O_TRUNC), (int)mode, 0, &vp, ctx);
	if (error) return error;
	if (vnode_vtype(vp) != VREG) error = EISDIR;
	/* O_TRUNC is not honoured on every backing filesystem here, so a shorter overwrite would keep the old file's trailing
	 * bytes; truncate the file to empty ourselves before the first piece (offset 0) lands. */
	if (!error && !offset) {
		struct vnode_attr tr;
		VATTR_INIT(&tr);
		VATTR_SET(&tr, va_data_size, 0);
		error = vnode_setattr(vp, &tr, ctx);
	}
	int resid = 0;
	if (!error && length) error = vn_rdwr(UIO_WRITE, vp, droidvm_export_data, (int)length, (off_t)offset, UIO_SYSSPACE, IO_SYNC,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (!error && resid) error = EIO;
	if (!error && !offset) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_SET(&va, va_mode, (mode_t)mode);
		error = vnode_setattr(vp, &va, ctx);
	}
	vnode_close(vp, FWRITE, ctx);
	return error;
}

extern vm_map_t get_task_map_reference(task_t);
extern kern_return_t vm_map_read_user(vm_map_t, vm_map_address_t, void *, vm_size_t);
extern void vm_map_deallocate(vm_map_t);

static int droidvm_export_memory(const char *pid_text, uint64_t address, uint64_t *length) {
	int pid = 0;
	for (const char *c = pid_text; *c; ++c) {
		if (*c < '0' || *c > '9' || pid > 1000000) return EINVAL;
		pid = pid * 10 + (*c - '0');
	}
	uint64_t size = MIN(*length, (uint64_t)DROIDVM_EXPORT_BYTES);
	*length = 0;
	proc_t p = pid > 0 ? proc_find(pid) : PROC_NULL;
	if (p == PROC_NULL) return ESRCH;
	vm_map_t map = get_task_map_reference(proc_task(p));
	proc_rele(p);
	if (!map) return ESRCH;
	kern_return_t kr = vm_map_read_user(map, (vm_map_address_t)address, droidvm_export_data, (vm_size_t)size);
	vm_map_deallocate(map);
	if (kr != KERN_SUCCESS) return EFAULT;
	*length = size;
	return 0;
}

struct droidvm_export_cursor {
	uint64_t used, count;
};

static int droidvm_export_process(proc_t p, void *argument) {
	struct droidvm_export_cursor *cursor = argument;
	int n = snprintf(droidvm_export_data + cursor->used, DROIDVM_EXPORT_BYTES - cursor->used, "%d %d %s\n",
	    proc_pid(p), proc_ppid(p), proc_best_name(p));
	if (n > 0 && cursor->used + (uint64_t)n < DROIDVM_EXPORT_BYTES) {
		cursor->used += (uint64_t)n;
		cursor->count++;
	}
	return PROC_RETURNED;
}

void droidvm_export_poll(void) {
	uint32_t request = __atomic_load_n(&droidvm_export_box.request, __ATOMIC_ACQUIRE);
	if (request == droidvm_export_box.done) return;
	char path[sizeof(droidvm_export_box.path)];
	memcpy(path, droidvm_export_box.path, sizeof(path));
	path[sizeof(path) - 1] = 0;
	uint64_t length = 0, total = 0;
	int status = EINVAL;
	if (droidvm_export_box.kind == 4) {
		length = droidvm_export_box.length;
		status = droidvm_export_memory(path, droidvm_export_box.offset, &length);
		total = length;
	} else if (droidvm_export_box.kind == 3) {
		int pid = (int)droidvm_export_box.offset, signal = (int)droidvm_export_box.length;
		proc_t target = pid > 1 && signal > 0 && signal < NSIG ? proc_find(pid) : PROC_NULL;
		status = ESRCH;
		if (target != PROC_NULL) {
			psignal(target, signal);
			proc_rele(target);
			status = 0;
		}
	} else if (droidvm_export_box.kind == 2) {
		struct droidvm_export_cursor cursor = {0, 0};
		proc_iterate(PROC_ALLPROCLIST | PROC_NOWAITTRANS, droidvm_export_process, &cursor, NULL, NULL);
		length = cursor.used;
		total = cursor.count;
		status = 0;
	} else if (droidvm_export_box.kind == 5) {
		status = droidvm_export_write(path, droidvm_export_box.offset, droidvm_export_box.length, droidvm_export_box.total);
		length = status ? 0 : droidvm_export_box.length;
	} else if (path[0] == '/') {
		if (droidvm_export_box.kind == 0) status = droidvm_export_file(path, droidvm_export_box.offset, &length, &total);
		else if (droidvm_export_box.kind == 1) status = droidvm_export_directory(path, droidvm_export_box.offset, &length, &total);
		else if (droidvm_export_box.kind == 6) status = droidvm_export_tree(path, droidvm_export_box.offset, &length, &total);
	}
	droidvm_export_box.status = status;
	droidvm_export_box.length = length;
	droidvm_export_box.total = total;
	__atomic_store_n(&droidvm_export_box.done, request, __ATOMIC_RELEASE);
}
