#!/usr/bin/env python3
"""The touch screen, keyboard, pen, trackpad and motion sensors of the iPadOS 27 board: NativeHidInput under the original IOHIDDevice
of the 24A446 fileset, and the virtio-input transport that feeds it, for the display archive (build-droidvm-legacy-display.py).

As the iPadOS 17 board builds them (scripts/build-native-legacy-display.py): ShengHidInput's NativeHidInput.cpp with the board's
keyboard sink and its motion sensors (kits/droidvm/DroidVMMotionHid.inc) in a staged copy, with the pen digitizer and the
trackpad's Magic Trackpad (kits/droidvm/DroidVMTrackpadHid.inc) of this board, the board probe
(kits/droidvm/HidInputProbe.cpp), the virtio-input transport (DroidVMVirtioInput.c) and the motion sysctl (DroidVMMotion.c). The
HID source is UniXNU-sheng's for iPadOS 27 (ShengHidInput 24f1f3f: 5f3a37c, the iPadOS 17 board's, and its arm64e edge-contact
work), compiled against the public IOHIDFamily headers of the OS profile's pin (refer/IOHIDFamily; NATIVE_HID_SOURCE_API), so every
IOHIDDevice virtual keeps its real name and pointer-authentication discriminator, as sheng's build-gop-legacy-display-27.py does.

The original class comes from the reviewed fixture hid-device-24A446.json (sheng's, for the same kernelcache: its 207 slot targets
and diversities, instance size and bindings). Each IOHIDDevice method the objects import is bound to the original entry of its slot
(the slot it fills in an owned vtable, or else the one slot whose diversity its name has); every owned vtable entry must carry the
original slot's diversity. An original function is reached through an owned stub that branches through an absolute literal (as the
board's display and accelerator stubs); the metaclass and the vtable are data, at their plain addresses.

Writes build/droidvm-hid-input/objects.json and sources.json (paths relative to the tree), which the display archive takes, and
report.json.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from kernel_target import ARCH, IOS_VERSION, TRIPLE
if ARCH != 'arm64e' or IOS_VERSION.split('.')[0] != '27':
    raise SystemExit('This HID builder only supports arm64e iPadOS 27 (24A446).')
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/droidvm-hid-input'
OUT.mkdir(parents=True, exist_ok=True)
HERE = Path(__file__).resolve().parent
# UniXNU-sheng's pin for iPadOS 27 (drivers/ShengHidInput of its feat/ipados27-gpu-parity 6ed0ba4).
HID_REPOSITORY = 'https://github.com/UniXNU/ShengHidInput.git'
HID_COMMIT = '24f1f3fbed6feff42e1ca02e2ce386a5c3c36da8'
RECORDS = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def load_discriminator():
    """The display builder's clang ptrauth_string_discriminator, one implementation for the board."""
    text = (HERE / 'build-droidvm-legacy-display.py').read_text()
    start = text.index('def discriminator(name):')
    namespace = {'struct': struct}
    exec(text[start:text.index('\n\n\n', start)], namespace)
    return namespace['discriminator']


def hid_source():
    """ShengHidInput at the pin, cloned once into the tree's source cache."""
    checkout = ROOT / '.cache/sources/ShengHidInput'
    if not (checkout / '.git').is_dir():
        checkout.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '-q', '--filter=blob:none', '--no-checkout', HID_REPOSITORY, checkout])
    if subprocess.run(['git', '-C', checkout, 'cat-file', '-e', HID_COMMIT], capture_output=True).returncode:
        run(['git', '-C', checkout, 'fetch', '-q', 'origin', HID_COMMIT])
    if run(['git', '-C', checkout, 'rev-parse', 'HEAD'], text=True).strip() != HID_COMMIT:
        run(['git', '-C', checkout, 'checkout', '-q', '-f', '--detach', HID_COMMIT])
    require(not run(['git', '-C', checkout, 'status', '--porcelain'], text=True), f'{checkout}: must be clean')
    return checkout / 'NativeHidInput.cpp'


def record(name):
    return json.loads((RECORDS / name).read_text().strip().rstrip(','))


