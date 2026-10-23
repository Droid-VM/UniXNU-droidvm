// lsdiag: ask LaunchServices what it has registered, from inside the guest.
//
//   lsdiag [-u UID] app BUNDLE_ID...     application record: URL, placeholder/restricted/installed state, extensions
//   lsdiag [-u UID] ext BUNDLE_ID...     extension record and plug-in proxy: URL and the containing bundle
//   lsdiag [-u UID] restrictions [BUNDLE_ID...]   LSApplicationRestrictionsManager's lists, and per-app verdicts
//   lsdiag [-u UID] rebuild [nosystem] [internal] [nouser]   rebuild the LaunchServices database (system and user apps)
//   lsdiag [-u UID] register APP_PATH...   re-register app bundles by URL (their plug-ins go under them)
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
//   lsdiag tree PATH [DEPTH]             a directory tree where only an entitled process may look (app containers)
//   lsdiag show PATH...                  a file: a property list printed, anything else as text
//   lsdiag remove PATH...                removes files or directories there (an app's broken data store)
//
// The records come from the private LaunchServices classes, looked up at run time so nothing private is linked; a
// selector a class does not answer prints as "n/a". -u switches to that user first (501 = mobile, whose database the
// apps and widgets use), since root sees the system domain. A leading -c runs the query in a coalition of its own.
#import <Foundation/Foundation.h>
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
    for (NSString *path in paths) {
        if (![workspace respondsToSelector:sel]) {
            line(@"register: LSApplicationWorkspace does not answer registerApplication:");
            return;
        }
        BOOL ok = ((BOOL (*)(id, SEL, NSURL *))objc_msgSend)(workspace, sel, [NSURL fileURLWithPath:path isDirectory:YES]);
        line(@"register %@: %@", path, ok ? @"YES" : @"NO");
    }
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
        } else if ([verb isEqualToString:@"point"]) {
            points(identifiers);
        } else if ([verb isEqualToString:@"tree"]) {
            tree(identifiers);
        } else if ([verb isEqualToString:@"remove"]) {
            removePaths(identifiers);
        } else if ([verb isEqualToString:@"show"]) {
            show(identifiers);
        } else {
            fprintf(stderr, "unknown verb %s\n", argv[at - 1]);
            return 2;
        }
    }
    return 0;
}
