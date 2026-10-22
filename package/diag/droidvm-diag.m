// SPDX-License-Identifier: MIT
// droidvm-diag: answers the host rig's questions about the guest's userland from inside it, where the frameworks that keep the
// state (LaunchServices, PlugInKit, the kernel's personas) can be asked directly. The rig puts a request file with
// `export.py put` (DroidVMFileExport.c writes under /private/var/ngpu/ only) and reads the answer with `export.py cat`:
//
//   request: /private/var/ngpu/diag-req[.mobile]   one command per line (below); the rig writes it with `export.py put`
//            (the kernel's file mailbox only writes under /private/var/ngpu/, and a mobile process can read it there)
//   answer:  /private/var/tmp/droidvm-diag[.mobile].out   the rig reads it with `export.py cat` (a mobile process can write
//            there; it cannot write under root-owned /private/var/ngpu). The suffix is .mobile for the uid-501 instance.
//
//   app BUNDLE_ID        the LaunchServices application record, its state and its extensions with their containing bundle
//   ext BUNDLE_ID        an application extension record and its containing bundle
//   plugin BUNDLE_ID     the PlugInKit view (LSPlugInKitProxy) of a plugin: what pkd sees
//   kvc BUNDLE_ID KEY    one key of the application record (valueForKey:)
//   apps                 every application record: bundle id, path, placeholder flag
//   points               every registered extension point (PKExtensionPoint); if a widget's point is missing, nothing hosts it
//   point IDENT          whether one named extension point is registered
//   appex BUNDLE_ID      the extension point each of an app's bundled .appex plug-ins declares (from its Info.plist)
//   personas             the kernel's personas (kpersona_find/kpersona_info) and this process's
//   whoami               uid, euid, pid
//
// launchd starts it as root under the program path of a job the board has no use for (scripts/build-droidvm-diag.py); it forks
// the mobile instance before either touches a framework. Nothing is changed by any command.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <unistd.h>

static NSString *describe(id value) {
	if (!value) return @"(nil)";
	@try {
		return [value description];
	} @catch (NSException *exception) {
		return [NSString stringWithFormat:@"<%@ description threw %@>", [value class], exception.reason];
	}
}

static id key(id object, NSString *name) {
	if (!object) return nil;
	@try {
		return [object valueForKey:name];
	} @catch (NSException *exception) {
		return [NSString stringWithFormat:@"<threw %@>", exception.name];
	}
}

static id applicationRecord(NSString *bundle, NSError **error) {
	Class cls = NSClassFromString(@"LSApplicationRecord");
	SEL sel = NSSelectorFromString(@"initWithBundleIdentifier:allowPlaceholder:error:");
	id record = ((id (*)(id, SEL, NSString *, BOOL, NSError **))objc_msgSend)([cls alloc], sel, bundle, YES, error);
	return record;
}

static void recordKeys(NSMutableString *out, id record, NSArray<NSString *> *names) {
	for (NSString *name in names) [out appendFormat:@"  %@ = %@\n", name, describe(key(record, name))];
}

static NSArray<NSString *> *kBundleKeys(void) {
	return @[ @"URL", @"bundleIdentifier", @"typeForInstallMachinery", @"applicationState", @"isPlaceholder", @"isSystemPlaceholder",
		  @"isDeletable", @"isLaunchProhibited", @"isBeta", @"developerType", @"persistentIdentifier", @"executableURL",
		  @"managedPersonas", @"bundleVersion" ];
}

static void commandApp(NSMutableString *out, NSString *bundle) {
	NSError *error = nil;
	id record = applicationRecord(bundle, &error);
	[out appendFormat:@"app %@: %@ error %@\n", bundle, [record class], describe(error)];
	if (!record) return;
	recordKeys(out, record, kBundleKeys());
	id extensions = key(record, @"applicationExtensionRecords");
	for (id extension in extensions) {
		[out appendFormat:@" extension %@ at %@\n", describe(key(extension, @"bundleIdentifier")), describe(key(extension, @"URL"))];
		id parent = key(extension, @"containingBundleRecord");
		[out appendFormat:@"  containingBundleRecord = %@ %@\n", [parent class], describe(key(parent, @"bundleIdentifier"))];
		[out appendFormat:@"  extensionPointRecord = %@\n", describe(key(key(extension, @"extensionPointRecord"), @"identifier"))];
	}
}

static void commandExtension(NSMutableString *out, NSString *bundle) {
	NSError *error = nil;
	Class cls = NSClassFromString(@"LSApplicationExtensionRecord");
	id record = ((id (*)(id, SEL, NSString *, NSError **))objc_msgSend)([cls alloc], NSSelectorFromString(@"initWithBundleIdentifier:error:"), bundle,
										   &error);
	[out appendFormat:@"ext %@: %@ error %@\n", bundle, [record class], describe(error)];
	if (!record) return;
	recordKeys(out, record, @[ @"URL", @"containingBundleRecord", @"extensionPointRecord", @"persistentIdentifier" ]);
	id parent = key(record, @"containingBundleRecord");
	if (parent) recordKeys(out, parent, kBundleKeys());
}

