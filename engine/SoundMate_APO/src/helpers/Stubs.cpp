#include <windows.h>
#include <strsafe.h>

// Mangled name for StringVPrintfWorkerW (long __stdcall)
// ?StringVPrintfWorkerW@@YAJPEAG_KPEA_KPEBGPEAD@Z
extern "C" long __stdcall StringVPrintfWorkerW(unsigned short* p1, unsigned __int64 p2, unsigned __int64* p3, unsigned short const* p4, char* p5) {
    return S_OK; 
}

