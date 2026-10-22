// lsdiag: ask LaunchServices what it has registered, from inside the guest.
//
//   lsdiag [-u UID] app BUNDLE_ID...     application record: URL, placeholder/restricted/installed state, extensions
//   lsdiag [-u UID] ext BUNDLE_ID...     extension record and plug-in proxy: URL and the containing bundle
//   lsdiag [-u UID] restrictions [BUNDLE_ID...]   LSApplicationRestrictionsManager's lists, and per-app verdicts
//   lsdiag [-u UID] boot [SERVICE...]    this process's bootstrap port, and a bootstrap_look_up of each service
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
#import <sys/wait.h>
#import <unistd.h>

static id call(id target, const char *name) {
    SEL sel = sel_registerName(name);
    if (!target || ![target respondsToSelector:sel])
        return nil;
    return ((id (*)(id, SEL))objc_msgSend)(target, sel);
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
    line(@"  class=%@ URL=%@", NSStringFromClass([record class]), [call(record, "URL") path]);
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
    line(@"  plug-in proxy: %@ URL=%@ containingBundle=%@ at %@", proxy ? @"found" : @"(nil)", [call(proxy, "bundleURL") path],
         call(containing, "bundleIdentifier"), [call(containing, "bundleURL") path]);
}

// Every plug-in LaunchServices offers for an extension point (default com.apple.widgetkit-extension), the way chronod's
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
            NSString *pt = call(proxy, "extensionPointIdentifier");
            [hist addObject:pt ?: @"(nil)"];
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
        NSString *pt = call(proxy, "extensionPointIdentifier");
        if (![pt isEqualToString:point]) continue;
        shown++;
        id containing = call(proxy, "containingBundle");
        line(@"  %@ containing=%@ at %@ restricted=%@", call(proxy, "pluginIdentifier"),
             call(containing, "bundleIdentifier") ?: @"(nil)", [call(proxy, "bundleURL") path], flag(proxy, "isRestricted"));
    }
    line(@"  (%lu for %@)", (unsigned long)shown, point);
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
            fprintf(stderr, "usage: lsdiag [-c] [-u UID] app|ext|restrictions|boot NAME...\n");
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
        } else if ([verb isEqualToString:@"boot"]) {
            bootstrap(identifiers);
        } else {
            fprintf(stderr, "unknown verb %s\n", argv[at - 1]);
            return 2;
        }
    }
    return 0;
}
