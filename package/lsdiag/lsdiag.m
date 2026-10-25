// lsdiag: ask LaunchServices what it has registered, from inside the guest.
//
//   lsdiag [-u UID] app BUNDLE_ID...     application record: URL, placeholder/restricted/installed state, extensions
//   lsdiag [-u UID] ext BUNDLE_ID...     extension record and plug-in proxy: URL and the containing bundle
//   lsdiag [-u UID] restrictions [BUNDLE_ID...]   LSApplicationRestrictionsManager's lists, and per-app verdicts
//   lsdiag [-u UID] rebuild [nosystem] [internal] [nouser]   rebuild the LaunchServices database (system and user apps)
//   lsdiag [-u UID] register APP_PATH...   re-register app bundles by URL (their plug-ins go under them)
//   lsdiag owner-install APP_PATH...      move trusted org.droidvm apps into MCM bundle containers, keep CLI symlinks
//   lsdiag -u 501 owner-register APP_PATH...   register those trusted apps with installation metadata
//   lsdiag owner-update APP_PATH FROM_DIR   replace files of an installed org.droidvm app with FROM_DIR's (same relative paths):
//                                         a re-signed Info.plist, executable and CodeResources, without moving the whole bundle again
//   lsdiag -u 501 launch BUNDLE_ID...      request a foreground launch (device must be unlocked)
//   lsdiag -u 501 launch-args OUT BUNDLE_ID [ARG...]   launch an app as a debugger does (FrontBoard's open-application service
//                                       with debug options): ARGs on its command line, any running instance ended first, its
//                                       standard output and error in OUT.out and OUT.err, and NSLog copied to them
//   lsdiag [-u UID] midump [FILTER...]    the item dictionaries installd gives LaunchServices (type, plug-in owner)
//   lsdiag [-u UID] relink [-clean] BUNDLE_ID...|all   re-register apps with their plug-ins as an install does (plug-ins get
//                                       their app); -clean drops each plug-in's records first (unregisterPlugin:)
//   lsdiag bundle PATH...                what CFBundle reads from a bundle directory (identifier, Info.plist, extension point)
//   lsdiag nodes PATH...                 stat/getattrlist IDs of each path and whether fsgetpath leads back to it
//   lsdiag fsnode PATH...                what LaunchServices' FSNode answers: path, length, file and volume IDs
//   lsdiag [-u UID] boot [SERVICE...]    this process's bootstrap port, and a bootstrap_look_up of each service
//   lsdiag [-u UID] container [-plugin] [-create] ID...   the data container MobileContainerManager keeps for each
//                                       identifier, whether it is on disk; -create makes a missing one
//   lsdiag [-u UID] point ID...          an extension point's LaunchServices record and the extensions enumerated for it
//   lsdiag protect [-set] PATH...        data protection: the mount's, fcntl's class, NSURLFileProtectionKey (-set: None)
//   lsdiag tree PATH [DEPTH]             a directory tree where only an entitled process may look (app containers)
//   lsdiag show PATH...                  a file: a property list printed, anything else as text
//   lsdiag procs [FILTER]                processes: pid, ppid, uid, p_comm, p_name (the GPU grant's name), executable path
//   lsdiag copy SRC DST                  a file copied where the shell may read it (out of an app container)
//   lsdiag image PATH [OUT.png]          an image as ImageIO decodes it: type, size, pixel mean and range; written as PNG
//   lsdiag remove PATH...                removes files or directories there (an app's broken data store)
//
// The records come from the private LaunchServices classes, looked up at run time so nothing private is linked; a
// selector a class does not answer prints as "n/a". -u switches to that user first (501 = mobile, whose database the
// apps and widgets use), since root sees the system domain. A leading -c runs the query in a coalition of its own.
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <dlfcn.h>
#import <mach/mach.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <spawn.h>
#import <sys/attr.h>
#import <sys/mount.h>
#import <sys/stat.h>
#import <fcntl.h>
#import <sys/wait.h>
#import <unistd.h>
#import <mach-o/loader.h>
#import <libkern/OSByteOrder.h>

static id call(id target, const char *name) {
    SEL sel = sel_registerName(name);
    if (!target || ![target respondsToSelector:sel])
        return nil;
    return ((id (*)(id, SEL))objc_msgSend)(target, sel);
}

// An unsigned property such as a record's platform (2 = iOS); -1 when the object does not answer it.
static long uintProperty(id target, const char *name) {
    SEL sel = sel_registerName(name);
    if (!target || ![target respondsToSelector:sel])
        return -1;
    return (long)((unsigned int (*)(id, SEL))objc_msgSend)(target, sel);
}

static NSString *flag(id target, const char *name) {
    SEL sel = sel_registerName(name);
    if (!target || ![target respondsToSelector:sel])
        return @"n/a";
    return ((BOOL (*)(id, SEL))objc_msgSend)(target, sel) ? @"YES" : @"no";
}

static void line(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);
static void line(NSString *format, ...) {
    va_list args;
    va_start(args, format);
    NSString *text = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    printf("%s\n", text.UTF8String);
}

static void describeState(NSString *indent, id state) {
    line(@"%@state: installed=%@ placeholder=%@ restricted=%@ valid=%@ blocked=%@ removed=%@", indent,
         flag(state, "isInstalled"), flag(state, "isPlaceholder"), flag(state, "isRestricted"), flag(state, "isValid"),
         flag(state, "isBlocked"), flag(state, "isRemovedSystemApp"));
}

static void app(NSString *identifier) {
    Class recordClass = NSClassFromString(@"LSApplicationRecord");
    NSError *error = nil;
    id record = ((id (*)(id, SEL, NSString *, BOOL, NSError **))objc_msgSend)(
        [recordClass alloc], sel_registerName("initWithBundleIdentifier:allowPlaceholder:error:"), identifier, YES, &error);
    line(@"app %@", identifier);
    if (!record) {
        line(@"  no record: %@", error);
        return;
    }
    line(@"  class=%@ URL=%@ platform=%ld", NSStringFromClass([record class]), [call(record, "URL") path],
         uintProperty(record, "platform"));
    line(@"  placeholder=%@ deletable=%@ deletableSystemApp=%@ systemPlaceholder=%@ typeForInstallMachinery=%@",
         flag(record, "isPlaceholder"), flag(record, "isDeletable"), flag(record, "isDeletableSystemApplication"),
         flag(record, "isSystemPlaceholder"), call(record, "typeForInstallMachinery"));
    describeState(@"  ", call(record, "applicationState"));
    for (id extension in call(record, "applicationExtensionRecords"))
        line(@"  extension %@ at %@", call(extension, "bundleIdentifier"), [call(extension, "URL") path]);

    Class proxyClass = NSClassFromString(@"LSApplicationProxy");
    id proxy = ((id (*)(id, SEL, NSString *))objc_msgSend)(proxyClass, sel_registerName("applicationProxyForIdentifier:"), identifier);
    line(@"  proxy: installed=%@ placeholder=%@ restricted=%@ plug-ins=%lu", flag(proxy, "isInstalled"),
         flag(proxy, "isPlaceholder"), flag(proxy, "isRestricted"), (unsigned long)[call(proxy, "plugInKitPlugins") count]);
}

