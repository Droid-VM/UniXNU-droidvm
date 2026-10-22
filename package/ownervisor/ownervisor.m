// SPDX-License-Identifier: MIT
// ownervisor — a tiny service manager for the DroidVM guest, launched once from a hijacked launchd slot
// (com.apple.mtmergeprops, category 3, unused with virtio input). It brings up the owner debug services
// (an ssh server, an IPv6 keepalive) so one launchd slot stands in for many, and lifts the "one unused
// RunAtLoad label per service" ceiling.
//
// Trust: this binary and every service binary it spawns must be in the owner trust module by CDHash
// (build/owner-trust-extra/ownervisor.json). AMFI checks the hash at exec, not the path, so the binaries
// live in a writable owner-Data dir and are swap-safe (a changed binary just fails to exec). A brand-new
// binary cannot be trusted at runtime; adding a service that runs an already-trusted binary needs no reseed.
//
// Layout under $OWNERVISOR_ROOT (default /private/var/ngpu/ownervisor), all in the writable Data volume:
//   services.d/<name>.json   service definition  {exec:[...], uid, keepalive, throttle, env:{...}}
//   enabled/<name>           marker: present => autostart at boot (systemd-style enable/disable)
//   bin/                     the trusted service binaries the seed ships
//   run/ctl.sock             control socket (ownerctl, local or over ssh)
//   control/<id>.req|.rep    file control spool: the host drops a .req (export.py put), reads the .rep
//   log/<name>.log           each service's stdout+stderr
//   state/status.json        the live table, refreshed each tick (host reads it with export.py cat)
//
// Control verbs (socket line-JSON or a .req file): list, status, start, stop, restart, enable, disable,
// add, remove, reload. enable/disable only change boot autostart; "now":true also starts/stops at once.
#import <Foundation/Foundation.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static NSString *gRoot;
static NSMutableDictionary *gServices;   // name -> Service
static int gKq;

// ---- one service ----
@interface Service : NSObject
@property (copy) NSString *name;
@property (copy) NSArray<NSString *> *exec;    // argv
@property (copy) NSDictionary<NSString *, NSString *> *env;
@property int uid;                             // 0 = root (no switch)
@property BOOL keepalive;
@property int throttle;                        // seconds between restarts
@property pid_t pid;                           // 0 when not running
@property BOOL manualStop;                     // stopped by a stop/disable, do not restart
@property int restarts;
@property int lastStatus;                      // last wait status, -1 if none
@property time_t startedAt;
@property time_t restartAt;                    // 0, or when a backing-off restart is due
@property (copy) NSString *killAfter;          // set to a time string when a SIGTERM awaits SIGKILL
@property time_t sigkillAt;
@end
@implementation Service @end

// ---- small helpers ----
static NSString *P(NSString *sub) { return [gRoot stringByAppendingPathComponent:sub]; }

static void ensureDirs(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *d in @[@"services.d", @"enabled", @"bin", @"run", @"control", @"log", @"state"])
        [fm createDirectoryAtPath:P(d) withIntermediateDirectories:YES attributes:nil error:nil];
}

static BOOL isEnabled(NSString *name) {
    return [NSFileManager.defaultManager fileExistsAtPath:[P(@"enabled") stringByAppendingPathComponent:name]];
}

static Service *parseDef(NSString *name, NSDictionary *d) {
    NSArray *exec = d[@"exec"];
    if (![exec isKindOfClass:NSArray.class] || exec.count == 0) return nil;
    for (id a in exec) if (![a isKindOfClass:NSString.class]) return nil;
    Service *s = gServices[name] ?: [Service new];
    s.name = name;
    s.exec = exec;
    s.env = [d[@"env"] isKindOfClass:NSDictionary.class] ? d[@"env"] : @{};
    s.uid = [d[@"uid"] respondsToSelector:@selector(intValue)] ? [d[@"uid"] intValue] : 0;
    s.keepalive = d[@"keepalive"] ? [d[@"keepalive"] boolValue] : YES;
    s.throttle = [d[@"throttle"] respondsToSelector:@selector(intValue)] ? [d[@"throttle"] intValue] : 5;
    if (s.throttle < 1) s.throttle = 1;
    if (!s.lastStatus) s.lastStatus = -1;
    return s;
}

