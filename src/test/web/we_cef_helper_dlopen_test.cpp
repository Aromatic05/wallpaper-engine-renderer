#include "backend/web/internal/cef/CefHostApi.h"

#include <cstdio>
#include <dlfcn.h>

int main(int argc, char** argv) {
    if (argc != 2) return 2;

    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (! handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto* get_api =
        reinterpret_cast<WeCefGetHostApiFn>(dlsym(handle, "we_cef_get_host_api"));
    if (! get_api) {
        std::fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }

    const WeCefHostApi* api = get_api(WE_CEF_HOST_API_VERSION);
    if (! api) return 1;
    if (api->version != WE_CEF_HOST_API_VERSION) return 1;
    if (api->struct_size < sizeof(WeCefHostApi)) return 1;
    if (! api->create_host || ! api->destroy_host || ! api->init) return 1;

    return 0;
}