static void extension(NSString *identifier) {
    Class recordClass = NSClassFromString(@"LSApplicationExtensionRecord");
    NSError *error = nil;
    id record = ((id (*)(id, SEL, NSString *, NSError **))objc_msgSend)(
        [recordClass alloc], sel_registerName("initWithBundleIdentifier:error:"), identifier, &error);
    line(@"ext %@", identifier);
    if (!record) {
        line(@"  no record: %@", error);
    } else {
        line(@"  URL=%@ point=%@", [call(record, "URL") path], call(call(record, "extensionPointRecord"), "identifier"));
        id container = call(record, "containingBundleRecord");
        line(@"  containingBundleRecord: %@ %@ at %@", container ? NSStringFromClass([container class]) : @"(nil)",
             call(container, "bundleIdentifier"), [call(container, "URL") path]);
        if (container)
            describeState(@"    ", call(container, "applicationState"));
    }
    Class proxyClass = NSClassFromString(@"LSPlugInKitProxy");
    id proxy = ((id (*)(id, SEL, NSString *))objc_msgSend)(proxyClass, sel_registerName("pluginKitProxyForIdentifier:"), identifier);
    id containing = call(proxy, "containingBundle");
    line(@"  plug-in proxy: %@ URL=%@ protocol=%@ platform=%ld containingBundle=%@ at %@", proxy ? @"found" : @"(nil)",
         [call(proxy, "bundleURL") path], call(proxy, "protocol"), uintProperty(proxy, "platform"),
         call(containing, "bundleIdentifier"), [call(containing, "bundleURL") path]);
}

// Every plug-in LaunchServices offers for an extension point (its "protocol"), the way chronod's
// discovery sees them: each plug-in's bundle id, extension point, and whether its containing bundle resolves (nil is what
// pkd rejects on). Uses LSApplicationWorkspace, looked up at run time.
static void plugins(NSArray<NSString *> *points) {
    id workspace = call(NSClassFromString(@"LSApplicationWorkspace"), "defaultWorkspace");
    id all = call(workspace, "installedPlugins");
    if (!all) {
        line(@"no installedPlugins (workspace %@)", workspace ? @"ok" : @"nil");
        return;
    }
    // No arg (or "all"): histogram of every plug-in's extension-point identifier, so a missing point stands out.
    if (points.count == 0 || [points[0] isEqualToString:@"all"]) {
        NSCountedSet *hist = [NSCountedSet set];
        for (id proxy in all) {
            // The extension point of an LSPlugInKitProxy is its "protocol" (NSExtensionPointIdentifier); a proxy that does
            // not answer it is counted apart, so a missing selector is never mistaken for a nil point.
            NSString *pt = [proxy respondsToSelector:sel_registerName("protocol")] ? (call(proxy, "protocol") ?: @"(nil)") : @"(n/a)";
            [hist addObject:pt];
        }
        line(@"%lu plug-ins installed, by extension point:", (unsigned long)[all count]);
        NSArray *sorted = [hist.allObjects sortedArrayUsingComparator:^(id a, id b) {
            return [@([hist countForObject:b]) compare:@([hist countForObject:a])];
        }];
        for (NSString *pt in sorted)
            line(@"  %4lu  %@", (unsigned long)[hist countForObject:pt], pt);
        return;
    }
    NSString *point = points[0];
    line(@"extension point %@: %lu plug-ins installed total", point, (unsigned long)[all count]);
    NSUInteger shown = 0;
    for (id proxy in all) {
        NSString *pt = call(proxy, "protocol");
        if (![pt isEqualToString:point]) continue;
        shown++;
        id containing = call(proxy, "containingBundle");
        line(@"  %@ containing=%@ at %@ restricted=%@", call(proxy, "pluginIdentifier"),
             call(containing, "bundleIdentifier") ?: @"(nil)", [call(proxy, "bundleURL") path], flag(proxy, "isRestricted"));
    }
    line(@"  (%lu for %@)", (unsigned long)shown, point);
}

// Ask LaunchServices to rebuild its database the way `mobile_install rebuild system user` does on an internal build:
// re-register the system apps (and their plug-ins under them) and the user's. Needs the rebuildappdatabases entitlement.
static void rebuild(NSArray<NSString *> *args) {
    BOOL system = YES, internal = NO, user = YES;
    for (NSString *a in args) {
        if ([a isEqualToString:@"nosystem"]) system = NO;
        if ([a isEqualToString:@"internal"]) internal = YES;
        if ([a isEqualToString:@"nouser"]) user = NO;
    }
    id workspace = call(NSClassFromString(@"LSApplicationWorkspace"), "defaultWorkspace");
    SEL sel = sel_registerName("_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:");
    if (![workspace respondsToSelector:sel]) {
        line(@"rebuild: LSApplicationWorkspace does not answer %s", sel_getName(sel));
        return;
    }
    line(@"rebuild: system=%d internal=%d user=%d ...", system, internal, user);
    BOOL ok = ((BOOL (*)(id, SEL, BOOL, BOOL, BOOL))objc_msgSend)(workspace, sel, system, internal, user);
    line(@"rebuild: returned %@", ok ? @"YES" : @"NO");
}

// Register app bundles by URL (-[LSApplicationWorkspace registerApplication:]): LaunchServices then builds the registration
// from the bundle directory itself, which takes the app's PlugIns/ and Extensions/ along with the app, each under it.
static void registerApps(NSArray<NSString *> *paths) {
    id workspace = call(NSClassFromString(@"LSApplicationWorkspace"), "defaultWorkspace");
    SEL sel = sel_registerName("registerApplication:");
    // The workspace wrapper reduces LSRegisterURL's OSStatus to BOOL. Preserve
    // that status so a failed registration can be diagnosed without guessing.
    int32_t (*registerURL)(CFURLRef, Boolean) = (int32_t (*)(CFURLRef, Boolean))dlsym(RTLD_DEFAULT, "LSRegisterURL");
    for (NSString *path in paths) {
        if (![workspace respondsToSelector:sel]) {
            line(@"register: LSApplicationWorkspace does not answer registerApplication:");
            return;
        }
        NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
        if (registerURL) {
            int32_t status = registerURL((CFURLRef)url, false);
            line(@"register %@: %@ (OSStatus %d)", path, status == 0 ? @"YES" : @"NO", status);
        } else {
            BOOL ok = ((BOOL (*)(id, SEL, NSURL *))objc_msgSend)(workspace, sel, url);
            line(@"register %@: %@ (LSRegisterURL unavailable)", path, ok ? @"YES" : @"NO");
        }
    }
}

// Owner-built, already trusted apps live on persistent Data outside installd's
// bundle directory. Supply installation metadata explicitly for this narrow path.
// registerApplicationDictionary: is also used by UIKitTools/TrollStore's uicache;
// this does not validate or add a code signature to the kernel trust cache.
static NSDictionary *ownerEntitlements(NSString *path) {
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    if (data.length < sizeof(struct mach_header_64)) return nil;
    const uint8_t *bytes = data.bytes;
    struct mach_header_64 header;
    memcpy(&header, bytes, sizeof(header));
    if (header.magic != MH_MAGIC_64 || header.sizeofcmds > data.length - sizeof(header)) return nil;
    size_t at = sizeof(header), end = at + header.sizeofcmds;
    for (uint32_t i = 0; i < header.ncmds; i++) {
        if (at > end || end - at < sizeof(struct load_command)) return nil;
        struct load_command cmd;
        memcpy(&cmd, bytes + at, sizeof(cmd));
        if (cmd.cmdsize < sizeof(cmd) || cmd.cmdsize > end - at) return nil;
        if (cmd.cmd == LC_CODE_SIGNATURE && cmd.cmdsize >= sizeof(struct linkedit_data_command)) {
            struct linkedit_data_command signature;
            memcpy(&signature, bytes + at, sizeof(signature));
            size_t off = signature.dataoff, len = signature.datasize;
            if (off > data.length || len > data.length - off || len < 12) return nil;
            const uint8_t *blob = bytes + off;
            uint32_t magic = OSReadBigInt32(blob, 0), total = OSReadBigInt32(blob, 4), count = OSReadBigInt32(blob, 8);
            if (magic != 0xfade0cc0 || total > len || total < 12 || count > (total - 12) / 8) return nil;
            for (uint32_t j = 0; j < count; j++) {
                if (OSReadBigInt32(blob, 12 + j * 8) != 5) continue;
                uint32_t entry = OSReadBigInt32(blob, 16 + j * 8);
                if (entry > total || total - entry < 8 || OSReadBigInt32(blob, entry) != 0xfade7171) return nil;
                uint32_t length = OSReadBigInt32(blob, entry + 4);
                if (length < 8 || length > total - entry) return nil;
                NSData *xml = [NSData dataWithBytes:blob + entry + 8 length:length - 8];
                id plist = [NSPropertyListSerialization propertyListWithData:xml options:0 format:NULL error:nil];
                return [plist isKindOfClass:[NSDictionary class]] ? plist : nil;
            }
            return nil;
        }
        at += cmd.cmdsize;
    }
    return nil;
}