def command(rec, src, dst, extra=()):
    """A kernel compile record with another source and output, its warnings replaced by the board's."""
    cmd, args, i = [], rec['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == rec['file']:
            cmd.append(str(src))
        elif value in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
            if value == '-o':
                cmd += ['-o', str(dst)]
            i += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    if Path(src).suffix != '.c':
        cmd += ['-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier']
    cmd += ['-mgeneral-regs-only', '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/droidvm'), *extra]
    run(cmd, cwd=rec['directory'])


def stage_hid(original):
    """NativeHidInput.cpp with the iPadOS 17 board's additions (build-native-legacy-display.py): report 1, the boot keyboard its
    descriptor declares, from the board's keyboard sink (DroidVMVirtioInput.c), and the motion sensors in the same translation
    unit, whose late static initializer the probe runs. And the pen: a third device, the pencil digitizer backboardd looks for
    (Digitizer/Stylus, report 4), sent from the board's pen sink, whose events the dispatch wrapper completes as an Apple
    Pencil's."""
    text = original.read_text()
    # The pen device: backboardd takes a Built-In, DisplayIntegrated service with the usage pair Digitizer/Stylus as the main
    # display's pencil digitizer ("acquired pencil digitizer"), and only that one's MaxHoverHeight gives its stylus events a hover
    # range (-[BKDirectTouchState ...]: read when the service's usage is 0x20; UIKit generates no hover without it,
    # _UIEventHIDShouldAllowHoverEventGeneration). Its report 4: Tip Switch, Barrel Switch, In Range, Eraser, X and Y as the
    # fingers' (0..32767), Z (the distance above the panel, 0..255), Tip Pressure (0..4095), X and Y Tilt (-90..90 degrees), in a
    # Stylus logical collection (IOHIDEventDriver takes a transducer's collection from a logical or physical one of Stylus and
    # beyond, and makes a stylus of any but a finger or a puck) with the range, the contact, the barrel and eraser buttons;
    # pressure and Z it scales to -1..1 (calibrated without a calibration) and the tilt it drops, which the wrapper corrects.
    pen_descriptor = (
        '// DroidVM (build-droidvm-hid-input.py): the pen digitizer\'s descriptor, report 4 (see stage_hid).\n'
        'static const unsigned char penDescriptor[]={\n'
        '    0x05,0x0d,0x09,0x20,0xa1,0x01, 0x85,0x04, 0x09,0x20,0xa1,0x02,\n'
        '    0x09,0x42,0x09,0x44,0x09,0x32,0x09,0x45,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x04,0x81,0x02, 0x75,0x04,0x95,0x01,0x81,0x03,\n'
        '    0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,\n'
        '    0x09,0x32,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x01,0x81,0x02,\n'
        '    0x05,0x0d,0x09,0x30,0x15,0x00,0x26,0xff,0x0f,0x75,0x10,0x95,0x01,0x81,0x02,\n'
        '    0x09,0x3d,0x09,0x3e,0x15,0xa6,0x25,0x5a,0x75,0x08,0x95,0x02,0x81,0x02,\n'
        '    0xc0,0xc0\n'
        '};\n')
    # UIKit reads a pencil's angles as the polar orientation of its stylus event (-[UITouch _updateWithChildEvent:]: altitudeAngle =
    # pi/2 - kIOHIDEventFieldDigitizerAltitude, the azimuth from kIOHIDEventFieldDigitizerAzimuth), and IOHIDEventDriver makes
    # tilt-type events whose tilt it never fills. No field sets the orientation type, so the wrapper sets it in the event's data
    # (IOHIDDigitizerEventData of IOHIDFamily-2238, as HID.framework's +polarDigitizerEvent: lays it out: 0xa0 bytes, orientationType
    # at 0x58, 0 tilt and 1 polar; IOHIDEvent::_data at +16) and the angles through IOHIDEvent::setFixedValue (slot 26, after
    # getFixedValue's 25; its discriminator that of _ZN10IOHIDEvent13setFixedValueEjij, as the others' are their methods').
    # Pressure and Z it sets from the report as well. backboardd gives UIKit the device's AccurateMaxDigitizerPressureValue as the
    # maximum force (-[BKDirectTouchState handlePathCollectionEvent:]) and its MaxHoverHeight as the maximum Z (1, Z runs 0..1).
    # A pencil's UITouch force is the event's pressure as it is over 120 (-[UITouch _unclampedForce]: _pressure / (120 *
    # _AXSForceTouchSensitivity(), 1.0 by default)), maximumPossibleForce the maximum over the same, and PencilKit widens its ink by
    # that force itself (InputPoint.inputPointForTouch), not by its share of the maximum: so the pressure runs 0..500 under a
    # maximum of 500, an Apple Pencil's 4.17 (0..1 under 1 gave every stroke a force below 0.01, the thinnest line).
    pen_orient = (
        '// DroidVM (build-droidvm-hid-input.py): the pen\'s stylus events get the polar angles of its last report (see stage_hid).\n'
        '#if __has_feature(ptrauth_calls)\n'
        'enum : uint16_t {kDivSetFixed=0x6e23};\n'
        '#else\n'
        'enum : uint16_t {kDivSetFixed=0};\n'
        '#endif\n'
        'enum {kSetFixed=26};\n'
        'enum : uint32_t {kTransducerType=0xB0004,kAltitude=0xB000F,kAzimuth=0xB0010,kPressure=0xB000A,kZ=0xB0002,kStylus=0,kPolar=1};\n'
        'static void orient(void *child) {\n'
        '    uint8_t *data=*reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(child)+16);\n'
        '    if(!data || *reinterpret_cast<uint32_t *>(data)!=0xa0 || *reinterpret_cast<uint32_t *>(data+4)!=kDigitizer) {\n'
        '        native_pen_hid[2]=native_pen_hid[2]+1;return;\n'
        '    }\n'
        '    *reinterpret_cast<uint32_t *>(data+0x58)=kPolar;\n'
        '    call<void,uint32_t,int32_t,IOOptionBits>(child,kSetFixed,kDivSetFixed,kAltitude,penAltitude,0);\n'
        '    call<void,uint32_t,int32_t,IOOptionBits>(child,kSetFixed,kDivSetFixed,kAzimuth,penAzimuth,0);\n'
        '    call<void,uint32_t,int32_t,IOOptionBits>(child,kSetFixed,kDivSetFixed,kPressure,penPressure,0);\n'
        '    call<void,uint32_t,int32_t,IOOptionBits>(child,kSetFixed,kDivSetFixed,kZ,penZ,0);\n'
        '    native_pen_hid[1]=native_pen_hid[1]+1;\n'
        '}\n')
    pen = ('    // DroidVM (build-droidvm-hid-input.py): the pen driver (DroidVMVirtioInput.c) calls penNotify with each pen frame, sent as\n'
           '    // report 4: flags (1 in range, 2 tip, 4 barrel, 8 eraser end), x, y, pressure, distance, tilt x and y in degrees, and\n'
           '    // the altitude and azimuth the dispatch wrapper gives the stylus event (IOFixed radians).\n'
           '    static void penNotify(void *context,const int32_t *pen) {\n'
           '        NativeHidInput *self=static_cast<NativeHidInput *>(context);\n'
           '        if(self && self->inputLoop && pen)self->inputLoop->runAction(&NativeHidInput::penAction,self,const_cast<int32_t *>(pen));\n'
           '    }\n'
           '    static IOReturn penAction(OSObject *owner,void *values,void *,void *,void *) {\n'
           '        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;\n'
           '        if(!self->hasEventDriverCached())return kIOReturnNotReady;\n'
           '        const int32_t *p=static_cast<const int32_t *>(values);\n'
           '        edge::penAltitude=p[7];edge::penAzimuth=p[8];\n'
           '        // in IOFixed: the pressure of 4095 as 0..500 (the device\'s AccurateMaxDigitizerPressureValue), the distance of 255\n'
           '        // (above the panel while not touching) as 0..1\n'
           '        edge::penPressure=(int32_t)(((int64_t)p[3]*500<<16)/4095);edge::penZ=(int32_t)(((int64_t)p[4]<<16)/255);\n'
           '        const unsigned flags=(unsigned)p[0],x=(unsigned)p[1],y=(unsigned)p[2],pressure=(unsigned)p[3];\n'
           '        // bit 0 Tip Switch, 1 Barrel Switch, 2 In Range, 3 Eraser, as the descriptor lists them\n'
           '        const unsigned char bits=(unsigned char)(((flags&2)?1:0)|((flags&4)?2:0)|((flags&1)?4:0)|((flags&8)?8:0));\n'
           '        unsigned char report[11]={4,bits,(unsigned char)x,(unsigned char)(x>>8),(unsigned char)y,(unsigned char)(y>>8),\n'
           '            (unsigned char)p[4],(unsigned char)pressure,(unsigned char)(pressure>>8),(unsigned char)(int8_t)p[5],(unsigned char)(int8_t)p[6]};\n'
           '        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);\n'
           '        IOReturn result=buffer?self->handleReport(buffer,kIOHIDReportTypeInput,0):kIOReturnNoMemory;\n'
           '        OSSafeReleaseNULL(buffer);\n'
           '        if(result==kIOReturnSuccess)edge::native_pen_hid[0]=edge::native_pen_hid[0]+1;\n'
           '        return result;\n'
           '    }\n')
    keyboard = ('    // DroidVM (build-droidvm-hid-input.py): the keyboard driver calls keyboardNotify with each boot keyboard state (modifier\n'
                '    // byte, reserved byte, six usages), sent as report 1.\n'
                '    static void keyboardNotify(void *context,const uint8_t *keys) {\n'
                '        NativeHidInput *self=static_cast<NativeHidInput *>(context);\n'
                '        if(self && self->inputLoop && keys)self->inputLoop->runAction(&NativeHidInput::keyboardAction,self,const_cast<uint8_t *>(keys));\n'
                '    }\n'
                '    static IOReturn keyboardAction(OSObject *owner,void *keys,void *,void *,void *) {\n'
                '        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;\n'
                '        if(!self->hasEventDriverCached())return kIOReturnNotReady;\n'
                '        unsigned char report[9]={1};\n'
                '        memcpy(report+1,keys,8);\n'
                '        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);\n'
                '        IOReturn result=buffer?self->handleReport(buffer,kIOHIDReportTypeInput,0):kIOReturnNoMemory;\n'
                '        OSSafeReleaseNULL(buffer);\n'
                '        return result;\n'
                '    }\n')
    for anchor, replacement in (
            ('extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *) __attribute__((weak_import));\n',
             'extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *) __attribute__((weak_import));\n'
             'extern "C" void native_keyboard_set_sink(void (*)(void *, const uint8_t *), void *);\n'),
            ('    bool hasEventDriverCached() {\n', keyboard + '    bool hasEventDriverCached() {\n'),
            ('        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);\n',
             '        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);\n'
             '        if(!touchOnly)native_keyboard_set_sink(&NativeHidInput::keyboardNotify,this);\n'),
            ('        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);\n',
             '        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);\n'
             '        if(!touchOnly)native_keyboard_set_sink(nullptr,nullptr);\n'),
            ('extern "C" void native_touch_set_sink(void (*)(void *), void *) __attribute__((weak_import));\n',
             'extern "C" void native_touch_set_sink(void (*)(void *), void *) __attribute__((weak_import));\n'
             'extern "C" void native_pen_set_sink(void (*)(void *, const int32_t *), void *);\n'),
            ('static const unsigned char touchDescriptor[]={\n', pen_descriptor + 'static const unsigned char touchDescriptor[]={\n'),
            ('namespace edge {\n',
             'namespace edge {\n'
             '// DroidVM (build-droidvm-hid-input.py): the pen\'s polar angles from its last report, and [0] pen reports sent, [1] stylus\n'
             '// events given them, [2] stylus events whose data was not the layout orient() knows.\n'
             'static int32_t penAltitude,penAzimuth,penPressure,penZ;\n'
             'extern "C" {volatile uint32_t native_pen_hid[3];}\n'),
            ('// One finger event; true when it now carries the bit.\nstatic bool mark(void *child) {\n'
             '    if(call<uint32_t,IOOptionBits>(child,kGetType,kDivGetType,0)!=kDigitizer)return false;   // the scan-time vendor child\n',
             pen_orient + '// One finger event; true when it now carries the bit.\nstatic bool mark(void *child) {\n'
             '    if(call<uint32_t,IOOptionBits>(child,kGetType,kDivGetType,0)!=kDigitizer)return false;   // the scan-time vendor child\n'
             '    if(call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kDivGetInteger,kTransducerType,0)==kStylus) {orient(child);return false;}\n'),
            ('    bool hasEventDriverCached() {\n', pen + '    bool hasEventDriverCached() {\n'),
            ('        if((touchOnly||touchEnabled) && &native_touch_set_sink)native_touch_set_sink(nullptr,nullptr);\n',
             '        if((touchOnly||touchEnabled) && &native_touch_set_sink)native_touch_set_sink(nullptr,nullptr);\n'
             '        if(penOnly)native_pen_set_sink(nullptr,nullptr);\n'),
            # The pen digitizer: its own descriptor, strings and properties, the pen sink, and the dispatch wrapper as the touch
            # screen's; never the keyboard's or the buttons' sinks, nor the touch sink (sheng-touch-hid is a board-wide switch).
            ('    bool touchOnly=false;\n', '    bool touchOnly=false;\n    bool penOnly=false;   // DroidVM: the pen digitizer (stage_hid)\n'),
            ('    void setTouchOnly() {touchOnly=true;}\n', '    void setTouchOnly() {touchOnly=true;}\n    void setPenOnly() {penOnly=true;}\n'),
            ('return OSString::withCString(touchOnly?"SPI":"Virtual");', 'return OSString::withCString(touchOnly||penOnly?"SPI":"Virtual");'),
            ('return OSString::withCString(touchOnly?"Offline touch digitizer":"Offline HID input probe");',
             'return OSString::withCString(penOnly?"Offline pen digitizer":touchOnly?"Offline touch digitizer":"Offline HID input probe");'),
            ('        if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}\n',
             '        if(penOnly){*out=IOBufferMemoryDescriptor::withBytes(penDescriptor,sizeof(penDescriptor),kIODirectionNone);'
             'return *out?kIOReturnSuccess:kIOReturnNoMemory;}\n'
             '        if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}\n'),
            ('        touchEnabled=PE_parse_boot_argn("sheng-touch-hid",&touch,sizeof(touch)) && touch==1;\n',
             '        touchEnabled=PE_parse_boot_argn("sheng-touch-hid",&touch,sizeof(touch)) && touch==1 && !penOnly;\n'
             '        if(penOnly) {\n'
             '            setProperty("Built-In",true);setProperty("DisplayIntegrated",true);setProperty("DigitizerCollectionDispatch",true);\n'
             '            setProperty("MaxHoverHeight",1ULL,32);setProperty("AccurateMaxDigitizerPressureValue",500ULL,32);\n'
             '        }\n'),
            ('        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);\n',
             '        if(penOnly)native_pen_set_sink(&NativeHidInput::penNotify,this);\n'
             '        if(!touchOnly && !penOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);\n'),
            ('        if(!touchOnly)native_keyboard_set_sink(&NativeHidInput::keyboardNotify,this);\n',
             '        if(!touchOnly && !penOnly)native_keyboard_set_sink(&NativeHidInput::keyboardNotify,this);\n'),
            ('        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);\n'
             '        if(!touchOnly)native_keyboard_set_sink(nullptr,nullptr);\n',
             '        if(!touchOnly && !penOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);\n'
             '        if(!touchOnly && !penOnly)native_keyboard_set_sink(nullptr,nullptr);\n'),
            ('        if(self->touchOnly){source->setTimeoutMS(1000);return;}\n',
             '        if(self->touchOnly||self->penOnly){source->setTimeoutMS(1000);return;}\n'),
            ('            if(driver && (touchEnabled || touchOnly))edge::install(driver);\n',
             '            if(driver && (touchEnabled || touchOnly || penOnly))edge::install(driver);\n'),
            # Further drivers of the same class (the pen digitizer's, after the touch screen's) take the same wrapped table.
            ('static void install(IORegistryEntry *driver) {\n    if(originalDispatch || !driver)return;\n'
             '    if(kSlotDiversity[kDispatch]!=kDivDispatch)return;\n',
             'static void **wrappedTable,**wrappedCopy;   // DroidVM: the table wrapped and its copy, for the next driver of its class\n'
             'static void install(IORegistryEntry *driver) {\n'
             '    if(driver && originalDispatch) {\n'
             '        if(wrappedCopy && vtableOf(driver)==wrappedTable)\n'
             '            *reinterpret_cast<void **>(driver)=ptrauth_sign_unauthenticated(wrappedCopy,ptrauth_key_process_independent_data,\n'
             '                ptrauth_blend_discriminator(driver,kVptrDiscriminator));\n'
             '        return;\n'
             '    }\n'
             '    if(originalDispatch || !driver)return;\n'
             '    if(kSlotDiversity[kDispatch]!=kDivDispatch)return;\n'),
            ('    native_touch_edge_install[0]=3;\n', '    native_touch_edge_install[0]=3;\n    wrappedTable=table;wrappedCopy=copy;\n')):
        require(text.count(anchor) == 1, 'NativeHidInput.cpp no longer has exactly one ' + anchor.strip())
        text = text.replace(anchor, replacement)
    text += ('\n// DroidVM (build-droidvm-hid-input.py): the pen digitizer, published by HidInputProbe.cpp when the VM has a pen.\n'
             'extern "C" IOService *native_hid_pen_allocate(void) {NativeHidInput *device=new NativeHidInput;'
             'if(device)device->setPenOnly();return device;}\n')
    text += '\n' + (ROOT / 'kits/droidvm/DroidVMMotionHid.inc').read_text()
    text += '\n' + (ROOT / 'kits/droidvm/DroidVMTrackpadHid.inc').read_text()
    # the same file name: the late static initializer is named after it (__GLOBAL__sub_I_NativeHidInput.cpp)
    staged = OUT / 'droidvm/NativeHidInput.cpp'
    staged.parent.mkdir(exist_ok=True)
    staged.write_text(text)
    return staged


