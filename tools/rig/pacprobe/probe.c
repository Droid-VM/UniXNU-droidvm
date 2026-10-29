// pacprobe's checks (see start.S). Freestanding, MMU off: aligned accesses only, no absolute pointers in data.
typedef unsigned long u64;
extern volatile u64 exc_rec[8];   // el1 esr, elr, far, count; el0 esr, elr, count
extern void run_el0(void (*fn)(volatile u64 *), volatile u64 *arg);
extern void el0_tests(volatile u64 *);

static void putc_(char c) { *(volatile unsigned char *)0x3f8 = (unsigned char)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void hex(u64 v) {
    for (int i = 60; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]);
}
static void kv(const char *k, u64 v) { puts_(k); puts_("="); hex(v); puts_(" "); }
static void nl(void) { puts_("\r\n"); }
static u64 fld(u64 v, int at) { return (v >> at) & 15; }

#define MRS(reg) ({ u64 _v; __asm__ volatile("mrs %0, " reg : "=r"(_v)); _v; })
#define MSR(reg, v) __asm__ volatile("msr " reg ", %0; isb" :: "r"((u64)(v)))

static u64 excs(void) { return exc_rec[3]; }

// One key register: write a pattern, read it back. Returns 1 ok, 2 trapped, 3 read back differs.
#define KEYTEST(name, reg, pat) do { \
    u64 before = excs(); MSR(reg, pat); u64 got = MRS(reg); \
    u64 r = excs() != before ? 2 : (got == (u64)(pat) ? 1 : 3); \
    kv(name, r); if (r == 2) { kv("esr", exc_rec[0]); } } while (0)

static volatile u64 el0res[8];

