#include "include/DeviceManager.h"
#include <iostream>
#include <fcntl.h>
#include <io.h>

int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    auto devices = DeviceManager::GetActiveDevices();
    for (auto& d : devices) {
        std::wcout << d.name << L"|" << d.id << L"|" << (d.isInstalled ? L"YES" : L"NO") << std::endl;
    }
    return 0;
}
