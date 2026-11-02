# VideoToolbox paravirtualization oracle (macOS)

What a real host answers to VideoToolbox's paravirtualized codecs, recorded byte for byte on a Mac (macOS 27, the same
VideoToolbox generation as iPadOS 27): the protocol `package/DroidVMVTHost.cpp` plays in the guest's kernel.

- `interpose.dylib` (`interpose.c`) replaces VideoToolboxParavirtualizationSupport.framework's guest transport, in the process
  it is loaded into, with a loopback to VideoToolbox's own host session (`VTParavirtualizationHostSessionCreate`,
  `...DeliverMessageFromGuest`): the process is its own guest and host, and every message of both directions is written to
  `$VTPV_LOG/NNNN-{g2h,h2g}-TYPE[-sID].bin`. The decoder and encoder lists (`decs`, `encs`) are asked inside VideoToolbox's
  registry initialization, which the in-process host needs too: they are answered from replies `hostonly` recorded
  (`$VTPV_CANNED/decs.bin`, `encs.bin`). It also grants `com.apple.videotoolbox.encode-in-process` (AMFI keeps the real
  entitlement from an ad-hoc signature), so encoders run in the process.
- `oracle` (`oracle.c`) decodes `tiny-h264.inc`'s IDR (`kVTVideoDecoderSpecification_RequireParavirtualizedDecoder`) and
  encodes frames (`VTPV_ENCODER=<encoder ID>` to pick one).
- `hostonly` (`hostonly.c`) is the host session alone: each file given is a guest message, each reply a file.
- `vtpvdump.py` decodes recorded messages (the 'vtpv' header, 'para' parameters, bplists, ImageDescriptions, nested atoms).

    clang -O1 -fblocks -dynamiclib -o interpose.dylib interpose.c -framework CoreFoundation -framework CoreMedia \
        -framework IOSurface -framework Security -Wl,-undefined,dynamic_lookup
    clang -O1 -o oracle oracle.c -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox
    clang -O1 -fblocks -o hostonly hostonly.c -framework CoreFoundation -framework CoreMedia
    mkdir canned log && ./hostonly canned req-decs.bin req-encs.bin   # requests: a recorded decs, its type changed for encs
    mv canned/reply-00-decs.bin canned/decs.bin; mv canned/reply-01-encs.bin canned/encs.bin
    VTPV_CANNED=$PWD/canned VTPV_LOG=$PWD/log DYLD_INSERT_LIBRARIES=$PWD/interpose.dylib ./oracle decode
    python3 vtpvdump.py log/*.bin

The paravirtualized encoders still do not run under the loopback (session creation answers -12903 before any '+enc').
