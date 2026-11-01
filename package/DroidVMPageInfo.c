// SPDX-License-Identifier: MIT
// What the host rig's file mailbox (DroidVMFileExport.c, kind 8; tools/rig export.py page) answers about one vm_page: its
// object, offset and state, its object's pager and, for a file's page, the file. A thread asleep in vm_page_sleep names its page
// only through a stackshot's PageBusy wait (the page's address permuted by vm_kernel_addrperm), and the guest's pages array is
// virtually contiguous but not physically, so the host cannot read the page itself. After a render server kill, threads of
// newly launched daemons slept on busy pages of their own executables with no page-in and no disk read in flight.
// Built with vm_dyld_pager.o's flags (osfmk); every read is unlocked: a diagnostic of pages that stay as they are.
#include <mach/mach_types.h>
#include <mach/memory_object_types.h>
#include <vm/vm_protos_internal.h>
#include <vm/vm_page_internal.h>
#include <vm/vm_object_internal.h>

extern vm_offset_t vm_kernel_addrperm;

static size_t
droidvm_page_object(vm_object_t object, const char *what, char *out, size_t size)
{
	size_t at = (size_t)snprintf(out, size, "%s %p: refs %u resident %u size %#llx paging %u activity %u created %d initialized %d "
	    "ready %d internal %d alive %d terminating %d shadow %p\n", what, object, os_ref_get_count_raw(&object->ref_count),
	    object->resident_page_count, (unsigned long long)object->vo_size, object->paging_in_progress, object->activity_in_progress,
	    object->pager_created, object->pager_initialized, object->pager_ready, object->internal, object->alive,
	    object->terminating, object->shadow);
	memory_object_t pager = object->pager;
	if (pager != MEMORY_OBJECT_NULL && at < size) {
		const char *name = pager->mo_pager_ops ? pager->mo_pager_ops->memory_object_pager_name : NULL;
		at += (size_t)snprintf(out + at, size - at, "  pager %p (%s)\n", pager, name ? name : "?");
		char path[256] = {}, file[128] = {};
		boolean_t truncated = FALSE;
		if (at < size && vnode_pager_get_object_name(pager, path, sizeof(path), file, sizeof(file), &truncated) == KERN_SUCCESS) {
			at += (size_t)snprintf(out + at, size - at, "  file %s%s (%s)\n", path, truncated ? "..." : "", file);
		}
	}
	return at < size ? at : size;
}

size_t
droidvm_page_info(uint64_t address, bool permuted, char *out, size_t size)
{
	if (permuted) {
		address -= vm_kernel_addrperm;
	}
	vm_page_t m = (vm_page_t)address;
	if (!vm_page_in_array(m)) {
		return (size_t)snprintf(out, size, "%#llx: not in the pages array\n", (unsigned long long)address);
	}
	size_t at = (size_t)snprintf(out, size, "page %p: offset %#llx queue %d busy %d wanted %d absent %d error %d clustered %d "
	    "cleaning %d laundry %d dirty %d restart %d unusual %d pmapped %d wire %u\n", m, (unsigned long long)m->vmp_offset,
	    m->vmp_q_state, m->vmp_busy, m->vmp_wanted, m->vmp_absent, m->vmp_error, m->vmp_clustered, m->vmp_cleaning,
	    m->vmp_laundry, m->vmp_dirty, m->vmp_restart, m->vmp_unusual, m->vmp_pmapped, m->vmp_wire_count);
	vm_object_t object = VM_PAGE_OBJECT(m);
	if (object == VM_OBJECT_NULL || at >= size) {
		return at < size ? at : size;
	}
	at += droidvm_page_object(object, "object", out + at, size - at);
	// A page of a pager's own object (the dyld pager's, a copy-on-write shadow's) leads to the file through the shadows.
	vm_object_t shadow = object->shadow;
	for (int depth = 0; shadow != VM_OBJECT_NULL && depth < 4 && at < size; depth++) {
		at += droidvm_page_object(shadow, "shadow", out + at, size - at);
		shadow = shadow->shadow;
	}
	return at < size ? at : size;
}