def macho(data):
    """Load commands, segments and sections of an arm64e relocatable object."""
    magic, cpu, _, _, count = struct.unpack_from('<5I', data)
    require(magic == 0xfeedfacf and cpu == 0x100000c, 'arm64 Mach-O object')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        cmd, size = struct.unpack_from('<2I', data, offset)
        commands.append((cmd, offset, size))
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                sections.append({'va': va, 'size': length, 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    return {'commands': commands, 'segments': segments}


def object_symbols(data):
    st = next(p for c, p, _ in macho(data)['commands'] if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', data, st + 8)
    found = []
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', data, sy + i * 16)
        found.append((data[strings + ix:data.index(b'\0', strings + ix)].decode(), va, kind))
    return found


def object_vtable(data, found, name, count):
    """[(symbol, signed word)] of the first count entries of vtable name (authenticated pointer relocations)."""
    va = next(v for n, v, _ in found if n == name)
    entries = {}
    for segment in macho(data)['segments']:
        for sec in segment['sections']:
            if sec['va'] <= va < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', data, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', data, ro + i * 8)
                    # extern, 8 bytes, ARM64_RELOC_AUTHENTICATED_POINTER
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        entries[sec['va'] + offset] = (found[bits & 0xffffff][0],
                                                       struct.unpack_from('<Q', data, sec['fileoff'] + offset)[0])
    return [entries.get(va + 16 + i * 8) for i in range(count)]


def main():
    discriminator = load_discriminator()
    abi = json.loads((HERE / 'hid-device-24A446.json').read_text())
    legacy = json.loads((HERE / 'legacy-display-24A446.json').read_text())
    require(abi['kernelcache_sha256'] == legacy['kernelcache_sha256'], 'Both fixtures describe the same kernelcache')
    slide = int(abi['collection_slide'], 16)
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    # __cxa_pure_virtual: the display fixture's abstract slot 214.
    pure_target = int(legacy['slots'][214][0], 16)
    pure = {i for i, (t, _) in enumerate(slots) if t == pure_target}
    require(len(slots) == 207 and pure == {180} and abi['instance_size'] == '0xd0',
            'The original IOHIDDevice: 207 slots, newReportDescriptor (180) abstract, 0xd0 bytes')

    # The public IOHIDFamily headers of the pinned source, after the kernel's own; the macOS SDK's Kernel.framework supplies the
    # generated HIDDriverKit declarations the source checkout lacks.
    family = ROOT / 'refer/IOHIDFamily'
    family_commit = run(['git', '-C', family, 'rev-parse', 'HEAD'], text=True).strip()
    include = OUT / 'headers/include'
    if include.exists():
        shutil.rmtree(include)
    for source_dir, relative in ((family / 'IOHIDFamily', 'IOKit/hid'), (family / 'IOHIDSystem/IOKit/hidsystem', 'IOKit/hidsystem')):
        destination = include / relative
        destination.mkdir(parents=True, exist_ok=True)
        for path in sorted(source_dir.glob('*.h')):
            shutil.copyfile(path, destination / path.name)
    sdk = Path(run(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    hid_includes = ('-I', str(include), '-idirafter', str(sdk / 'System/Library/Frameworks/Kernel.framework/Versions/A/Headers'))

    cpp, c_mach, c_bsd = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json'), \
        record('bsd/DEVELOPMENT/kern_exec.o.json')
    original = hid_source()
    staged = stage_hid(original)
    hid_obj = OUT / 'NativeHidInput.o'
    command(cpp, staged, hid_obj, (*hid_includes, '-DNATIVE_HID_SOURCE_API=1'))
    probe = ROOT / 'kits/droidvm/HidInputProbe.cpp'
    command(cpp, probe, OUT / 'HidInputProbe.o')
    # and as the input kext's (ipados27/kexts/DroidVMInput.json): NativeHidInput registered as the kext loads, not staged here
    command(cpp, probe, OUT / 'HidInputProbe.kext.o', ('-DDROIDVM_NATIVE_KEXT=1',))
    transport = ROOT / 'kits/droidvm/DroidVMVirtioInput.c'
    # Its queue and event pages come from the kernel map (DROIDVM_INPUT_DYNAMIC): the native __DATA must fit the original kernel's slot.
    command(c_mach, transport, OUT / 'DroidVMVirtioInput.o', ('-DDROIDVM_INPUT_DYNAMIC=1',))
    motion = ROOT / 'kits/droidvm/DroidVMMotion.c'
    command(c_bsd, motion, OUT / 'DroidVMMotion.o')

    raw = hid_obj.read_bytes()
    found = object_symbols(raw)
    bindings = {n: int(v, 16) + slide for n, v in abi['bindings'].items()}
    by_diversity = {}
    for i, (_, d) in enumerate(slots):
        by_diversity.setdefault(d, []).append(i)
    vtables = [n for n, _, k in found if n.startswith('__ZTV') and k != 1 and '9MetaClassE' not in n]
    require(set(vtables) == {'__ZTV14NativeHidInput', '__ZTV17AppleSPUHIDDevice', '__ZTV24DroidVMTrackpadHIDDevice'},
            f'The owned HID classes: {vtables}')
    # An original method in an owned vtable is bound to the original entry of that slot (a base-class override signs with the
    # discriminator of the introducing IOService declaration, so its own name does not identify the slot); any other import by
    # the slot whose discriminator its name has.
    slot_of = {}
    for vt_name in vtables:
        for i, entry in enumerate(object_vtable(raw, found, vt_name, len(slots))):
            if entry and '11IOHIDDevice' in entry[0]:
                require(slot_of.setdefault(entry[0], i) == i, f'{entry[0]}: one slot in every owned vtable')
    for name in sorted({n for n, _, k in found if k == 1 and '11IOHIDDevice' in n} - set(bindings)):
        if name in slot_of:
            slot = slot_of[name]
        else:
            matches = by_diversity.get(discriminator(name[1:]), [])
            require(len(matches) == 1, f'{name}: one original IOHIDDevice slot with its discriminator')
            slot = matches[0]
        require(slot not in pure, f'{name}: not an abstract original slot')
        bindings[name] = slots[slot][0] + slide
    owned = {}
    for vt_name in vtables:
        entries = object_vtable(raw, found, vt_name, len(slots))
        require(all(entries), f'{vt_name}: at least the {len(slots)} IOHIDDevice entries')
        for i, (name, word) in enumerate(entries):
            require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == slots[i][1],
                    f'{vt_name} slot {i} ({name}) signed with the original diversity {slots[i][1]:#x}')
            if '11IOHIDDevice' in name:
                require(bindings.get(name) == slots[i][0] + slide, f'{vt_name} slot {i} ({name}) is the original entry')
            elif i in pure or i >= 168:
                require(vt_name[len('__ZTV'):] in name, f'{vt_name} slot {i} ({name}): an owned implementation')
        owned[vt_name] = [i for i, (name, _) in enumerate(entries) if vt_name[len('__ZTV'):] in name]

    DATA = {'__ZN11IOHIDDevice10gMetaClassE', '__ZTV11IOHIDDevice'}
    text, addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(bindings.items()):
        if name in DATA:
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            # An absolute literal: neither signed nor moved by the fileset emitter, so the stub lands on the original wherever the
            # native code is placed. Original entries begin with bti c or pacibsp, which accept a BR through x16.
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += f'.globl {name}\n.p2align 3\n{name}:\n bti c\n ldr x16, 1f\n br x16\n1: .quad {name}.original\n'
    (OUT / 'hid-bindings.S').write_text(text)
    (OUT / 'hid-addresses.S').write_text(addresses)
    ios_sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    for stem in ('hid-bindings', 'hid-addresses'):
        run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', ios_sdk, '-c', OUT / (stem + '.S'), '-o', OUT / (stem + '.o')])
    llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    late = OUT / 'NativeHidInput.late.o'
    run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp',
         hid_obj, late])

    objects = [late, OUT / 'hid-bindings.o', OUT / 'hid-addresses.o', OUT / 'HidInputProbe.o', OUT / 'DroidVMVirtioInput.o',
               OUT / 'DroidVMMotion.o']
    sources = [original, ROOT / 'kits/droidvm/DroidVMMotionHid.inc', ROOT / 'kits/droidvm/DroidVMTrackpadHid.inc', probe, transport,
               motion, HERE / 'hid-device-24A446.json']
    (OUT / 'objects.json').write_text(json.dumps([str(p.relative_to(ROOT)) for p in objects], indent=2) + '\n')
    # The same driver as a kext of the collection: NativeHidInput with its static initializer, the kext form of the probe.
    kext_objects = [hid_obj, OUT / 'hid-bindings.o', OUT / 'hid-addresses.o', OUT / 'HidInputProbe.kext.o',
                    OUT / 'DroidVMVirtioInput.o', OUT / 'DroidVMMotion.o']
    (OUT / 'kext-objects.json').write_text(json.dumps([str(p.relative_to(ROOT)) for p in kext_objects], indent=2) + '\n')
    (OUT / 'sources.json').write_text(json.dumps([str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p) for p in sources],
                                                 indent=2) + '\n')
    (OUT / 'report.json').write_text(json.dumps(dict(
        hid_source=dict(repository=HID_REPOSITORY, commit=HID_COMMIT, sha256=sha(original)), iohidfamily_commit=family_commit,
        kernelcache_sha256=abi['kernelcache_sha256'], bindings={n: hex(v) for n, v in sorted(bindings.items())},
        owned_slots=owned, objects={str(p.relative_to(ROOT)): sha(p) for p in objects}, original_instructions_modified=False),
        indent=2) + '\n')
    print(f'HID: {len(bindings)} original IOHIDDevice bindings; {", ".join(f"{v} owns {len(s)} slots" for v, s in owned.items())}')


if __name__ == '__main__':
    main()
