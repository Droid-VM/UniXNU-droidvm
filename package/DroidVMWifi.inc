// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest's Wi-Fi interface, beside the virtio-net Ethernet one (the controller above it in DroidVMVirtioEthernet.cpp), as an iPad
// with an Ethernet adapter has both: an IO80211Controller with an IO80211Interface, which configd names and gives a network
// service as IEEE 802.11 (it tells one by its controller's class, IOObjectConformsTo "IO80211Controller"), and which wifid drives
// through the Apple80211 requests of IO80211.framework (Apple80211Get/Set: the SIOCGA80211/SIOCSA80211 ioctls on the interface,
// which IONetworkInterface hands to performCommand) and the IEEE 802.11 kernel events it watches. IO80211.framework lists an
// interface as Wi-Fi by its SIOCGIFMEDIA type (IFM_IEEE80211), which the controller's one medium gives. The request layouts and
// the answers follow UniXNU-sheng's package/WifiNet.cpp (BSD-2-Clause-Patent), whose Ath12k radio sits behind its module's
// station interface (nw_abi.h); the station below has the same shape, so a radio of the host's can take its place.
//
// The station has no radio: a scan finds no network, so Wi-Fi is on and joined to nothing, and the guest's traffic goes over
// Ethernet. droidvm-wifi-ssid=NAME gives it one open network of that name to join, for the user interface only: frames sent
// while joined are dropped (this controller has no transport).
#include <sys/kern_event.h>
#include <net/if_media.h>

// sys/kern_event.h declares kev_msg_post without C linkage; the symbol is the C one
extern "C" errno_t droidvm_kev_msg_post(struct kev_msg *event_msg) __asm__("_kev_msg_post");

