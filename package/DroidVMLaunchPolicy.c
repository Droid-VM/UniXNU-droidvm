// SPDX-License-Identifier: MIT
// launchd overrides for the jobs a DroidVM guest cannot serve, written as root into the mounted scratch Data
// before launchd reads them (XNU_BOARD_LAUNCH_POLICY, patches/xnu/native-board-launch-policy.patch).
//
// UniXNU-sheng's package/BootWatchdogPolicy.c without its Setup reset: the same plist and checks, the labels
// from droidvm-disable-jobs=<label>[,<label>...] (restricted characters, bounded length; a label without a "com." domain
// is under com.apple.). launchd does not
// honour a disabled.plist written into the seed on the Mac, which belongs to the user who built the seed.
// The UniXNU guide lists the jobs that block the first boot (docs/guide/skip-setup.md); watchdogd has no
// watchdog backend here.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <pexpert/pexpert.h>

// Owner service manager bootstrap repair. spawn_service_manager posix_spawns ownervisor from this hook while launchd is
// still being exec'd (load_init_program_at_path, before the execve), so launchd has not yet created its bootstrap port;
// ownervisor -- and every service it spawns -- inherits a null TASK_BOOTSTRAP_PORT, and a guest tool reached over ssh then
// cannot talk to launchd (XPC lookups fail "Reentrancy avoided", LaunchServices unreachable). A bootstrap port is inherited
// only at task creation and has no userspace recovery path, so once launchd is up the kernel copies pid 1's send right into
// ownervisor's task; ownervisor waits for that before spawning its services, so they inherit the good one at birth.
// droidvm_fix_ownervisor_bootstrap() does the copy and is called from the board monitor thread (DroidVMStubs.c) until it
// takes once. Inert unless owner-service-manager=1 actually spawned ownervisor (droidvm_ownervisor_pid stays 0 otherwise).
typedef struct ipc_port *droidvm_ipc_port_t;
extern struct proc *initproc;
extern kern_return_t task_get_special_port(task_t, int, droidvm_ipc_port_t *);
extern kern_return_t task_set_special_port(task_t, int, droidvm_ipc_port_t);
extern void ipc_port_release_send(droidvm_ipc_port_t);
#define DROIDVM_TASK_BOOTSTRAP_PORT 4
#define DROIDVM_IP_NULL ((droidvm_ipc_port_t)0)
#define DROIDVM_IP_DEAD ((droidvm_ipc_port_t)~0UL)
#define DROIDVM_IP_VALID(p) ((p)!=DROIDVM_IP_NULL && (p)!=DROIDVM_IP_DEAD)
volatile int droidvm_ownervisor_pid;
volatile int droidvm_ownervisor_bootstrap_status; // 0 pending/no donor yet, 1 adopted, -2 ownervisor gone
volatile int droidvm_bootstrap_donor_pid;         // which daemon's bootstrap we copied (for diagnosis via peek)
char droidvm_bootstrap_donor_name[16];

// launchd (pid 1) keeps no bootstrap in its own task: it sets each job's TASK_BOOTSTRAP_PORT through posix_spawn port
// actions, so the port to copy lives on its children, not on initproc. The GUI session daemons (backboardd, SpringBoard;
// uid 501) hold the user-domain bootstrap the LaunchServices tools over ssh need, so prefer those, else any daemon that
// has one. A bootstrap port is a capability; the service identifies the caller by its audit token, not the port.
static int donor_name_preferred(const char *n){
 static const char *const pref[]={"backboardd","SpringBoard"};
 for(unsigned i=0;i<sizeof(pref)/sizeof(pref[0]);i++){
  const char *a=n,*b=pref[i];
  while(*a && *a==*b){a++;b++;}
  if(*a==0 && *b==0)return 1;
 }
 return 0;
}
static droidvm_ipc_port_t find_bootstrap_donor(int preferred_only,int *donor_pid,char donor_name[16]){
 for(int q=1;q<=2048;q++){
  if(q==droidvm_ownervisor_pid)continue;
  proc_t pr=proc_find(q);
  if(!pr)continue;
  task_t t=proc_task(pr);
  const char *nm=proc_best_name(pr);
  droidvm_ipc_port_t bp=DROIDVM_IP_NULL;
  if(t && (!preferred_only || donor_name_preferred(nm)) &&
     task_get_special_port(t,DROIDVM_TASK_BOOTSTRAP_PORT,&bp)==0 && DROIDVM_IP_VALID(bp)){
   *donor_pid=q;
   int i=0;for(;i<15 && nm[i];i++)donor_name[i]=nm[i];donor_name[i]=0;
   proc_rele(pr);
   return bp;
  }
  if(bp!=DROIDVM_IP_NULL && DROIDVM_IP_VALID(bp))ipc_port_release_send(bp);
  proc_rele(pr);
 }
 return DROIDVM_IP_NULL;
}
void droidvm_fix_ownervisor_bootstrap(void) {
 if(droidvm_ownervisor_bootstrap_status!=0 || droidvm_ownervisor_pid<=0)return;
 int dp=0;char dn[16]={0};
 droidvm_ipc_port_t boot=find_bootstrap_donor(1,&dp,dn);       // a GUI session daemon first
 if(!DROIDVM_IP_VALID(boot))boot=find_bootstrap_donor(0,&dp,dn); // else any daemon with a bootstrap
 if(!DROIDVM_IP_VALID(boot))return;                            // nobody has one yet; retry next pass
 proc_t ov=proc_find(droidvm_ownervisor_pid);
 if(!ov){ipc_port_release_send(boot);droidvm_ownervisor_bootstrap_status=-2;return;}
 task_t t=proc_task(ov);
 if(!t){ipc_port_release_send(boot);proc_rele(ov);return;} // dying; retry next pass
 droidvm_ipc_port_t cur=DROIDVM_IP_NULL;
 if(task_get_special_port(t,DROIDVM_TASK_BOOTSTRAP_PORT,&cur)==0 && DROIDVM_IP_VALID(cur)){
  ipc_port_release_send(cur);ipc_port_release_send(boot);droidvm_ownervisor_bootstrap_status=1;proc_rele(ov);return;}
 task_set_special_port(t,DROIDVM_TASK_BOOTSTRAP_PORT,boot); // consumes the boot send right
 droidvm_bootstrap_donor_pid=dp;
 { int i=0;for(;i<16;i++)droidvm_bootstrap_donor_name[i]=dn[i]; }
 droidvm_ownervisor_bootstrap_status=1;
 proc_rele(ov);
}

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
   // a label without a "com." domain is Apple's: watchdogd stands for com.apple.watchdogd (the command line is short)
   if((e-c<4 || strncmp(c,"com.",4)) && strlcat(policy,"com.apple.",sizeof(policy))>=sizeof(policy))return EINVAL;
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

