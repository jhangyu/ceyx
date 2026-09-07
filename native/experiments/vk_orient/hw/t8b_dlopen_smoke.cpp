// T8b live-load proof: does the REFRESHED plugin .so actually load on device?
//
// Promoted from native/scripts/tmp/t8b_dlopen_smoke.cpp (gitignored scratch
// lane) to this tracked directory per Round 3 cleanup task #15 item 2 --
// the wrapper script that drove this binary could not go red (see
// run_t8b_dlopen_smoke_gate.sh's header for the bug and the fix), so the
// mechanism needs a committed, re-runnable form instead of a scratch file.
//
// Everything proven so far about the jniLibs refresh is a static-linkage
// argument (DT_NEEDED closure, symbol tables, hashes). This converts that into
// one real successful run, which is the standard for a mechanism claim.
//
// RTLD_NOW on purpose: RTLD_LAZY would defer function relocations and could
// report success even with an unresolvable dependency, which is precisely the
// failure mode under test. RTLD_NOW forces the full resolution of the whole
// dependency closure at load time, so a missing transitive library fails loudly
// and dlerror() names it.
//
// argv[1] = path to the .so to load (so the same binary can be pointed at a
// deliberately INCOMPLETE directory as a negative control -- a green result
// means nothing unless this program is capable of reporting red).

#include <dlfcn.h>
#include <cstdio>

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *path =
        (argc > 1) ? argv[1] : "libdng_decoder_native.so";
    printf("DLOPEN_TARGET=%s\n", path);
    printf("DLOPEN_FLAGS=RTLD_NOW\n");

    dlerror();  // clear any stale error before the call under test
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        // dlerror() is CONSUMING: it clears the error and the next call returns
        // NULL. Calling it twice in one expression (`dlerror() ? dlerror() :
        // ...`) therefore always prints "(null)" and destroys the one piece of
        // information the probe exists to collect. Capture it ONCE.
        const char *err = dlerror();
        printf("DLOPEN_RESULT=FAIL\n");
        printf("DLERROR=%s\n", err ? err : "(no error string available)");
        printf("SMOKE_VERDICT=FAIL\n");
        return 1;
    }
    printf("DLOPEN_RESULT=OK handle=%p\n", h);

    // Resolve exported FFI entries. dlsym returning non-null proves the symbol
    // is really exported by the loaded image, not merely present in a strings
    // dump of the file on disk.
    struct { const char *name; bool required; } syms[] = {
        {"ceyx_decode_into_buffer_oriented", true},
        {"ceyx_orient_rgba", true},
        {"ceyx_orientation_transposes", true},
        {"dng_decode_and_process", true},
    };
    int missing = 0;
    for (auto &s : syms) {
        dlerror();  // clear
        void *p = dlsym(h, s.name);
        const char *err = dlerror();
        printf("DLSYM %s = %p%s\n", s.name, p,
               (p == nullptr) ? (err ? err : " (null, no error)") : "");
        if (p == nullptr && s.required) ++missing;
    }

    printf("DLSYM_MISSING=%d\n", missing);
    dlclose(h);
    printf("SMOKE_VERDICT=%s\n", missing == 0 ? "PASS" : "FAIL");
    return missing == 0 ? 0 : 2;
}
