#include "ui.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    // Crash reporter for bad_alloc — print before abort
    std::set_new_handler([] {
        std::fputs("[FATAL] std::bad_alloc — heap exhausted\n", stderr);
        std::fflush(stderr);
        // Remove handler so the next new throws normally
        std::set_new_handler(nullptr);
    });

    try {
        std::fputs("[main] constructing NagraApp\n", stderr); std::fflush(stderr);
        NagraApp app;
        std::fputs("[main] NagraApp constructed OK, entering run()\n", stderr); std::fflush(stderr);
        app.run();
    }
    catch (const std::bad_alloc& e) {
        std::fprintf(stderr, "[CRASH] bad_alloc: %s\n", e.what()); std::fflush(stderr);
        return 1;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[CRASH] exception: %s\n", e.what()); std::fflush(stderr);
        return 1;
    }
    catch (...) {
        std::fputs("[CRASH] unknown exception\n", stderr); std::fflush(stderr);
        return 1;
    }
    return 0;
}
