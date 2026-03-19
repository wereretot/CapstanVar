#include "ui.hpp"
#include <cstdio>
#include <exception>
#include <new>

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    std::set_new_handler([] {
        std::fputs("[FATAL] Out of memory\n", stderr);
        std::fflush(stderr);
        std::set_new_handler(nullptr);
    });
    try {
        CapstanApp app;
        app.run();
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[CRASH] %s\n", e.what());
        return 1;
    }
    return 0;
}