static NSURL *ownerContainer(NSString *className, NSString *identifier, BOOL create) {
    dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
    Class cls = NSClassFromString(className);
    SEL lookup = sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:");
    if (![cls respondsToSelector:lookup]) { line(@"owner container: unavailable %@", className); return nil; }
    NSError *error = nil;
    BOOL existed = NO;
    id container = ((id (*)(Class, SEL, NSString *, BOOL, BOOL *, NSError **))objc_msgSend)(cls, lookup, identifier, create, &existed, &error);
    NSURL *url = call(container, "url");
    if (!url) line(@"owner container %@ %@: %@", className, identifier, error);
    return url;
}

static void installOwnerApps(NSArray<NSString *> *paths) {
    for (NSString *input in paths) {
        NSString *source = input.stringByResolvingSymlinksInPath.stringByStandardizingPath;
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[source stringByAppendingPathComponent:@"Info.plist"]];
        NSString *identifier = info[@"CFBundleIdentifier"];
        if (!([source hasPrefix:@"/private/var/ngpu/apps/"] || [source hasPrefix:@"/var/ngpu/apps/"]) ||
            ![source.pathExtension isEqualToString:@"app"] || ![identifier isKindOfClass:[NSString class]] || ![identifier hasPrefix:@"org.droidvm."]) {
            line(@"owner-install %@: expected an unmoved org.droidvm app in ngpu/apps", input); continue;
        }
        NSURL *container = ownerContainer(@"MCMAppContainer", identifier, YES);
        NSURL *dataContainer = ownerContainer(@"MCMAppDataContainer", identifier, YES);
        if (!container || !dataContainer) continue;
        NSString *destination = [container.path stringByAppendingPathComponent:source.lastPathComponent];
        struct stat src, dst, existing;
        if (stat(source.fileSystemRepresentation, &src) || stat(container.fileSystemRepresentation, &dst) || src.st_dev != dst.st_dev) {
            line(@"owner-install %@: source and container must exist on the same volume", input); continue;
        }
        if (lstat(destination.fileSystemRepresentation, &existing) == 0 || errno != ENOENT) {
            line(@"owner-install %@: destination already exists or cannot be checked: %@", input, destination); continue;
        }
        if (rename(source.fileSystemRepresentation, destination.fileSystemRepresentation)) {
            line(@"owner-install %@: rename: %s", input, strerror(errno)); continue;
        }
        if (symlink(destination.fileSystemRepresentation, source.fileSystemRepresentation))
            line(@"owner-install %@: moved but CLI symlink failed: %s", input, strerror(errno));
        line(@"owner-install %@ -> %@; data %@", identifier, destination, dataContainer.path);
    }
}

// Root cannot write an MCM bundle container; this process's AppBundles storage entitlement can. Each file is written beside its
// target and renamed over it, keeping the source's mode. FROM_DIR must be under /private/var/ngpu (where export.py puts files).
static void updateOwnerApp(NSArray<NSString *> *words) {
    if (words.count != 2) { line(@"owner-update: APP_PATH FROM_DIR"); return; }
    NSString *app = words[0].stringByResolvingSymlinksInPath.stringByStandardizingPath;
    NSString *from = words[1].stringByResolvingSymlinksInPath.stringByStandardizingPath;
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
    NSString *identifier = info[@"CFBundleIdentifier"];
    if (!([app hasPrefix:@"/private/var/containers/Bundle/Application/"] || [app hasPrefix:@"/var/containers/Bundle/Application/"]) ||
        ![app.pathExtension isEqualToString:@"app"] || ![identifier isKindOfClass:[NSString class]] || ![identifier hasPrefix:@"org.droidvm."] ||
        !([from hasPrefix:@"/private/var/ngpu/"] || [from hasPrefix:@"/var/ngpu/"])) {
        line(@"owner-update: expected an installed org.droidvm app and a directory under ngpu (app=%@ identifier=%@ from=%@)", app, identifier, from);
        return;
    }
    NSFileManager *files = [NSFileManager defaultManager];
    for (NSString *relative in [files enumeratorAtPath:from]) {
        NSString *source = [from stringByAppendingPathComponent:relative];
        BOOL directory = NO;
        if (![files fileExistsAtPath:source isDirectory:&directory] || directory) continue;
        if ([relative.pathComponents containsObject:@".."]) { line(@"owner-update %@: refused", relative); continue; }
        NSString *target = [app stringByAppendingPathComponent:relative], *staged = [target stringByAppendingString:@".owner-update"];
        NSData *data = [NSData dataWithContentsOfFile:source];
        struct stat st;
        if (!data || stat(source.fileSystemRepresentation, &st)) { line(@"owner-update %@: cannot read the source", relative); continue; }
        unlink(staged.fileSystemRepresentation);
        if (![data writeToFile:staged atomically:NO]) { line(@"owner-update %@: cannot write %@: %s", relative, staged, strerror(errno)); continue; }
        chmod(staged.fileSystemRepresentation, st.st_mode & 07777);
        if (rename(staged.fileSystemRepresentation, target.fileSystemRepresentation)) {
            line(@"owner-update %@: rename: %s", relative, strerror(errno)); unlink(staged.fileSystemRepresentation); continue;
        }
        line(@"owner-update %@: %@ (%lu bytes, mode %o)", identifier, relative, (unsigned long)data.length, st.st_mode & 07777);
    }
}

static void registerOwnerApps(NSArray<NSString *> *paths) {
    id workspace = call(NSClassFromString(@"LSApplicationWorkspace"), "defaultWorkspace");
    SEL registerItems = sel_registerName("registerContainerizedApplicationWithInfoDictionaries:personaUniqueStrings:operationUUID:requestContext:saveObserver:registrationError:");
    if (![workspace respondsToSelector:registerItems]) { line(@"owner-register: selector unavailable"); return; }
    for (NSString *input in paths) {
        NSString *path = input.stringByResolvingSymlinksInPath.stringByStandardizingPath;
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
        NSString *identifier = info[@"CFBundleIdentifier"];
        NSString *executable = info[@"CFBundleExecutable"];
        if (!([path hasPrefix:@"/private/var/containers/Bundle/Application/"] || [path hasPrefix:@"/var/containers/Bundle/Application/"]) || ![path.pathExtension isEqualToString:@"app"] ||
            ![identifier isKindOfClass:[NSString class]] || ![identifier hasPrefix:@"org.droidvm."] ||
            ![executable isKindOfClass:[NSString class]] || ![executable isEqualToString:executable.lastPathComponent] ||
            ![[NSFileManager defaultManager] isExecutableFileAtPath:[path stringByAppendingPathComponent:executable]]) {
            line(@"owner-register %@: invalid owner app path=%@ identifier=%@ executable=%@ accessible=%d", input, path, identifier, executable,
                 executable ? [[NSFileManager defaultManager] isExecutableFileAtPath:[path stringByAppendingPathComponent:executable]] : NO); continue;
        }
        NSMutableDictionary *item = [info mutableCopy];
        NSDictionary *entitlements = ownerEntitlements([path stringByAppendingPathComponent:executable]);
        NSURL *dataContainer = ownerContainer(@"MCMAppDataContainer", identifier, NO);
        if (!entitlements || !dataContainer) { line(@"owner-register %@: missing signed entitlements or data container", input); continue; }
        item[@"Entitlements"] = entitlements;
        item[@"Container"] = dataContainer.path;
        // User, as an app in an MCM bundle container is: SpringBoard tags a System app hidden unless its bundle identifier is in
        // the default icon state (-[SBApplicationInfo _configureTags:hasVisibilityOverride:]), so a System registration put no
        // icon on the home screen or in the App Library.
        [item addEntriesFromDictionary:@{@"ApplicationType": @"User", @"CodeInfoIdentifier": identifier,
            @"Path": path, @"CompatibilityState": @0, @"IsContainerized": @(![entitlements[@"com.apple.private.security.no-container"] boolValue]), @"IsDeletable": @NO,
            @"IsAdHocSigned": @YES, @"LSInstallType": @1, @"_LSBundlePlugins": @{}}];
        NSError *error = nil;
        BOOL ok = ((BOOL (*)(id, SEL, NSArray *, NSArray *, NSUUID *, id, id, NSError **))objc_msgSend)(
            workspace, registerItems, @[item], nil, [NSUUID UUID], nil, nil, &error);
        line(@"owner-register %@ (%@): %@ %@", path, identifier, ok ? @"YES" : @"NO", error ?: @"");
    }
}

