#!/usr/bin/env python3
"""Build everything the iPadOS 27 DroidVM guest boots from, from fresh checkouts, on the build Mac.

  build-droidvm-all.py --unique-chip-id HEX [--unixnu DIR] [--ipsw-cache DIR] [--generation DIR] [--allow-dirty]
                       [--from STEP] [--to STEP] [--only STEP,...] [--list]

DIR is the UniXNU checkout (default: ../UniXNU beside this device repository), with its submodules at their pins. Each step
leaves a stamp in the board's output (build-all/) with what it was built from -- the commits of the checkouts (and the digest
of their local changes), the Data seed, the chip id -- and runs again when that differs: after a git pull the same command
builds what changed (the builds below the steps are incremental). setup, policies and a new Data seed run once; --from/--only
run steps again whatever their stamps say. The result is release/ in the board's output: droidvm-disk.img (the boot disk: UniXNU.efi, DarwinBoot.efi,
kernel collection, ADT, trust, root policies, Data seed), os.img and systemos.img (the read-only OS and SystemOS disks),
plugin/ (the Metal plugin as the seed holds it, for a VM that keeps its Data), kernel.native.macho (symbols) and release.json:
every commit, pin and hash that went in. Nothing is taken from outside the two checkouts, the pinned sources they name and
the IPSW; a checkout with local changes is refused unless --allow-dirty (the release then says so).

A VM's Data lives in a store on the phone that holds the changes to the seed it first booted from, and the kernel mounts Data
by the seed's volume UUID. So there are two kinds of build:

  a new VM          (default) a new Data seed, with this build's plugin and tools in it. tools/rig/deploy27.sh --new-vm
  a VM that exists  --generation DIR: the seed that VM was made from, kept with its record (DIR/seed.json, DIR/<seed>.dbr)
                    and DIR/trust/*.json, the trust records of what its Data holds (the tools and the plugin as they were
                    built then, apps installed since); they stay trusted beside this build's. tools/rig/deploy27.sh puts
                    the new plugin into the running guest before it boots the new disk.

Steps, in order:
  setup      unixnu defconfig + fetch + ipsw fetch (kernelcache, device tree, OS, SystemOS and App images, their trust caches),
             the two encrypted images decrypted, the build tree
  policies   the owner root keys, the raw OS and SystemOS disks and the signed root policies over them
  package    unixnu build package (UniXNU.efi) and DarwinBoot.efi
  tools      the debug shell (ownervisor with applebox, dropbear, zsh, debugserver, libimobiledevice) and the board's tools
             (toybox, gfxprobe, lsdiag, ipatool, MetalCheck), arm64e
  plugin     the Metal plugin (AdrenoMetal frontend, metal2vulkan, Turnip) for arm64e
  data       the Data seed (or the generation's), and its volume UUID into the kernel configuration
  kernel     unixnu build kernel, the reviewed fileset pairings, unixnu build prelink
  trust      the owner trust bundle (plugin, tools, the generation's records, the IPSW's own modules)
  payload    unixnu build payload around that trust and seed, then the boot disk
  release    release/ and release.json
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

BOARD = Path(__file__).resolve().parents[1]          # <repo>/ipados27
REPO = BOARD.parent
STEPS = ['setup', 'policies', 'package', 'tools', 'plugin', 'data', 'kernel', 'trust', 'payload', 'release']
STICKY = {'setup', 'policies'}        # run once (identity()); the Data seed too, unless the generation changes
# The owner tools, in build order (scripts/build-droidvm-<name>.py of the repository, shared with the iPadOS 17 board): each
# writes build/<name>/... and build/owner-trust-extra/<name>.json. iPadOS 27 kills an arm64 platform binary at exec.
TOOLS = ['ownervisor', 'toybox', 'gfxprobe', 'lsdiag', 'ipatool', 'metalcheck']
TOOL_TARGET = 'arm64e-apple-ios27.0'
# What the build takes from the IPSW besides the board's own list (the kernelcache).
IPSW_COMPONENTS = ['DeviceTree', 'OS', 'Cryptex1,SystemOS', 'Cryptex1,AppOS', 'StaticTrustCache', 'Cryptex1,SystemTrustCache',
                   'Cryptex1,AppTrustCache']
NATIVE_KERNEL = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'


def log(message):
    print(f'== {time.strftime("%H:%M:%S")} {message}', flush=True)


def run(command, cwd=None, env=None, logfile=None, check=True):
    command = [str(part) for part in command]
    log('$ ' + ' '.join(command) + (f'  > {logfile}' if logfile else ''))
    if logfile:
        Path(logfile).parent.mkdir(parents=True, exist_ok=True)
        with open(logfile, 'w') as stream:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode and check:
            tail = Path(logfile).read_text(errors='replace').splitlines()[-25:]
            sys.exit('\n'.join(tail) + f'\nfailed ({result.returncode}): {" ".join(command)}\nfull log: {logfile}')
        return result.returncode
    result = subprocess.run(command, cwd=cwd, env=env)
    if result.returncode and check:
        sys.exit(f'failed ({result.returncode}): {" ".join(command)}')
    return result.returncode


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(8 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def clone(source, destination):
    """An APFS clone (cp -c): large files stay one copy on disk."""
    if destination.is_file() and destination.stat().st_size == source.stat().st_size \
            and destination.stat().st_mtime >= source.stat().st_mtime:
        return
    destination.unlink(missing_ok=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(['cp', '-c', source, destination])


def git(path, *arguments):
    return subprocess.run(['git', '-C', str(path), *arguments], capture_output=True, text=True).stdout.strip()


def checkout_state(path):
    """A checkout's commit, what differs from it (tracked files changed, files git does not know and does not ignore) and a
    digest of those differences (the changes to tracked files and the untracked files' contents)."""
    changed = [line for line in git(path, 'status', '--porcelain', '--ignore-submodules=none').splitlines() if line.strip()]
    state = {'commit': git(path, 'rev-parse', 'HEAD'), 'changed': changed}
    if changed:
        digest = hashlib.sha256(git(path, 'diff', 'HEAD', '--ignore-submodules=all').encode())
        for line in changed:
            untracked = Path(path) / line[3:]
            if line.startswith('??') and untracked.is_file():
                digest.update(untracked.read_bytes())
        state['digest'] = digest.hexdigest()
    return state


class Build:
    def __init__(self, args):
        self.args = args
        self.unixnu = args.unixnu.resolve()
        self.board = tomllib.loads((BOARD / 'board.toml').read_text())
        # The OS profile (--os; the board's by default) and its defconfig ([package.defconfig], as unixnu --os takes it).
        self.os_name = args.os_name or self.board['board']['os_profile']
        override = self.board.get('os_overrides', {}).get(self.os_name, {})
        self.defconfig = (self.board.get('package', {}).get('defconfig', {}).get(self.os_name)
                          or override.get('kernel', {}).get('defconfig') or self.board['kernel']['defconfig'])
        self.profile_path = self.unixnu / 'profiles/os' / self.os_name / 'profile.toml'
        self.profile = tomllib.loads(self.profile_path.read_text())
        # macOS 27 (meta todo/macos-bringup.md): no iPadOS tools, its Metal plugin not yet, its Data seed made as root
        # (scripts/make-droidvm-macos-seed.py, run with sudo; --generation takes it), the kexts and reviewed sites of macOS.
        self.macos = self.profile['os'].get('target') == 'macosx'
        self.build_id = self.profile['ipsw']['build']
        self.out = Path(os.environ.get('UNIXNU_OUT', self.unixnu / 'out')).resolve()
        self.work = self.out / self.board['board']['name'] / self.os_name / self.defconfig.removesuffix('_defconfig')
        self.tree = self.work / 'tree'
        self.stamps = self.work / 'build-all'
        self.logs = self.work / 'build-all/logs'
        self.release = self.work / 'release'
        self.ipsw = self.out / 'ipsw' / self.build_id
        self.data = self.tree / f'build/droidvm-data-{self.build_id}'
        self.plugin = self.tree / ('build/adreno-metal-plugin-macos27' if self.macos else 'build/adreno-metal-plugin27')
        self.trust = self.tree / f'build/droidvm-trust-{self.build_id}'
        # PYTHONHASHSEED: Mesa's generators (the ir3 instruction encoder) emit statements in a set's iteration order, which
        # follows the interpreter's hash seed: with a fixed one, two builds of the same source give the same code.
        # PYTHONDONTWRITEBYTECODE: the build imports modules of the checkouts (VerifiedRoot's manifest tool), whose
        # __pycache__ a checkout would then show as a local change.
        self.env = dict(os.environ, PYTHONHASHSEED='0', PYTHONDONTWRITEBYTECODE='1',
                        PATH=os.pathsep.join([str(Path.home() / '.cargo/bin'), '/opt/homebrew/bin', os.environ.get('PATH', '')]))

    def unixnu_cmd(self, *arguments, logfile=None):
        env = dict(self.env, UNIXNU_OUT=str(self.out))
        return run([self.unixnu / 'unixnu', arguments[0], '-D', BOARD, '--os', self.os_name, *arguments[1:]], cwd=self.unixnu,
                   env=env, logfile=logfile)

    def script(self, name, *arguments, logfile=None, check=True, env=None):
        """A script of the assembled tree (UniXNU's scripts with this board's on top), run from the tree."""
        return run(['python3', self.tree / 'scripts' / name, *arguments], cwd=self.tree, env=env or self.env,
                   logfile=logfile or self.logs / (Path(name).stem + '.log'), check=check)

    def record(self, name, value=None):
        """A small fact one step leaves for another (build-all/<name>.json)."""
        path = self.stamps / f'{name}.json'
        if value is None:
            return json.loads(path.read_text()) if path.is_file() else None
        self.stamps.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2) + '\n')
        return value

    # sources -------------------------------------------------------------------------------------------------------------
    def checkouts(self):
        """Every checkout the build reads: UniXNU, its submodules, this repository."""
        result = {'unixnu': checkout_state(self.unixnu), 'device': checkout_state(REPO)}
        for line in git(self.unixnu, 'submodule', 'status').splitlines():
            flag, commit, path = line[0], line[1:].split()[0], line[1:].split()[1]
            state = checkout_state(self.unixnu / path) if flag != '-' else {'commit': commit, 'changed': ['not checked out']}
            if flag == '+':
                state['changed'].append(f'not at the pin {git(self.unixnu, "rev-parse", "HEAD:" + path)}')
            result[path] = state
        return result

    def check_clean(self):
        dirty = {name: state['changed'] for name, state in self.checkouts().items() if state['changed']}
        if dirty and not self.args.allow_dirty:
            lines = [f'  {name}: ' + '; '.join(changed[:4]) + (' ...' if len(changed) > 4 else '')
                     for name, changed in dirty.items()]
            sys.exit('these checkouts differ from their commits (a release is built from commits; --allow-dirty for an '
                     'experiment):\n' + '\n'.join(lines))
        return dirty

    def sync_sources(self):
        """Every git source at the commit the feeds pin (the local unixnu.lock, not in git, would otherwise keep an older
        one), then the tree again: it holds copies of the scripts, made when unixnu assembles it."""
        self.unixnu_cmd('fetch', '--update', logfile=self.logs / 'fetch.log')
        self.unixnu_cmd('tree', logfile=self.logs / 'tree.log')
        self.link_sources()

    def link_sources(self):
        """What the board's tree does not hold as components: NativeAdreno, AdrenoMetal and their kits (the board's own
        link-droidvm-gpu-sources.py) and DarwinBoot, whose EFI driver and seed packer the build uses."""
        self.script('link-droidvm-gpu-sources.py', logfile=self.logs / 'link-gpu-sources.log')
        link = self.tree / 'drivers/DarwinBoot'
        if not link.exists():
            link.symlink_to(self.unixnu / 'drivers/DarwinBoot')

    # setup ---------------------------------------------------------------------------------------------------------------
    def step_setup(self):
        if self.args.ipsw_cache:
            # Members another checkout has extracted already (the profile pins them; ipsw fetch checks them and keeps them).
            for source in sorted(self.args.ipsw_cache.resolve().iterdir()):
                if source.is_file() and not source.name.endswith('.partial') and not (self.ipsw / source.name).exists():
                    clone(source, self.ipsw / source.name)
        self.unixnu_cmd('defconfig', self.defconfig)
        self.unixnu_cmd('fetch', '--update', logfile=self.logs / 'fetch.log')
        self.unixnu_cmd('ipsw', 'fetch', logfile=self.logs / 'ipsw-fetch.log')
        self.unixnu_cmd('ipsw', 'fetch', *IPSW_COMPONENTS, logfile=self.logs / 'ipsw-fetch-members.log')
        for component in ('OS', 'Cryptex1,SystemOS'):
            self.member(component)
        self.unixnu_cmd('tree', logfile=self.logs / 'tree.log')
        self.link_sources()
        # The J820's device tree, where board.toml's --identity-tree looks for it.
        firmware = self.tree / '.cache/firmware'
        firmware.mkdir(parents=True, exist_ok=True)
        tree = self.member('DeviceTree')
        shutil.copyfile(tree, firmware / tree.name)

    def member(self, component):
        """A member of the IPSW in the cache (unixnu ipsw fetch recorded it), decrypted beside it when it is an AEA archive."""
        for record in sorted(self.ipsw.glob('*.json')):
            info = json.loads(record.read_text())
            if info.get('component') != component:
                continue
            path = record.with_name(record.name[:-len('.json')])
            if path.suffix != '.aea':
                return path
            plain = path.with_suffix('')
            if not (plain.is_file() and plain.with_suffix(plain.suffix + '.json').is_file()):
                run(['python3', '-c', 'import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); '
                     'from unixnu import ipsw; print(ipsw.decrypt_aea(Path(sys.argv[2])))', self.unixnu / 'scripts', path],
                    env=self.env, logfile=self.logs / f'decrypt-{plain.name}.log')
            return plain
        sys.exit(f'{self.ipsw}: no {component} member (unixnu ipsw fetch {component})')

    # policies ------------------------------------------------------------------------------------------------------------
    def step_policies(self):
        host = self.tree / 'build/verified-root-host'
        if not (host / 'verified-root.dylib').is_file():
            openssl = subprocess.check_output(['brew', '--prefix', 'openssl@3'], text=True, env=self.env).strip()
            run(['python3', self.tree / 'kits/VerifiedRoot/tests/check.py', '--output', host, '--openssl-prefix', openssl],
                cwd=self.tree, env=self.env, logfile=self.logs / 'verified-root-host.log')
        self.script('build-droidvm-root-policies.py', '--tree', self.tree, '--profile', self.profile_path,
                    '--os', self.member('OS'), '--systemos', self.member('Cryptex1,SystemOS'))

    # package -------------------------------------------------------------------------------------------------------------
    def efi_dir(self):
        return self.work / 'package/Build/Out/AARCH64-DEBUG'

    def step_package(self):
        self.unixnu_cmd('build', 'package', logfile=self.logs / 'package.log')
        self.link_sources()
        edk2 = sorted((self.out / 'feeds').glob('mu_basecore-*'))
        if not edk2:
            sys.exit('unixnu build package left no out/feeds/mu_basecore-*')
        env = dict(self.env)
        if (self.work / 'toolchain').is_dir():
            # the CLANGPDB tools unixnu linked for the package step (Homebrew LLVM's llvm-lib and lld-link beside clang)
            env['CLANG_BIN'] = str(self.work / 'toolchain') + '/'
        run(['bash', REPO / 'scripts/build-darwinboot-efi.sh', self.tree, edk2[-1], 'DEBUG'], env=env,
            logfile=self.logs / 'darwinboot.log')
        for name in ('UniXNU.efi', 'DarwinBoot.efi'):
            if not (self.efi_dir() / name).is_file():
                sys.exit(f'no {self.efi_dir() / name}')

    # tools ---------------------------------------------------------------------------------------------------------------
    def step_tools(self):
        if self.macos:   # the owner boot shims the macOS Data seed carries (build-droidvm-macos-shims.py)
            self.script('build-droidvm-macos-shims.py', '--tree', self.tree)
            return
        self.link_sources()            # gfxprobe takes NativeAdreno's headers from the tree
        # feeds.toml's pins (reference sources: a schema 2 tree takes kits only from the catalog); applebox's over the one in
        # ownervisor's build.py
        env = dict(self.env, DROIDVM_IOS_TARGET=TOOL_TARGET, OWNERVISOR=str(self.tree / 'refer/ownervisor'),
                   APPLEBOX=str(self.tree / 'refer/applebox'))
        for name in ('OWNERVISOR', 'APPLEBOX'):
            if not Path(env[name]).is_dir():
                sys.exit(f'no {env[name]}: feeds.toml pins it, unixnu fetch and unixnu tree put it there')
        for tool in TOOLS:
            run(['python3', REPO / f'scripts/build-droidvm-{tool}.py', self.tree], cwd=REPO, env=env,
                logfile=self.logs / f'tool-{tool}.log')

    # plugin --------------------------------------------------------------------------------------------------------------
    def prepare_mesa(self):
        """What UniXNU's Turnip build takes as given (docs/porting/migration.md, "build inputs"): the pinned Mesa source with the
        recorded patches in .cache/mesa-work, meson and mako in .cache/mesa-venv, bison and flex in .cache/mesa-tools, and the
        pinned metal2vulkan source in .cache/gpu-reference/metal2vulkan. Returns what the release records of them."""
        cache = self.tree / '.cache'
        cache.mkdir(parents=True, exist_ok=True)
        pins = json.loads((self.tree / 'scripts/data/adreno-compiler-inputs.json').read_text())
        venv = cache / 'mesa-venv'
        if not (venv / 'bin/meson').is_file():
            run(['python3', '-m', 'venv', venv], env=self.env)
            run([venv / 'bin/pip', 'install', '-q', 'meson==1.12.1', 'Mako==1.4.3', 'PyYAML==6.0.3', 'packaging'],
                env=self.env, logfile=self.logs / 'mesa-venv.log')
        tools = cache / 'mesa-tools/bin'
        tools.mkdir(parents=True, exist_ok=True)
        for name in ('bison', 'flex'):
            prefix = subprocess.check_output(['brew', '--prefix', name], text=True, env=self.env).strip()
            if not Path(prefix, 'bin', name).is_file():
                sys.exit(f'{name} is required: brew install {name}')
            (tools / name).unlink(missing_ok=True)
            (tools / name).symlink_to(Path(prefix, 'bin', name))
        version = subprocess.check_output([tools / 'bison', '--version'], text=True).splitlines()[0]
        if not version.endswith(pins['bison']['version']):
            sys.exit(f'bison {pins["bison"]["version"]} is required, {version} found')
        if not shutil.which('glslangValidator', path=self.env['PATH']):
            sys.exit('glslangValidator is required: brew install glslang')
        work = cache / 'mesa-work'
        mesa = pins['mesa']
        if not (work / '.git').is_dir():
            run(['git', 'clone', '-q', '--filter=blob:none', '--no-checkout', '-b', mesa['branch'], mesa['repository'], work],
                env=self.env)
            run(['git', '-C', work, 'sparse-checkout', 'set', '--cone', *mesa['sparse_directories']], env=self.env)
        if subprocess.run(['git', '-C', work, 'cat-file', '-e', mesa['commit']], capture_output=True).returncode:
            run(['git', '-C', work, 'fetch', '-q', 'origin', mesa['commit']], env=self.env)
        # The pin and the recorded patches, in order, from a clean checkout -- once: a new checkout touches every file, and
        # Turnip's builds would compile everything again. The stamp also holds the digest of the patched source's difference
        # from the pin: a file edited in place afterwards (an experiment) makes the source be prepared again.
        identity = mesa['commit'] + ''.join(sha256(self.tree / patch) for patch in pins['patches'])
        stamp = cache / 'mesa-work.prepared'

        def difference():
            listing = subprocess.run(['git', '-C', str(work), 'status', '--porcelain'], capture_output=True).stdout
            content = subprocess.run(['git', '-C', str(work), 'diff', 'HEAD'], capture_output=True).stdout
            return hashlib.sha256(listing + content).hexdigest()
        state = json.loads(stamp.read_text()) if stamp.is_file() and stamp.read_text().startswith('{') else {}
        edited = state.get('identity') == identity and state.get('difference') != difference()
        if edited and self.args.allow_dirty:
            log(f'{work} was edited after its patches: kept (--allow-dirty), the release says so')
        elif state.get('identity') != identity or edited:
            stamp.unlink(missing_ok=True)
            run(['git', '-C', work, 'checkout', '-q', '-f', '--detach', mesa['commit']], env=self.env)
            run(['git', '-C', work, 'clean', '-q', '-fdx'], env=self.env)
            for patch in pins['patches']:
                run(['git', '-C', work, 'apply', self.tree / patch], env=self.env)
            stamp.write_text(json.dumps({'identity': identity, 'difference': difference()}) + '\n')
            edited = False
        translator = json.loads((self.tree / 'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']
        m2v = cache / 'gpu-reference/metal2vulkan'
        if not (m2v / '.git').is_dir():
            # the pin only, as the pin file records the qualified clone (depth 1, blob filter): the history is 1.7 GiB
            m2v.mkdir(parents=True, exist_ok=True)
            run(['git', '-C', m2v, 'init', '-q'], env=self.env)
            run(['git', '-C', m2v, 'remote', 'add', 'origin', translator['repository']], env=self.env)
        if subprocess.run(['git', '-C', m2v, 'cat-file', '-e', translator['commit']], capture_output=True).returncode:
            run(['git', '-C', m2v, 'fetch', '-q', '--depth', '1', '--filter=blob:none', 'origin', translator['commit']],
                env=self.env)
        # build-adreno-metal-bridge.py applies its recorded patches itself, on the pin (it accepts the pin with any prefix of them)
        if git(m2v, 'rev-parse', 'HEAD') != translator['commit']:
            run(['git', '-C', m2v, 'checkout', '-q', '-f', '--detach', translator['commit']], env=self.env)
            run(['git', '-C', m2v, 'clean', '-q', '-fdx'], env=self.env)
        return self.record('gpu-sources', {'mesa': mesa['commit'], 'mesa_patches': len(pins['patches']),
                                           'mesa_edited_in_place': edited, 'metal2vulkan': translator['commit']})

    def step_plugin(self):
        # macOS: the same plugin for arm64e-apple-macos27.0 in the macOS bundle layout (Contents/MacOS), its device class an
        # IOGPUMetalDevice (NativeMetal's IOGPUMetalDeviceShim.h); the payload's GPU addon carries it to /Library/GPUBundles
        # (step payload, gpu-addon=1)
        target = 'macos27' if self.macos else 'ios27'
        self.link_sources()
        self.prepare_mesa()
        self.script('build-turnip-darwin.py', '--target', target, logfile=self.logs / f'turnip-{target}.log')
        # Without the prefix replay of Turnip's first submission (ADRENO_METAL_REPLAY=0): it tests NativeAdreno's own command
        # processor, which this guest does not run (its GPU is the virtio GPU), and recording it needs the host simulation of
        # that kernel stack, which does not compile at this fork's pins (Adreno740 stays at the commit the board's kernel
        # patches fit, older than the SMMU kit's on-demand tables).
        self.script('build-adreno-metal-plugin.py', '--target', target, logfile=self.logs / 'plugin.log',
                    env=dict(self.env, ADRENO_METAL_REPLAY='0'))

    # data ----------------------------------------------------------------------------------------------------------------
    def step_data(self):
        self.data.mkdir(parents=True, exist_ok=True)
        if self.args.generation:
            source = self.args.generation.resolve()
            record = json.loads((source / 'seed.json').read_text())
            if record.get('build') != self.build_id:
                sys.exit(f'{source}: a seed of {record.get("build")}, this board is {self.build_id}')
            if sha256(source / record['seed']) != record['seed_sha256']:
                sys.exit(f'{source / record["seed"]} is not the seed its record names')
            clone(source / record['seed'], self.data / record['seed'])
            shutil.copyfile(source / 'seed.json', self.data / 'seed.json')
            origin = str(source)
        elif self.macos:
            sys.exit('macOS: the Data seed keeps root and service accounts as owners; make it once (as root in a lima VM, no sudo): '
                     'python3 ' + str(self.tree / 'scripts/make-droidvm-macos-seed.py') + ' --tree ' + str(self.tree) + ' --ipsw '
                     + str(self.ipsw) + ' --output /Volumes/DATA/UniXNU/generations/<name> [--user NAME:PASSWORD], then --generation')
        else:
            self.link_sources()
            run(['python3', BOARD / 'scripts/build-droidvm-data-seed.py', '--tree', self.tree, '--profile', self.profile_path,
                 '--ipsw', self.ipsw, '--plugin', self.plugin, '--tools', self.tree, '--store-gib', self.args.data_gib],
                cwd=self.tree, env=self.env, logfile=self.logs / 'data-seed.log')
            record = json.loads((self.data / 'seed.json').read_text())
            origin = 'built'
        self.record('seed', {'seed': record['seed'], 'seed_sha256': record['seed_sha256'],
                             'volume_uuid': record['volume_uuid'], 'origin': origin})
        self.apply_data_uuid()

    def seed(self):
        record = self.record('seed')
        if not record:
            sys.exit('no Data seed yet: run the data step')
        return record

    def apply_data_uuid(self):
        """The kernel mounts Data by the seed's APFS volume UUID: put it into the output's .config (unixnu defconfig puts
        the defconfig's back). The XNU build passes it as a define and does not track defines, so kern_exec.o, its only
        user, is removed when it was compiled with another."""
        uuid = self.seed()['volume_uuid']
        config = self.work / '.config'
        text = config.read_text()
        line = f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{uuid}"'
        text, count = re.subn(r'^CONFIG_XNU_OWNER_DATA_VOLUME_UUID=.*$', line, text, flags=re.M)
        new = text if count else text.rstrip('\n') + '\n' + line + '\n'
        if new != config.read_text():
            config.write_text(new)
        compiled = self.record('kernel-data-uuid')
        if compiled and compiled.get('volume_uuid') != uuid:
            for stale in self.tree.glob('build/xnu/ios-obj/*/bsd/DEVELOPMENT/kern_exec.o'):
                log(f'Data volume UUID changed: {stale} is compiled again')
                stale.unlink()
        log(f'Data volume UUID {uuid}')

    # kernel --------------------------------------------------------------------------------------------------------------
    def step_kernel(self):
        self.link_sources()
        self.apply_data_uuid()
        self.unixnu_cmd('build', 'kernel', logfile=self.logs / 'kernel.log')
        self.record('kernel-data-uuid', {'volume_uuid': self.seed()['volume_uuid']})
        # What the matcher leaves unpaired and this kernel's boot reaches (fileset-reviewed.json, 24A446's), then the collection
        # again. A macOS collection keeps its symbols and pairs by name.
        if not self.macos:
            self.script('apply-fileset-reviewed.py', '--tree', self.tree, '--map', self.work / 'kernel/kernel.map.json',
                        '--reviewed', BOARD / 'fileset-reviewed.json')
        self.unixnu_cmd('build', 'prelink', logfile=self.logs / 'prelink.log')
        self.add_kexts()

    def add_kexts(self):
        """The board's drivers that ship as kexts (ipados27/kexts) into the collection the prelink just made (kernel.macho, rewritten
        by every prelink): compiled against this kernel, then linked and added to the fileset (scripts/fileset-add-kexts.py)."""
        if not sorted((BOARD / 'kexts').glob('*.json')):
            return
        # the prelink's tree step dropped the GPU sources' links the kernel step made: a kext compiling them (the macOS display's
        # DroidVMVirtioGpu3D.cpp includes Adreno740's DroidVMVirtgpuABI.h) needs them again
        self.script('link-droidvm-gpu-sources.py', logfile=self.logs / 'link-gpu-sources.log')
        self.script('build-droidvm-kexts.py', '--tree', self.tree, '--target', 'macosx' if self.macos else 'ios')
        # reviewed per-site bindings for the kernelcaches XnuPrelink has no fixture of (applied only to their own collection)
        sites = [part for path in sorted((BOARD / 'fileset-sites').glob('fileset-*-symbols.json')) for part in ('--sites', path)]
        kernel = self.work / 'kernel'
        native = json.loads((kernel / 'kernel.json').read_text())['native']
        staged = kernel / 'kernel.kexts.macho'
        staged.unlink(missing_ok=True)
        self.script('fileset-add-kexts.py', '--tree', self.tree, '--fileset', kernel / 'kernel.macho', '--native', native,
                    '--manifest', self.tree / 'build/droidvm-kexts/manifest.json', '--output', staged,
                    '--report', kernel / 'kexts.json', *sites)
        staged.replace(kernel / 'kernel.macho')

    # trust ---------------------------------------------------------------------------------------------------------------
    def trust_records(self):
        records = [*([self.plugin / 'trust.json'] if (self.plugin / 'trust.json').is_file() or not self.macos else []),
                   *sorted((self.tree / 'build/owner-trust-extra').glob('macos-*.json' if self.macos else '*.json'))]
        if self.args.generation:
            records += sorted((self.args.generation.resolve() / 'trust').glob('*.json'))
        return records

    def trust_bundle(self):
        """The bundle with the owner's module, or the IPSW's own modules alone when no owner record exists (macOS, for now)."""
        return self.trust / ('owner.bundle' if self.trust_records() else 'original.bundle')

    def step_trust(self):
        owners = [part for record in self.trust_records() for part in ('--owner', record)]
        self.script('build-droidvm-trust.py', '--tree', self.tree, '--ipsw', self.ipsw, *owners, '--name', 'owner',
                    logfile=self.logs / 'trust.log')

    # payload -------------------------------------------------------------------------------------------------------------
    def step_payload(self):
        chip = self.args.unique_chip_id
        if not chip:
            sys.exit('the payload needs --unique-chip-id (or UNIXNU_UNIQUE_CHIP_ID): the platform seed, 16 hex digits')
        bundle = self.trust_bundle()
        addon = []
        if self.macos and (self.plugin / 'trust.json').is_file():
            # macOS: the Metal plugin in the payload's GPU addon, below GPUBundles/, which the kernel writes to Data's
            # /Library/GPUBundles and protects for IOGPU's trust check (gpu-addon=1, macos-gpu-bundles.patch); published (the render
            # server and every client take the native device), no probe daemon
            addon_file = self.work / 'payload/gpu-addon.bin'
            addon_file.parent.mkdir(parents=True, exist_ok=True)
            self.script('make-gpu-addon.py', '--publish', '--no-daemon', '--prefix', 'GPUBundles', addon_file,
                        logfile=self.logs / 'gpu-addon.log', env=dict(self.env, ADRENO_PLUGIN_DIR=str(self.plugin)))
            addon = ['--gpu-addon', addon_file]
        self.unixnu_cmd('build', 'payload', '--unique-chip-id', chip, '--data-ramdisk', self.data / self.seed()['seed'],
                        '--trust-cache', bundle, *addon, logfile=self.logs / 'payload.log')
        run(['bash', REPO / 'scripts/make-droidvm-disk.sh', self.work / 'payload/payload.img', self.efi_dir(),
             self.work / 'payload/droidvm-disk.img'], env=self.env, logfile=self.logs / 'disk.log')
        self.record('payload', {'unique_chip_id': chip})

    # release -------------------------------------------------------------------------------------------------------------
    def step_release(self):
        dirty = self.check_clean()
        self.release.mkdir(parents=True, exist_ok=True)
        for name in ('os', 'systemos'):
            clone(self.tree / f'build/droidvm-root-images/{name}.img', self.release / f'{name}.img')
        shutil.copyfile(self.work / 'payload/droidvm-disk.img', self.release / 'droidvm-disk.img')
        shutil.copyfile(self.tree / NATIVE_KERNEL, self.release / 'kernel.native.macho')
        shutil.copyfile(self.work / 'kernel/kernel.macho', self.release / 'kernel.macho')
        # Where fileset-add-kexts.py put the board's kexts, with their symbols (no symbol table of the collection has them).
        kexts = self.work / 'kernel/kexts.json'
        (self.release / 'kexts.json').unlink(missing_ok=True)
        if kexts.is_file():
            shutil.copyfile(kexts, self.release / 'kexts.json')
        plugin = self.release / 'plugin'
        if plugin.exists():
            shutil.rmtree(plugin)
        plugin.mkdir()
        trust = {}
        if (self.plugin / 'trust.json').is_file():   # none for macOS yet
            shutil.copytree(self.plugin / 'NativeAdrenoMetal.bundle', plugin / 'NativeAdrenoMetal.bundle', symlinks=True)
            shutil.copyfile(self.plugin / 'adrenoprobed', plugin / 'adrenoprobed')
            trust = json.loads((self.plugin / 'trust.json').read_text())
        plugin_files = {str(path.relative_to(plugin)): sha256(path) for path in sorted(plugin.rglob('*')) if path.is_file()}
        tools = {path.stem: json.loads(path.read_text()).get('cdhashes')
                 for path in sorted((self.tree / 'build/owner-trust-extra').glob('*.json'))}
        checkouts = self.checkouts()
        lock = BOARD / 'unixnu.lock'
        release = {
            'built': time.strftime('%Y-%m-%d %H:%M:%S %z'),
            'board': f'{self.board["board"]["name"]} {self.board["board"]["os_profile"]}',
            'clean': not dirty and not (self.record('gpu-sources') or {}).get('mesa_edited_in_place'),
            'commits': {name: state['commit'] for name, state in checkouts.items()},
            'changed': {name: state['changed'] for name, state in checkouts.items() if state['changed']},
            'feeds_lock_sha256': sha256(lock) if lock.is_file() else None,
            'gpu_sources': self.record('gpu-sources'),
            'seed': self.seed(),
            'unique_chip_id': (self.record('payload') or {}).get('unique_chip_id'),
            'trust_sha256': sha256(self.trust_bundle()),
            'trust_records': [str(path) for path in self.trust_records()],
            'plugin': {'cdhashes': {k: trust[k] for k in ('cdhash', 'daemon_cdhash', 'turnip_cdhash') if k in trust},
                       'files': plugin_files},
            'owner_tools': tools,
            'checks': self.record('checks') or {},
            'files': {name: {'bytes': (self.release / name).stat().st_size, 'sha256': sha256(self.release / name)}
                      for name in ('droidvm-disk.img', 'kernel.macho', 'kernel.native.macho', 'os.img', 'systemos.img', 'kexts.json')
                      if (self.release / name).is_file()},
        }
        (self.release / 'release.json').write_text(json.dumps(release, indent=2) + '\n')
        log(f'release in {self.release}' + ('' if release['clean'] else ' (NOT clean: see "changed")'))

    def stamp(self, step):
        return self.stamps / (step + '.done')

    def identity(self, step):
        """What a step's result depends on besides the steps before it; a stamp with another identity is stale. setup and
        policies depend on the IPSW and the pins the fetch follows anyway; a new Data seed is made once (another one is another
        volume UUID, which no VM made from this one's could mount)."""
        if step in STICKY:
            return {}
        if step == 'data':
            return {'generation': str(self.args.generation.resolve()) if self.args.generation else None}
        identity = {'checkouts': {name: [state['commit'], state.get('digest')] for name, state in self.checkouts().items()}}
        if step in ('kernel', 'trust', 'payload', 'release'):
            identity['seed'] = (self.record('seed') or {}).get('seed_sha256')
        if step == 'trust' and self.args.generation:
            # the generation's records are files outside the checkouts: a record added there (an app installed since)
            identity['generation_trust'] = [sha256(path) for path in
                                            sorted((self.args.generation.resolve() / 'trust').glob('*.json'))]
        if step in ('payload', 'release'):
            identity['unique_chip_id'] = self.args.unique_chip_id
        return identity

    def stale(self, step):
        stamp = self.stamp(step)
        if not stamp.is_file():
            return True
        try:
            return json.loads(stamp.read_text()).get('identity') != self.identity(step)
        except ValueError:
            return True

    def main(self):
        if self.args.list:
            for step in STEPS:
                print(('     ' if not self.stamp(step).is_file() else 'stale' if self.stale(step) else 'done ') + ' ' + step)
            return
        wanted = STEPS
        if self.args.only:
            wanted = [s for s in self.args.only.split(',')]
        elif self.args.from_step or self.args.to_step:
            start = STEPS.index(self.args.from_step) if self.args.from_step else 0
            end = STEPS.index(self.args.to_step) + 1 if self.args.to_step else len(STEPS)
            wanted = STEPS[start:end]
        forced = set(self.args.only.split(',')) if self.args.only else (
            set(STEPS[STEPS.index(self.args.from_step):]) if self.args.from_step else set())
        for step in wanted:
            if step not in STEPS:
                sys.exit(f'unknown step {step}; steps: {" ".join(STEPS)}')
        self.check_clean()
        if self.stamp('setup').is_file() and 'setup' not in wanted:
            self.sync_sources()
        rebuilt = False
        for step in wanted:
            # a step after one that ran runs too (its inputs are new), except the ones that run once
            if step not in forced and not self.stale(step) and not (rebuilt and step not in STICKY and step != 'data'):
                log(f'step {step}: done before')
                continue
            log(f'step {step}')
            started = time.time()
            getattr(self, 'step_' + step.replace('-', '_'))()
            self.stamps.mkdir(parents=True, exist_ok=True)
            self.stamp(step).write_text(json.dumps({'done': time.strftime('%Y-%m-%d %H:%M:%S'),
                                                    'identity': self.identity(step)}, indent=1) + '\n')
            rebuilt = rebuilt or step not in STICKY
            log(f'step {step}: done in {int(time.time() - started)} s')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--unixnu', type=Path, default=REPO.parent / 'UniXNU', help='the UniXNU checkout')
    parser.add_argument('--os', dest='os_name', help='the OS profile (default: the board\'s); macos-27.0.1-26A434 builds macOS 27 '
                        '(meta todo/macos-bringup.md)')
    parser.add_argument('--unique-chip-id', default=os.environ.get('UNIXNU_UNIQUE_CHIP_ID'),
                        help='the platform seed of the payload (16 hex digits; or UNIXNU_UNIQUE_CHIP_ID)')
    parser.add_argument('--generation', type=Path,
                        help='for a VM that exists: the directory of the Data seed it was made from (seed.json, the seed, '
                             'trust/*.json); default: a new seed')
    parser.add_argument('--data-gib', type=int, default=12, help='a new seed\'s Data container size (default 12)')
    parser.add_argument('--ipsw-cache', type=Path, help='out/ipsw/<build> of another checkout: its members are cloned, not '
                        'downloaded again (the OS and SystemOS images are about 10 GiB)')
    parser.add_argument('--allow-dirty', action='store_true', help='build from checkouts with local changes (an experiment)')
    parser.add_argument('--from', dest='from_step')
    parser.add_argument('--to', dest='to_step')
    parser.add_argument('--only')
    parser.add_argument('--list', action='store_true')
    Build(parser.parse_args()).main()


if __name__ == '__main__':
    main()
