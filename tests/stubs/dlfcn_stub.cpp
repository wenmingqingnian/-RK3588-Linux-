// Minimal dlfcn stub for host tests of dlopen-based modules.
//
// Simulates "library not present": dlopen() always fails so the module under
// test must take its soft-fail path. This lets the MPP decoder/encoder
// soft-fail behavior be unit-tested on hosts without librockchip_mpp.so.
//
// Build with: -Itests/stubs  (so #include <dlfcn.h> resolves to the stub header)

#include <cstddef>

extern "C" {

// 可变 char 数组（非 const）：dlerror() 签名要求返回 char*，
// 若用 const char* 指向字符串字面量再 const_cast 转走 const，
// 调用方写入会触发未定义行为（字符串字面量位于只读存储区）。
// 改用 static char[] 数组，存储为独立可变副本，无需 const_cast。
static char kStubError[] =
    "stub dlfcn: librockchip_mpp.so not available in host test environment";

void *dlopen(const char *fileName, int /*mode*/)
{
    (void)fileName;
    return nullptr;
}

void *dlsym(void * /*handle*/, const char * /*symbol*/)
{
    return nullptr;
}

int dlclose(void * /*handle*/)
{
    return 0;
}

char *dlerror()
{
    return kStubError;
}

} // extern "C"
