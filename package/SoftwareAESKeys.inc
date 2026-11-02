// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The hardware keys a DroidVM guest is asked for, in software, for SoftwareAES's NativeAES (DroidVMSoftwareAES.cpp): the build
// inserts this into a staged copy of NativeAES.cpp after its includes, has isKeySupported answer these selectors and has performAES
// take the derived key in place of the selector. A guest has no UID or GID: each key here is SHA-256 of a per-device seed (of the
// device tree's /chosen/unique-chip-id) and the selector, so encryption and decryption agree and every guest has its own; like the
// rest of this keystore it protects nothing. Other selectors stay unsupported (the known-answer test asks 2001 to be) and are logged.
//   0x8a4  AppleImage4's nonce key (its kern sysctl handlers encrypt with it; "panic: encryption failed" on kIOReturnUnsupported)
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/crypto/sha2.h>

static bool droidvmHardwareKey(unsigned selector) { return selector == 0x8a4; }

static bool droidvmDeriveKey(const IOAESKeyData &in, IOAESKeyData &out) {
  static uint8_t seed[32];
  static bool seeded;
  if (!seeded) {
    uint8_t chip[8] = {};
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
      OSData *id = OSDynamicCast(OSData, chosen->getProperty("unique-chip-id"));
      if (id && id->getLength() == sizeof chip) memcpy(chip, id->getBytesNoCopy(), sizeof chip);
      chosen->release();
    }
    static const char label[] = "DroidVM hardware key seed";
    SHA256_CTX context;
    SHA256_Init(&context);
    SHA256_Update(&context, label, sizeof label - 1);
    SHA256_Update(&context, chip, sizeof chip);
    SHA256_Final(seed, &context);
    seeded = true;
  }
  if (in.bits != 128 && in.bits != 192 && in.bits != 256) return false;
  SHA256_CTX context;
  SHA256_Init(&context);
  SHA256_Update(&context, seed, sizeof seed);
  SHA256_Update(&context, &in.selector, sizeof in.selector);
  out.bits = in.bits;
  SHA256_Final(out.bytes, &context);
  out.selector = 0;
  return true;
}

// The key performAES uses: the caller's software key, the derived one for a provided hardware key, else the caller's (refused).
static IOAESKeyData *droidvmKeyFor(IOAESKeyData *key, IOAESKeyData &derived) {
  if (!key->selector) return key;
  if (droidvmHardwareKey(key->selector) && droidvmDeriveKey(*key, derived)) return &derived;
  static unsigned said;
  if (said++ < 8) IOLog("NativeAES: hardware key %#x (%u bits) not provided\n", key->selector, key->bits);
  return key;
}