static void launchApps(NSArray<NSString *> *identifiers) {
    id workspace = call(NSClassFromString(@"LSApplicationWorkspace"), "defaultWorkspace");
    SEL launch = sel_registerName("openApplicationWithBundleID:");
    if (![workspace respondsToSelector:launch]) { line(@"launch: selector unavailable"); return; }
    for (NSString *identifier in identifiers) {
        BOOL ok = ((BOOL (*)(id, SEL, NSString *))objc_msgSend)(workspace, launch, identifier);
        line(@"launch %@: %@", identifier, ok ? @"YES" : @"NO");
    }
}

// FrontBoardServices' open-application service with debug options, which Xcode's launches use: the arguments, the environment
// (OS_ACTIVITY_DT_MODE copies os_log/NSLog to standard error) and where standard output and error go. Run as mobile (-u 501).
static void launchWithArguments(NSArray<NSString *> *words) {
    if (words.count < 2) { line(@"launch-args: OUT BUNDLE_ID [ARG...]"); return; }
    dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW);
    Class serviceClass = NSClassFromString(@"FBSOpenApplicationService"), optionsClass = NSClassFromString(@"FBSOpenApplicationOptions");
    id service = serviceClass ? call(serviceClass, "serviceWithDefaultShellEndpoint") : nil;
    if (!service || !optionsClass) { line(@"launch-args: FBSOpenApplicationService unavailable"); return; }
    NSString *out = words[0], *identifier = words[1];
    NSArray *arguments = [words subarrayWithRange:NSMakeRange(2, words.count - 2)];
    // The option keys are FrontBoardServices' exported constants; one it does not export is left out (and printed).
    NSString * (^key)(const char *) = ^NSString *(const char *name) {
        NSString *const *constant = (NSString *const *)dlsym(RTLD_DEFAULT, name);
        if (!constant) line(@"launch-args: %s not exported", name);
        return constant ? *constant : nil;
    };
    NSMutableDictionary *debug = [NSMutableDictionary dictionary], *dictionary = [NSMutableDictionary dictionary];
    void (^put)(NSMutableDictionary *, const char *, id) = ^(NSMutableDictionary *into, const char *name, id value) {
        NSString *k = key(name);
        if (k) into[k] = value;
    };
    put(debug, "FBSDebugOptionKeyArguments", arguments);
    put(debug, "FBSDebugOptionKeyEnvironment", @{@"OS_ACTIVITY_DT_MODE": @"YES"});
    put(debug, "FBSDebugOptionKeyStandardOutPath", [out stringByAppendingString:@".out"]);
    put(debug, "FBSDebugOptionKeyStandardErrorPath", [out stringByAppendingString:@".err"]);
    put(dictionary, "FBSOpenApplicationOptionKeyDebuggingOptions", debug);
    put(dictionary, "FBSOpenApplicationOptionKeyKillExisting", @YES);
    put(dictionary, "FBSOpenApplicationOptionKeyUnlockDevice", @YES);
    put(dictionary, "FBSOpenApplicationOptionKeyActivateSuspended", @NO);
    line(@"launch-args options %@", dictionary);
    id options = ((id (*)(id, SEL, NSDictionary *))objc_msgSend)(optionsClass, sel_registerName("optionsWithDictionary:"), dictionary);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSString *result = @"no answer in 30 s";
    ((void (*)(id, SEL, NSString *, id, void (^)(id, NSError *)))objc_msgSend)(service,
        sel_registerName("openApplication:withOptions:completion:"), identifier, options, ^(id process, NSError *error) {
            // Owned, not autoreleased: this runs on FrontBoard's queue, whose pool drains before the main thread prints it (no ARC here).
            result = error ? [[NSString alloc] initWithFormat:@"failed: %@", error] : [[NSString alloc] initWithFormat:@"launched: %@", process];
            dispatch_semaphore_signal(done);
        });
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
    line(@"launch-args %@ %@: %@", identifier, [arguments componentsJoinedByString:@" "], result);
}

