#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include "overlay_detector.h"

namespace PS1 {

// Forward declaration
class CDROMController;

/**
 * Represents information about a loaded overlay
 */
struct OverlayInfo {
    std::string name;
    uint32_t load_address;
    uint32_t size;
    bool is_loaded;
    void* data;
};

/**
 * Represents information about a registered function
 */
struct FunctionInfo {
    void* ptr;                    // Pointer to the function
    std::string overlay_name;     // Name of overlay this function belongs to
};

/**
 * Manages loading, unloading, and tracking of PS1 overlays
 */
class OverlayManager {
public:
    /**
     * Constructor
     */
    OverlayManager();

    /**
     * Destructor - unloads all loaded overlays
     */
    ~OverlayManager();

    /**
     * Loads an overlay into memory
     * @param name Name of the overlay
     * @param data Pointer to overlay data
     * @param size Size of overlay data in bytes
     * @param load_addr Memory address where overlay should be loaded
     * @return true if overlay was loaded successfully, false otherwise
     */
    bool LoadOverlay(const std::string& name, const uint8_t* data, uint32_t size, uint32_t load_addr);

    /**
     * Unloads an overlay from memory
     * @param name Name of the overlay to unload
     * @return true if overlay was unloaded successfully, false otherwise
     */
    bool UnloadOverlay(const std::string& name);

    /**
     * Checks if an overlay is currently loaded
     * @param name Name of the overlay
     * @return true if overlay is loaded, false otherwise
     */
    bool IsOverlayLoaded(const std::string& name) const;

    /**
     * Gets a function pointer from a loaded overlay
     * @param overlay_name Name of the overlay
     * @param address Address of the function within the overlay
     * @return Pointer to the function, or nullptr if not found
     */
    void* GetOverlayFunction(const std::string& overlay_name, uint32_t address);

    /**
     * Sets the CD-ROM controller for loading overlays from ISO files
     * @param controller Pointer to the CD-ROM controller
     */
    void SetCDROMController(CDROMController* controller);

    /**
     * Loads an overlay from an ISO file
     * @param filename Name of the file in the ISO filesystem
     * @param load_addr Memory address where overlay should be loaded
     * @return true if overlay was loaded successfully, false otherwise
     */
    bool LoadOverlayFromISO(const std::string& filename, uint32_t load_addr);

    /**
     * Loads all detected overlays from ISO using OverlayDetector results
     * @param overlays Vector of detected overlays from OverlayDetector::DetectOverlays()
     * @return number of overlays successfully loaded
     */
    int LoadDetectedOverlays(const std::vector<DetectedOverlay>& overlays);

    /**
     * Registers a function in the function registry
     * @param overlay_name Name of the overlay this function belongs to
     * @param ps1_addr PS1 memory address of the function
     * @param func_ptr Pointer to the recompiled function
     */
    void RegisterFunction(const std::string& overlay_name, uint32_t ps1_addr, void* func_ptr);

    /**
     * Looks up a function by its PS1 address
     * @param ps1_addr PS1 memory address of the function
     * @return Pointer to the recompiled function, or nullptr if not found
     */
    void* LookupFunction(uint32_t ps1_addr);

private:
    std::map<std::string, OverlayInfo> overlays_;
    CDROMController* cdrom_controller_;
    std::map<uint32_t, FunctionInfo> function_registry_;  // PS1 address -> function info
};

} // namespace PS1
