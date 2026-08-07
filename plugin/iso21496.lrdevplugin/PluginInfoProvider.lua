--[[----------------------------------------------------------------------------
PluginInfoProvider.lua — the panel Lightroom shows in the Plug-in Manager.
Reports whether the native encoder is present and runnable, which is the first
thing to check when an export fails.
------------------------------------------------------------------------------]]

local LrDialogs = import 'LrDialogs'
local LrPrefs = import 'LrPrefs'
local LrView = import 'LrView'

local IsoEncoder = require 'IsoEncoder'
local IsoLogger = require 'IsoLogger'

local PluginInfoProvider = {}

function PluginInfoProvider.sectionsForTopOfDialog(f, propertyTable)
	local prefs = LrPrefs.prefsForPlugin()
	if prefs.enableLogging == nil then prefs.enableLogging = false end

	local available, message = IsoEncoder.checkAvailable()
	local version = available and IsoEncoder.version() or nil

	local status
	if not available then
		status = message
	elseif version then
		status = LOC('$$$/Iso21496/StatusOk=Ready. Encoder version ^1.', version)
	else
		status = LOC '$$$/Iso21496/StatusNoRun=The encoder is present but did not run. Check that it is not blocked by security policy.'
	end

	return {
		{
			title = LOC '$$$/Iso21496/InfoTitle=ISO 21496-1 HDR JPEG',
			f:row {
				f:static_text {
					title = status,
					width_in_chars = 60,
					height_in_lines = 2,
				},
			},
			f:row {
				f:static_text {
					title = LOC('$$$/Iso21496/BinaryPath=Encoder: ^1', IsoEncoder.binaryPath()),
					width_in_chars = 60,
				},
			},
			f:row {
				f:checkbox {
					title = LOC '$$$/Iso21496/EnableLogging=Write a diagnostic log to the Documents folder',
					value = LrView.bind { key = 'enableLogging', object = prefs },
				},
			},
			f:row {
				f:push_button {
					title = LOC '$$$/Iso21496/TestButton=Test the encoder',
					action = function()
						IsoLogger.refresh()
						local ok, why = IsoEncoder.checkAvailable()
						if not ok then
							LrDialogs.message(LOC '$$$/Iso21496/TestFailed=Encoder not available', why, 'critical')
							return
						end
						local v = IsoEncoder.version()
						if v then
							LrDialogs.message(
								LOC '$$$/Iso21496/TestOk=Encoder is working',
								LOC('$$$/Iso21496/TestOkDetail=Reported version ^1.', v), 'info')
						else
							LrDialogs.message(
								LOC '$$$/Iso21496/TestFailed=Encoder not available',
								LOC '$$$/Iso21496/TestNoVersion=The binary exists but would not run. On macOS this is usually Gatekeeper: right-click the binary in Finder and choose Open once, or run xattr -dr com.apple.quarantine on the plug-in folder.',
								'critical')
						end
					end,
				},
			},
		},
	}
end

return PluginInfoProvider
