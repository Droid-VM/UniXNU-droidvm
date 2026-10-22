// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest's virtio-net device as an IOKit ethernet controller: an IOEthernetController from Apple's IONetworkingFamily-177,
// which scripts/build-native-legacy-display.py compiles into the kernel. Its IOEthernetInterface is what IONetworkStack and
// configd's InterfaceNamer name en0, so iOS's own network configuration (IPConfiguration's DHCP, routes, DNS) manages it.
// The virtqueues stay in the transport half (DroidVMVirtioNet.c); this class only adapts them:
//   outputPacket -> droidvm_net_tx_frame          transport receive poll -> droidvm_ethernet_input -> inputPacket
// Like the other statically linked IOKit classes here, the object's static initializer is staged for a late call, made by
// DroidVMVirtioNetIf.cpp together with the family's own.
#include <IOKit/IOLocks.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>
#include <net/ethernet.h>

extern "C" {
int droidvm_net_tx_frame(const void *frame, unsigned length);
IOService *droidvm_ethernet_create(const uint8_t mac[6], IOService *provider);
void droidvm_ethernet_input(IOService *controller, const void *frame, unsigned length);
// For the host rig (peek.py): the stack's calls into the controller, and the interface as BSD sees it (unit + 1 once IONetworkStack
// has attached an ifnet, its flags, and whether the interface came out IOBuiltin). The guest's addresses need no probe: the
// app's br-wifi proxy shows them on the phone's wlan0.
volatile uint32_t droidvm_ethernet_enables, droidvm_ethernet_disables, droidvm_ethernet_outputs, droidvm_ethernet_inputs;
volatile uint32_t droidvm_ethernet_unit, droidvm_ethernet_ifflags, droidvm_ethernet_builtin;
}

#define DROIDVM_NET_FRAME 1514
#define DROIDVM_NET_SPEED (1000ULL * 1000 * 1000)

class DroidVMVirtioEthernet : public IOEthernetController {
    OSDeclareDefaultStructors(DroidVMVirtioEthernet)
    typedef IOEthernetController super;

    IOEthernetAddress address;
    IOEthernetInterface *interface;
    IONetworkMedium *medium;
    IOLock *transmitLock;
    bool enabled;
    uint8_t transmitFrame[DROIDVM_NET_FRAME];

public:
    bool initWithAddress(const uint8_t mac[6]);
    void free() override;
    bool start(IOService *provider) override;
    IOReturn getHardwareAddress(IOEthernetAddress *addressOut) override;
    IOReturn enable(IONetworkInterface *client) override;
    IOReturn disable(IONetworkInterface *client) override;
    UInt32 outputPacket(mbuf_t packet, void *parameter) override;
    // The device has no filter to program: the transport keeps broadcast and our own unicast (see net_rx_poll).
    IOReturn setPromiscuousMode(bool active) override { (void)active; return kIOReturnSuccess; }
    IOReturn setMulticastMode(bool active) override { (void)active; return kIOReturnSuccess; }
    IOReturn setMulticastList(IOEthernetAddress *addresses, UInt32 count) override { (void)addresses; (void)count; return kIOReturnSuccess; }
    void input(const void *frame, unsigned length);
};

OSDefineMetaClassAndStructors(DroidVMVirtioEthernet, IOEthernetController)

bool DroidVMVirtioEthernet::initWithAddress(const uint8_t mac[6]) {
    if (!super::init(nullptr)) return false;
    memcpy(address.bytes, mac, kIOEthernetAddressSize);
    transmitLock = IOLockAlloc();
    return transmitLock != nullptr;
}

void DroidVMVirtioEthernet::free() {
    OSSafeReleaseNULL(interface);
    OSSafeReleaseNULL(medium);
    if (transmitLock) IOLockFree(transmitLock);
    transmitLock = nullptr;
    super::free();
}