void probe_main(void) {
    for (int i = 0; i < 8; i++) { exc_rec[i] = 0; el0res[i] = 0; }
    puts_("\r\nPACPROBE begin\r\n");
    u64 midr = MRS("midr_el1"), mpidr = MRS("mpidr_el1");
    u64 isar1 = MRS("id_aa64isar1_el1"), isar2 = MRS("s3_0_c0_c6_2"), pfr0 = MRS("id_aa64pfr0_el1"), pfr1 = MRS("id_aa64pfr1_el1");
    u64 mmfr0 = MRS("id_aa64mmfr0_el1"), el = MRS("CurrentEL"), sctlr = MRS("sctlr_el1");
    kv("midr", midr); kv("mpidr", mpidr); kv("el", el >> 2); nl();
    kv("isar1", isar1); kv("isar2", isar2); kv("pfr0", pfr0); kv("pfr1", pfr1); kv("mmfr0", mmfr0); kv("sctlr", sctlr); nl();
    // ISAR1: APA [7:4] (QARMA5), API [11:8] (IMP DEF), GPA [27:24], GPI [31:28]; ISAR2: GPA3 [11:8], APA3 [15:12]
    // PFR1: BT [3:0], SME [27:24]; PFR0: SVE [35:32]; MMFR0: TGran4 [31:28] (0 = 4K supported)
    kv("APA", fld(isar1, 4)); kv("API", fld(isar1, 8)); kv("GPA", fld(isar1, 24)); kv("GPI", fld(isar1, 28));
    kv("APA3", fld(isar2, 12)); kv("GPA3", fld(isar2, 8)); kv("BT", fld(pfr1, 0)); kv("SME", fld(pfr1, 24));
    kv("SVE", fld(pfr0, 32)); kv("TGran4", fld(mmfr0, 28)); nl();

    KEYTEST("APIAKeyLo", "s3_0_c2_c1_0", 0x1111222233334444ul); KEYTEST("APIAKeyHi", "s3_0_c2_c1_1", 0x5555666677778888ul);
    KEYTEST("APIBKeyLo", "s3_0_c2_c1_2", 0x123456789abcdef0ul); KEYTEST("APIBKeyHi", "s3_0_c2_c1_3", 0x0fedcba987654321ul);
    KEYTEST("APDAKeyLo", "s3_0_c2_c2_0", 0x1357924680abcdeful); KEYTEST("APDAKeyHi", "s3_0_c2_c2_1", 0x2468135790fedcbaul);
    KEYTEST("APDBKeyLo", "s3_0_c2_c2_2", 0xa5a5a5a55a5a5a5aul); KEYTEST("APDBKeyHi", "s3_0_c2_c2_3", 0x3c3c3c3cc3c3c3c3ul);
    KEYTEST("APGAKeyLo", "s3_0_c2_c3_0", 0x0102030405060708ul); KEYTEST("APGAKeyHi", "s3_0_c2_c3_1", 0x1020304050607080ul);
    nl();

    // PACGA does not depend on SCTLR: it computes with the generic key whenever the feature is on.
    u64 before = excs(), g;
    __asm__ volatile("pacga %0, %1, %2" : "=r"(g) : "r"(0x401234ul), "r"(0x77ul));
    kv("pacga", g); kv("pacga_trap", excs() != before); nl();

    // Enable all four instruction/data keys (EnIA 31, EnIB 30, EnDA 27, EnDB 13), as the arm64e profile does.
    before = excs();
    MSR("sctlr_el1", sctlr | (1ul << 31) | (1ul << 30) | (1ul << 27) | (1ul << 13));
    kv("sctlr_set", MRS("sctlr_el1")); kv("sctlr_trap", excs() != before); nl();

    const u64 p = 0x401234ul, mod = 0x77ul;
    u64 ia = p, ib = p, da = p, db = p, bad;
    before = excs();
    __asm__ volatile("pacia %0, %1" : "+r"(ia) : "r"(mod));
    __asm__ volatile("pacib %0, %1" : "+r"(ib) : "r"(mod));
    __asm__ volatile("pacda %0, %1" : "+r"(da) : "r"(mod));
    __asm__ volatile("pacdb %0, %1" : "+r"(db) : "r"(mod));
    kv("pacia", ia); kv("pacib", ib); kv("pacda", da); kv("pacdb", db); kv("pac_traps", excs() - before); nl();
    u64 a1 = ia, a2 = ib, a3 = da, a4 = db;
    before = excs();
    __asm__ volatile("autia %0, %1" : "+r"(a1) : "r"(mod));
    __asm__ volatile("autib %0, %1" : "+r"(a2) : "r"(mod));
    __asm__ volatile("autda %0, %1" : "+r"(a3) : "r"(mod));
    __asm__ volatile("autdb %0, %1" : "+r"(a4) : "r"(mod));
    kv("aut_ok", (a1 == p) + 2 * (a2 == p) + 4 * (a3 == p) + 8 * (a4 == p)); kv("aut_traps", excs() - before); nl();
    // A wrong modifier must fail authentication: an error code in the pointer, or an exception with FEAT_FPAC.
    bad = ia; before = excs();
    __asm__ volatile("autia %0, %1" : "+r"(bad) : "r"(mod + 1));
    kv("aut_wrong", bad); kv("aut_wrong_exc", excs() - before); if (excs() != before) kv("esr", exc_rec[0]); nl();

    // Combined instructions (non-hint space: UNDEFINED without the feature): BLRAA, RETAA, LDRAA.
    u64 reached = 0, target;
    before = excs();
    __asm__ volatile(
        "adr %1, 1f\n pacia %1, %2\n blraa %1, %2\n b 2f\n"
        "1: mov %0, #1\n ret\n"
        "2:\n" : "+r"(reached), "=&r"(target) : "r"(mod) : "x30");
    kv("blraa", reached); kv("blraa_traps", excs() - before);
    u64 r2 = 0; before = excs();
    __asm__ volatile(
        "bl 1f\n b 2f\n"
        "1: paciasp\n mov %0, #1\n retaa\n"
        "2:\n" : "+r"(r2) :: "x30");
    kv("retaa", r2); kv("retaa_traps", excs() - before);
    static volatile u64 cell = 0xfeedfacecafebeeful;
    u64 addr = (u64)&cell, v = 0;
    __asm__ volatile("pacdza %0" : "+r"(addr));
    before = excs();
    __asm__ volatile("ldraa %0, [%1]" : "=r"(v) : "r"(addr));
    kv("ldraa", v == 0xfeedfacecafebeeful); kv("ldraa_traps", excs() - before); nl();

    // The same at EL0 (keys and SCTLR as set above).
    run_el0(el0_tests, el0res);
    kv("el0_pacia", el0res[0]); kv("el0_autia", el0res[1]); kv("el0_pacga", el0res[2]); kv("el0_blraa", el0res[3]);
    kv("el0_xpaci", el0res[4]); kv("el0_excs", exc_rec[6]); if (exc_rec[6]) kv("el0_esr", exc_rec[4]); nl();

    int el1_ok = fld(isar1, 4) | fld(isar1, 8) | fld(isar2, 12);
    int gen_ok = fld(isar1, 24) | fld(isar1, 28) | fld(isar2, 8);
    int works = el1_ok && gen_ok && ia != p && a1 == p && reached && r2 && v == 0xfeedfacecafebeeful && el0res[0] != p &&
                el0res[1] == p && el0res[3] == 1 && exc_rec[6] == 0;
    puts_(works ? "PACPROBE VERDICT usable\r\n" : "PACPROBE VERDICT NOT-usable\r\n");
    kv("el1_exceptions_total", excs()); nl();
    puts_("PACPROBE end\r\n");
}