// Load (or reload) every services.d/*.json into gServices. Returns the set of names now defined.
static NSMutableSet *loadServices(void) {
    NSMutableSet *seen = [NSMutableSet set];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *f in [fm contentsOfDirectoryAtPath:P(@"services.d") error:nil]) {
        if (![f.pathExtension isEqual:@"json"]) continue;
        NSString *name = f.stringByDeletingPathExtension;
        NSData *data = [NSData dataWithContentsOfFile:[P(@"services.d") stringByAppendingPathComponent:f]];
        if (!data) continue;
        id j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![j isKindOfClass:NSDictionary.class]) continue;
        Service *s = parseDef(name, j);
        if (s) { gServices[name] = s; [seen addObject:name]; }
    }
    return seen;
}

// ---- spawn / stop ----
// fork+exec so a service can run under its own uid (posix_spawn cannot switch user). Between fork and
// execv only async-signal-safe calls run: argv/env/paths are built as C arrays in the parent first.
static void startService(Service *s) {
    if (s.pid) return;
    s.manualStop = NO;
    s.restartAt = 0;

    int argc = (int)s.exec.count;
    char **argv = calloc(argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) argv[i] = strdup(s.exec[i].fileSystemRepresentation);
    NSMutableDictionary *envd = [NSMutableDictionary dictionary];   // inherit our environ, then overlay the service's env
    for (char **e = environ; *e; e++) {
        NSString *kv = @(*e);
        NSRange eq = [kv rangeOfString:@"="];
        if (eq.location != NSNotFound) envd[[kv substringToIndex:eq.location]] = [kv substringFromIndex:eq.location + 1];
    }
    [envd addEntriesFromDictionary:s.env];
    NSMutableArray *envp = [NSMutableArray array];
    [envd enumerateKeysAndObjectsUsingBlock:^(NSString *k, NSString *v, BOOL *stop) {
        (void)stop;
        [envp addObject:[NSString stringWithFormat:@"%@=%@", k, v]];
    }];
    char **env = calloc(envp.count + 1, sizeof(char *));
    for (NSUInteger i = 0; i < envp.count; i++) env[i] = strdup([envp[i] UTF8String]);
    const char *path = argv[0];
    const char *logc = [[P(@"log") stringByAppendingPathComponent:
                        [s.name stringByAppendingPathExtension:@"log"]] fileSystemRepresentation];
    char *logpath = strdup(logc);
    int uid = s.uid;

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        if (uid > 0) { setgid((gid_t)uid); setuid((uid_t)uid); }
        execve(path, argv, env);
        _exit(127);
    }
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    for (NSUInteger i = 0; i < envp.count; i++) free(env[i]);
    free(env);
    free(logpath);
    if (pid < 0) { NSLog(@"ownervisor: fork for %@ failed: %s", s.name, strerror(errno)); return; }
    s.pid = pid;
    s.startedAt = time(NULL);
    NSLog(@"ownervisor: started %@ pid %d", s.name, pid);
}

static void stopService(Service *s) {
    s.manualStop = YES;
    s.restartAt = 0;
    if (s.pid) {
        kill(s.pid, SIGTERM);
        s.sigkillAt = time(NULL) + 5;   // escalate if it ignores SIGTERM
    }
}

// ---- status ----
static NSString *stateString(Service *s) {
    if (s.pid) return @"running";
    if (s.restartAt) return @"backoff";
    if (s.manualStop) return @"stopped";
    return s.lastStatus >= 0 ? @"exited" : @"idle";
}

static NSDictionary *statusOf(Service *s) {
    time_t now = time(NULL);
    NSMutableDictionary *m = [@{
        @"name": s.name,
        @"state": stateString(s),
        @"enabled": @(isEnabled(s.name)),
        @"keepalive": @(s.keepalive),
        @"restarts": @(s.restarts),
        @"exec": s.exec,
    } mutableCopy];
    if (s.pid) { m[@"pid"] = @(s.pid); m[@"uptime"] = @(now - s.startedAt); }
    int st = s.lastStatus;
    if (st >= 0) {
        if (WIFEXITED(st)) m[@"last_exit"] = @(WEXITSTATUS(st));
        else if (WIFSIGNALED(st)) m[@"last_signal"] = @(WTERMSIG(st));
    }
    return m;
}

static NSData *jsonData(id obj) {
    return [NSJSONSerialization dataWithJSONObject:obj
            options:NSJSONWritingSortedKeys error:nil] ?: [NSData data];
}

static void writeStatusFile(void) {
    NSMutableArray *a = [NSMutableArray array];
    for (NSString *n in [gServices.allKeys sortedArrayUsingSelector:@selector(compare:)])
        [a addObject:statusOf(gServices[n])];
    NSString *tmp = [P(@"state") stringByAppendingPathComponent:@".status.tmp"];
    NSString *dst = [P(@"state") stringByAppendingPathComponent:@"status.json"];
    if ([jsonData(a) writeToFile:tmp atomically:NO])
        rename(tmp.fileSystemRepresentation, dst.fileSystemRepresentation);
}