// The item dictionaries installd hands LaunchServices when it builds its database
// (MobileInstallationEnumerateAllInstalledItemDictionaries, as lsd calls it). LaunchServices files each app's plug-ins under
// it only when the plug-in's dictionary names the app in PluginOwnerBundleID; the rest it registers standalone, with no
// containing app. Prints the items whose bundle identifier contains FILTER.
static void midump(NSArray<NSString *> *filters) {
    void *mi = dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation", RTLD_NOW);
    id (*enumerate)(NSDictionary *, void (^)(NSDictionary *, NSError *)) =
        (id (*)(NSDictionary *, void (^)(NSDictionary *, NSError *)))dlsym(mi, "MobileInstallationEnumerateAllInstalledItemDictionaries");
    if (!enumerate) {
        line(@"midump: no MobileInstallationEnumerateAllInstalledItemDictionaries");
        return;
    }
    __block NSUInteger total = 0;
    id result = enumerate(@{}, ^(NSDictionary *item, NSError *error) {
        if (error) {
            line(@"midump: error %@", error);
            return;
        }
        total++;
        NSString *identifier = item[@"CFBundleIdentifier"];
        BOOL wanted = filters.count == 0;
        for (NSString *filter in filters)
            if ([identifier containsString:filter]) wanted = YES;
        if (!wanted) return;
        line(@"%@ type=%@ owner=%@ path=%@", identifier, item[@"ApplicationType"], item[@"PluginOwnerBundleID"], item[@"Path"]);
        line(@"    keys: %@", [[item.allKeys sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@" "]);
        // where the item's data lives: its container and the kinds of container it is given
        for (NSString *key in @[@"Container", @"IsContainerized", @"HasSystemContainer", @"HasSystemGroupContainers", @"GroupContainers"])
            if (item[key]) line(@"    %@ = %@", key, [[item[key] description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "]);
    });
    line(@"midump: %lu items, returned %@", (unsigned long)total, result);
}

// How the file system names a bundle, the way LaunchServices finds a plug-in's app by node: stat's device and inode,
// getattrlist's fsid, object/file/parent IDs, and whether fsgetpath(fsid, fileID) leads back to the same path.
extern ssize_t fsgetpath(char *buf, size_t bufsize, fsid_t *fsid, uint64_t objid);
static void nodes(NSArray<NSString *> *paths) {
    for (NSString *path in paths) {
        struct stat st;
        if (stat(path.fileSystemRepresentation, &st)) {
            line(@"%@: stat %s", path, strerror(errno));
            continue;
        }
        struct attrlist al = {.bitmapcount = ATTR_BIT_MAP_COUNT,
                              .commonattr = ATTR_CMN_FSID | ATTR_CMN_OBJID | ATTR_CMN_PAROBJID | ATTR_CMN_FILEID | ATTR_CMN_PARENTID};
        struct __attribute__((packed)) {
            uint32_t length;
            fsid_t fsid;
            fsobj_id_t objid, parobjid;
            uint64_t fileid, parentid;
        } a = {0};
        int r = getattrlist(path.fileSystemRepresentation, &al, &a, sizeof a, 0);
        char back[1024] = "";
        fsid_t fsid = a.fsid;   // an aligned copy: the attribute buffer is packed
        ssize_t n = r ? -1 : fsgetpath(back, sizeof back, &fsid, a.fileid);
        line(@"%@\n    stat dev=%d ino=%llu | attr fsid=%#x:%#x objid=%u parobjid=%u fileid=%llu parentid=%llu (r=%d)\n    fsgetpath -> %s",
             path, st.st_dev, (unsigned long long)st.st_ino, a.fsid.val[0], a.fsid.val[1], a.objid.fid_objno, a.parobjid.fid_objno,
             a.fileid, a.parentid, r, n < 0 ? strerror(errno) : back);
        if (S_ISREG(st.st_mode)) {
            // A regular file: the length getattrlist reports (what LaunchServices sizes its Mach-O read by) next to stat's,
            // and the first bytes read through the file.
            struct attrlist fl = {.bitmapcount = ATTR_BIT_MAP_COUNT, .fileattr = ATTR_FILE_DATALENGTH | ATTR_FILE_TOTALSIZE};
            struct __attribute__((packed)) { uint32_t length; off_t total, data; } fa = {0};
            int fr = getattrlist(path.fileSystemRepresentation, &fl, &fa, sizeof fa, 0);
            uint32_t magic = 0;
            int fd = open(path.fileSystemRepresentation, O_RDONLY);
            ssize_t got = fd >= 0 ? pread(fd, &magic, sizeof magic, 0) : -1;
            if (fd >= 0) close(fd);
            line(@"    file stat size=%lld | attr totalsize=%lld datalength=%lld (r=%d %s) | magic=%#x (read %zd)",
                 (long long)st.st_size, (long long)fa.total, (long long)fa.data, fr, fr ? strerror(errno) : "", magic, got);
        }
    }
}

// What LaunchServices' own file node (CoreServices FSNode) answers for each path -- _LSAddExecutableFormatInfo reads an
// executable's load commands (and so the bundle's platform) only when the node gives it a path and a nonzero length.
static void fsnodes(NSArray<NSString *> *paths) {
    Class FSNode = NSClassFromString(@"FSNode");
    for (NSString *path in paths) {
        NSError *error = nil;
        id node = ((id (*)(id, SEL, NSURL *, unsigned long, NSError **))objc_msgSend)(
            [FSNode alloc], sel_registerName("initWithURL:flags:error:"), [NSURL fileURLWithPath:path], 0, &error);
        if (!node) {
            line(@"%@: FSNode %@", path, error);
            continue;
        }
        char fsr[1024] = "";
        NSError *e1 = nil, *e2 = nil, *e3 = nil, *e4 = nil;
        BOOL hasPath = ((BOOL (*)(id, SEL, char *, NSError **))objc_msgSend)(node, sel_registerName("getFileSystemRepresentation:error:"), fsr, &e1);
        unsigned long long length = 0, fileID = 0, volumeID = 0;
        BOOL hasLength = ((BOOL (*)(id, SEL, unsigned long long *, NSError **))objc_msgSend)(node, sel_registerName("getLength:error:"), &length, &e2);
        BOOL hasFile = ((BOOL (*)(id, SEL, unsigned long long *, NSError **))objc_msgSend)(node, sel_registerName("getFileIdentifier:error:"), &fileID, &e3);
        BOOL hasVolume = ((BOOL (*)(id, SEL, unsigned long long *, NSError **))objc_msgSend)(node, sel_registerName("getVolumeIdentifier:error:"), &volumeID, &e4);
        line(@"%@\n    path %@ %s %@\n    length %@ %llu %@\n    fileID %@ %llu %@ | volumeID %@ %#llx %@\n    directory=%@ regular=%@",
             path, hasPath ? @"YES" : @"NO", fsr, e1 ?: @"", hasLength ? @"YES" : @"NO", length, e2 ?: @"", hasFile ? @"YES" : @"NO",
             fileID, e3 ?: @"", hasVolume ? @"YES" : @"NO", volumeID, e4 ?: @"", flag(node, "isDirectory"), flag(node, "isRegularFile"));
    }
}

// Re-register apps the way an install does, so LaunchServices files each app's plug-ins under it: the app's installd dictionary
// and those of the plug-ins whose PluginOwnerBundleID names it go to registerContainerizedApplicationWithInfoDictionaries:, which
// lsd (performPostInstallationRegistration) collapses into one registration -- the app with _LSBundlePlugins. The first-boot
// seeding registers system apps and their plug-ins one by one instead, and a plug-in registered after its app gets no parent.
static void relink(NSArray<NSString *> *bundleIDs) {
    // -clean first: each plug-in's records are dropped (unregisterPlugin:) before the app is registered with them, so a record the
    // first-boot seeding left without its app -- which pkd rejects, "plug-ins outside containing apps must be in /System" -- goes.
    BOOL clean = bundleIDs.count && [bundleIDs[0] isEqualToString:@"-clean"];
    if (clean)
        bundleIDs = [bundleIDs subarrayWithRange:NSMakeRange(1, bundleIDs.count - 1)];
    void *mi = dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation", RTLD_NOW);
    id (*enumerate)(NSDictionary *, void (^)(NSDictionary *, NSError *)) =
        (id (*)(NSDictionary *, void (^)(NSDictionary *, NSError *)))dlsym(mi, "MobileInstallationEnumerateAllInstalledItemDictionaries");
    if (!enumerate) {
        line(@"relink: no MobileInstallationEnumerateAllInstalledItemDictionaries");
        return;
    }
    // "all": every app under /Applications that owns a plug-in (the system apps the first-boot seeding left them orphaned from).
    BOOL all = bundleIDs.count == 1 && [bundleIDs[0] isEqualToString:@"all"];
    NSMutableDictionary<NSString *, NSMutableArray *> *groups = [NSMutableDictionary dictionary];
    if (!all)
        for (NSString *bundleID in bundleIDs)
            groups[bundleID] = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSDictionary *> *apps = [NSMutableDictionary dictionary];
    NSMutableArray<NSDictionary *> *plugins = [NSMutableArray array];
    enumerate(@{}, ^(NSDictionary *item, NSError *error) {
        if (error) return;
        NSString *identifier = item[@"CFBundleIdentifier"], *type = item[@"ApplicationType"];
        if ([type isEqual:@"PluginKitPlugin"]) [plugins addObject:item];
        // A deletable system app comes twice: the app itself (System, in /Applications through owner-staged-apps) and its
        // SystemAppPlaceholder. Its plug-ins belong to the app, so the placeholder never replaces it.
        else if (identifier && !(apps[identifier] && [type isEqual:@"SystemAppPlaceholder"])) apps[identifier] = item;
    });
    for (NSDictionary *plugin in plugins) {
        NSString *owner = plugin[@"PluginOwnerBundleID"];
        NSDictionary *app = owner ? apps[owner] : nil;
        if (!app) continue;
        if (all && !groups[owner] && [app[@"Path"] hasPrefix:@"/Applications/"])
            groups[owner] = [NSMutableArray array];
        [groups[owner] addObject:plugin];
    }
    for (NSString *bundleID in groups)
        if (apps[bundleID]) [groups[bundleID] insertObject:apps[bundleID] atIndex:0];   // the app first, its plug-ins after
    if (all)
        bundleIDs = [groups.allKeys sortedArrayUsingSelector:@selector(compare:)];
    id workspace = call(NSClassFromString(@"LSApplicationWorkspace"), "defaultWorkspace");
    SEL sel = sel_registerName("registerContainerizedApplicationWithInfoDictionaries:personaUniqueStrings:operationUUID:requestContext:saveObserver:registrationError:");
    if (![workspace respondsToSelector:sel]) {
        line(@"relink: LSApplicationWorkspace does not answer %s", sel_getName(sel));
        return;
    }
    for (NSString *bundleID in bundleIDs) {
        NSArray *items = groups[bundleID];
        if (items.count == 0 || [items[0][@"ApplicationType"] isEqual:@"PluginKitPlugin"]) {
            line(@"relink %@: installd has no app record for it", bundleID);
            continue;
        }
        if (clean) {
            SEL unregister = sel_registerName("unregisterPlugin:");
            for (NSDictionary *item in items) {
                if (![item[@"ApplicationType"] isEqual:@"PluginKitPlugin"] || !item[@"Path"]) continue;
                BOOL dropped = ((BOOL (*)(id, SEL, NSURL *))objc_msgSend)(workspace, unregister, [NSURL fileURLWithPath:item[@"Path"]]);
                line(@"  unregisterPlugin %@: %@", item[@"CFBundleIdentifier"], dropped ? @"YES" : @"NO");
            }
        }
        NSError *error = nil;
        BOOL ok = ((BOOL (*)(id, SEL, NSArray *, NSArray *, NSUUID *, id, id, NSError **))objc_msgSend)(
            workspace, sel, items, nil, [NSUUID UUID], nil, nil, &error);
        line(@"relink %@ (%lu plug-ins): %@ %@", bundleID, (unsigned long)items.count - 1, ok ? @"YES" : @"NO", error ?: @"");
    }
}

// What CoreFoundation's bundle machinery reads from a bundle directory -- what LaunchServices records a plug-in's extension
// point from: CFBundle's identifier, its Info.plist URL, the NSExtension point in its info dictionary, and the dictionary read
// straight from the directory, for comparing a bundle shown through the bindfs view with one on the system volume.
extern CFURLRef _CFBundleCopyInfoPlistURL(CFBundleRef bundle);   // CoreFoundation, private
static void bundle(NSArray<NSString *> *paths) {
    for (NSString *path in paths) {
        NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
        CFBundleRef b = CFBundleCreate(NULL, (CFURLRef)url);
        NSDictionary *info = b ? (NSDictionary *)CFBundleGetInfoDictionary(b) : nil;
        CFURLRef infoURL = b ? _CFBundleCopyInfoPlistURL(b) : NULL;
        NSDictionary *direct = (NSDictionary *)CFBundleCopyInfoDictionaryInDirectory((CFURLRef)url);
        line(@"%@\n    CFBundle %@ id=%@ infoURL=%@ keys=%lu point=%@\n    in-directory keys=%lu point=%@", path, b ? @"ok" : @"(nil)",
             b ? (NSString *)CFBundleGetIdentifier(b) : nil, infoURL ? [(NSURL *)infoURL path] : nil, (unsigned long)info.count,
             info[@"NSExtension"][@"NSExtensionPointIdentifier"], (unsigned long)direct.count,
             direct[@"NSExtension"][@"NSExtensionPointIdentifier"]);
    }
}

static void restrictions(NSArray<NSString *> *identifiers) {
    id manager = call(NSClassFromString(@"LSApplicationRestrictionsManager"), "sharedInstance");
    if (!manager) {
        line(@"no LSApplicationRestrictionsManager");
        return;
    }
    static const char *lists[] = {"restrictedBundleIDs", "blacklistedBundleIDs", "whitelistedBundleIDs",
                                  "removedSystemApplications", "maximumRating"};
    for (size_t i = 0; i < sizeof lists / sizeof *lists; i++)
        line(@"%s = %@", lists[i], call(manager, lists[i]));
    line(@"whitelistEnabled=%@ ratingEnabled=%@", flag(manager, "isWhitelistEnabled"), flag(manager, "isRatingEnabled"));
    SEL restricted = sel_registerName("isApplicationRestricted:");
    SEL removed = sel_registerName("isAppRemoved:");
    for (NSString *identifier in identifiers) {
        BOOL r = [manager respondsToSelector:restricted] && ((BOOL (*)(id, SEL, NSString *))objc_msgSend)(manager, restricted, identifier);
        BOOL d = [manager respondsToSelector:removed] && ((BOOL (*)(id, SEL, NSString *))objc_msgSend)(manager, removed, identifier);
        line(@"%@: restricted=%@ removed=%@", identifier, r ? @"YES" : @"no", d ? @"YES" : @"no");
    }
}

// libSystem's bootstrap calls; the iphoneos SDK ships no <servers/bootstrap.h>.
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *name, mach_port_t *service);
extern char *bootstrap_strerror(kern_return_t error);

// The bootstrap port this process reaches launchd through, and a plain lookup of each name on it.
// The data container MobileContainerManager keeps for each identifier (an app's, or with -plugin an extension's), and whether its
// directory exists; -create asks it to make a missing one (createIfNecessary), as an install would.
static void containers(NSArray<NSString *> *arguments) {
    BOOL create = NO, plugin = NO;
    NSMutableArray<NSString *> *identifiers = [NSMutableArray array];
    for (NSString *argument in arguments) {
        if ([argument isEqualToString:@"-create"]) create = YES;
        else if ([argument isEqualToString:@"-plugin"]) plugin = YES;
        else [identifiers addObject:argument];
    }
    dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
    Class cls = NSClassFromString(plugin ? @"MCMPluginKitPluginDataContainer" : @"MCMAppDataContainer");
    SEL lookup = sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:");
    if (!cls || ![cls respondsToSelector:lookup]) {
        line(@"container: no %@", plugin ? @"MCMPluginKitPluginDataContainer" : @"MCMAppDataContainer");
        return;
    }
    for (NSString *identifier in identifiers) {
        BOOL existed = NO;
        NSError *error = nil;
        id container = ((id (*)(Class, SEL, NSString *, BOOL, BOOL *, NSError **))objc_msgSend)(cls, lookup, identifier, create, &existed,
                                                                                               &error);
        NSURL *url = [container respondsToSelector:@selector(url)] ? [container performSelector:@selector(url)] : nil;
        struct stat st;
        BOOL present = url && stat(url.fileSystemRepresentation, &st) == 0;
        line(@"container %@: %@ existed=%d on-disk=%d%@", identifier, url.path ?: @"(none)", existed, present,
             error ? [NSString stringWithFormat:@" error=%@", error] : @"");
    }
}

// A directory tree (paths, sizes, modification dates) where only an entitled process may look: the app data containers.
static void tree(NSArray<NSString *> *arguments) {
    if (!arguments.count) return;
    NSUInteger depth = arguments.count > 1 ? (NSUInteger)arguments[1].integerValue : 4;
    NSString *root = arguments[0];
    NSDirectoryEnumerator *walk = [NSFileManager.defaultManager enumeratorAtURL:[NSURL fileURLWithPath:root]
        includingPropertiesForKeys:@[NSURLFileSizeKey, NSURLContentModificationDateKey, NSURLIsDirectoryKey] options:0
        errorHandler:^BOOL(NSURL *url, NSError *error) { line(@"tree: %@: %@", url.path, error.localizedDescription); return YES; }];
    for (NSURL *url in walk) {
        if (walk.level > depth) {[walk skipDescendants]; continue;}
        NSDictionary *values = [url resourceValuesForKeys:@[NSURLFileSizeKey, NSURLContentModificationDateKey, NSURLIsDirectoryKey] error:nil];
        line(@"%@%@ %@ %@", [@"" stringByPaddingToLength:2 * (walk.level - 1) withString:@" " startingAtIndex:0],
             [url.path substringFromIndex:MIN(root.length + 1, url.path.length)],
             [values[NSURLIsDirectoryKey] boolValue] ? @"/" : values[NSURLFileSizeKey], values[NSURLContentModificationDateKey]);
    }
}

// A file's content: a property list as its description, anything else as text.
static void show(NSArray<NSString *> *paths) {
    for (NSString *path in paths) {
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (!data) {line(@"show %@: unreadable (%s)", path, strerror(errno)); continue;}
        id plist = [NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:nil];
        line(@"show %@ (%lu bytes):\n%@", path, (unsigned long)data.length,
             plist ?: [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"(binary)");
    }
}

// The processes as the kernel names them: p_comm (16 characters), p_name (32: what proc_selfname and the GPU grant compare) and the
// executable's path, those whose names hold FILTER if one is given (toybox here has no ps).
// libproc, which the iOS SDK leaves out (macOS's libproc.h and sys/proc_info.h; libsystem has it on iOS too).
int proc_listallpids(void *buffer, int size);
int proc_pidinfo(int pid, int flavor, uint64_t argument, void *buffer, int size);
int proc_pidpath(int pid, void *buffer, uint32_t size);
struct proc_bsdinfo {
    uint32_t pbi_flags, pbi_status, pbi_xstatus, pbi_pid, pbi_ppid;
    uid_t pbi_uid; gid_t pbi_gid; uid_t pbi_ruid; gid_t pbi_rgid; uid_t pbi_svuid; gid_t pbi_svgid;
    uint32_t rfu_1;
    char pbi_comm[16], pbi_name[32];
    uint32_t pbi_nfiles, pbi_pgid, pbi_pjobc, e_tdev, e_tpgid;
    int32_t pbi_nice;
    uint64_t pbi_start_tvsec, pbi_start_tvusec;
};
#define PROC_PIDTBSDINFO 3
static void procs(NSArray<NSString *> *arguments) {
    const char *filter = arguments.count ? arguments[0].UTF8String : NULL;
    int count = proc_listallpids(NULL, 0);
    if (count <= 0) {line(@"procs: %s", strerror(errno)); return;}
    pid_t *pids = calloc((size_t)count + 64, sizeof(pid_t));
    count = proc_listallpids(pids, (int)(((size_t)count + 64) * sizeof(pid_t)));
    for (int i = 0; i < count; i++) {
        struct proc_bsdinfo info;
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof info) != (int)sizeof info) continue;
        char path[4 * 1024] = {}, comm[17] = {}, name[33] = {};
        memcpy(comm, info.pbi_comm, 16);
        memcpy(name, info.pbi_name, 32);
        proc_pidpath(pids[i], path, sizeof path);
        if (filter && !strstr(comm, filter) && !strstr(name, filter) && !strstr(path, filter)) continue;
        line(@"%5d %5u %4u comm '%s' name '%s' %s", pids[i], info.pbi_ppid, info.pbi_uid, comm, name, path);
    }
    free(pids);
}

// A file copied where the shell may read it (an app container's file: lsdiag holds the data-vault entitlement, the shell does not).
static void copyFile(NSArray<NSString *> *arguments) {
    if (arguments.count != 2) {line(@"copy SRC DST"); return;}
    NSError *error = nil;
    [[NSFileManager defaultManager] removeItemAtPath:arguments[1] error:nil];
    BOOL ok = [[NSFileManager defaultManager] copyItemAtPath:arguments[0] toPath:arguments[1] error:&error];
    line(@"copy %@ -> %@: %@", arguments[0], arguments[1], ok ? @"done" : error.localizedDescription);
}

// An image as ImageIO decodes it in this process: its type, size, mean and range of its pixels (a blank snapshot is uniform), and
// optionally the first image written as a PNG.
static void image(NSArray<NSString *> *arguments) {
    if (!arguments.count) {line(@"image PATH [OUT.png]"); return;}
    NSURL *url = [NSURL fileURLWithPath:arguments[0]];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    if (!source) {line(@"image %@: no image source", arguments[0]); return;}
    line(@"image %@: type %@, %zu image(s), status %d, properties %@", arguments[0], CGImageSourceGetType(source), CGImageSourceGetCount(source),
         (int)CGImageSourceGetStatus(source), CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, NULL)));
    CGImageRef picture = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!picture) {line(@"  no image decoded"); return;}
    size_t width = CGImageGetWidth(picture), height = CGImageGetHeight(picture);
    uint8_t *pixels = calloc(width * height, 4);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(pixels, width, height, 8, width * 4, space, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), picture);
    double sum[4] = {};
    uint8_t low[4] = {255, 255, 255, 255}, high[4] = {};
    for (size_t i = 0; i < width * height; ++i)
        for (int c = 0; c < 4; ++c) {
            const uint8_t v = pixels[i * 4 + c];
            sum[c] += v; if (v < low[c]) low[c] = v; if (v > high[c]) high[c] = v;
        }
    line(@"  %zux%zu bpc %zu: mean rgba %.1f %.1f %.1f %.1f, min %u %u %u %u, max %u %u %u %u", width, height, CGImageGetBitsPerComponent(picture),
         sum[0] / (width * height), sum[1] / (width * height), sum[2] / (width * height), sum[3] / (width * height),
         low[0], low[1], low[2], low[3], high[0], high[1], high[2], high[3]);
    if (arguments.count > 1) {
        CGImageDestinationRef out = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:arguments[1]], CFSTR("public.png"), 1, NULL);
        if (out) {
            CGImageDestinationAddImage(out, picture, NULL);
            line(@"  png %@: %@", arguments[1], CGImageDestinationFinalize(out) ? @"written" : @"failed");
            CFRelease(out);
        }
    }
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    free(pixels);
    CGImageRelease(picture);
}

