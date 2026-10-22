// SPDX-License-Identifier: MIT
// ownerctl — the client for ownervisor's control socket. Used in the guest, or over ssh once the ssh
// service is up. The host, before ssh, drives the same verbs through the file spool with export.py put.
//
//   ownerctl [--sock PATH] list
//   ownerctl [--sock PATH] status|start|stop|restart|enable|disable|remove NAME [--now]
//   ownerctl [--sock PATH] add NAME --def '{"exec":["/private/var/ngpu/ownervisor/bin/foo"],"keepalive":true}'
//   ownerctl [--sock PATH] reload
#import <Foundation/Foundation.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *sock = @"/private/var/ngpu/ownervisor/run/ctl.sock";
        NSMutableArray *rest = [NSMutableArray array];
        NSString *defJSON = nil;
        BOOL now = NO;
        for (int i = 1; i < argc; i++) {
            NSString *a = @(argv[i]);
            if ([a isEqual:@"--sock"] && i + 1 < argc) sock = @(argv[++i]);
            else if ([a isEqual:@"--def"] && i + 1 < argc) defJSON = @(argv[++i]);
            else if ([a isEqual:@"--now"]) now = YES;
            else [rest addObject:a];
        }
        if (rest.count < 1) { fprintf(stderr, "usage: ownerctl [--sock P] CMD [NAME] [--now] [--def JSON]\n"); return 2; }

        NSMutableDictionary *req = [@{@"cmd": rest[0]} mutableCopy];
        if (rest.count >= 2) req[@"name"] = rest[1];
        if (now) req[@"now"] = @YES;
        if (defJSON) {
            id d = [NSJSONSerialization JSONObjectWithData:[defJSON dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
            if (!d) { fprintf(stderr, "ownerctl: --def is not valid json\n"); return 2; }
            req[@"def"] = d;
        }

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa = {0};
        sa.sun_family = AF_UNIX;
        strlcpy(sa.sun_path, sock.fileSystemRepresentation, sizeof(sa.sun_path));
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            fprintf(stderr, "ownerctl: cannot reach %s: %s\n", sa.sun_path, strerror(errno));
            return 1;
        }
        NSMutableData *out = [[NSJSONSerialization dataWithJSONObject:req options:0 error:nil] mutableCopy];
        [out appendBytes:"\n" length:1];
        write(fd, out.bytes, out.length);

        NSMutableData *in = [NSMutableData data];
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) { [in appendBytes:buf length:n]; if (memchr(buf, '\n', n)) break; }
        close(fd);

        id rep = [NSJSONSerialization JSONObjectWithData:in options:0 error:nil];
        NSData *pretty = [NSJSONSerialization dataWithJSONObject:(rep ?: @{}) options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
        fwrite(pretty.bytes, 1, pretty.length, stdout);
        fputc('\n', stdout);
        return [rep[@"ok"] boolValue] ? 0 : 1;
    }
}