// ---- control ----
// One request dict -> one reply dict. cmd in {list,status,start,stop,restart,enable,disable,add,remove,reload}.
static NSDictionary *handle(NSDictionary *req) {
    NSString *cmd = req[@"cmd"];
    NSString *name = req[@"name"];
    BOOL now = [req[@"now"] boolValue];
    NSFileManager *fm = NSFileManager.defaultManager;

    if ([cmd isEqual:@"list"]) {
        NSMutableArray *a = [NSMutableArray array];
        for (NSString *n in [gServices.allKeys sortedArrayUsingSelector:@selector(compare:)])
            [a addObject:statusOf(gServices[n])];
        return @{@"ok": @YES, @"services": a};
    }
    if ([cmd isEqual:@"reload"]) {
        NSMutableSet *now_defined = loadServices();
        for (NSString *n in gServices.allKeys.copy)
            if (![now_defined containsObject:n] && !((Service *)gServices[n]).pid)
                [gServices removeObjectForKey:n];
        return @{@"ok": @YES, @"services": @(now_defined.count)};
    }
    if ([cmd isEqual:@"add"]) {
        if (!name || ![req[@"def"] isKindOfClass:NSDictionary.class]) return @{@"ok": @NO, @"error": @"add needs name and def"};
        Service *ns = parseDef(name, req[@"def"]);
        if (!ns) return @{@"ok": @NO, @"error": @"bad def (need exec:[...])"};
        gServices[name] = ns;
        NSString *path = [[P(@"services.d") stringByAppendingPathComponent:name] stringByAppendingPathExtension:@"json"];
        [jsonData(req[@"def"]) writeToFile:path atomically:YES];
        return @{@"ok": @YES};
    }

    if (!name) return @{@"ok": @NO, @"error": @"name required"};
    Service *s = gServices[name];
    if (!s && !([cmd isEqual:@"remove"])) return @{@"ok": @NO, @"error": @"no such service"};

    if ([cmd isEqual:@"status"]) return @{@"ok": @YES, @"service": statusOf(s)};
    if ([cmd isEqual:@"start"]) { startService(s); return @{@"ok": @YES, @"service": statusOf(s)}; }
    if ([cmd isEqual:@"stop"]) { stopService(s); return @{@"ok": @YES}; }
    if ([cmd isEqual:@"restart"]) { stopService(s); s.manualStop = NO; s.restartAt = time(NULL); return @{@"ok": @YES}; }
    if ([cmd isEqual:@"enable"]) {
        [fm createFileAtPath:[P(@"enabled") stringByAppendingPathComponent:name] contents:[NSData data] attributes:nil];
        if (now) startService(s);
        return @{@"ok": @YES};
    }
    if ([cmd isEqual:@"disable"]) {
        [fm removeItemAtPath:[P(@"enabled") stringByAppendingPathComponent:name] error:nil];
        if (now) stopService(s);
        return @{@"ok": @YES};
    }
    if ([cmd isEqual:@"remove"]) {
        if (s) stopService(s);
        [fm removeItemAtPath:[[P(@"services.d") stringByAppendingPathComponent:name] stringByAppendingPathExtension:@"json"] error:nil];
        [fm removeItemAtPath:[P(@"enabled") stringByAppendingPathComponent:name] error:nil];
        if (s && !s.pid) [gServices removeObjectForKey:name];
        return @{@"ok": @YES};
    }
    return @{@"ok": @NO, @"error": @"unknown cmd"};
}

static NSDictionary *handleBytes(NSData *data) {
    id j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![j isKindOfClass:NSDictionary.class]) return @{@"ok": @NO, @"error": @"bad request json"};
    return handle(j);
}

// Poll the file-control spool: each control/<id>.req -> run -> control/<id>.rep, then delete the .req.
static void pollControlSpool(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *f in [fm contentsOfDirectoryAtPath:P(@"control") error:nil]) {
        if (![f.pathExtension isEqual:@"req"]) continue;
        NSString *reqp = [P(@"control") stringByAppendingPathComponent:f];
        NSData *data = [NSData dataWithContentsOfFile:reqp];
        NSDictionary *rep = data ? handleBytes(data) : @{@"ok": @NO, @"error": @"empty"};
        NSString *base = f.stringByDeletingPathExtension;
        NSString *tmp = [P(@"control") stringByAppendingPathComponent:[base stringByAppendingPathExtension:@"tmp"]];
        NSString *repp = [P(@"control") stringByAppendingPathComponent:[base stringByAppendingPathExtension:@"rep"]];
        [jsonData(rep) writeToFile:tmp atomically:NO];
        rename(tmp.fileSystemRepresentation, repp.fileSystemRepresentation);
        [fm removeItemAtPath:reqp error:nil];
    }
}