// An extension point as LaunchServices records it, and the extensions it enumerates for it (what ExtensionKit's queries read).
static void points(NSArray<NSString *> *identifiers) {
    Class point = NSClassFromString(@"LSExtensionPointRecord"), extension = NSClassFromString(@"LSApplicationExtensionRecord");
    for (NSString *identifier in identifiers) {
        NSError *error = nil;
        id record = ((id (*)(id, SEL, NSString *, NSError **))objc_msgSend)([point alloc], sel_registerName("initWithIdentifier:error:"),
                                                                           identifier, &error);
        line(@"point %@: %@%@", identifier, record ? [record description] : @"no record", error ? [NSString stringWithFormat:@" (%@)", error] : @"");
        if (!record) continue;
        id enumerator = ((id (*)(Class, SEL, id, NSUInteger))objc_msgSend)(extension,
                                                                            sel_registerName("enumeratorWithExtensionPointRecord:options:"), record, 0);
        NSUInteger count = 0;
        for (id found in enumerator) {
            line(@"  %@ %@", [found valueForKey:@"bundleIdentifier"], [[found valueForKey:@"URL"] path]);
            ++count;
        }
        line(@"  %lu extensions", (unsigned long)count);
    }
}

// Removes files or directories (recursively) where only an entitled process may write, e.g. an app's broken data store.
static void removePaths(NSArray<NSString *> *paths) {
    for (NSString *path in paths) {
        NSError *error = nil;
        BOOL removed = [NSFileManager.defaultManager removeItemAtPath:path error:&error];
        line(@"remove %@: %@", path, removed ? @"removed" : error.localizedDescription);
    }
}

