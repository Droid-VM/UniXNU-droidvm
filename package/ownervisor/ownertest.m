// SPDX-License-Identifier: MIT
// ownertest — the PoC service: it proves an owner binary can be posix_spawned by another owner binary
// (ownervisor) under AMFI. It writes a marker with its pid/uid/ppid, then idles until SIGTERM so the
// supervisor shows it "running" and a stop exits it cleanly.
#import <Foundation/Foundation.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t gRun = 1;
static void onTerm(int s) { (void)s; gRun = 0; }

int main(int argc, char **argv) {
    @autoreleasepool {
        signal(SIGTERM, onTerm);
        NSString *root = @(getenv("OWNERVISOR_ROOT") ?: "/private/var/ngpu/ownervisor");
        NSString *tag = argc > 1 ? @(argv[1]) : @"ownertest";
        NSDictionary *m = @{@"tag": tag, @"pid": @(getpid()), @"ppid": @(getppid()),
                            @"uid": @(getuid()), @"time": @((long)time(NULL))};
        NSString *path = [[root stringByAppendingPathComponent:@"state"]
                          stringByAppendingPathComponent:[tag stringByAppendingPathExtension:@"marker"]];
        [[NSJSONSerialization dataWithJSONObject:m options:NSJSONWritingSortedKeys error:nil] writeToFile:path atomically:YES];
        NSLog(@"ownertest(%@): pid %d ppid %d uid %d marker %@", tag, getpid(), getppid(), getuid(), path);
        while (gRun) sleep(1);
        return 0;
    }
}