// owner-staged-apps=1 shows the deletable system apps in /Applications through a bindfs view (UniXNU patches/xnu), which is a volume
// of its own. The first time lsd starts after a boot it drops the apps of every volume but the system's own
// (_LSServerCleanEphemeralMobileContainersForFirstBoot), and only the first boot of a Data store registers them, so they would be gone
// from the second boot on. lsd skips that first-boot work when the POSIX named semaphore com.apple.lsd.firstboot exists (it makes it
// once the work is done; the semaphore lives until the next boot), so it is made here, readable by lsd's user (mobile). Never fatal.
volatile int droidvm_lsd_firstboot_status=-1;
static int mark_lsd_first_boot_done(proc_t p, uint64_t scratch) {
 uint32_t staged=0;
 if(!PE_parse_boot_argn("owner-staged-apps",&staged,sizeof(staged)) || staged!=1)return 0;
 const char *name="com.apple.lsd.firstboot";
 int error=copyout(name,scratch,strlen(name)+1);
 if(error)return error;
 struct sem_open_args op={.name=scratch,.oflag=O_CREAT,.mode=0444,.value=0};
 user_addr_t sem=0;
 int32_t result=0;
 if((error=sem_open(p,&op,&sem)))return error;
 struct sem_close_args cl={.sem=sem};
 return sem_close(p,&cl,&result);
}

// owner-app-cryptex=1: the cryptexes as iPadOS lays them out under /private/preboot/Cryptexes, which /System/Cryptexes/{App,OS} lead to.
//  - App (Cryptex1,AppOS: Safari's daemons, SafariSupport.bundle, Safari's settings pane) is owner Data here: the seed carries its files
//    under /private/var/ngpu/preboot/Cryptexes/App (scripts/add-app-cryptex-data.py), shown read-only over the empty /private/preboot as
//    the boot-time GPU addon mount does (bindfs, files reported as root's). Its binaries pass AMFI through the cryptex's own trust module,
//    which the owner trust bundle carries unchanged.
//  - OS is the SystemOS volume, already a union over the root. The OS volume still reaches it by that path: its framework entries are
//    links such as /System/Library/PrivateFrameworks/SafariCore.framework -> ../../../System/Cryptexes/OS/System/Library/...
//    It must not lead back into the union: there a directory's lower layer is the OS volume, whose entries are links into /System/Cryptexes/OS, and
//    a link to / made getdirentries' union traversal arrive at the same directory forever. So each top-level directory of the SystemOS
//    volume (/System and /usr, which a lookup from the root finds on that volume) is bound, read-only and not as a union, over its
//    mount point in the seed's Cryptexes/OS: bindfs looks up in the SystemOS volume itself, with no union below it.
// Once per boot (the policy runs before every launchd attempt); never fatal.
extern volatile int native_bindfs_boot_mount;
volatile int droidvm_app_cryptex_status=-1;
static int bind_view(proc_t p, uint64_t scratch, const char *source, const char *point, int flags) {
 struct mount_args request={.type=scratch,.path=scratch+0x20,.flags=MNT_RDONLY|flags,.data=scratch+0x100};
 int32_t result=0;
 int error;
 if((error=copyout("bindfs",request.type,7)) || (error=copyout(point,request.path,strlen(point)+1)) ||
    (error=copyout(source,request.data,strlen(source)+1)))return error;
 native_bindfs_boot_mount=1;
 error=mount(p,&request,&result);
 native_bindfs_boot_mount=0;
 return error;
}
static int mount_app_cryptex(proc_t p, uint64_t scratch) {
 static int mounted;
 static const char *const os[][2]={{"/System","/private/preboot/Cryptexes/OS/System"},{"/usr","/private/preboot/Cryptexes/OS/usr"}};
 static const char *const visible[]={"/private/preboot/Cryptexes/App/usr/libexec/webbookmarksd",
  "/System/Cryptexes/OS/System/Library/PrivateFrameworks/SafariCore.framework/Info.plist"};
 uint32_t enabled=0;
 if(mounted || !PE_parse_boot_argn("owner-app-cryptex",&enabled,sizeof(enabled)) || enabled!=1)return 0;
 vnode_t probe=NULLVP;
 int error=vnode_lookup("/private/var/ngpu/preboot",0,&probe,vfs_context_current());
 if(error)return error;
 vnode_put(probe);
 if((error=bind_view(p,scratch,"/private/var/ngpu/preboot","/private/preboot",MNT_UNION)))return error;
 mounted=1;
 for(size_t i=0;i<sizeof(os)/sizeof(os[0]);i++)
  if((error=bind_view(p,scratch,os[i][0],os[i][1],0)))return error;
 // A file of each cryptex must be visible where iPadOS looks for it.
 for(size_t i=0;i<sizeof(visible)/sizeof(visible[0]);i++) {
  probe=NULLVP;
  if((error=vnode_lookup(visible[i],0,&probe,vfs_context_current())))return error;
  vnode_put(probe);
 }
 return 0;
}