// A file's data protection as the kernel and Foundation see it: the mount's content protection (MNT_CPROTECT), fcntl's class,
// NSURLFileProtectionKey, and whether -set (NSURLFileProtectionNone) takes.
static void protect(NSArray<NSString *> *arguments) {
    BOOL set = arguments.count && [arguments[0] isEqualToString:@"-set"];
    for (NSString *path in set ? [arguments subarrayWithRange:NSMakeRange(1, arguments.count - 1)] : arguments) {
        struct statfs fs;
        int mounted = statfs(path.fileSystemRepresentation, &fs);
        int fd = open(path.fileSystemRepresentation, O_RDONLY);
        int cls = fd >= 0 ? fcntl(fd, F_GETPROTECTIONCLASS) : -1;
        int why = errno;
        if (fd >= 0) close(fd);
        NSURL *url = [NSURL fileURLWithPath:path];
        id value = nil;
        NSError *error = nil;
        [url getResourceValue:&value forKey:NSURLFileProtectionKey error:&error];
        line(@"protect %@: mount %s cprotect=%d, fcntl class %d%@, NSURLFileProtectionKey %@%@", path, mounted ? "?" : fs.f_mntonname,
             mounted ? -1 : !!(fs.f_flags & MNT_CPROTECT), cls, cls < 0 ? [NSString stringWithFormat:@" (%s)", strerror(why)] : @"",
             value, error ? [NSString stringWithFormat:@" (%@)", error.localizedDescription] : @"");
        if (set) {
            NSError *setError = nil;
            BOOL ok = [url setResourceValue:NSURLFileProtectionNone forKey:NSURLFileProtectionKey error:&setError];
            [url removeCachedResourceValueForKey:NSURLFileProtectionKey];
            value = nil;
            [url getResourceValue:&value forKey:NSURLFileProtectionKey error:nil];
            line(@"  set NSURLFileProtectionNone: %@, now %@", ok ? @"YES" : setError.localizedDescription, value);
        }
    }
}