namespace droidvm_wifi {

#define SIOCSA80211 0x802869c8UL   // _IOW('i', 200, struct apple80211req)
#define SIOCGA80211 0xc02869c9UL   // _IOWR('i', 201, struct apple80211req)

struct apple80211req {
    char req_if_name[16];
    int32_t req_type;
    int32_t req_val;
    uint32_t req_len;
    user_addr_t req_data __attribute__((aligned(8)));
};

// request types
enum {
    kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10,
    kScanResult = 11, kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20,
    kAssociateResult = 21, kDisassociate = 22, kHostApMode = 25, kSupportedChannels = 27, kLocale = 28, kDeauth = 29, kDriverVersion = 43,
    kHardwareVersion = 44, kCountryCode = 51, kRadioInfo = 54, kScanReqMultiple = 86, kScanCacheClear = 90, kCurrentNetwork = 103,
    kChannelsInfo = 207, kHwSupportedChannels = 254,
};
// events: class IEEE 802.11 (6), subclass 1, the data starting with the interface name
enum { kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4, kEvAssocDone = 9, kEvScanDone = 10 };
// results of an association, as the framework numbers them
enum { kAssocOk = 1, kAssocFailed = 2 };
// the station's states (nw_abi.h's NW_STA_*)
enum { kStaOff, kStaIdle, kStaScanning, kStaAuth, kStaAssoc, kStaKeys, kStaUp };

struct channel { uint32_t version, channel, flags; };
struct channels_list { uint32_t version, count; channel list[401]; uint32_t unused; };
struct scan_result {
    uint32_t version; channel ch; uint16_t noise_known; int16_t noise, snr, rssi; uint16_t beacon_interval, capability; uint8_t bssid[6];
    uint8_t rate_count, unused0; uint32_t rates[15]; uint8_t ssid_length, ssid[32], unused1[3]; uint32_t age_ms; uint8_t flags, unused2;
    uint16_t elements_length; uint8_t elements[1024];
};
struct assoc_request {
    uint32_t version; uint16_t mode, auth_lower, auth_upper, unused0; uint32_t ssid_length; uint8_t ssid[32]; uint8_t bssid[6], unused1[2];
    uint32_t key_version, key_length, key_cipher; uint16_t key_flags, key_index; uint8_t key[64]; uint8_t rest[0x250 - 0x88];
};
struct link_event {
    uint8_t down, unused0[3]; int32_t rssi; uint16_t snr, noise; uint8_t unused1[4]; uint8_t involuntary, unused2[3]; uint32_t reason, subreason;
    uint8_t unused3[4];
};
static_assert(sizeof(apple80211req) == 0x28, "request layout");
static_assert(sizeof(scan_result) == 1164 && __builtin_offsetof(scan_result, ssid_length) == 0x60 &&
              __builtin_offsetof(scan_result, age_ms) == 0x84 && __builtin_offsetof(scan_result, elements) == 0x8c, "scan result layout");
static_assert(sizeof(assoc_request) == 592 && __builtin_offsetof(assoc_request, key) == 0x48, "association layout");
static_assert(sizeof(channels_list) == 4824, "channel list layout");
static_assert(sizeof(link_event) == 32, "link event layout");

// ---- the station ----
// The network droidvm-wifi-ssid names, if any: what a beacon of it says (an SSID element and the rates), on 5 GHz channel 36.
struct Bss { uint8_t bssid[6]; uint8_t ssid_len; uint8_t ssid[32]; uint32_t freq_mhz; int8_t signal_dbm; uint16_t caps, beacon_int;
             uint16_t ie_len; uint8_t ie[64]; };
static Bss gBss;
static unsigned gNetworks;   // 0 or 1
static uint32_t gState = kStaIdle;
static bool gPower = true;
static int gAssocResult = kAssocFailed;
static unsigned gScanCursor;
static ifnet_t gIf;
static thread_call_t gScanCall;
static void (*gLink)(bool up);   // the controller's link status
static uint32_t gLogged[256 / 32];

extern "C" {
// For the host rig (peek.py): requests answered and refused, the last refused type, joins and leaves, the interface's commands.
volatile uint32_t droidvm_wifi_requests, droidvm_wifi_refused, droidvm_wifi_last_refused, droidvm_wifi_joins, droidvm_wifi_leaves;
volatile uint32_t droidvm_wifi_commands;
}

static void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log(const char *fmt, ...) {
    char line[200]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    IOLog("droidvm-wifi: %s\n", line);
}

static void station_init(const uint8_t mac[6]) {
    char ssid[33] = {};
    memset(&gBss, 0, sizeof gBss);
    if (!PE_parse_boot_arg_str("droidvm-wifi-ssid", ssid, sizeof ssid) || !ssid[0]) return;
    gNetworks = 1;
    memcpy(gBss.bssid, mac, 6); gBss.bssid[0] = (uint8_t)((mac[0] | 0x02) ^ 0x04);   // a locally administered address of its own
    gBss.ssid_len = (uint8_t)strnlen(ssid, 32); memcpy(gBss.ssid, ssid, gBss.ssid_len);
    gBss.freq_mhz = 5180; gBss.signal_dbm = -42; gBss.caps = 0x0001; gBss.beacon_int = 100;   // ESS, no privacy: an open network
    static const uint8_t rates[] = { 1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };   // 6 (basic), 9, 12, 18, 24, 36, 48, 54 Mb/s
    gBss.ie[0] = 0; gBss.ie[1] = gBss.ssid_len; memcpy(gBss.ie + 2, gBss.ssid, gBss.ssid_len);
    memcpy(gBss.ie + 2 + gBss.ssid_len, rates, sizeof rates);
    gBss.ie_len = (uint16_t)(2 + gBss.ssid_len + sizeof rates);
}

static void post_event(uint32_t code, const void *payload, size_t length) {
    uint8_t data[128]; if (length > sizeof data - 16) length = sizeof data - 16;
    memset(data, 0, 16);
    if (gIf) snprintf(reinterpret_cast<char *>(data), 16, "%s%u", ifnet_name(gIf), ifnet_unit(gIf));
    if (length) memcpy(data + 16, payload, length);
    struct kev_msg msg; memset(&msg, 0, sizeof msg);
    msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = 6; msg.kev_subclass = 1; msg.event_code = code;
    msg.dv[0].data_length = static_cast<uint32_t>(16 + length); msg.dv[0].data_ptr = data;
    droidvm_kev_msg_post(&msg);
}

static void link_changed(bool up) {
    if (gLink) gLink(up);
    link_event ev; memset(&ev, 0, sizeof ev);
    ev.down = up ? 0 : 1; ev.rssi = gBss.signal_dbm; ev.noise = static_cast<uint16_t>(-95); ev.snr = static_cast<uint16_t>(gBss.signal_dbm + 95);
    post_event(kEvLinkChanged, &ev, sizeof ev);
}

// A join of the one network, done at once with the events a driver sends as its association completes.
static bool join(const uint8_t *ssid, uint32_t length) {
    if (!gPower || !gNetworks || length != gBss.ssid_len || memcmp(ssid, gBss.ssid, length)) {
        gAssocResult = kAssocFailed; post_event(kEvAssocDone, nullptr, 0); return false;
    }
    gState = kStaUp; gAssocResult = kAssocOk; droidvm_wifi_joins = droidvm_wifi_joins + 1;
    post_event(kEvAssocDone, nullptr, 0); post_event(kEvSsidChanged, nullptr, 0); post_event(kEvBssidChanged, nullptr, 0);
    link_changed(true);
    return true;
}
static void leave() {
    if (gState != kStaUp) return;
    gState = kStaIdle; droidvm_wifi_leaves = droidvm_wifi_leaves + 1;
    post_event(kEvSsidChanged, nullptr, 0); post_event(kEvBssidChanged, nullptr, 0);
    link_changed(false);
}
// A scan ends after the time a short one takes: answered at once, the daemon asks again straight away.
static void scan_done(thread_call_param_t, thread_call_param_t) { gScanCursor = 0; if (gState == kStaScanning) gState = kStaIdle; post_event(kEvScanDone, nullptr, 0); }
static void scan() {
    if (!gScanCall) gScanCall = thread_call_allocate(scan_done, nullptr);
    if (gState == kStaIdle) gState = kStaScanning;
    gScanCursor = 0;
    uint64_t at; clock_interval_to_deadline(300, kMillisecondScale, &at); thread_call_enter_delayed(gScanCall, at);
}

// ---- Apple80211 ----
static void fill_channel(channel &c, uint32_t mhz) {
    c.version = 1;
    if (mhz >= 4900) { c.channel = (mhz - 5000) / 5; c.flags = 0x2 | 0x10; } else { c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5; c.flags = 0x2 | 0x8; }
}
static void fill_result(scan_result &r) {
    memset(&r, 0, sizeof r); r.version = 1; fill_channel(r.ch, gBss.freq_mhz); r.noise_known = 1; r.noise = -95; r.rssi = gBss.signal_dbm;
    r.snr = static_cast<int16_t>(gBss.signal_dbm + 95); r.beacon_interval = gBss.beacon_int; r.capability = gBss.caps; memcpy(r.bssid, gBss.bssid, 6);
    r.ssid_length = gBss.ssid_len; memcpy(r.ssid, gBss.ssid, 32); r.elements_length = gBss.ie_len; memcpy(r.elements, gBss.ie, gBss.ie_len);
}
static int copy_out(const apple80211req &rq, const void *src, size_t size) {
    if (rq.req_len == 0 || !rq.req_data) return EINVAL;
    return copyout(src, rq.req_data, rq.req_len < size ? rq.req_len : size);
}
// A get that gives no buffer takes a single value in req_val; one with a buffer gets {version, value}.
static int answer_value(apple80211req &rq, uint32_t value) {
    if (rq.req_len == 0 || !rq.req_data) { rq.req_val = static_cast<int32_t>(value); return 0; }
    const uint32_t w[2] = { 1, value }; return copyout(w, rq.req_data, rq.req_len < sizeof w ? rq.req_len : sizeof w);
}
// The raw bytes, their length in req_len.
static int answer_bytes(apple80211req &rq, const void *src, uint32_t size) {
    if (!rq.req_data) return EINVAL;
    const uint32_t n = rq.req_len < size ? rq.req_len : size; rq.req_len = size; return copyout(src, rq.req_data, n);
}
static int copy_in(const apple80211req &rq, void *dst, size_t size) {
    if (!rq.req_data) return EINVAL;
    memset(dst, 0, size);
    return copyin(rq.req_data, dst, rq.req_len < size ? rq.req_len : size);
}
static int map_state(uint32_t s) {
    switch (s) { case kStaScanning: return 1; case kStaAuth: return 2; case kStaAssoc: case kStaKeys: return 3; case kStaUp: return 4; default: return 0; }
}

static int answer(bool set, apple80211req &rq) {
    const int type = rq.req_type;
    union Out { uint8_t raw[5120]; channels_list chans; scan_result res; };   // not on the stack: the largest answer is 4824 bytes
    Out *outp = static_cast<Out *>(IOMallocData(sizeof(Out)));
    if (!outp) return ENOMEM;
    memset(outp, 0, sizeof(Out));
    struct Free { Out *p; ~Free() { IOFreeData(p, sizeof(Out)); } } freeOut{outp};
    Out &out = *outp;
    uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);
    if (set) {
        switch (type) {
        case kPower: {
            uint32_t in[6]; if (copy_in(rq, in, sizeof in)) return EFAULT;
            gPower = in[2] != 0; log("power %u", in[2]);
            if (!gPower) { leave(); gState = kStaOff; } else if (gState == kStaOff) gState = kStaIdle;
            post_event(kEvPowerChanged, nullptr, 0); return 0; }
        case kScanReq: case kScanReqMultiple: scan(); return 0;
        case kAssociate: {
            assoc_request a; if (copy_in(rq, &a, sizeof a)) return EFAULT;
            if (a.ssid_length > 32) return EINVAL;
            log("join: ssid length %u, key length %u cipher %u", a.ssid_length, a.key_length, a.key_cipher);
            const bool joined = join(a.ssid, a.ssid_length);
            memset(&a, 0, sizeof a);
            return joined ? 0 : EINVAL; }
        case kDisassociate: case kDeauth: log("leave"); leave(); return 0;
        // host access point mode: none (wifid sets it to 2 as it starts; accepted, it would offer a personal hotspot)
        case kHostApMode: return EOPNOTSUPP;
        // Every other set is accepted and has no effect: wifid tells the driver its state and policies that way (the device's
        // orientation, the external power state, the app state, coexistence profiles, the heartbeat, which events to report:
        // APPLE80211_IOC_FILTER_MONITOR_EVENTS refused, it gave up scheduling the device and no client got one), none of which a
        // radio without hardware acts on.
        default: return 0;
        }
    }
    switch (type) {
    case kPower: w[0] = 1; w[1] = 1; w[2] = gPower ? 1 : 0; return copy_out(rq, w, 24);
    case kState: return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(gState) : 0));
    case kOpMode: return answer_value(rq, 1);   // station
    case kAuthType: w[0] = 1; w[1] = 1; w[2] = 0; return copy_out(rq, w, 12);   // {version, lower: open system, upper: none}
    case kPowerSave: return answer_value(rq, 0);
    case kCardCaps: {   // a bitmap, bit n in byte n / 8: AES, AES-CCM, power management, short slot and preamble, WPA2, WME, short guard interval
        static const uint8_t has[] = { 2, 3, 6, 9, 10, 14, 17, 19 };
        for (uint8_t b : has) out.raw[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
        return answer_bytes(rq, out.raw, 12); }
    case kPhyMode: w[0] = 1; w[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40; w[2] = gState == kStaUp ? 0x8 : 0; return copy_out(rq, w, 12);
    case kSsid: if (gState < kStaAssoc) return ENOENT; return answer_bytes(rq, gBss.ssid, gBss.ssid_len);
    case kBssid: if (gState < kStaAssoc) return ENOENT; return answer_bytes(rq, gBss.bssid, 6);
    case kChannel: if (gState < kStaAuth) return ENOENT; w[0] = 1; fill_channel(*reinterpret_cast<channel *>(out.raw + 4), gBss.freq_mhz); return copy_out(rq, w, 16);
    case kRssi: case kNoise: {   // {version, radios, unit (0: dBm), per chain[4], aggregate, extension per chain[4], its aggregate}
        if (type == kRssi && gState != kStaUp) return ENOENT;
        const uint32_t v = static_cast<uint32_t>(type == kRssi ? static_cast<int32_t>(gBss.signal_dbm) : -95);
        w[0] = 1; w[1] = 1; w[2] = 0; for (int i = 3; i < 13; i++) w[i] = v; return copy_out(rq, w, 13 * 4); }
    case kRate: return answer_value(rq, gState == kStaUp ? 866 : 0);
    case kTxPower: return answer_value(rq, 15);
    case kDriverVersion: case kHardwareVersion: {
        strlcpy(reinterpret_cast<char *>(out.raw), type == kDriverVersion ? "droidvm-wifi 1" : "virtual", 256); return copy_out(rq, out.raw, 256); }
    case kCountryCode: return answer_bytes(rq, "XX", 3);   // the two letters and a NUL (the daemon asks with a 3-byte buffer)
    case kAssociateResult: return answer_value(rq, static_cast<uint32_t>(gAssocResult));
    case kSupportedChannels: case kHwSupportedChannels: {
        out.chans.version = 1; unsigned n = 0;
        static const uint16_t known[] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825 };
        for (uint16_t f : known) { fill_channel(out.chans.list[n], f); out.chans.list[n].flags |= 0x80; n++; }
        out.chans.count = n; return copy_out(rq, &out.chans, sizeof out.chans); }
    case kScanResult:
        if (gScanCursor >= gNetworks) { gScanCursor = 0; return EIO; }   // after the last record
        gScanCursor++; fill_result(out.res); return copy_out(rq, &out.res, sizeof out.res);
    case kCurrentNetwork: if (gState < kStaAssoc) return ENOENT; fill_result(out.res); return copy_out(rq, &out.res, sizeof out.res);
    default: return EOPNOTSUPP;
    }
}

