#include "app.h"

#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    try {
        kanekist::App app(instance);
        return app.run(showCommand);
    } catch (const std::exception& error) {
        kanekist::Logger::instance().write(L"ERROR", L"Fatal startup exception");
        (void)error;
        return 1;
    } catch (...) {
        kanekist::Logger::instance().write(L"ERROR", L"Fatal startup crash");
        return 1;
    }
}
