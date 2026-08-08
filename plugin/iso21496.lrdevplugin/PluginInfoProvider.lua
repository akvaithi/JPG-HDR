--[[----------------------------------------------------------------------------
PluginInfoProvider.lua — the panel Lightroom shows in the Plug-in Manager.
Reports whether the native encoder is present and runnable, which is the first
thing to check when an export fails.
------------------------------------------------------------------------------]]

local LrDialogs = import 'LrDialogs'
local LrPrefs = import 'LrPrefs'
local LrTasks = import 'LrTasks'
local LrView = import 'LrView'

local IsoEncoder = require 'IsoEncoder'
local IsoLogger = require 'IsoLogger'

local PluginInfoProvider = {}

function PluginInfoProvider.sectionsForTopOfDialog(f, propertyTable)
	local prefs = LrPrefs.prefsForPlugin()
	if prefs.enableLogging == nil then prefs.enableLogging = false end

	-- Lightroom builds this section on the main thread, so the encoder cannot be
	-- run here: it would fail with "We can only wait from within a task" and
	-- take the whole panel down. Show a placeholder and fill it in from a task.
	propertyTable.isoStatus = LOC '$$$/Iso21496/StatusChecking=Checking the encoder…'

	local function refreshStatus()
		LrTasks.startAsyncTask(function()
			local available, message = IsoEncoder.checkAvailable()
			local version = available and IsoEncoder.version() or nil
			if not available then
				propertyTable.isoStatus = message
			elseif version then
				propertyTable.isoStatus =
					LOC('$$$/Iso21496/StatusOk=Ready. Encoder version ^1.', version)
			else
				propertyTable.isoStatus = LOC '$$$/Iso21496/StatusNoRun=The encoder is present but did not run. Check that it is not blocked by security policy.'
			end
		end)
	end

	refreshStatus()

	return {
		{
			title = LOC '$$$/Iso21496/InfoTitle=ISO 21496-1 HDR JPEG',
			f:row {
				f:static_text {
					title = LrView.bind { key = 'isoStatus', object = propertyTable },
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
					-- A button action is not a task either, and this one runs the
					-- encoder, so it has to start its own.
					action = function()
						LrTasks.startAsyncTask(function()
							IsoLogger.refresh()
							-- The user is asking precisely because something
							-- changed; don't answer from the cache.
							IsoEncoder.forget()
							local ok, why = IsoEncoder.checkAvailable()
							if not ok then
								refreshStatus()
								LrDialogs.message(LOC '$$$/Iso21496/TestFailed=Encoder not available', why, 'critical')
								return
							end
							local v = IsoEncoder.version()
							refreshStatus()
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
						end)
					end,
				},
			},
		},
	}
end

return PluginInfoProvider
