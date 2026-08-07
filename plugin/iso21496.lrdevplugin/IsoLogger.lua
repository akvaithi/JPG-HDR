--[[----------------------------------------------------------------------------
IsoLogger.lua — one shared LrLogger for the whole plugin.

Logging is off by default. Turn it on in the Plug-in Manager; output then goes
to the usual place (~/Documents/iso21496.log on macOS,
%USERPROFILE%\Documents\iso21496.log on Windows).
------------------------------------------------------------------------------]]

local LrLogger = import 'LrLogger'
local LrPrefs = import 'LrPrefs'

local logger = LrLogger('iso21496')

local IsoLogger = {}

local enabled = nil

local function isEnabled()
	if enabled == nil then
		local ok, prefs = pcall(function() return LrPrefs.prefsForPlugin() end)
		enabled = ok and prefs and prefs.enableLogging == true or false
		if enabled then logger:enable('logfile') end
	end
	return enabled
end

--- Re-reads the preference; call after the user toggles logging.
function IsoLogger.refresh()
	enabled = nil
	isEnabled()
end

function IsoLogger.info(message)
	if isEnabled() then logger:info(message) end
end

function IsoLogger.warn(message)
	if isEnabled() then logger:warn(message) end
end

function IsoLogger.error(message)
	if isEnabled() then logger:error(message) end
end

return IsoLogger