// Each request with its answer for the first few hundred, then a type's first one only.
static int request(bool set, apple80211req &rq) {
    const uint32_t length = rq.req_len; const int32_t value = rq.req_val, type = rq.req_type;
    const int error = answer(set, rq);
    droidvm_wifi_requests = droidvm_wifi_requests + 1;
    if (error == EOPNOTSUPP) { droidvm_wifi_refused = droidvm_wifi_refused + 1; droidvm_wifi_last_refused = static_cast<uint32_t>(type); }
    static unsigned shown;
    const bool first = type >= 0 && type < 256 && !(gLogged[type / 32] & (1u << (type % 32)));
    if (type >= 0 && type < 256) gLogged[type / 32] |= 1u << (type % 32);
    if (shown < 300 || first) {
        ++shown;
        log("request %u: type %d %s len %u val %d -> %d (len %u val %d)", shown, type, set ? "set" : "get", length, value, error, rq.req_len, rq.req_val);
    }
    return error;
}

}  // namespace droidvm_wifi

// The classes configd and wifid know a Wi-Fi interface by; Apple's IO80211Family, which defines both, is not in this kernel.
class IO80211Interface : public IOEthernetInterface {
    OSDeclareDefaultStructors(IO80211Interface)
    typedef IOEthernetInterface super;
public:
    // wifid finds the interface an if_attached event names by IOPropertyMatch {IOInterfaceName: "en1"} (its
    // __WiFiManagerDeviceAddedCallback, "No Matching Service Found" otherwise), a property IONetworkInterface does not publish: set
    // as the interface is attached to DLIL, whose name and unit IONetworkStack has given it by then.
    bool initIfnetParams(struct ifnet_init_params *params) override {
        if (!super::initIfnetParams(params)) return false;
        char name[16];   // IFNAMSIZ
        snprintf(name, sizeof name, "%s%u", params->name, params->unit);
        setProperty("IOInterfaceName", name);
        return true;
    }
    SInt32 performCommand(IONetworkController *controller, unsigned long command, void *argument0, void *argument1) override {
        if (!droidvm_wifi::gIf) droidvm_wifi::gIf = getIfnet();
        SInt32 result;
        if (command == SIOCGA80211 || command == SIOCSA80211) {
            if (!argument1) return EINVAL;
            result = droidvm_wifi::request(command == SIOCSA80211, *static_cast<droidvm_wifi::apple80211req *>(argument1));
        } else {
            result = super::performCommand(controller, command, argument0, argument1);
            // the other commands with their results, the first ones: which the frameworks send to a Wi-Fi interface
            if (droidvm_wifi::droidvm_wifi_commands < 64) droidvm_wifi::log("command %#lx -> %d", command, (int)result);
        }
        droidvm_wifi::droidvm_wifi_commands = droidvm_wifi::droidvm_wifi_commands + 1;
        return result;
    }
};
OSDefineMetaClassAndStructors(IO80211Interface, IOEthernetInterface)

