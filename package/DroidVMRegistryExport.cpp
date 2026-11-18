// SPDX-License-Identifier: MIT
// The IORegistry for the host rig's file mailbox (DroidVMFileExport.c kind 9; tools/rig export.py ioreg): a guest whose user space
// never came up -- a black screen, no network, no shell -- has no ioreg to run. One request walks the service plane in the kernel
// and writes it as text, one line per entry in the shape of `ioreg`:
//
//   +-o pci1af4,1041@2  <class IOPCIDevice, id 0x1000001af, registered, matched, active, busy 0, retain 12>
//       "IOInterruptSpecifiers" = (<4000000005000100>,...)
//
// the properties only under the entries the request names (path: "" none, "*" all, else every entry whose name or class contains
// that text, and the entries below it). Taken whole into a buffer of its own at offset 0, which later offsets read on.
//
// Properties are copied with dictionaryWithProperties(), never read through getProperty(): an IOPCIDevice answers a read of its
// IOInterruptSpecifiers by fixing its interrupt vectors (IOPCIFamily's resolveInterrupts), which would change what is being looked at.
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <sys/errno.h>
#include <stdarg.h>

#define DROIDVM_REGISTRY_BYTES (8u << 20)

static char *droidvm_registry;
static size_t droidvm_registry_used;

namespace {

struct Text {
	char *buffer;
	size_t size, used;
	bool full;
	// one piece at a time through a line-sized buffer: a piece that does not fit ends the text with a note
	void add(const char *format, ...) __printflike(2, 3)
	{
		if (full) return;
		char piece[1024];
		va_list arguments;
		va_start(arguments, format);
		int n = vsnprintf(piece, sizeof(piece), format, arguments);
		va_end(arguments);
		if (n < 0) return;
		size_t length = (size_t)n < sizeof(piece) ? (size_t)n : sizeof(piece) - 1;
		static const char note[] = "\n[truncated: the buffer is full]\n";
		if (used + length + sizeof(note) > size) {
			memcpy(buffer + used, note, sizeof(note));
			used += sizeof(note) - 1;
			full = true;
			return;
		}
		memcpy(buffer + used, piece, length);
		used += length;
		buffer[used] = 0;
	}
};

// a substring test without the C library's strnstr, whose header the IOKit flags do not bring in
bool contains(const char *text, const char *part)
{
	if (!text) return false;
	size_t n = strlen(part);
	for (; *text; text++) if (!strncmp(text, part, n)) return true;
	return false;
}

void indent(Text &text, unsigned depth)
{
	for (unsigned i = 0; i < depth; i++) text.add("  ");
}

void value(Text &text, OSObject *object)
{
	if (OSString *string = OSDynamicCast(OSString, object)) {
		text.add("\"%.240s\"%s", string->getCStringNoCopy(), string->getLength() > 240 ? "..." : "");
	} else if (OSNumber *number = OSDynamicCast(OSNumber, object)) {
		unsigned long long n = number->unsigned64BitValue();
		if (n < 10) text.add("%llu", n);
		else text.add("%llu (%#llx)", n, n);
	} else if (OSBoolean *boolean = OSDynamicCast(OSBoolean, object)) {
		text.add("%s", boolean->isTrue() ? "Yes" : "No");
	} else if (OSData *data = OSDynamicCast(OSData, object)) {
		const uint8_t *bytes = (const uint8_t *)data->getBytesNoCopy();
		unsigned length = data->getLength(), shown = length < 64 ? length : 64;
		text.add("<");
		for (unsigned i = 0; bytes && i < shown; i++) text.add("%02x", bytes[i]);
		text.add(length > shown ? "...> (%u bytes)" : ">", length);
	} else if (object) {
		// arrays, dictionaries, sets: their XML, cut short
		OSSerialize *serial = OSSerialize::withCapacity(512);
		if (serial && object->serialize(serial)) {
			const char *xml = serial->text();
			size_t length = strlen(xml);
			text.add("%.600s%s", xml, length > 600 ? "..." : "");
		} else {
			text.add("<%s>", object->getMetaClass()->getClassName());
		}
		OSSafeReleaseNULL(serial);
	}
}

void properties(Text &text, IORegistryEntry *entry, unsigned depth)
{
	OSDictionary *copy = entry->dictionaryWithProperties();
	if (!copy) return;
	if (OSCollectionIterator *keys = OSCollectionIterator::withCollection(copy)) {
		while (OSSymbol *key = OSDynamicCast(OSSymbol, keys->getNextObject())) {
			indent(text, depth + 2);
			text.add("\"%s\" = ", key->getCStringNoCopy());
			value(text, copy->getObject(key));
			text.add("\n");
		}
		keys->release();
	}
	copy->release();
}

bool named(IORegistryEntry *entry, const char *filter)
{
	if (!filter[0]) return false;
	if (filter[0] == '*' && !filter[1]) return true;
	const char *name = entry->getName(gIOServicePlane), *kind = entry->getMetaClass()->getClassName();
	return contains(name, filter) || contains(kind, filter);
}

void walk(Text &text, const char *filter)
{
	IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
	if (!it) return;
	// entries below one the filter names get their properties too: the depth at which that subtree began, or ~0u
	unsigned subtree = ~0u;
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		text.used = 0; text.full = false; subtree = ~0u;
		text.buffer[0] = 0;
		it->reset();
		while (IORegistryEntry *entry = it->getNextObject()) {
			unsigned depth = entry->getDepth(gIOServicePlane);
			if (depth <= subtree) subtree = ~0u;
			if (subtree == ~0u && named(entry, filter)) subtree = depth;
			indent(text, depth);
			const char *location = entry->getLocation(gIOServicePlane);
			text.add("+-o %s%s%s  <class %s, id %#llx", entry->getName(gIOServicePlane), location ? "@" : "", location ? location : "",
			    entry->getMetaClass()->getClassName(), entry->getRegistryEntryID());
			if (IOService *service = OSDynamicCast(IOService, entry)) {
				IOOptionBits state = service->getState();
				text.add(", %sregistered, %smatched, %s, busy %u", (state & kIOServiceRegisteredState) ? "" : "!",
				    (state & kIOServiceMatchedState) ? "" : "!", service->isInactive() ? "inactive" : "active",
				    (unsigned)service->getBusyState());
			}
			text.add(", retain %d>\n", entry->getRetainCount());
			if (subtree != ~0u) properties(text, entry, depth);
		}
		if (it->isValid()) break;
		// the registry changed under the walk: take it again
	}
	it->release();
}

} // namespace

// Kind 9: the text of the service plane, `filter` choosing whose properties go along, from `offset` (total = its size).
extern "C" int droidvm_export_registry(const char *filter, uint64_t offset, char *out, uint64_t room, uint64_t *length, uint64_t *total)
{
	if (offset == 0) {
		if (!droidvm_registry) droidvm_registry = (char *)IOMallocData(DROIDVM_REGISTRY_BYTES);
		if (!droidvm_registry) return ENOMEM;
		Text text = {droidvm_registry, DROIDVM_REGISTRY_BYTES, 0, false};
		walk(text, filter);
		droidvm_registry_used = text.used;
	}
	if (!droidvm_registry || offset > droidvm_registry_used) return EINVAL;
	uint64_t count = droidvm_registry_used - offset;
	if (count > room) count = room;
	memcpy(out, droidvm_registry + offset, (size_t)count);
	*length = count;
	*total = droidvm_registry_used;
	return 0;
}