static void commandPlugin(NSMutableString *out, NSString *bundle) {
	Class cls = NSClassFromString(@"LSPlugInKitProxy");
	id proxy = ((id (*)(id, SEL, NSString *))objc_msgSend)(cls, NSSelectorFromString(@"pluginKitProxyForIdentifier:"), bundle);
	[out appendFormat:@"plugin %@: %@\n", bundle, [proxy class]];
	if (!proxy) return;
	recordKeys(out, proxy, @[ @"bundleURL", @"containingBundle", @"pluginIdentifier", @"protocol", @"isOnSystemPartition", @"registrationDate" ]);
	id parent = key(proxy, @"containingBundle");
	if (parent) recordKeys(out, parent, @[ @"bundleURL", @"bundleIdentifier", @"bundleType", @"applicationType" ]);
}

static void commandApps(NSMutableString *out) {
	Class cls = NSClassFromString(@"LSApplicationRecord");
	id enumerator = ((id (*)(id, SEL, unsigned long))objc_msgSend)(cls, NSSelectorFromString(@"enumeratorWithOptions:"), 0);
	unsigned count = 0;
	for (id record in enumerator) {
		[out appendFormat:@"%@ %@ placeholder=%@ type=%@\n", describe(key(record, @"bundleIdentifier")), [describe(key(record, @"URL"))
								 stringByReplacingOccurrencesOfString:@"file://" withString:@""],
				  describe(key(key(record, @"applicationState"), @"isPlaceholder")), describe(key(record, @"typeForInstallMachinery"))];
		++count;
	}
	[out appendFormat:@"%u records\n", count];
}

struct droidvm_kpersona_info {   // xnu bsd/sys/persona.h (version 2)
	uint32_t persona_info_version;
	uint32_t persona_id;
	int persona_type;
	uint32_t persona_gid;
	uint32_t persona_ngroups;
	uint32_t persona_groups[16];
	uint32_t persona_gmuid;
	char persona_name[256];
	uint32_t persona_uid;
};

// Every registered extension point, as PlugInKit sees them. If the widgets' extension points are absent here, nothing can host
// them. PKExtensionPoint is in PlugInKit; its +allExtensionPoints returns the registered points.
static void commandPoints(NSMutableString *out) {
	dlopen("/System/Library/PrivateFrameworks/PlugInKit.framework/PlugInKit", RTLD_NOW | RTLD_LOCAL);
	dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW | RTLD_LOCAL);
	Class cls = NSClassFromString(@"PKExtensionPoint");
	if (!cls) {
		[out appendString:@"PKExtensionPoint unavailable\n"];
		return;
	}
	id all = ((id (*)(id, SEL))objc_msgSend)(cls, NSSelectorFromString(@"allExtensionPoints"));
	[out appendFormat:@"PKExtensionPoint allExtensionPoints: %@ (%lu)\n", [all class], (unsigned long)[all count]];
	NSArray *points = [all isKindOfClass:NSArray.class] ? all : [all respondsToSelector:@selector(allValues)] ? [all allValues] : nil;
	for (id point in points) [out appendFormat:@"  %@\n", describe(key(point, @"identifier"))];
}

// Is one named extension point registered?  PKExtensionPoint +extensionPointWithIdentifier:
static void commandPoint(NSMutableString *out, NSString *identifier) {
	Class cls = NSClassFromString(@"PKExtensionPoint");
	id point = cls ? ((id (*)(id, SEL, NSString *))objc_msgSend)(cls, NSSelectorFromString(@"extensionPointWithIdentifier:"), identifier) : nil;
	[out appendFormat:@"point %@ = %@ %@\n", identifier, [point class], describe(key(point, @"identifier"))];
}

// What extension point each of an app's bundled .appex plug-ins declares, read straight from its Info.plist (NSExtension ->
// NSExtensionPointIdentifier). This is what the extension asks for; commandPoint says whether the system offers it.
static void commandAppex(NSMutableString *out, NSString *bundleOrPath) {
	// A path (/Applications/Weather.app) is read straight from disk; a bundle id is resolved through LaunchServices first.
	NSString *path = [bundleOrPath hasPrefix:@"/"] ? bundleOrPath : nil;
	if (!path) {
		NSError *error = nil;
		id url = key(applicationRecord(bundleOrPath, &error), @"URL");
		path = [url isKindOfClass:NSURL.class] ? [url path] : nil;
		if (!path) {
			[out appendFormat:@"appex %@: no app URL (%@)\n", bundleOrPath, describe(error)];
			return;
		}
	}
	NSString *plugins = [path stringByAppendingPathComponent:@"PlugIns"];
	for (NSString *entry in [NSFileManager.defaultManager contentsOfDirectoryAtPath:plugins error:nil] ?: @[]) {
		NSString *info = [[plugins stringByAppendingPathComponent:entry] stringByAppendingPathComponent:@"Info.plist"];
		NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:info];
		NSDictionary *ns = plist[@"NSExtension"];
		[out appendFormat:@"  %@: id %@ point %@ principal %@\n", entry, plist[@"CFBundleIdentifier"],
			  ns[@"NSExtensionPointIdentifier"], ns[@"NSExtensionPrincipalClass"] ?: ns[@"NSExtensionMainStoryboard"]];
	}
}

