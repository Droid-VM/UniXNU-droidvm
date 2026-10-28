#!/usr/bin/env python3
"""Build everything a DroidVM iPadOS guest boots from, from a fresh checkout, on the build Mac.

  build-droidvm-all.py [--unixnu DIR] [--unique-chip-id HEX] [--ipsw-cache DIR] [--from STEP] [--to STEP] [--only STEP,...] [--list]

DIR is the UniXNU checkout (default: ../UniXNU beside this device repository). Each step leaves a stamp in
out/droidvm/build-all/ and is skipped on the next run; --from/--only run steps again (and --from everything after).
The result is out/droidvm/release/: droidvm-disk.img (the boot disk: UniXNU.efi, DarwinBoot.efi, kernel, ADT, trust, Data seed),
os.img and systemos.img (the read-only OS and SystemOS disks) and release.json (what went in, with hashes).
tools/rig/fresh-deploy.sh puts them on a phone as a new VM.

Steps, in order:
  setup      unixnu defconfig + fetch + ipsw fetch, and the OS and SystemOS members of the IPSW
  volumes    the IPSW volumes in tree/.cache/firmware/target-volumes; the OS image attached read only at os-mount
  policies   the owner root keys, the signed OS and SystemOS root policies, the original trust caches
  data-base  the Data seed from the OS's own /private/var: Setup skipped, the App cryptex staged; its APFS volume UUID goes into
             the kernel configuration (CONFIG_XNU_OWNER_DATA_VOLUME_UUID), which mounts Data by it
  reference  the reference candidate the storage and display audits read: this board's kernel WITHOUT its storage, display and
             owner-root drivers, built in out-reference/ from a copy of this repository (only the archives such a kernel links)
  kernel     unixnu build kernel
  package    unixnu build package (UniXNU.efi) and DarwinBoot.efi
  tools      the owner tools (ownervisor, toybox, zsh, dropbear, gfxprobe, lsdiag, applebox, ipatool, debugserver)
  plugin     the Metal plugin (AdrenoMetal frontend, metal2vulkan, Turnip)
  data       the plugin and the owner tools staged into the Data seed, grown to --data-gib
  trust      the owner trust bundle (plugin, tools, the original modules)
  payload    unixnu build payload around that trust and seed, then the boot disk
  images     os.img and systemos.img, release.json
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
STEPS = ['setup', 'volumes', 'policies', 'data-base', 'reference', 'kernel', 'package', 'tools', 'plugin', 'data', 'trust',
         'payload', 'images']
# The owner tools, in build order: each writes build/<name>/... and build/owner-trust-extra/<name>.json.
TOOLS = ['ownervisor', 'toybox', 'zsh', 'dropbear', 'gfxprobe', 'lsdiag', 'applebox', 'ipatool', 'debugserver']
# What a member of the IPSW is called (board.toml [ipsw.sha256] pins both).
OS_DMG, SYSTEMOS_DMG = '044-76040-038.dmg', '044-76034-038.dmg'
OS_IMAGE_SHA256 = 'e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2'   # the raw OS disk, 6077580800 bytes
APP_TRUST_SHA256 = '5d9bf23c900cb0b6ea241b1208a08aeef9cd863536452d85e68d1c0ef98e77c4'  # Cryptex1,AppTrustCache (IM4P)
# The GPU clients the seed's plugin names before the kernel's own list (package/DroidVMBootArgs.c) applies.
GPU_CLIENTS = 'WidgetRenderer_Default,PosterBoard,iconservicesagent'
# The reference candidate is this board's kernel without what binds or replaces original classes: the storage drivers (whose
# exports would hide the original IOBlockStorageDevice dispatch from the storage audit), the legacy display and the owner root
# chain that needs the storage drivers.
REFERENCE_OFF = ['NATIVE_VIRTIO_BLOCK', 'NATIVE_RAM_BLOCK', 'VERIFIED_ROOT_PROBE', 'OWNER_ROOT_AUDIT', 'OWNER_ROOT_TRUST',
                 'OWNER_CACHE_MOUNT', 'OWNER_CACHE_STARTUP', 'OWNER_BOOT_MANIFEST', 'OWNER_DATA_MOUNT', 'OWNER_UNION_SYSTEMOS',
                 'NATIVE_LEGACY_DISPLAY', 'BOARD_LAUNCH_POLICY']
REFERENCE_ARCHIVES = ['build-corecrypto-14.py', 'build-corecrypto-14.py --namespace', 'build-arm-driver.py ArmDTPlatform', 'build-arm-driver.py ArmCpu',
                      'build-arm-driver.py NativeAES', 'build-native-aes-bindings.py', 'build-libpthread-kernel.py']


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
    """An APFS clone (cp -c): the IPSW members stay one copy on disk."""
    if destination.is_file() and destination.stat().st_size == source.stat().st_size:
        return
    destination.unlink(missing_ok=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(['cp', '-c', source, destination])


class Build:
    def __init__(self, args):
        self.args = args
        self.unixnu = args.unixnu.resolve()
        self.board = tomllib.loads((DEVICE / 'board.toml').read_text())
        self.out = Path(os.environ.get('UNIXNU_OUT', self.unixnu / 'out')).resolve()
        self.work = self.out / self.board['board']['name']
        self.tree = self.work / 'tree'
        self.stamps = self.work / 'build-all'
        self.logs = self.work / 'build-all/logs'
        self.release = self.work / 'release'
        self.ipsw = self.out / 'ipsw' / self.board['ipsw']['build']
        self.volumes = self.tree / '.cache/firmware/target-volumes'
        self.data = self.tree / 'build/droidvm-data'
        self.scratch = Path('/tmp') / f'droidvm-build-{os.environ.get("USER", "build")}'
        self.env = dict(os.environ, PATH=os.pathsep.join([str(Path.home() / '.cargo/bin'), '/opt/homebrew/bin',
                                                          os.environ.get('PATH', '')]))

    def unixnu_cmd(self, *arguments, logfile=None, out=None):
        env = dict(self.env, UNIXNU_OUT=str(out or self.out))
        return run([self.unixnu / 'unixnu', *arguments], cwd=self.unixnu, env=env, logfile=logfile)

    def script(self, name, *arguments, logfile=None, check=True):
        """A script of the assembled tree (UniXNU's scripts with this repository's on top), run from the tree."""
        return run(['python3', self.tree / 'scripts' / name, *arguments], cwd=self.tree, env=self.env,
                   logfile=logfile or self.logs / (Path(name).stem + '.log'), check=check)

    # setup ---------------------------------------------------------------------------------------------------------------
    def step_setup(self):
        if self.args.ipsw_cache:
            # Members another checkout has extracted already (board.toml pins them; ipsw fetch checks them and keeps them).
            for source in sorted(self.args.ipsw_cache.resolve().iterdir()):
                if source.is_file() and not source.name.endswith('.partial') and not (self.ipsw / source.name).exists():
                    clone(source, self.ipsw / source.name)
        self.unixnu_cmd('defconfig', self.board['kernel']['defconfig'], '-D', DEVICE)
        self.unixnu_cmd('fetch', logfile=self.logs / 'fetch.log')
        self.unixnu_cmd('ipsw', 'fetch', logfile=self.logs / 'ipsw-fetch.log')
        # The root disks and the policies over them need the OS and SystemOS volumes too (about 9 GiB).
        self.unixnu_cmd('ipsw', 'fetch', 'OS', 'Cryptex1,SystemOS', logfile=self.logs / 'ipsw-fetch-volumes.log')
        self.unixnu_cmd('tree', logfile=self.logs / 'tree.log')

    # volumes -------------------------------------------------------------------------------------------------------------
    # macOS refuses an image attachment (and a mount point) on the external DATA volume to a process started over ssh, so every
    # image the build attaches lives in the scratch directory on the system volume: the OS image (a copy, which
    # target-volumes/<dmg> and target-volumes/os-mount point to), and the Data seed scripts' working images (seed_script).
    def os_attachment(self):
        """The whole-disk device of the OS image's read-only attachment, attaching it at os-mount when it is not."""
        source, image, mount = self.ipsw / OS_DMG, self.scratch / OS_DMG, self.scratch / 'os-mount'
        if not image.is_file() or image.stat().st_size != source.stat().st_size:
            self.scratch.mkdir(parents=True, exist_ok=True)
            run(['cp', source, image])
        for name, target in ((OS_DMG, image), ('os-mount', mount)):
            link = self.volumes / name
            if not link.is_symlink() or link.resolve() != target.resolve():
                if link.is_dir() and not link.is_symlink():
                    link.rmdir()
                else:
                    link.unlink(missing_ok=True)
                link.parent.mkdir(parents=True, exist_ok=True)
                link.symlink_to(target)
        for item in plistlib.loads(subprocess.check_output(['hdiutil', 'info', '-plist']))['images']:
            if Path(item['image-path']).resolve() == image.resolve():
                entities = item['system-entities']
                if item.get('writeable') is False and any(e.get('mount-point') == str(mount.resolve()) for e in entities):
                    return next(e['dev-entry'] for e in entities if e.get('content-hint') == 'GUID_partition_scheme')
                run(['hdiutil', 'detach', entities[0]['dev-entry']])
        mount.mkdir(parents=True, exist_ok=True)
        attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-readonly', '-nobrowse', '-owners', 'off',
                                                           '-mountpoint', str(mount), '-plist', str(image)]))
        return next(e['dev-entry'] for e in attached['system-entities'] if e.get('content-hint') == 'GUID_partition_scheme')

    def step_volumes(self):
        clone(self.ipsw / SYSTEMOS_DMG, self.volumes / SYSTEMOS_DMG)
        log(f'OS image attached: {self.os_attachment()}')

    # policies ------------------------------------------------------------------------------------------------------------
    def step_policies(self):
        self.os_attachment()
        host = self.tree / 'build/verified-root-host'
        if not (host / 'verified-root.dylib').is_file():
            openssl = subprocess.check_output(['brew', '--prefix', 'openssl@3'], text=True, env=self.env).strip()
            run(['python3', self.tree / 'kits/VerifiedRoot/tests/check.py', '--output', host, '--openssl-prefix', openssl],
                cwd=self.tree, env=self.env, logfile=self.logs / 'verified-root-host.log')
        for policy, name in (('verified-root-os', 'build-owner-os-policy.py'),
                             ('verified-root-systemos', 'build-owner-systemos-policy.py')):
            if not (self.tree / 'build' / policy / 'verification.json').is_file():
                self.script(name)
        self.script('build-original-trust-from-members.py', self.ipsw)
        # The App cryptex's own trust module, byte for byte (make-owner-trust-bundle.py accepts it by its pinned hash).
        candidates = [p for p in self.ipsw.glob('*.trustcache') if sha256(p) == APP_TRUST_SHA256]
        if not candidates:
            sys.exit(f'no member of {self.ipsw} is the App cryptex trust cache (sha256 {APP_TRUST_SHA256})')
        clone(candidates[0], self.tree / 'build/original-trust-extra/app.trustcache')

    # data-base -----------------------------------------------------------------------------------------------------------
    def seed_script(self, name, *arguments):
        """A Data seed script, run from a copy of the tree's scripts on the system volume: the scripts make their working
        images in ROOT/build, which must not be on the DATA volume (see os_attachment). Their inputs in build/ are links to
        the tree's."""
        root = self.scratch / 'seedtree'
        for directory in ('scripts', 'drivers/DarwinBoot/Test'):
            if (root / directory).exists():
                shutil.rmtree(root / directory)
            shutil.copytree(self.tree / directory, root / directory, symlinks=False,
                            ignore=shutil.ignore_patterns('__pycache__'))
        (root / 'build').mkdir(parents=True, exist_ok=True)
        if not (root / '.cache').is_symlink():
            (root / '.cache').symlink_to(self.tree / '.cache')     # the attached OS volume (target-volumes/os-mount)
        for entry in (self.tree / 'build').iterdir():
            link = root / 'build' / entry.name
            if not link.exists() and not link.is_symlink():
                link.symlink_to(entry)
        return run(['python3', root / 'scripts' / name, *arguments], cwd=root, env=self.env,
                   logfile=self.logs / (Path(name).stem + '.log'))

    def pack(self, raw, output):
        run(['python3', '-c', 'import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); import packed_ramdisk; '
             'print(packed_ramdisk.pack(Path(sys.argv[2]), Path(sys.argv[3])))',
             self.tree / 'drivers/DarwinBoot/Test', raw, output], cwd=self.tree, env=self.env)

    def seed_chain(self, base, chain):
        """Make each seed of CHAIN ([(name, [script, args...])]) from the one before; '{base}' and '{out}' place the two
        seeds in the arguments, which are otherwise BASE OUTPUT after them. Returns the last one."""
        for name, command in chain:
            output = self.data / name
            if not output.is_file():
                arguments = [str(base) if a == '{base}' else str(output) if a == '{out}' else a for a in command[1:]]
                if '{base}' not in command:
                    arguments += [str(base), str(output)]
                self.seed_script(command[0], *arguments)
            base = output
        return base

    def step_data_base(self):
        self.os_attachment()
        self.data.mkdir(parents=True, exist_ok=True)
        fixture = self.tree / 'build/owner-data-fixture'
        if not (fixture / 'owner-data-32.cdr').is_file():
            # A fresh APFS volume holding the OS's own /private/var: its volume UUID is new each time (apply_data_uuid).
            scratch = self.scratch / 'owner-data-fixture'
            if scratch.exists():
                shutil.rmtree(scratch)
            self.seed_script('prepare-owner-data-fixture.py', '--output', scratch)
            fixture.mkdir(parents=True, exist_ok=True)
            for name in ('owner-data-32.cdr', 'seed.json'):
                shutil.copyfile(scratch / name, fixture / name)
        if not (self.data / 'data-32.dbr').is_file():
            self.pack(fixture / 'owner-data-32.cdr', self.data / 'data-32.dbr')
        # the App cryptex is attached too: a copy on the system volume
        cryptex = self.scratch / self.app_cryptex()
        shutil.copyfile(self.ipsw / self.app_cryptex(), cryptex)
        self.seed_chain(self.data / 'data-32.dbr', [
            ('data-2g.dbr', ['seed-data-grow.py', '--volatile', '--gib', '2']),
            ('data-2g-skip.dbr', [self.board['payload']['skip_setup_tool'].split('/')[-1]]),
            ('data-2g-skip-app.dbr', ['add-app-cryptex-data.py', '{base}', str(cryptex), '{out}'])])
        self.apply_data_uuid()

    def apply_data_uuid(self):
        """The kernel mounts Data by its APFS volume UUID, and every new fixture has a new one: put it into the .config
        (unixnu defconfig puts the defconfig's back)."""
        seed = json.loads((self.tree / 'build/owner-data-fixture/seed.json').read_text())
        self.set_config('OWNER_DATA_VOLUME_UUID', '"' + seed['volume_uuid'] + '"')
        log(f'Data volume UUID {seed["volume_uuid"]}')

    def app_cryptex(self):
        for path in sorted(self.ipsw.glob('*.dmg.json')):
            record = json.loads(path.read_text())
            if record.get('component') == 'Cryptex1,AppOS':
                return record['member'].split('/')[-1]
        sys.exit('ipsw fetch left no Cryptex1,AppOS member')

    def set_config(self, name, value):
        """Set CONFIG_XNU_<name> in the output's .config (what unixnu defconfig merged), keeping everything else."""
        config = self.work / '.config'
        text = config.read_text()
        line = f'CONFIG_XNU_{name}={value}'
        text, count = re.subn(rf'^CONFIG_XNU_{name}=.*$', line, text, flags=re.M)
        config.write_text(text if count else text.rstrip('\n') + '\n' + line + '\n')

    # reference -----------------------------------------------------------------------------------------------------------
    def step_reference(self):
        out = self.unixnu / 'out-reference'
        device = out / 'device'
        if device.exists():
            shutil.rmtree(device)
        shutil.copytree(DEVICE, device, ignore=shutil.ignore_patterns('.git', '__pycache__', 'build', '.cache'))
        board = (device / 'board.toml').read_text()
        archives = '\n'.join(f'  "{entry}",' for entry in REFERENCE_ARCHIVES)
        board, count = re.subn(r'^archives = \[\n.*?^\]', 'archives = [\n' + archives + '\n]', board, flags=re.M | re.S)
        if count != 1:
            sys.exit('board.toml: no [kernel] archives list to replace')
        (device / 'board.toml').write_text(board)
        defconfig = (device / 'configs' / self.board['kernel']['defconfig']).read_text()
        for name in REFERENCE_OFF:
            defconfig = re.sub(rf'^CONFIG_XNU_{name}=.*$', f'CONFIG_XNU_{name}=n', defconfig, flags=re.M)
        defconfig = re.sub(r'^CONFIG_XNU_(NATIVE_VIRTIO_BLOCK|VERIFIED_ROOT)_ARCHIVE=.*\n', '', defconfig, flags=re.M)
        (device / 'configs/droidvm_reference_defconfig').write_text(defconfig)
        self.unixnu_cmd('defconfig', 'droidvm_reference_defconfig', '-D', device, out=out)
        self.unixnu_cmd('fetch', out=out, logfile=self.logs / 'reference-fetch.log')
        self.unixnu_cmd('ipsw', 'fetch', out=out, logfile=self.logs / 'reference-ipsw.log')
        self.heal_bootstrap(out / self.board['board']['name'])
        self.unixnu_cmd('build', 'kernel', out=out, logfile=self.logs / 'reference-kernel.log')
        target = self.tree / 'build/reference-candidate'
        target.mkdir(parents=True, exist_ok=True)
        for suffix in ('.macho', '.json'):
            shutil.copyfile(out / self.board['board']['name'] / 'kernel' / ('kernel' + suffix), target / ('kernel' + suffix))

    # kernel --------------------------------------------------------------------------------------------------------------
    @staticmethod
    def heal_bootstrap(work):
        """A kernel build that failed after its bootstrap pass can leave startup.o's compile record without startup.o (a
        rebuild removes the object first); the next build then skips the bootstrap pass and the archive builders find no
        object. Without the record the bootstrap pass runs again."""
        record = work / 'tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
        if record.is_file() and not record.with_suffix('').is_file():
            log(f'{record.with_suffix("")} is missing: the bootstrap pass runs again')
            record.unlink()

    def step_kernel(self):
        self.os_attachment()
        self.apply_data_uuid()
        self.heal_bootstrap(self.work)
        self.unixnu_cmd('build', 'kernel', logfile=self.logs / 'kernel.log')

    # package -------------------------------------------------------------------------------------------------------------
    def efi_dir(self):
        return self.work / 'package/Build/Out/AARCH64-DEBUG'

    def step_package(self):
        self.unixnu_cmd('build', 'package', logfile=self.logs / 'package.log')
        edk2 = sorted((self.out / 'feeds').glob('mu_basecore-*'))
        if not edk2:
            sys.exit('unixnu build package left no out/feeds/mu_basecore-*')
        # the CLANGPDB tools unixnu linked for the package step (Homebrew LLVM's llvm-lib and lld-link beside clang)
        run(['bash', DEVICE / 'scripts/build-darwinboot-efi.sh', self.tree, edk2[-1], 'DEBUG'],
            env=dict(self.env, CLANG_BIN=str(self.work / 'toolchain') + '/'), logfile=self.logs / 'darwinboot.log')
        for name in ('UniXNU.efi', 'DarwinBoot.efi'):
            if not (self.efi_dir() / name).is_file():
                sys.exit(f'no {self.efi_dir() / name}')

    # tools ---------------------------------------------------------------------------------------------------------------
    def step_tools(self):
        for tool in TOOLS:
            run(['python3', DEVICE / f'scripts/build-droidvm-{tool}.py', self.tree], cwd=DEVICE, env=self.env,
                logfile=self.logs / f'tool-{tool}.log')

    # plugin --------------------------------------------------------------------------------------------------------------
    def prepare_mesa(self):
        """What UniXNU's Turnip build takes as given (docs/porting/migration.md, "build inputs"): the pinned Mesa source with the
        recorded patches in .cache/mesa-work, meson and mako in .cache/mesa-venv, bison and flex in .cache/mesa-tools, and the
        pinned metal2vulkan source in .cache/gpu-reference/metal2vulkan."""
        cache = self.tree / '.cache'
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
        # the pin and the recorded patches, in order, from a clean checkout -- once: a new checkout touches every file, and
        # Turnip's builds would compile everything again
        identity = mesa['commit'] + ''.join(sha256(self.tree / patch) for patch in pins['patches'])
        stamp = cache / 'mesa-work.prepared'
        if not stamp.is_file() or stamp.read_text() != identity:
            stamp.unlink(missing_ok=True)
            run(['git', '-C', work, 'checkout', '-q', '-f', '--detach', mesa['commit']], env=self.env)
            run(['git', '-C', work, 'clean', '-q', '-fdx'], env=self.env)
            for patch in pins['patches']:
                run(['git', '-C', work, 'apply', self.tree / patch], env=self.env)
            stamp.write_text(identity)
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
        head = subprocess.run(['git', '-C', m2v, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
        if head != translator['commit']:
            run(['git', '-C', m2v, 'checkout', '-q', '-f', '--detach', translator['commit']], env=self.env)
            run(['git', '-C', m2v, 'clean', '-q', '-fdx'], env=self.env)

    def step_plugin(self):
        self.prepare_mesa()
        self.script('build-turnip-darwin.py', '--target', 'ios', logfile=self.logs / 'turnip-ios.log')
        # the host build: the plugin replays Turnip's first submission, which gen-turnip-replay.py records on the host simulation
        self.script('build-turnip-darwin.py', '--target', 'host', logfile=self.logs / 'turnip-host.log')
        # The host simulation (Turnip against the host copy of the NativeAdreno kernel stack) builds plugin_smoke, which the
        # plugin's replay recording runs, then runs its regression checks. A failing check is reported (release.json), not a
        # failed build: the plugin needs only the binaries.
        status = self.script('check-turnip-darwin-host.py', logfile=self.logs / 'turnip-host-check.log', check=False)
        failed = sorted(p.stem for p in (self.tree / 'build/turnip-darwin-host').glob('*.log')
                        if 'FAIL' in p.read_text(errors='replace'))
        self.record_check('turnip-host', 'PASS' if status == 0 else 'FAIL ' + ' '.join(failed or ['(see the log)']))
        if not (self.tree / 'build/turnip-darwin-host/plugin_smoke').is_file():
            sys.exit('the host simulation left no plugin_smoke: ' + str(self.logs / 'turnip-host-check.log'))
        self.script('build-adreno-metal-plugin.py', logfile=self.logs / 'plugin.log')

    def record_check(self, name, result):
        checks = self.stamps / 'checks.json'
        data = json.loads(checks.read_text()) if checks.is_file() else {}
        data[name] = result
        self.stamps.mkdir(parents=True, exist_ok=True)
        checks.write_text(json.dumps(data, indent=2) + '\n')
        log(f'check {name}: {result}')

    # data ----------------------------------------------------------------------------------------------------------------
    def seed_name(self):
        return f'data-{self.args.data_gib}g-skip-ov.dbr'

    def step_data(self):
        self.seed_chain(self.data / 'data-2g-skip-app.dbr', [
            ('data-2g-skip-gpu.dbr', ['add-gpu-plugin-data.py', '--publish', '--clients', GPU_CLIENTS]),
            ('data-2g-skip-ov.dbr', ['add-ownervisor-data.py']),
            (self.seed_name(), ['seed-data-grow.py', '--gib', str(self.args.data_gib)])])

    # trust ---------------------------------------------------------------------------------------------------------------
    def step_trust(self):
        self.script('make-owner-trust-bundle.py', logfile=self.logs / 'trust.log')

    # payload -------------------------------------------------------------------------------------------------------------
    def step_payload(self):
        chip = self.args.unique_chip_id
        if not chip:
            sys.exit('the payload needs --unique-chip-id (or UNIXNU_UNIQUE_CHIP_ID): the platform seed, 16 hex digits')
        self.unixnu_cmd('build', 'payload', '--owner-trust-cache', self.tree / 'build/adreno-metal-plugin/owner-trust.bundle',
                        '--data-ramdisk', self.data / self.seed_name(), '--unique-chip-id', chip,
                        logfile=self.logs / 'payload.log')
        run(['bash', DEVICE / 'scripts/make-droidvm-disk.sh', self.work / 'payload/payload.img', self.efi_dir(),
             self.work / 'payload/droidvm-disk.img'], env=self.env, logfile=self.logs / 'disk.log')

    # images --------------------------------------------------------------------------------------------------------------
    def step_images(self):
        self.release.mkdir(parents=True, exist_ok=True)
        os_image = self.release / 'os.img'
        if not os_image.is_file() or sha256(os_image) != OS_IMAGE_SHA256:
            # the raw OS disk: the whole virtual medium of the read-only attachment, which the OS policy signed block by block
            whole = self.os_attachment()
            run(['dd', f'if=/dev/r{Path(whole).name}', f'of={os_image}', 'bs=4m'], env=self.env)
            if sha256(os_image) != OS_IMAGE_SHA256:
                sys.exit(f'{os_image} is not the OS image the root policy signed')
        clone(self.ipsw / SYSTEMOS_DMG, self.release / 'systemos.img')   # already a raw APFS container
        for source, name in ((self.work / 'payload/droidvm-disk.img', 'droidvm-disk.img'),
                             (self.work / 'kernel/kernel.macho', 'kernel.macho')):
            shutil.copyfile(source, self.release / name)

        def commit(path):
            head = subprocess.run(['git', '-C', path, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
            dirty = subprocess.run(['git', '-C', path, 'status', '--porcelain', '--untracked-files=no'],
                                   capture_output=True, text=True).stdout.strip()
            return head + ('-dirty' if dirty else '')
        plugin = json.loads((self.tree / 'build/adreno-metal-plugin/trust.json').read_text())
        tools = {}
        for path in sorted((self.tree / 'build/owner-trust-extra').glob('*.json')):
            tools[path.stem] = json.loads(path.read_text())['cdhashes']
        release = {
            'built': time.strftime('%Y-%m-%d %H:%M:%S %z'),
            'unixnu': commit(self.unixnu), 'device': commit(DEVICE),
            'darwinboot': commit(self.unixnu / 'drivers/DarwinBoot'),
            'data_seed': self.seed_name(), 'data_volume_uuid': json.loads(
                (self.tree / 'build/owner-data-fixture/seed.json').read_text())['volume_uuid'],
            'trust_sha256': sha256(self.tree / 'build/adreno-metal-plugin/owner-trust.bundle'),
            'plugin': {k: plugin[k] for k in ('cdhash', 'daemon_cdhash', 'turnip_cdhash')},
            'owner_tools': tools,
            'checks': json.loads((self.stamps / 'checks.json').read_text()) if (self.stamps / 'checks.json').is_file() else {},
            'files': {name: {'bytes': (self.release / name).stat().st_size, 'sha256': sha256(self.release / name)}
                      for name in ('droidvm-disk.img', 'kernel.macho', 'os.img', 'systemos.img')},
        }
        (self.release / 'release.json').write_text(json.dumps(release, indent=2) + '\n')
        log(f'release in {self.release}')

    def stamp(self, step):
        return self.stamps / (step + '.done')

    def main(self):
        if self.args.list:
            for step in STEPS:
                print(('done ' if self.stamp(step).is_file() else '     ') + step)
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
            if self.stamp(step).is_file() and step not in forced:
                log(f'step {step}: done before')
                continue
            function = getattr(self, 'step_' + step.replace('-', '_'), None)
            if function is None:
                sys.exit(f'step {step}: not written yet')
            log(f'step {step}')
            started = time.time()
            function()
            self.stamps.mkdir(parents=True, exist_ok=True)
            self.stamp(step).write_text(time.strftime('%Y-%m-%d %H:%M:%S') + '\n')
            log(f'step {step}: done in {int(time.time() - started)} s')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--unixnu', type=Path, default=DEVICE.parent / 'UniXNU', help='the UniXNU checkout')
    parser.add_argument('--unique-chip-id', default=os.environ.get('UNIXNU_UNIQUE_CHIP_ID'),
                        help='the platform seed of the payload (16 hex digits; or UNIXNU_UNIQUE_CHIP_ID)')
    parser.add_argument('--data-gib', type=int, default=12, help='the Data container size (default 12)')
    parser.add_argument('--ipsw-cache', type=Path, help='out/ipsw/<build> of another checkout: its members are cloned, not '
                        'downloaded again (the OS and SystemOS volumes are about 8 GiB)')
    parser.add_argument('--from', dest='from_step')
    parser.add_argument('--to', dest='to_step')
    parser.add_argument('--only')
    parser.add_argument('--list', action='store_true')
    Build(parser.parse_args()).main()


if __name__ == '__main__':
    main()
