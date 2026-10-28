// ipatool: unpack, install, inspect and remove an .ipa inside the guest -- the debug shell's app-installer, a dedicated tool
// where lsdiag only had the owner-install/owner-register halves. It is a debug facility: an app only *runs* once every Mach-O
// in it is trusted (the boot trust cache, or the planned runtime-trust SPI -- plans/UNIXNUSETTING.md), so ipatool installs and
// registers the bundle and reports the CDHashes that must be trusted; it does not itself grant trust.
//
//   ipatool scan IPA                 unpack to a temp dir, then for every Mach-O (main executable, Frameworks/*, PlugIns/*.appex
//                                    and nested bundles) print its CDHash (SHA-256, 20-byte trust-cache form) and whether it is
//                                    FairPlay-encrypted (LC_ENCRYPTION_INFO cryptid != 0). Leaves the unpacked tree in place.
//   ipatool install [-f] IPA         scan, refuse if any Mach-O is encrypted (-f installs anyway), move Payload/<app> into an
//                                    MCM bundle container, make its data container, and register it with LaunchServices as a
//                                    User, ad-hoc-signed app. Prints the CDHashes to trust. Run the register step as mobile (-u 501).
//   ipatool remove BUNDLE_ID...      unregister from LaunchServices and destroy the MCM bundle and data containers.
//   ipatool unpack IPA DEST          only unpack the zip into DEST (path-traversal-safe), for inspection.
//
// Zip is inflated with libz (the iphoneos SDK ships libz.tbd and every process links it; zlib.h is shimmed from the macOS SDK
// at build time). Entitlements and trust: scripts/build-droidvm-ipatool.py (the curated debug set, category 3 in the owner
// module), like lsdiag. Only root, or an owner process ownervisor spawns, runs it; it is not on ownervisor's app.sock.
#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <zlib.h>

static int gStatus = 0;
static void line(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);
static void line(NSString *format, ...) {
    va_list args; va_start(args, format);
    NSString *s = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    fputs(s.UTF8String, stdout); fputc('\n', stdout);
}
static id call(id target, const char *selector) {
    SEL s = sel_registerName(selector);
    return [target respondsToSelector:s] ? ((id (*)(id, SEL))objc_msgSend)(target, s) : nil;
}
// LSApplicationWorkspace lives in the LaunchServices code the MobileCoreServices umbrella loads; linking the framework pulls
// it in, and this dlopen makes the dependency explicit (and covers a run where it was not already loaded).
static id launchServices(void) {
    dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
    return call(NSClassFromString(@"LSApplicationWorkspace"), "defaultWorkspace");
}

