// NexusOS Wine campaign: C++ exceptions through ten frames with destructors (x64: SEH-based unwinding, x86: DWARF).
// Prints "caught deep dtors=11" and exits 0 when every destructor ran during unwinding.
#include <cstdio>
#include <stdexcept>
struct Guard { int* p; ~Guard() { (*p)++; } };
static int depth(int n, int* dtors) { Guard g{dtors}; if (!n) throw std::runtime_error("deep"); return depth(n - 1, dtors) + 1; }
int main() {
    int dtors = 0;
    try { depth(10, &dtors); }
    catch (const std::exception& e) { std::printf("caught %s dtors=%d\n", e.what(), dtors); return dtors == 11 ? 0 : 1; }
    return 2;
}
