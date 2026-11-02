// SPDX-License-Identifier: BSD-2-Clause-Patent
// aks_wrap_key (selector 10) and aks_unwrap_key (11) for NativeKeyStoreEvents' user client, which refuses both
// (kIOReturnUnsupported): securityd wraps every keychain item's key with them, and without them each add failed
// ("ks_crypt: e00002c7 failed to 'oe' item"), so bluetoothd (its LE identity root), Wi-Fi passwords and every app stored
// nothing. libaks calls both with the scalars {keybag, class} and the key in and out as structures, and for 10 the class as
// the one scalar out. A wrapped key is an 8-byte header ('DVMK' and the class) and the key: like the rest of this keystore it
// protects nothing. The build inserts this into a staged copy of NativeKeyStoreEvents.cpp, before its anonymous namespace
// closes, and sends selectors 10 and 11 here (iPadOS 17: scripts/build-native-legacy-display.py; iPadOS 27:
// ipados27/scripts/build-droidvm-legacy-display.py).
IOReturn droidvmWrap(uint32_t selector, IOExternalMethodArguments *args) {
    const bool wrap = selector == 10;
    if (args->scalarInputCount != 2 || !args->scalarInput) return kIOReturnBadArgument;
    if (wrap ? (args->scalarOutputCount < 1 || !args->scalarOutput) : args->scalarOutputCount != 0) return kIOReturnBadArgument;
    uint8_t in[512], out[520];
    uint32_t length = 0, produced = 0;
    if (args->structureInputDescriptor) {
        length = (uint32_t)args->structureInputDescriptor->getLength();
        if (length > sizeof in || args->structureInputDescriptor->readBytes(0, in, length) != length) return kIOReturnBadArgument;
    } else {
        length = args->structureInputSize;
        if (length > sizeof in || (length && !args->structureInput)) return kIOReturnBadArgument;
        memcpy(in, args->structureInput, length);
    }
    const uint32_t keyclass = (uint32_t)args->scalarInput[1];
    if (wrap) {
        memcpy(out, "DVMK", 4); memcpy(out + 4, &keyclass, 4); memcpy(out + 8, in, length); produced = length + 8;
    } else {
        if (length < 8 || memcmp(in, "DVMK", 4)) return kIOReturnBadArgument;
        produced = length - 8; memcpy(out, in + 8, produced);
    }
    if (args->structureOutputDescriptor) {
        if (produced > args->structureOutputDescriptor->getLength()) return kIOReturnNoSpace;
        args->structureOutputDescriptor->writeBytes(0, out, produced);
        args->structureOutputDescriptorSize = produced;
    } else if (args->structureOutput && produced <= args->structureOutputSize) {
        memcpy(args->structureOutput, out, produced);
        args->structureOutputSize = produced;
    } else {
        return kIOReturnNoSpace;
    }
    if (wrap) {args->scalarOutput[0] = keyclass; args->scalarOutputCount = 1;}
    return kIOReturnSuccess;
}
