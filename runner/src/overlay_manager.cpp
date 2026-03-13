#include "overlay_manager.h"
#include "cdrom_controller.h"

namespace PS1 {

OverlayManager::OverlayManager() : cdrom_controller_(nullptr) {
    // Default constructor
}

OverlayManager::~OverlayManager() {
    // Unload all loaded overlays
    for (auto& pair : overlays_) {
        UnloadOverlay(pair.first);
    }
}

bool OverlayManager::LoadOverlay(const std::string& name, const uint8_t* data, uint32_t size, uint32_t load_addr) {
    // 1. Check if already loaded (return true if so)
    auto it = overlays_.find(name);
    if (it != overlays_.end() && it->second.is_loaded) {
        return true;
    }

    // 2. Allocate memory
    void* allocated_data = malloc(size);
    if (!allocated_data) {
        return false;
    }

    // 3. Copy overlay data
    memcpy(allocated_data, data, size);

    // 4. Create OverlayInfo and add to overlays_ map
    OverlayInfo info;
    info.name = name;
    info.load_address = load_addr;
    info.size = size;
    info.is_loaded = true;
    info.data = allocated_data;

    overlays_[name] = info;

    // 5. Return true on success
    return true;
}

bool OverlayManager::UnloadOverlay(const std::string& name) {
    // 1. Find overlay in overlays_ map
    auto it = overlays_.find(name);

    // 2. If not found, return false
    if (it == overlays_.end()) {
        return false;
    }

    // 3. free(overlay_info.data) to free allocated memory
    if (it->second.data != nullptr) {
        free(it->second.data);
        it->second.data = nullptr;
    }

    // 4. Set overlay_info.is_loaded = false
    it->second.is_loaded = false;

    // 5. Remove all functions for this overlay from registry
    for (auto func_it = function_registry_.begin(); func_it != function_registry_.end(); ) {
        if (func_it->second.overlay_name == name) {
            func_it = function_registry_.erase(func_it);
        } else {
            ++func_it;
        }
    }

    // 6. Return true
    return true;
}

bool OverlayManager::IsOverlayLoaded(const std::string& name) const {
    // 1. Find overlay in overlays_ map
    auto it = overlays_.find(name);

    // 2. Return is_loaded flag (or false if not found)
    if (it != overlays_.end()) {
        return it->second.is_loaded;
    }

    return false;
}

void* OverlayManager::GetOverlayFunction(const std::string& overlay_name, uint32_t address) {
    // TODO: Implement in later checkpoint
    return nullptr;
}

void OverlayManager::RegisterFunction(const std::string& overlay_name, uint32_t ps1_addr, void* func_ptr) {
    FunctionInfo info;
    info.ptr = func_ptr;
    info.overlay_name = overlay_name;
    function_registry_[ps1_addr] = info;
}

void* OverlayManager::LookupFunction(uint32_t ps1_addr) {
    auto it = function_registry_.find(ps1_addr);
    if (it != function_registry_.end()) {
        return it->second.ptr;
    }
    return nullptr;
}

void OverlayManager::SetCDROMController(CDROMController* controller) {
    cdrom_controller_ = controller;
}

bool OverlayManager::LoadOverlayFromISO(const std::string& filename, uint32_t load_addr) {
    if (!cdrom_controller_) {
        return false;  // No CD-ROM controller set
    }

    // Read file from ISO
    std::vector<uint8_t> data = cdrom_controller_->ReadFile(filename);
    if (data.empty()) {
        return false;  // File not found or empty
    }

    // Load the overlay
    return LoadOverlay(filename, data.data(), data.size(), load_addr);
}

int OverlayManager::LoadDetectedOverlays(const std::vector<DetectedOverlay>& overlays) {
    int loaded_count = 0;
    for (const auto& overlay : overlays) {
        // Skip overlays without a valid load address
        if (overlay.load_address == 0) continue;

        // Skip if already loaded
        if (IsOverlayLoaded(overlay.filename)) {
            loaded_count++;  // Count already-loaded as success
            continue;
        }

        // Try to load from ISO
        if (LoadOverlayFromISO(overlay.filename, overlay.load_address)) {
            loaded_count++;
        }
    }
    return loaded_count;
}

} // namespace PS1
