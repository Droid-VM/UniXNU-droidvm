// SPDX-License-Identifier: MIT
// launchd overrides for the jobs a DroidVM guest cannot serve, written as root into the mounted scratch Data
// before launchd reads them (XNU_BOARD_LAUNCH_POLICY, patches/xnu/native-board-launch-policy.patch).
//
// UniXNU-sheng's package/BootWatchdogPolicy.c without its Setup reset: the same plist and checks, the labels
// from droidvm-disable-jobs=<label>[,<label>...] (restricted characters, bounded length). launchd does not
// honour a disabled.plist written into the seed on the Mac, which belongs to the user who built the seed.
// The UniXNU guide lists the jobs that block the first boot (docs/guide/skip-setup.md); watchdogd has no
// watchdog backend here.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <pexpert/pexpert.h>

static const char policy_head[] =
 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
 "<plist version=\"1.0\"><dict>";
static const char policy_tail[]="</dict></plist>\n";
static char policy[768],jobs[200];
volatile int droidvm_launch_policy_status=-1;
volatile unsigned droidvm_launch_policy_calls;

static int write_overrides(void *process, uint64_t scratch) {
 proc_t p=process;
 int error=0,fd=-1;int32_t result=0;
 droidvm_launch_policy_calls=droidvm_launch_policy_calls+1;
 if(!PE_parse_boot_argn("droidvm-disable-jobs",jobs,sizeof(jobs)))jobs[0]=0;jobs[sizeof(jobs)-1]=0;
 for(const char *c=jobs;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='-'||*c==','))return EINVAL;
 if(!jobs[0]){droidvm_launch_policy_status=0;return 0;}
 policy[0]=0;strlcat(policy,policy_head,sizeof(policy));
 for(const char *c=jobs;*c;) {
  const char *e=c;while(*e && *e!=',')++e;
  if(e>c) {
   if(strlcat(policy,"<key>",sizeof(policy))>=sizeof(policy))return EINVAL;
   size_t at=strlen(policy);if(at+(size_t)(e-c)+1>=sizeof(policy))return EINVAL;
   memcpy(policy+at,c,(size_t)(e-c));policy[at+(size_t)(e-c)]=0;
   if(strlcat(policy,"</key><true/>",sizeof(policy))>=sizeof(policy))return EINVAL;
  }
  c=*e?e+1:e;
 }
 strlcat(policy,policy_tail,sizeof(policy));
 const char *directory="/private/var/db/com.apple.xpc.launchd";
 const char *path="/private/var/db/com.apple.xpc.launchd/disabled.plist";
 if((error=copyout(directory,scratch,strlen(directory)+1)))goto done;
 struct mkdir_args md={.path=scratch,.mode=0755};
 error=mkdir(p,&md,&result);
 if(error && error!=EEXIST)goto done;
 if((error=copyout(path,scratch,strlen(path)+1)))goto done;
 // Rewritten in place before every launchd attempt; symlinks are refused (O_NOFOLLOW) and the fstat check below insists on a
 // root-owned regular file.
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0644};
 if((error=open(p,&op,&result)))goto done;
 fd=result;
 if((error=copyout(policy,scratch+0x200,strlen(policy))))goto done;
 struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=strlen(policy)};
 user_ssize_t written=0;
 if((error=write(p,&wr,&written)))goto done;
 if(written!=(user_ssize_t)(strlen(policy))){error=EIO;goto done;}
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 if((error=fchown(p,&owner,&result)))goto done;
 struct fchmod_args mode={.fd=fd,.mode=0644};
 if((error=fchmod(p,&mode,&result)))goto done;
 struct fsync_args sync={.fd=fd};
 if((error=fsync(p,&sync,&result)))goto done;
 struct fstat64_args sa={.fd=fd,.ub=scratch+0x800};struct stat64 st={0};
 if((error=sys_fstat64(p,&sa,&result)) || (error=copyin(sa.ub,&st,sizeof(st))))goto done;
 if(st.st_uid!=0 || st.st_gid!=0 || !S_ISREG(st.st_mode) || (st.st_mode&0777)!=0644 || st.st_size!=(off_t)(strlen(policy)))error=EINVAL;
 done:
 if(fd>=0){struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;}
 // A real empty migration marker selects the qualified modern override loader.
 if(!error) {
  const char *marker="/private/var/db/com.apple.xpc.launchd/disabled.migrated";
  error=copyout(marker,scratch,strlen(marker)+1);
  if(!error) {
   struct open_args mark={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
   error=open(p,&mark,&result);
   if(!error){struct close_args cl={.fd=result};error=sys_close(p,&cl,&result);}
   else if(error==EEXIST)error=0;
  }
 }
 droidvm_launch_policy_status=error;
 printf("droidvm launchd overrides (%s): %d\n",jobs,error);
 return error;
}

// droidvm-gpu-clients=<name>[,<name>...] ("*" = every process): the processes besides the render server that the Adreno Metal plugin
// gives a native Metal device. The plugin reads NativeAdrenoMetal.clients beside its bundle, which is this Data directory in the addon
// mount's view of /System/Library/Extensions (sheng-gpu-addon=1), so the list changes with a boot argument instead of a new Data seed.
// Written before every launchd attempt like the overrides; without the argument the file the Data seed carries stays. A failure is
// recorded, never fatal (the hook's errors panic): the processes then keep the software renderer.
static char clients[200];
volatile int droidvm_gpu_clients_status=-1;
static int write_gpu_clients(proc_t p, uint64_t scratch) {
 if(!PE_parse_boot_argn("droidvm-gpu-clients",clients,sizeof(clients)))return 0;
 clients[sizeof(clients)-2]=0;
 size_t length=strlen(clients);
 for(size_t i=0;i<length;++i) {
  const char c=clients[i];
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_'||c=='*'||c==','))return EINVAL;
  if(c==',')clients[i]='\n';
 }
 clients[length++]='\n';clients[length]=0;
 const char *path="/private/var/ngpu/addons/NativeAdrenoMetal.clients";
 int error=0,fd=-1;int32_t result=0;
 if((error=copyout(path,scratch,strlen(path)+1)))return error;
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0644};
 if((error=open(p,&op,&result)))return error;
 fd=result;
 if((error=copyout(clients,scratch+0x200,length)))goto done;
 struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=length};
 user_ssize_t written=0;
 if((error=write(p,&wr,&written)))goto done;
 if(written!=(user_ssize_t)length){error=EIO;goto done;}
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 if((error=fchown(p,&owner,&result)))goto done;
 struct fchmod_args mode={.fd=fd,.mode=0644};
 if((error=fchmod(p,&mode,&result)))goto done;
 struct fsync_args sync={.fd=fd};
 error=fsync(p,&sync,&result);
 done:
 {struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;}
 return error;
}

int native_board_launch_policy(void *process, uint64_t scratch) {
 const int error=write_overrides(process,scratch);
 droidvm_gpu_clients_status=write_gpu_clients(process,scratch);
 if(clients[0])printf("droidvm GPU clients: %d\n",droidvm_gpu_clients_status);
 return error;
}