// ---- zip (IPA) extraction with libz, path-traversal-safe ----
// A minimal reader: find the End Of Central Directory, walk the central directory, and for each entry inflate (method 8) or
// copy (method 0) the local file into DEST. Entry names are rejected if absolute or containing a ".." component, and the
// resolved path must stay under DEST. Directories (trailing '/') only make the directory.
static BOOL safeJoin(NSString *dest, NSString *name, NSString **out) {
    // A zip entry name is placed under dest. The only directory-traversal vectors in a zip name are an absolute path and a
    // ".." component, so reject those (standardizing the path is avoided: on iOS it rewrites /private/var <-> /var, which
    // broke a prefix test). Symlink entries are written as regular files, so they cannot redirect later writes.
    if (name.length == 0 || [name hasPrefix:@"/"]) return NO;
    for (NSString *c in name.pathComponents) if ([c isEqual:@".."]) return NO;
    *out = [dest stringByAppendingPathComponent:name];
    return YES;
}
static BOOL inflateRaw(const uint8_t *in, uint64_t inLen, uint8_t *out, uint64_t outLen) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return NO;          // raw DEFLATE, no zlib header
    z.next_in = (Bytef *)in; z.avail_in = (uInt)inLen;
    z.next_out = out; z.avail_out = (uInt)outLen;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return (r == Z_STREAM_END) && z.total_out == outLen;
}
static BOOL extractZip(NSString *ipa, NSString *dest) {
    NSData *data = [NSData dataWithContentsOfFile:ipa options:NSDataReadingMappedIfSafe error:nil];
    if (!data) { line(@"ipatool: cannot read %@", ipa); return NO; }
    const uint8_t *b = data.bytes; uint64_t n = data.length;
    // End Of Central Directory: signature 0x06054b50, within the last 64 KiB + 22.
    uint64_t lo = n > 0x10016 ? n - 0x10016 : 0, eocd = 0;
    for (uint64_t i = (n >= 22 ? n - 22 : 0); i + 22 <= n && i + 1 > lo; i--) {
        if (OSReadLittleInt32(b, i) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (!eocd) { line(@"ipatool: %@ is not a zip (no EOCD)", ipa); return NO; }
    uint32_t count = OSReadLittleInt16(b, eocd + 10);
    uint64_t cd = OSReadLittleInt32(b, eocd + 16), cdAt = cd;
    NSFileManager *fm = NSFileManager.defaultManager;
    for (uint32_t e = 0; e < count; e++) {
        if (cdAt + 46 > n || OSReadLittleInt32(b, cdAt) != 0x02014b50) { line(@"ipatool: bad central directory"); return NO; }
        uint16_t method = OSReadLittleInt16(b, cdAt + 10);
        uint32_t comp = OSReadLittleInt32(b, cdAt + 20), uncomp = OSReadLittleInt32(b, cdAt + 24);
        uint16_t nameLen = OSReadLittleInt16(b, cdAt + 28), extraLen = OSReadLittleInt16(b, cdAt + 30);
        uint16_t commentLen = OSReadLittleInt16(b, cdAt + 32);
        uint32_t localAt = OSReadLittleInt32(b, cdAt + 42);
        if (cdAt + 46 + nameLen > n) { line(@"ipatool: bad central directory name"); return NO; }
        NSString *name = [[NSString alloc] initWithBytes:b + cdAt + 46 length:nameLen encoding:NSUTF8StringEncoding] ?: @"";
        cdAt += 46 + nameLen + extraLen + commentLen;
        NSString *path = nil;
        if (![name hasSuffix:@"/"] && !safeJoin(dest, name, &path)) { line(@"ipatool: unsafe zip entry refused: %@", name); return NO; }
        if ([name hasSuffix:@"/"]) {
            NSString *dir = nil;
            if (safeJoin(dest, [name substringToIndex:name.length - 1], &dir))
                [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            continue;
        }
        // local header: 30 + nameLen + extraLen, then the compressed data
        if (localAt + 30 > n || OSReadLittleInt32(b, localAt) != 0x04034b50) { line(@"ipatool: bad local header for %@", name); return NO; }
        uint16_t lNameLen = OSReadLittleInt16(b, localAt + 26), lExtraLen = OSReadLittleInt16(b, localAt + 28);
        uint64_t dataAt = localAt + 30 + lNameLen + lExtraLen;
        if (dataAt + comp > n) { line(@"ipatool: truncated data for %@", name); return NO; }
        [fm createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        NSMutableData *out = [NSMutableData dataWithLength:uncomp];
        if (method == 0) {
            if (comp != uncomp) { line(@"ipatool: stored size mismatch for %@", name); return NO; }
            memcpy(out.mutableBytes, b + dataAt, uncomp);
        } else if (method == 8) {
            if (!inflateRaw(b + dataAt, comp, out.mutableBytes, uncomp)) { line(@"ipatool: inflate failed for %@", name); return NO; }
        } else { line(@"ipatool: unsupported zip method %u for %@", method, name); return NO; }
        if (![out writeToFile:path atomically:NO]) { line(@"ipatool: cannot write %@", path); return NO; }
    }
    return YES;
}

// ---- Mach-O inspection: CDHash (trust-cache form) and FairPlay encryption ----
// Returns the 20-byte truncated SHA-256 of the primary CodeDirectory as hex, or nil; sets *encrypted if any segment carries
// an LC_ENCRYPTION_INFO(_64) with cryptid != 0. A fat binary's arm64/arm64e slice is used.
static NSString *machoCDHash(NSString *path, BOOL *encrypted) {
    if (encrypted) *encrypted = NO;
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    if (data.length < sizeof(struct mach_header_64)) return nil;
    const uint8_t *bytes = data.bytes; uint64_t total = data.length, base = 0;
    uint32_t magic = OSReadBigInt32(bytes, 0);
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        uint32_t narch = OSReadBigInt32(bytes, 4);
        for (uint32_t i = 0; i < narch && 8 + (i + 1) * 20 <= total; i++) {
            uint32_t cputype = OSReadBigInt32(bytes, 8 + i * 20);
            uint32_t off = OSReadBigInt32(bytes, 8 + i * 20 + 8);
            if (cputype == CPU_TYPE_ARM64) { base = off; break; }
        }
        if (!base && narch) base = OSReadBigInt32(bytes, 8 + 8);
    }
    if (base + sizeof(struct mach_header_64) > total) return nil;
    struct mach_header_64 h; memcpy(&h, bytes + base, sizeof h);
    if (h.magic != MH_MAGIC_64) return nil;
    uint64_t at = base + sizeof h, end = at + h.sizeofcmds;
    if (end > total) return nil;
    NSString *cdhash = nil;
    for (uint32_t i = 0; i < h.ncmds && at + sizeof(struct load_command) <= end; i++) {
        struct load_command lc; memcpy(&lc, bytes + at, sizeof lc);
        if (lc.cmdsize < sizeof lc || at + lc.cmdsize > end) break;
        if ((lc.cmd == LC_ENCRYPTION_INFO || lc.cmd == LC_ENCRYPTION_INFO_64) && lc.cmdsize >= 20) {
            uint32_t cryptid = OSReadLittleInt32(bytes + at, 16);
            if (cryptid != 0 && encrypted) *encrypted = YES;
        }
        if (lc.cmd == LC_CODE_SIGNATURE && lc.cmdsize >= sizeof(struct linkedit_data_command)) {
            struct linkedit_data_command sig; memcpy(&sig, bytes + at, sizeof sig);
            uint64_t off = base + sig.dataoff, len = sig.datasize;
            if (off + len > total || len < 12) { at += lc.cmdsize; continue; }
            const uint8_t *blob = bytes + off;
            if (OSReadBigInt32(blob, 0) != 0xfade0cc0) { at += lc.cmdsize; continue; }  // embedded signature superblob
            uint32_t sbTotal = OSReadBigInt32(blob, 4), sbCount = OSReadBigInt32(blob, 8);
            if (sbTotal > len || sbCount > (sbTotal - 12) / 8) { at += lc.cmdsize; continue; }
            for (uint32_t j = 0; j < sbCount; j++) {
                uint32_t type = OSReadBigInt32(blob, 12 + j * 8), entry = OSReadBigInt32(blob, 16 + j * 8);
                if (type != 0) continue;                                 // CSSLOT_CODEDIRECTORY = 0 (primary CD)
                if (entry + 8 > sbTotal || OSReadBigInt32(blob, entry) != 0xfade0c02) continue;  // CodeDirectory magic
                uint32_t cdLen = OSReadBigInt32(blob, entry + 4);
                if (entry + cdLen > sbTotal) continue;
                uint8_t digest[CC_SHA256_DIGEST_LENGTH];
                CC_SHA256(blob + entry, cdLen, digest);                  // SHA-256 of the whole CodeDirectory
                NSMutableString *hex = [NSMutableString string];
                for (int k = 0; k < 20; k++) [hex appendFormat:@"%02x", digest[k]];   // 20-byte trust-cache truncation
                cdhash = hex;
                break;
            }
        }
        at += lc.cmdsize;
    }
    return cdhash;
}

// Every Mach-O in a bundle: the main executable and whatever lives under Frameworks/ PlugIns/ and nested bundles. A file is a
// Mach-O if it begins with a 64-bit or fat magic.
static NSArray<NSString *> *bundleMachOs(NSString *app) {
    NSMutableArray *found = [NSMutableArray array];
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
    NSString *exe = info[@"CFBundleExecutable"];
    if ([exe isKindOfClass:NSString.class]) {
        NSString *p = [app stringByAppendingPathComponent:exe];
        if ([NSFileManager.defaultManager fileExistsAtPath:p]) [found addObject:p];
    }
    NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtPath:app];
    for (NSString *rel in e) {
        NSString *p = [app stringByAppendingPathComponent:rel];
        NSData *head = [NSData dataWithContentsOfFile:p options:NSDataReadingMappedIfSafe error:nil];
        if (head.length < 4) continue;
        uint32_t m = OSReadBigInt32(head.bytes, 0);
        if ((m == MH_MAGIC_64 || m == FAT_MAGIC || m == FAT_CIGAM) && ![found containsObject:p]) [found addObject:p];
    }
    return found;
}

// Print the CDHash and encryption verdict of each Mach-O in app. Returns YES if any Mach-O is FairPlay-encrypted.
static BOOL scanBundle(NSString *app, NSMutableArray<NSString *> *cdhashes) {
    BOOL anyEncrypted = NO;
    for (NSString *macho in bundleMachOs(app)) {
        BOOL enc = NO;
        NSString *h = machoCDHash(macho, &enc);
        anyEncrypted |= enc;
        if (h && cdhashes) [cdhashes addObject:h];
        line(@"  %@  %@%@", h ?: @"(no CDHash)                        ", enc ? @"ENCRYPTED " : @"", macho.lastPathComponent);
    }
    return anyEncrypted;
}

// ---- LaunchServices / MobileContainerManager (private, looked up at run time) ----
static NSURL *ownerContainer(NSString *className, NSString *identifier, BOOL create, BOOL *existed) {
    dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
    Class cls = NSClassFromString(className);
    SEL lookup = sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:");
    if (![cls respondsToSelector:lookup]) { line(@"ipatool: MCM %@ unavailable", className); return nil; }
    NSError *error = nil; BOOL did = NO;
    id container = ((id (*)(Class, SEL, NSString *, BOOL, BOOL *, NSError **))objc_msgSend)(cls, lookup, identifier, create, &did, &error);
    if (existed) *existed = did;
    NSURL *url = call(container, "url");
    if (!url) line(@"ipatool: MCM %@ %@: %@", className, identifier, error);
    return url;
}

// Entitlements embedded in the signed executable, for the registration dictionary.
static NSDictionary *machoEntitlements(NSString *path) {
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    if (data.length < sizeof(struct mach_header_64)) return nil;
    const uint8_t *bytes = data.bytes; uint64_t total = data.length;
    struct mach_header_64 h; memcpy(&h, bytes, sizeof h);
    if (h.magic != MH_MAGIC_64) return nil;
    uint64_t at = sizeof h, end = at + h.sizeofcmds;
    if (end > total) return nil;
    for (uint32_t i = 0; i < h.ncmds && at + sizeof(struct load_command) <= end; i++) {
        struct load_command lc; memcpy(&lc, bytes + at, sizeof lc);
        if (lc.cmdsize < sizeof lc || at + lc.cmdsize > end) break;
        if (lc.cmd == LC_CODE_SIGNATURE && lc.cmdsize >= sizeof(struct linkedit_data_command)) {
            struct linkedit_data_command sig; memcpy(&sig, bytes + at, sizeof sig);
            uint64_t off = sig.dataoff, len = sig.datasize;
            if (off + len > total || len < 12) return nil;
            const uint8_t *blob = bytes + off;
            uint32_t sbTotal = OSReadBigInt32(blob, 4), sbCount = OSReadBigInt32(blob, 8);
            if (OSReadBigInt32(blob, 0) != 0xfade0cc0 || sbTotal > len || sbCount > (sbTotal - 12) / 8) return nil;
            for (uint32_t j = 0; j < sbCount; j++) {
                uint32_t entry = OSReadBigInt32(blob, 16 + j * 8);
                if (entry + 8 > sbTotal || OSReadBigInt32(blob, entry) != 0xfade7171) continue;  // entitlements blob
                uint32_t length = OSReadBigInt32(blob, entry + 4);
                if (length < 8 || entry + length > sbTotal) return nil;
                NSData *xml = [NSData dataWithBytes:blob + entry + 8 length:length - 8];
                id plist = [NSPropertyListSerialization propertyListWithData:xml options:0 format:NULL error:nil];
                return [plist isKindOfClass:NSDictionary.class] ? plist : nil;
            }
        }
        at += lc.cmdsize;
    }
    return nil;
}

#define STAGING @"/private/var/ngpu/apps"
// A path under one of the given roots, in either the /private/var or the /var spelling (stringByStandardizingPath rewrites
// /private/var -> /var, so a single-prefix test misses the other form).
static BOOL underRoot(NSString *path, NSString *root) {
    NSString *alt = [root hasPrefix:@"/private/var/"] ? [root substringFromIndex:8] : [@"/private" stringByAppendingString:root];
    return [path hasPrefix:root] || [path hasPrefix:alt];
}

// Move an unpacked .app into an MCM bundle container and register it with LaunchServices, as lsdiag's owner-install +
// owner-register do, but for any bundle identifier (not only org.droidvm.*). The app must already sit under STAGING.
static BOOL installApp(NSString *app) {
    NSString *source = app.stringByStandardizingPath;
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[source stringByAppendingPathComponent:@"Info.plist"]];
    NSString *identifier = info[@"CFBundleIdentifier"], *executable = info[@"CFBundleExecutable"];
    if (!underRoot(source, STAGING @"/") || ![source.pathExtension isEqual:@"app"] ||
        ![identifier isKindOfClass:NSString.class] || ![executable isKindOfClass:NSString.class]) {
        line(@"ipatool: install expects an unpacked .app under %@ with an Info.plist (id=%@)", STAGING, identifier); return NO;
    }
    NSURL *container = ownerContainer(@"MCMAppContainer", identifier, YES, NULL);
    NSURL *dataContainer = ownerContainer(@"MCMAppDataContainer", identifier, YES, NULL);
    if (!container || !dataContainer) return NO;
    NSString *destination = [container.path stringByAppendingPathComponent:source.lastPathComponent];
    struct stat a, b;
    if (stat(source.fileSystemRepresentation, &a) || stat(container.path.fileSystemRepresentation, &b) || a.st_dev != b.st_dev) {
        line(@"ipatool: install: source and container are on different volumes"); return NO;
    }
    struct stat existing;
    if (lstat(destination.fileSystemRepresentation, &existing) == 0) {
        line(@"ipatool: install: destination already exists: %@ (remove the app first)", destination); return NO;
    }
    if (rename(source.fileSystemRepresentation, destination.fileSystemRepresentation)) {
        line(@"ipatool: install: rename: %s", strerror(errno)); return NO;
    }
    symlink(destination.fileSystemRepresentation, source.fileSystemRepresentation);   // keep a CLI handle in staging

    id workspace = launchServices();
    SEL reg = sel_registerName("registerContainerizedApplicationWithInfoDictionaries:personaUniqueStrings:operationUUID:requestContext:saveObserver:registrationError:");
    if (![workspace respondsToSelector:reg]) { line(@"ipatool: register selector unavailable"); return NO; }
    NSDictionary *ents = machoEntitlements([destination stringByAppendingPathComponent:executable]);
    NSMutableDictionary *item = [info mutableCopy];
    if (ents) item[@"Entitlements"] = ents;
    item[@"Container"] = dataContainer.path;
    [item addEntriesFromDictionary:@{@"ApplicationType": @"User", @"CodeInfoIdentifier": identifier, @"Path": destination,
        @"CompatibilityState": @0, @"IsContainerized": @(!(ents && [ents[@"com.apple.private.security.no-container"] boolValue])),
        @"IsDeletable": @YES, @"IsAdHocSigned": @YES, @"LSInstallType": @1, @"_LSBundlePlugins": @{}}];
    NSError *error = nil;
    BOOL ok = ((BOOL (*)(id, SEL, NSArray *, NSArray *, NSUUID *, id, id, NSError **))objc_msgSend)(
        workspace, reg, @[item], nil, [NSUUID UUID], nil, nil, &error);
    line(@"ipatool: install %@ -> %@: register %@ %@", identifier, destination, ok ? @"OK" : @"FAILED", error ?: @"");
    return ok;
}

static void removeApp(NSString *identifier) {
    id workspace = launchServices();
    // Resolve the real installed bundle URL (a random-UUID path) through the application proxy, a class method on
    // LSApplicationProxy, before anything is destroyed.
    Class proxyClass = NSClassFromString(@"LSApplicationProxy");
    SEL proxyFor = sel_registerName("applicationProxyForIdentifier:");
    id proxy = (proxyClass && [proxyClass respondsToSelector:proxyFor]) ?
        ((id (*)(Class, SEL, NSString *))objc_msgSend)(proxyClass, proxyFor, identifier) : nil;
    NSURL *bundleURL = call(proxy, "bundleURL");

    // Unregister FIRST (so LaunchServices still sees the bundle), then destroy the containers. Try unregisterApplication:
    // with the real URL; fall back to the containerized-by-identifier unregister that mirrors our register call.
    BOOL unregistered = NO;
    SEL unreg = sel_registerName("unregisterApplication:");
    if (bundleURL && [workspace respondsToSelector:unreg])
        unregistered = ((BOOL (*)(id, SEL, NSURL *))objc_msgSend)(workspace, unreg, bundleURL);
    if (!unregistered) {
        SEL cunreg = sel_registerName("unregisterContainerizedApplicationWithBundleIdentifier:operationUUID:unregistrationOperation:requestContext:saveObserver:unregistrationError:");
        if ([workspace respondsToSelector:cunreg]) {
            NSError *err = nil;
            unregistered = ((BOOL (*)(id, SEL, NSString *, NSUUID *, id, id, id, NSError **))objc_msgSend)(
                workspace, cunreg, identifier, [NSUUID UUID], nil, nil, nil, &err);
            if (!unregistered) line(@"ipatool: remove %@: containerized unregister: %@", identifier, err ?: @"FAILED");
        }
    }
    line(@"ipatool: remove %@: unregister %@", identifier, unregistered ? @"OK" : @"FAILED");

    for (NSString *cls in @[@"MCMAppContainer", @"MCMAppDataContainer"]) {
        BOOL existed = NO;
        NSURL *url = ownerContainer(cls, identifier, NO, &existed);
        if (!url) continue;
        dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
        id container = ((id (*)(Class, SEL, NSString *, BOOL, BOOL *, NSError **))objc_msgSend)(
            NSClassFromString(cls), sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:"), identifier, NO, &existed, NULL);
        SEL destroy = sel_registerName("destroyContainerWithCompletion:");
        if (container && ![container respondsToSelector:destroy]) destroy = sel_registerName("destroyContainerWithCompletionHandler:");
        if (container && [container respondsToSelector:destroy]) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block NSError *err = nil;
            ((void (*)(id, SEL, void (^)(NSError *)))objc_msgSend)(container, destroy, ^(NSError *e){ err = e; dispatch_semaphore_signal(done); });
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
            line(@"ipatool: remove %@: %@ destroy %@", identifier, cls, err ?: @"OK");
        } else {
            [NSFileManager.defaultManager removeItemAtURL:url error:NULL];
            line(@"ipatool: remove %@: %@ removed %@ (no destroy selector)", identifier, cls, url.path);
        }
    }
    if (bundleURL && underRoot(bundleURL.path, @"/private/var/containers/Bundle/Application/"))
        [NSFileManager.defaultManager removeItemAtURL:bundleURL error:NULL];
}

// Unpack the IPA into a fresh staging dir and return the Payload/<app> path.
static NSString *unpackToStaging(NSString *ipa) {
    NSString *dest = [STAGING stringByAppendingPathComponent:[@"ipatool-" stringByAppendingString:[NSUUID UUID].UUIDString]];
    [NSFileManager.defaultManager createDirectoryAtPath:dest withIntermediateDirectories:YES attributes:nil error:nil];
    if (!extractZip(ipa, dest)) return nil;
    NSString *payload = [dest stringByAppendingPathComponent:@"Payload"];
    for (NSString *entry in [NSFileManager.defaultManager contentsOfDirectoryAtPath:payload error:nil])
        if ([entry.pathExtension isEqual:@"app"]) return [payload stringByAppendingPathComponent:entry];
    line(@"ipatool: no Payload/*.app in %@", ipa);
    return nil;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSMutableArray<NSString *> *args = [NSMutableArray array];
        BOOL force = NO;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-f")) force = YES; else [args addObject:@(argv[i])];
        }
        NSString *verb = args.count ? args[0] : nil;
        if ([verb isEqual:@"unpack"] && args.count == 3) {
            if (!extractZip(args[1], args[2])) gStatus = 1;
            else line(@"ipatool: unpacked %@ -> %@", args[1], args[2]);
        } else if ([verb isEqual:@"scan"] && args.count == 2) {
            NSString *app = unpackToStaging(args[1]);
            if (!app) { gStatus = 1; }
            else { line(@"ipatool: %@", app); scanBundle(app, nil); }
        } else if ([verb isEqual:@"install"] && args.count == 2) {
            NSString *app = unpackToStaging(args[1]);
            if (!app) { gStatus = 1; }
            else {
                line(@"ipatool: %@", app);
                NSMutableArray *cdhashes = [NSMutableArray array];
                BOOL enc = scanBundle(app, cdhashes);
                if (enc && !force) { line(@"ipatool: refusing to install a FairPlay-encrypted app (use -f to override)"); gStatus = 1; }
                else {
                    if (!installApp(app)) gStatus = 1;
                    line(@"ipatool: CDHashes to trust (%lu):", (unsigned long)cdhashes.count);
                    for (NSString *h in cdhashes) line(@"    %@", h);
                }
            }
        } else if ([verb isEqual:@"remove"] && args.count >= 2) {
            for (NSUInteger i = 1; i < args.count; i++) removeApp(args[i]);
        } else if ([verb isEqual:@"sels"] && args.count >= 2) {
            // introspection: print the methods of a (private) class whose name contains the optional substring, so the exact
            // MCM/LaunchServices selectors are known from the device instead of guessed.
            dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
            dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
            NSString *want = args.count >= 3 ? args[2].lowercaseString : nil;
            Class cls = NSClassFromString(args[1]);
            if (!cls) { line(@"ipatool: no class %@", args[1]); gStatus = 1; }
            else for (Class c = cls; c; c = class_getSuperclass(c)) {
                Class meties[2] = {c, object_getClass(c)};   // instance methods, then class methods (the metaclass)
                for (int mi = 0; mi < 2; mi++) {
                    unsigned n = 0; Method *m = class_copyMethodList(meties[mi], &n);
                    for (unsigned i = 0; i < n; i++) {
                        NSString *name = NSStringFromSelector(method_getName(m[i]));
                        if (!want || [name.lowercaseString containsString:want]) line(@"  %@[%@ %@]", mi ? @"+" : @"-", NSStringFromClass(c), name);
                    }
                    free(m);
                }
            }
        } else {
            fprintf(stderr, "usage: ipatool scan IPA | install [-f] IPA | remove BUNDLE_ID... | unpack IPA DEST | sels CLASS [SUBSTR]\n");
            gStatus = 2;
        }
    }
    return gStatus;
}
