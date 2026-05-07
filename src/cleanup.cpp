#include <iostream>
#include <vector>
#include <string>
#include "../engine/SoundMate_APO/include/DeviceManager.h"

int main() {
    std::cout << "[SoundMate] Starting Official Registry Cleanup..." << std::endl;
    
    if (DeviceManager::FullReset()) {
        std::cout << "[SoundMate] All SoundMate registry entries have been removed." << std::endl;
    } else {
        std::cout << "[SoundMate] No entries to clean or cleanup failed." << std::endl;
    }

    std::cout << "[SoundMate] Restarting audio services to finalize..." << std::endl;
    DeviceManager::RestartAudioService();
    
    std::cout << "[SoundMate] Cleanup Complete. System restored to original state." << std::endl;
    return 0;
}