class IO80211Controller : public IOEthernetController {
    OSDeclareAbstractStructors(IO80211Controller)
    typedef IOEthernetController super;
public:
    // IO80211.framework's interface list (Apple80211GetIfListCopy, which wifid and every Wi-Fi client start from) takes an interface
    // whose SIOCGIFTYPE, which the kernel answers from the ifnet, is IFT_ETHER of the Wi-Fi subfamily: set before DLIL attaches it.
    IONetworkInterface *createInterface() override {
        IO80211Interface *interface = OSTypeAlloc(IO80211Interface);
        if (interface && !interface->init(this)) OSSafeReleaseNULL(interface);
        if (interface) {
            interface->setInterfaceSubType(IFNET_SUBFAMILY_WIFI);
            interface->setProperty("IO80211InterfaceRole", "Infrastructure");
        }
        return interface;
    }
};
OSDefineMetaClassAndAbstractStructors(IO80211Controller, IOEthernetController)

// The Wi-Fi controller: one IEEE 802.11 medium, the link active only while the station is joined, and no transport.
class DroidVMVirtualWifi : public IO80211Controller {
    OSDeclareDefaultStructors(DroidVMVirtualWifi)
    typedef IO80211Controller super;

    IOEthernetAddress address;
    IONetworkInterface *interface;
    IONetworkMedium *medium;

