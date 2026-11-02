// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The original IOAESAccelerator that SoftwareAES's NativeAES subclasses (DroidVMSoftwareAES.cpp): the metaclass, constructor,
// destructors and methods of com.apple.iokit.IOCryptoAcceleratorFamily in the iPadOS 27.0.1 (24A446) fileset, where the original
// components keep their addresses between builds. Read from the fileset (out/kernel.macho of the rig):
//   - the instance vtable (0xfffffff008312668, address point +0x10): 181 slots, IOService's 168 then the class's 13 in the order
//     of SoftwareAES's OriginalAESABI.h (iPadOS 17's, unchanged: performAES(task) calls kauth_getuid and copyClientEntitlement,
//     performAES(memory descriptor) IOMemoryDescriptor::getPhysicalAddress, performAESQuantum's default logs "subclass is
//     required to implement performAESQuantum"); of IOService's it overrides slots 0, 1 (D1, D0), 7, 86, 111 and 117.
//   - gMetaClass: the object the static initializer constructs ("IOAESAccelerator", 0x120 bytes, parent IOCryptoAccelerator)
//     and getMetaClass returns.
//   - C2: the protected constructor that forwards x1 to IOCryptoAccelerator's and installs the vtable; D2 the tail call to the
//     parent's ahead of D1 (bti c, so 8 bytes before it).
// The metaclass is data: an absolute symbol, reached through a GOT entry that keeps its value. Every method is reached through a
// stub of its own in kernel text, which loads the original's address from a literal (a constant, no fixup) and branches to it: the
// fileset emitter moves the native kernel's code (a PC-relative call to an absolute address landed 0x39a8000 past it, an instruction
// fetch abort in NativeAES::MetaClass::alloc), and the stub lands on the original wherever it is placed, as
// build-droidvm-legacy-display.py's original bindings do. Original entries begin with bti c or pacibsp, which accept a BR through x16.
// DroidVMSoftwareAES.cpp checks gMetaClass against the live one before anything uses these.
.globl __ZN16IOAESAccelerator10gMetaClassE
.set __ZN16IOAESAccelerator10gMetaClassE, 0xfffffff00bb3ddf0

.text
.globl __ZN16IOAESAcceleratorC2EPK11OSMetaClass
.p2align 3
__ZN16IOAESAcceleratorC2EPK11OSMetaClass:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a7e14
.globl __ZN16IOAESAcceleratorD2Ev
.p2align 3
__ZN16IOAESAcceleratorD2Ev:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a7e84
.globl __ZN16IOAESAcceleratorD1Ev
.p2align 3
__ZN16IOAESAcceleratorD1Ev:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a7e8c
.globl __ZN16IOAESAcceleratorD0Ev
.p2align 3
__ZN16IOAESAcceleratorD0Ev:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a7e94
.globl __ZNK16IOAESAccelerator12getMetaClassEv
.p2align 3
__ZNK16IOAESAccelerator12getMetaClassEv:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a7ef4
.globl __ZN16IOAESAccelerator5startEP9IOService
.p2align 3
__ZN16IOAESAccelerator5startEP9IOService:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a8360
.globl __ZNK16IOAESAccelerator11getWorkLoopEv
.p2align 3
__ZNK16IOAESAccelerator11getWorkLoopEv:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a8bf8
.globl __ZN16IOAESAccelerator20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_
.p2align 3
__ZN16IOAESAccelerator20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a8ad8
.globl __ZN16IOAESAccelerator16getAlgorithmNameEv
.p2align 3
__ZN16IOAESAccelerator16getAlgorithmNameEv:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a8350
.globl __ZN16IOAESAccelerator10performAESEPvS0_P4taskyP7IOAESIV14IOAESOperationP12IOAESKeyDataPFvS0_iES0_
.p2align 3
__ZN16IOAESAccelerator10performAESEPvS0_P4taskyP7IOAESIV14IOAESOperationP12IOAESKeyDataPFvS0_iES0_:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a8c30
.globl __ZN16IOAESAccelerator10performAESEP18IOMemoryDescriptorS1_yP7IOAESIV14IOAESOperationP12IOAESKeyDatayyPFvPviES7_
.p2align 3
__ZN16IOAESAccelerator10performAESEP18IOMemoryDescriptorS1_yP7IOAESIV14IOAESOperationP12IOAESKeyDatayyPFvPviES7_:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0ab084
.globl __ZN16IOAESAccelerator14isKeySupportedEj
.p2align 3
__ZN16IOAESAccelerator14isKeySupportedEj:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a9138
.globl __ZN16IOAESAccelerator19getSupportedOptionsEv
.p2align 3
__ZN16IOAESAccelerator19getSupportedOptionsEv:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a9144
.globl __ZN16IOAESAccelerator17performAESQuantumEP23IOAESAcceleratorCommand
.p2align 3
__ZN16IOAESAccelerator17performAESQuantumEP23IOAESAcceleratorCommand:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a90f4
.globl __ZN16IOAESAccelerator18completeAESQuantumEP23IOAESAcceleratorCommand
.p2align 3
__ZN16IOAESAccelerator18completeAESQuantumEP23IOAESAcceleratorCommand:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a8ff8
.globl __ZN16IOAESAccelerator19performImmediateAESEP23IOAESAcceleratorCommand
.p2align 3
__ZN16IOAESAccelerator19performImmediateAESEP23IOAESAcceleratorCommand:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a9120
.globl __ZN16IOAESAccelerator18disableHardwareKeyEj
.p2align 3
__ZN16IOAESAccelerator18disableHardwareKeyEj:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a9130
.globl __ZN16IOAESAccelerator17createSpecialKeysEv
.p2align 3
__ZN16IOAESAccelerator17createSpecialKeysEv:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a95fc
.globl __ZN16IOAESAccelerator12_queueLockedEP23IOAESAcceleratorCommand
.p2align 3
__ZN16IOAESAccelerator12_queueLockedEP23IOAESAcceleratorCommand:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a915c
.globl __ZN16IOAESAccelerator13_startCommandEv
.p2align 3
__ZN16IOAESAccelerator13_startCommandEv:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a9314
.globl __ZN16IOAESAccelerator12_completeAESEP23IOAESAcceleratorCommand
.p2align 3
__ZN16IOAESAccelerator12_completeAESEP23IOAESAcceleratorCommand:
 bti c
 ldr x16, 1f
 br x16
1: .quad 0xfffffff00a0a93f0