static void commandPersonas(NSMutableString *out) {
	int (*find)(const char *, uid_t, uint32_t *, size_t *) = dlsym(RTLD_DEFAULT, "kpersona_find");
	int (*info)(uint32_t, struct droidvm_kpersona_info *) = dlsym(RTLD_DEFAULT, "kpersona_info");
	int (*mine)(uint32_t *) = dlsym(RTLD_DEFAULT, "kpersona_get");
	if (!find || !info) {
		[out appendString:@"kpersona_find/kpersona_info missing\n"];
		return;
	}
	uint32_t ids[64];
	size_t count = 64;
	int status = find(NULL, (uid_t)-1, ids, &count);
	[out appendFormat:@"kpersona_find(any) = %d errno %d count %zu\n", status, errno, count];
	for (size_t i = 0; status >= 0 && i < count && i < 64; ++i) {
		struct droidvm_kpersona_info entry = {.persona_info_version = 2};
		int got = info(ids[i], &entry);
		[out appendFormat:@" persona %u: %d type %d uid %u name %s\n", ids[i], got, entry.persona_type, entry.persona_uid, entry.persona_name];
	}
	uint32_t own = 0;
	if (mine) [out appendFormat:@"kpersona_get = %d id %u\n", mine(&own), own];
}

static void run(FILE *answer, NSString *request) {
	for (NSString *line in [request componentsSeparatedByString:@"\n"]) {
		NSMutableString *out = [NSMutableString string];
		NSArray<NSString *> *words = [[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]
		    componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		NSString *command = words.firstObject;
		if (!command.length) continue;
		@try {
			if ([command isEqualToString:@"app"] && words.count > 1) commandApp(out, words[1]);
			else if ([command isEqualToString:@"ext"] && words.count > 1) commandExtension(out, words[1]);
			else if ([command isEqualToString:@"plugin"] && words.count > 1) commandPlugin(out, words[1]);
			else if ([command isEqualToString:@"kvc"] && words.count > 2) {
				NSError *error = nil;
				[out appendFormat:@"%@ %@ = %@\n", words[1], words[2], describe(key(applicationRecord(words[1], &error), words[2]))];
			} else if ([command isEqualToString:@"apps"]) commandApps(out);
			else if ([command isEqualToString:@"points"]) commandPoints(out);
			else if ([command isEqualToString:@"point"] && words.count > 1) commandPoint(out, words[1]);
			else if ([command isEqualToString:@"appex"] && words.count > 1) commandAppex(out, words[1]);
			else if ([command isEqualToString:@"personas"]) commandPersonas(out);
			else if ([command isEqualToString:@"whoami"]) [out appendFormat:@"uid %u euid %u pid %d\n", getuid(), geteuid(), getpid()];
			else [out appendFormat:@"unknown command: %@\n", line];
		} @catch (NSException *exception) {
			[out appendFormat:@"%@ threw %@: %@\n", line, exception.name, exception.reason];
		}
		// Flush after every command: a private LaunchServices/PlugInKit selector can crash with a signal @catch cannot trap, and
		// the lines already written then show which command did it.
		fputs(out.UTF8String, answer);
		fflush(answer);
	}
}

static void serve(NSString *suffix) {
	NSString *requestPath = [NSString stringWithFormat:@"/private/var/ngpu/diag-req%@", suffix];
	NSString *answerPath = [NSString stringWithFormat:@"/private/var/tmp/droidvm-diag%@.out", suffix];
	for (;;) {
		@autoreleasepool {
			NSString *request = [NSString stringWithContentsOfFile:requestPath encoding:NSUTF8StringEncoding error:nil];
			if (request) {
				unlink(requestPath.fileSystemRepresentation);
				// Run in this process: LaunchServices and PlugInKit keep XPC connections that do not survive a bare fork().
				// A private selector can still crash with a signal @catch cannot trap, but run() flushes after every command,
				// so the answer already holds the lines up to the crash; launchd relaunches us, and the request is gone, so we
				// do not hit the same crash twice.
				FILE *answer = fopen(answerPath.fileSystemRepresentation, "w");
				if (answer) {
					run(answer, request);
					fclose(answer);
				}
			}
		}
		sleep(2);
	}
}

int main(void) {
	if (getuid() == 0 && fork() == 0) {
		if (setgid(501) || setuid(501)) _exit(1);
		@autoreleasepool {
			serve(@".mobile");
		}
	}
	@autoreleasepool {
		serve(getuid() ? @".mobile" : @"");
	}
	return 0;
}