// ---- socket ----
static int listenSocket(void) {
    const char *sp = [P(@"run/ctl.sock") fileSystemRepresentation];
    unlink(sp);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    strlcpy(a.sun_path, sp, sizeof(a.sun_path));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}

static void serveClient(int cfd) {
    NSMutableData *in = [NSMutableData data];
    char buf[4096];
    for (;;) {
        ssize_t n = read(cfd, buf, sizeof(buf));
        if (n <= 0) break;
        [in appendBytes:buf length:n];
        if (memchr(buf, '\n', n)) break;             // one line is one request
    }
    NSDictionary *rep = in.length ? handleBytes(in) : @{@"ok": @NO, @"error": @"empty"};
    NSMutableData *out = [jsonData(rep) mutableCopy];
    [out appendBytes:"\n" length:1];
    write(cfd, out.bytes, out.length);
    close(cfd);
}

// ---- reaping + restart scheduling ----
static void reapChildren(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (NSString *n in gServices) {
            Service *s = gServices[n];
            if (s.pid != pid) continue;
            s.pid = 0;
            s.lastStatus = status;
            s.sigkillAt = 0;
            NSLog(@"ownervisor: %@ exited (status 0x%x)", n, status);
            if (s.keepalive && !s.manualStop) { s.restarts++; s.restartAt = time(NULL) + s.throttle; }
            break;
        }
    }
}

static void noopHandler(int s) { (void)s; }   // SIGCHLD wakeup only; reaping is done from the kqueue loop

static void tick(void) {
    time_t now = time(NULL);
    for (NSString *n in gServices) {
        Service *s = gServices[n];
        if (s.restartAt && now >= s.restartAt) { s.restartAt = 0; startService(s); }
        if (s.pid && s.sigkillAt && now >= s.sigkillAt) { kill(s.pid, SIGKILL); s.sigkillAt = 0; }
    }
    pollControlSpool();
    writeStatusFile();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        const char *env = getenv("OWNERVISOR_ROOT");
        gRoot = env ? @(env) : @"/private/var/ngpu/ownervisor";
        gServices = [NSMutableDictionary dictionary];
        ensureDirs();
        // launchd gives us /dev/null for stderr (the hijacked job writes nowhere), so keep our own log in
        // the Data volume where the rig can read it. Both streams, so a service's pre-exec write lands too.
        const char *boot = [[P(@"state") stringByAppendingPathComponent:@"boot.log"] fileSystemRepresentation];
        freopen(boot, "a", stdout);
        freopen(boot, "a", stderr);
        setvbuf(stderr, NULL, _IOLBF, 0);
        NSLog(@"ownervisor: root %@ pid %d uid %d", gRoot, getpid(), getuid());

        loadServices();
        for (NSString *n in gServices) if (isEnabled(n)) startService(gServices[n]);

        int lfd = listenSocket();
        if (lfd < 0) NSLog(@"ownervisor: control socket failed: %s", strerror(errno));

        // SIGCHLD must NOT be SIG_IGN: that makes the kernel auto-reap and never deliver the signal,
        // so EVFILT_SIGNAL never fires. An empty handler leaves the child reapable and the event firing.
        signal(SIGCHLD, noopHandler);
        signal(SIGPIPE, SIG_IGN);
        gKq = kqueue();
        struct kevent ev[3];
        int nev = 0;
        if (lfd >= 0) EV_SET(&ev[nev++], lfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        EV_SET(&ev[nev++], SIGCHLD, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
        EV_SET(&ev[nev++], 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);   // 1 s
        kevent(gKq, ev, nev, NULL, 0, NULL);
        writeStatusFile();

        for (;;) {
            struct kevent out;
            int n = kevent(gKq, NULL, 0, &out, 1, NULL);
            if (n <= 0) continue;
            if (out.filter == EVFILT_READ && (int)out.ident == lfd) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd >= 0) @autoreleasepool { serveClient(cfd); }
            } else if (out.filter == EVFILT_SIGNAL) {
                reapChildren();
            } else if (out.filter == EVFILT_TIMER) {
                @autoreleasepool { tick(); }
            }
        }
    }
}