    static DroidVMVirtualWifi *gSelf;
    static void link(bool up) {
        if (gSelf) gSelf->setLinkStatus(up ? kIONetworkLinkValid | kIONetworkLinkActive : kIONetworkLinkValid, up ? gSelf->medium : nullptr,
                                        up ? DROIDVM_WIFI_SPEED : 0);
    }

public:
    bool initWithAddress(const uint8_t mac[6]) {
        if (!super::init(nullptr)) return false;
        memcpy(address.bytes, mac, kIOEthernetAddressSize);
        droidvm_wifi::station_init(mac);
        return true;
    }
    void free() override {
        OSSafeReleaseNULL(interface);
        OSSafeReleaseNULL(medium);
        super::free();
    }
    bool start(IOService *provider) override {
        if (!super::start(provider)) return false;
        medium = IONetworkMedium::medium(kIOMediumIEEE80211Auto, DROIDVM_WIFI_SPEED);
        OSDictionary *media = OSDictionary::withCapacity(1);
        bool published = medium && media && IONetworkMedium::addMedium(media, medium) && publishMediumDictionary(media);
        OSSafeReleaseNULL(media);
        if (!published || !setCurrentMedium(medium)) return false;
        if (!attachInterface(&interface, true)) return false;
        gSelf = this;
        droidvm_wifi::gLink = link;
        IOLog("DroidVMVirtualWifi: interface attached, %u network(s)\n", droidvm_wifi::gNetworks);
        return true;
    }
    IOReturn getHardwareAddress(IOEthernetAddress *addressOut) override { *addressOut = address; return kIOReturnSuccess; }
    IOReturn enable(IONetworkInterface *client) override {
        (void)client;
        droidvm_wifi::gIf = interface->getIfnet();
        link(droidvm_wifi::gState == droidvm_wifi::kStaUp);
        IOLog("DroidVMVirtualWifi: enabled\n");
        return kIOReturnSuccess;
    }
    IOReturn disable(IONetworkInterface *client) override { (void)client; setLinkStatus(kIONetworkLinkValid, nullptr); return kIOReturnSuccess; }
    UInt32 outputPacket(mbuf_t packet, void *parameter) override { (void)parameter; freePacket(packet); return kIOReturnOutputDropped; }
    IOReturn setPromiscuousMode(bool active) override { (void)active; return kIOReturnSuccess; }
    IOReturn setMulticastMode(bool active) override { (void)active; return kIOReturnSuccess; }
    IOReturn setMulticastList(IOEthernetAddress *addresses, UInt32 count) override { (void)addresses; (void)count; return kIOReturnSuccess; }
};
OSDefineMetaClassAndStructors(DroidVMVirtualWifi, IO80211Controller)
DroidVMVirtualWifi *DroidVMVirtualWifi::gSelf;

IOService *droidvm_wifi_create(const uint8_t mac[6], IOService *provider) {
    DroidVMVirtualWifi *controller = OSTypeAlloc(DroidVMVirtualWifi);
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