static void bootstrap(NSArray<NSString *> *names) {
    mach_port_t special = MACH_PORT_NULL;
    kern_return_t kr = task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &special);
    line(@"pid %d ppid %d uid %d: bootstrap_port=%#x, TASK_BOOTSTRAP_PORT=%#x (kr %d)", getpid(), getppid(), getuid(),
         bootstrap_port, special, kr);
    if (special != MACH_PORT_NULL) {
        mach_port_type_t type = 0;
        mach_port_type(mach_task_self(), special, &type);
        line(@"  port rights: %#x (send=%d dead=%d)", type, !!(type & MACH_PORT_TYPE_SEND),
             !!(type & MACH_PORT_TYPE_DEAD_NAME));
    }
    for (NSString *name in names) {
        mach_port_t service = MACH_PORT_NULL;
        kr = bootstrap_look_up(bootstrap_port, name.UTF8String, &service);
        line(@"  bootstrap_look_up(%@) = %d (%s) port %#x", name, kr, bootstrap_strerror(kr), service);
    }
}

// From xnu osfmk/mach/coalition.h and libsyscall spawn_private.h; libSystem exports both.
extern int coalition_create(uint64_t *cid_out, uint32_t flags);
extern int posix_spawnattr_setcoalition_np(const posix_spawnattr_t *attr, uint64_t coalition, int type, int role);
extern char **environ;

// Run this tool again as the leader of a new resource and jetsam coalition and return its exit status. Everything
// ownervisor starts inherits launchd's own coalition (the kernel spawned ownervisor from launchd's process), and
// launchd refuses lookups from its coalition as reentrant (error 141); a coalition of its own may not be.
static int respawnInNewCoalition(int argc, char **argv) {
    uint64_t ids[2];
    for (int type = 0; type < 2; type++) {
        if (coalition_create(&ids[type], (uint32_t)(type << 4))) {
            perror("coalition_create");
            return -1;
        }
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    for (int type = 0; type < 2; type++)
        posix_spawnattr_setcoalition_np(&attr, ids[type], type, 1 /* COALITION_TASKROLE_LEADER */);
    setenv("LSDIAG_IN_COALITION", "1", 1);
    pid_t pid;
    int error = posix_spawn(&pid, argv[0], NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (error) {
        fprintf(stderr, "posix_spawn into coalitions %llu/%llu: %s\n", ids[0], ids[1], strerror(error));
        return -1;
    }
    printf("(in new coalitions resource %llu, jetsam %llu)\n", ids[0], ids[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    (void)argc;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);   // keep what was printed if a private method faults later
        int at = 1;
        if (at < argc && !strcmp(argv[at], "-c")) {
            if (!getenv("LSDIAG_IN_COALITION")) {
                int status = respawnInNewCoalition(argc, argv);
                if (status >= 0)
                    return status;
            }
            at++;
        }
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
        if (at + 1 < argc && !strcmp(argv[at], "-u")) {
            uid_t uid = (uid_t)atoi(argv[at + 1]);
            if (setgid(uid) || setuid(uid)) {
                perror("setuid");
                return 1;
            }
            at += 2;
        }
        if (at >= argc) {
            fprintf(stderr, "usage: lsdiag [-c] [-u UID] app|ext|plugins|restrictions|rebuild|register|midump|nodes|fsnode|relink|bundle|boot NAME...\n");
            return 2;
        }
        NSString *verb = @(argv[at++]);
        NSMutableArray<NSString *> *identifiers = [NSMutableArray array];
        for (; at < argc; at++)
            [identifiers addObject:@(argv[at])];
        if ([verb isEqualToString:@"app"]) {
            for (NSString *identifier in identifiers)
                app(identifier);
        } else if ([verb isEqualToString:@"ext"]) {
            for (NSString *identifier in identifiers)
                extension(identifier);
        } else if ([verb isEqualToString:@"restrictions"]) {
            restrictions(identifiers);
        } else if ([verb isEqualToString:@"plugins"]) {
            plugins(identifiers);
        } else if ([verb isEqualToString:@"rebuild"]) {
            rebuild(identifiers);
        } else if ([verb isEqualToString:@"register"]) {
            registerApps(identifiers);
        } else if ([verb isEqualToString:@"owner-register"]) {
            registerOwnerApps(identifiers);
        } else if ([verb isEqualToString:@"owner-install"]) {
            installOwnerApps(identifiers);
        } else if ([verb isEqualToString:@"owner-update"]) {
            updateOwnerApp(identifiers);
        } else if ([verb isEqualToString:@"launch"]) {
            launchApps(identifiers);
        } else if ([verb isEqualToString:@"launch-args"]) {
            launchWithArguments(identifiers);
        } else if ([verb isEqualToString:@"midump"]) {
            midump(identifiers);
        } else if ([verb isEqualToString:@"nodes"]) {
            nodes(identifiers);
        } else if ([verb isEqualToString:@"fsnode"]) {
            fsnodes(identifiers);
        } else if ([verb isEqualToString:@"relink"]) {
            relink(identifiers);
        } else if ([verb isEqualToString:@"bundle"]) {
            bundle(identifiers);
        } else if ([verb isEqualToString:@"boot"]) {
            bootstrap(identifiers);
        } else if ([verb isEqualToString:@"container"]) {
            containers(identifiers);
        } else if ([verb isEqualToString:@"protect"]) {
            protect(identifiers);
        } else if ([verb isEqualToString:@"point"]) {
            points(identifiers);
        } else if ([verb isEqualToString:@"tree"]) {
            tree(identifiers);
        } else if ([verb isEqualToString:@"remove"]) {
            removePaths(identifiers);
        } else if ([verb isEqualToString:@"procs"]) {
            procs(identifiers);
        } else if ([verb isEqualToString:@"copy"]) {
            copyFile(identifiers);
        } else if ([verb isEqualToString:@"image"]) {
            image(identifiers);
        } else if ([verb isEqualToString:@"show"]) {
            show(identifiers);
        } else {
            fprintf(stderr, "unknown verb %s\n", argv[at - 1]);
            return 2;
        }
    }
    return 0;
}
