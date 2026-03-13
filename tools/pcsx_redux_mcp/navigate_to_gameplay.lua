-- navigate_to_gameplay.lua
-- Auto-navigates from boot to gameplay and saves a checkpoint state.
-- Run via: loadfile("F:/Projects/psxrecomp-v2/tools/pcsx_redux_mcp/navigate_to_gameplay.lua")()

local ISO_PATH  = "F:/Projects/psxrecomp-v2/isos/Tomba! (USA).cue"
local SAVE_PATH = "C:/temp/tomba_gameplay.sstate"
local CROSS = PCSX.CONSTS.PAD.BUTTON.CROSS
local START = PCSX.CONSTS.PAD.BUTTON.START
local pad   = PCSX.SIO0.slots[1].pads[1]

local phase = "wait_boot"
local frame = 0
local L

local function saveState()
    local s = PCSX.createSaveState()
    local f = Support.File.open(SAVE_PATH, "CREATE")
    f:write(s)
    f:close()
    PCSX.log("[navigate] Saved gameplay state to " .. SAVE_PATH)
end

-- Read a 16-bit value from PS1 RAM
local function read16(addr)
    local ram = PCSX.getMemPtr()
    -- addr is a PS1 virtual address, mask to physical
    local off = bit.band(addr, 0x1FFFFF)
    local lo = ram:read(off)
    local hi = ram:read(off + 1)
    if lo == nil or hi == nil then return 0 end
    return lo + hi * 256
end

L = PCSX.Events.createEventListener('GPU::Vsync', function()
    frame = frame + 1

    -- TCB state at 0x801FD848 (load_state word)
    local state = read16(0x801FD848)
    local sub   = read16(0x801FD84A)

    if phase == "wait_boot" then
        -- Hold CROSS to skip all intro FMVs until we hit main menu (state=4)
        pad.setOverride(CROSS, true)
        if state == 4 then
            phase = "at_menu"
            frame = 0
            PCSX.log("[navigate] Main menu reached at frame " .. frame)
        end

    elseif phase == "at_menu" then
        -- Release CROSS, wait 60 frames, then press CROSS for NEW GAME
        pad.clearOverride(CROSS)
        if frame >= 60 then
            pad.setOverride(CROSS, true)
            phase = "starting_game"
            frame = 0
            PCSX.log("[navigate] Pressing CROSS for NEW GAME")
        end

    elseif phase == "starting_game" then
        -- Release CROSS after 5 frames, then hold START to skip intro FMV
        if frame >= 5 then
            pad.clearOverride(CROSS)
            pad.setOverride(START, true)
        end
        -- Wait for sub=1 (gameplay dispatch active)
        if sub == 1 then
            pad.clearOverride(START)
            phase = "gameplay"
            frame = 0
            PCSX.log("[navigate] Gameplay reached! Waiting 120 frames to stabilise...")
        end

    elseif phase == "gameplay" then
        -- Wait 120 frames for scene to fully load, then save state
        if frame >= 120 then
            saveState()
            phase = "done"
            L:remove()
            PCSX.log("[navigate] Done. State saved to " .. SAVE_PATH)
        end
    end
end)

PCSX.log("[navigate] Auto-navigation started (phase: wait_boot)")
print("[navigate] Running — will save state to " .. SAVE_PATH .. " when gameplay is reached")
