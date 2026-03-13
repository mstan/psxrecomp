-- PCSX-Redux startup script for PSXRecomp debugging
-- Load via Lua Console: loadfile("F:/Projects/psxrecomp-v2/tools/pcsx_redux_mcp/startup.lua")()
-- Requires emulation running (GPU::Vsync fires once per frame).

local CMD_FILE    = "C:/temp/pcsx_cmd.lua"
local RESULT_FILE = "C:/temp/pcsx_result.txt"

-- Must be stored in a global to prevent garbage collection
_psxrecomp_listener = PCSX.Events.createEventListener("GPU::Vsync", function()
    local f = io.open(CMD_FILE, "r")
    if not f then return end

    local code = f:read("*a")
    f:close()
    os.remove(CMD_FILE)

    local fn, err = load(code)
    local result
    if not fn then
        result = "ERROR (compile): " .. tostring(err)
    else
        local ok, val = pcall(fn)
        if ok then
            result = tostring(val ~= nil and val or "nil")
        else
            result = "ERROR (runtime): " .. tostring(val)
        end
    end

    local rf = io.open(RESULT_FILE, "w")
    if rf then
        rf:write(result)
        rf:close()
    end
end)

PCSX.log("[psxrecomp-mcp] listener stored, polling " .. CMD_FILE .. " on GPU::Vsync")
print("[psxrecomp-mcp] ready — listener stored as _psxrecomp_listener")