// owner-service-manager=1: start the owner service manager (package/ownervisor) once at boot, from this hook. The launchd
// overlay the GPU addon mounts does not actually launch an owner binary: launchd takes its jobs from the signed compiled cache
// (/System/Library/xpc/launchd.plist) and ignores the on-disk plist's ProgramArguments, and a brand-new label is never
// enumerated. This hook already runs as root with a user scratch page and calls BSD syscalls directly (mount above), so it
// posix_spawns /private/var/ngpu/bin/ownervisor the same way. ownervisor then brings up the debug services (an ssh server, an
// IPv6 keepalive) and the GPU probe, so one spawn stands in for a whole launchd job set. Best-effort and never fatal: a failure
// only leaves the guest without the manager. Retried on each hook call until it succeeds once -- launchd is loaded a few times
// early in boot, and the first attempt can be too early for a new userspace process.
volatile int droidvm_service_manager_status=-2;
static int spawn_service_manager(proc_t p, uint64_t scratch) {
 static int started;
 uint32_t enabled=0;
 if(started || !PE_parse_boot_argn("owner-service-manager",&enabled,sizeof(enabled)) || enabled!=1)return droidvm_service_manager_status;
 static const char path[]="/private/var/ngpu/bin/ownervisor";
 user_addr_t argv[2]={scratch,0}, envp[1]={0};
 int error;
 if((error=copyout(path,scratch,sizeof(path))) ||
    (error=copyout(argv,scratch+0x40,sizeof(argv))) ||
    (error=copyout(envp,scratch+0x60,sizeof(envp))))return error;
 struct posix_spawn_args request={.pid=scratch+0x68,.path=scratch,.adesc=USER_ADDR_NULL,.argv=scratch+0x40,.envp=scratch+0x60};
 int32_t result=0;
 error=posix_spawn(p,&request,&result);
 if(!error){
  started=1;
  // posix_spawn copied the child pid to request.pid (scratch+0x68); remember it so the monitor thread can hand
  // ownervisor launchd's bootstrap port once launchd is up (droidvm_fix_ownervisor_bootstrap).
  int32_t pid=0;
  if(copyin((user_addr_t)(scratch+0x68),&pid,sizeof(pid))==0 && pid>0)droidvm_ownervisor_pid=pid;
 }
 return error;
}

// The calendar clock from the host before launchd and everything it starts read the time (DroidVMClock.c); never fatal.
extern void droidvm_rtc_set_calendar(void);

int native_board_launch_policy(void *process, uint64_t scratch) {
 droidvm_rtc_set_calendar();
 const int error=write_overrides(process,scratch);
 droidvm_gpu_clients_status=write_gpu_clients(process,scratch);
 droidvm_lsd_firstboot_status=mark_lsd_first_boot_done(process,scratch);
 const int cryptex=mount_app_cryptex(process,scratch);
 if(cryptex || droidvm_app_cryptex_status<0)droidvm_app_cryptex_status=cryptex;
 droidvm_service_manager_status=spawn_service_manager(process,scratch);
 if(clients[0])printf("droidvm GPU clients: %d\n",droidvm_gpu_clients_status);
 if(droidvm_app_cryptex_status)printf("droidvm App cryptex: %d\n",droidvm_app_cryptex_status);
 if(droidvm_service_manager_status>0)printf("droidvm service manager: %d\n",droidvm_service_manager_status);
 return error;
}