// One fixed medium (auto, gigabit), published and selected before the interface is attached, so the link can be reported up.
bool DroidVMVirtioEthernet::start(IOService *provider) {
    if (!super::start(provider)) return false;
    medium = IONetworkMedium::medium(kIOMediumEthernetAuto, DROIDVM_NET_SPEED);
    OSDictionary *media = OSDictionary::withCapacity(1);
    bool published = medium && media && IONetworkMedium::addMedium(media, medium) && publishMediumDictionary(media);
    OSSafeReleaseNULL(media);
    if (!published || !setCurrentMedium(medium)) return false;
    if (!attachInterface((IONetworkInterface **)&interface, true)) return false;
    droidvm_ethernet_builtin = interface->getProperty(kIOBuiltin) == kOSBooleanTrue;
    IOLog("DroidVMVirtioEthernet: interface attached, IOBuiltin %u\n", droidvm_ethernet_builtin);
    return true;
}

IOReturn DroidVMVirtioEthernet::getHardwareAddress(IOEthernetAddress *addressOut) {
    *addressOut = address;
    return kIOReturnSuccess;
}

IOReturn DroidVMVirtioEthernet::enable(IONetworkInterface *client) {
    (void)client;
    enabled = true;
    droidvm_ethernet_enables = droidvm_ethernet_enables + 1;
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, medium, DROIDVM_NET_SPEED);
    IOLog("DroidVMVirtioEthernet: enabled, link up\n");
    return kIOReturnSuccess;
}

IOReturn DroidVMVirtioEthernet::disable(IONetworkInterface *client) {
    (void)client;
    enabled = false;
    droidvm_ethernet_disables = droidvm_ethernet_disables + 1;
    setLinkStatus(kIONetworkLinkValid, nullptr);
    IOLog("DroidVMVirtioEthernet: disabled\n");
    return kIOReturnSuccess;
}

// No output queue: the interface calls in from the stack's threads, so the shared flat buffer is taken under a lock.
UInt32 DroidVMVirtioEthernet::outputPacket(mbuf_t packet, void *parameter) {
    (void)parameter;
    droidvm_ethernet_outputs = droidvm_ethernet_outputs + 1;
    size_t length = mbuf_pkthdr_len(packet);
    if (enabled && length > 0 && length <= sizeof(transmitFrame)) {
        IOLockLock(transmitLock);
        if (mbuf_copydata(packet, 0, length, transmitFrame) == 0) droidvm_net_tx_frame(transmitFrame, (unsigned)length);
        IOLockUnlock(transmitLock);
    }
    freePacket(packet);
    return kIOReturnOutputSuccess;
}

// Called by the transport's single receive thread, so inputPacket calls are serialized as the family requires. Each frame
// also refreshes the rig's view of the BSD side.
void DroidVMVirtioEthernet::input(const void *frame, unsigned length) {
    if (!interface) return;
    if (ifnet_t ifp = interface->getIfnet()) {
        droidvm_ethernet_unit = ifnet_unit(ifp) + 1;
        droidvm_ethernet_ifflags = (uint32_t)ifnet_flags(ifp);
    }
    if (!enabled || length < ETHER_HDR_LEN || length > DROIDVM_NET_FRAME) return;
    droidvm_ethernet_inputs = droidvm_ethernet_inputs + 1;
    mbuf_t packet = allocatePacket(length);
    if (!packet) return;
    if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
        freePacket(packet);
        return;
    }
    interface->inputPacket(packet, length);
}

IOService *droidvm_ethernet_create(const uint8_t mac[6], IOService *provider) {
    DroidVMVirtioEthernet *controller = OSTypeAlloc(DroidVMVirtioEthernet);
    if (!controller || !controller->initWithAddress(mac)) {
        OSSafeReleaseNULL(controller);
        return nullptr;
    }
    if (!controller->attach(provider)) {
        controller->release();
        return nullptr;
    }
    if (!controller->start(provider)) {
        controller->detach(provider);
        controller->release();
        return nullptr;
    }
    return controller;
}

void droidvm_ethernet_input(IOService *controller, const void *frame, unsigned length) {
    static_cast<DroidVMVirtioEthernet *>(controller)->input(frame, length);
}
