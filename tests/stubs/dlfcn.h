#ifndef TEST_STUB_DLFCN_H
#define TEST_STUB_DLFCN_H

#define RTLD_LAZY 0x0001
#define RTLD_NOLOAD 0x0004

extern "C" {

void *dlopen(const char *fileName, int mode);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror();

}

#endif // TEST_STUB_DLFCN_H
