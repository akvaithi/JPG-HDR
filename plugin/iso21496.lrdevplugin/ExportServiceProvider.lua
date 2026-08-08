--[[----------------------------------------------------------------------------
ExportServiceProvider.lua — the "ISO 21496-1 HDR JPEG" export destination.

Lightroom renders each photo to an uncompressed 16-bit ProPhoto TIFF (the
intermediate the build spec calls for, because the Lua SDK cannot control gain
map channel counts, subsampling or APP2 payloads), the bundled native encoder
turns that into the final gain map JPEG, and the intermediate is deleted.

This is the recommended entry point: because the service owns file naming, the
files that land in the export folder are named .jpg.
------------------------------------------------------------------------------]]

local LrApplication = import 'LrApplication'
local LrDialogs = import 'LrDialogs'
local LrFileUtils = import 'LrFileUtils'
local LrPathUtils = import 'LrPathUtils'

local ExportDialogSections = require 'ExportDialogSections'
local IsoEncoder = require 'IsoEncoder'
local IsoLogger = require 'IsoLogger'
local IsoSettings = require 'IsoSettings'

local exportServiceProvider = {}

exportServiceProvider.exportPresetFields = IsoSettings.exportPresetFields()

-- The intermediate is always a TIFF, so there is nothing for the user to pick
-- in the File Settings section.
exportServiceProvider.allowFileFormats = { 'TIFF' }
exportServiceProvider.allowColorSpaces = { 'ProPhotoRGB' }
exportServiceProvider.hidePrintResolution = true
exportServiceProvider.canExportVideo = false

function exportServiceProvider.sectionsForBottomOfDialog(f, propertyTable)
	return ExportDialogSections.sectionsForBottomOfDialog(f, propertyTable)
end

local function setIfPresent(settings, key, value)
	-- Only touch keys this Lightroom version actually knows about, so the
	-- plug-in keeps working across SDK revisions.
	if settings[key] ~= nil then settings[key] = value end
end

function exportServiceProvider.updateExportSettings(exportSettings)
	IsoSettings.applyDefaults(exportSettings)

	-- Spec section 4.1: never use Lightroom's own gain map writer. Ask for the
	-- widest, highest precision intermediate it can render instead.
	exportSettings.LR_format = 'TIFF'
	exportSettings.LR_export_bitDepth = 16
	exportSettings.LR_export_colorSpace = 'ProPhotoRGB'
	exportSettings.LR_tiff_compressionMethod = 'compressionMethod_None'
	setIfPresent(exportSettings, 'LR_tiff_preserveTransparency', false)
	setIfPresent(exportSettings, 'LR_reimportExportedPhoto', false)
	setIfPresent(exportSettings, 'LR_removeLocationMetadata', false)

	-- HDR Output on, and no baked SDR-compatible copy. Without these the
	-- intermediate is an ordinary SDR render and there is no headroom for the
	-- gain map to describe. These two key names are the ones Lightroom Classic
	-- 14 actually uses.
	exportSettings.LR_export_useHDR = true
	exportSettings.LR_export_maximizeCompatibility = false
end

-- Checks the settings that actually determine whether there is HDR data to
-- work with. Returns a list of human-readable problems, empty when all is well.
local function intermediateProblems(settings)
	local issues = {}
	if settings.LR_format ~= 'TIFF' then
		issues[#issues + 1] = LOC('$$$/Iso21496/IssueFormat=Format is ^1; it must be TIFF, which is the intermediate the encoder reads.',
			tostring(settings.LR_format))
	end
	if (tonumber(settings.LR_export_bitDepth) or 8) < 16 then
		issues[#issues + 1] = LOC('$$$/Iso21496/IssueDepth=Bit depth is ^1; it must be 16 so the gain map can be derived without banding.',
			tostring(settings.LR_export_bitDepth))
	end
	if settings.LR_export_useHDR == false then
		issues[#issues + 1] = LOC '$$$/Iso21496/IssueHdr=HDR Output is off; without it the render has no highlights above SDR white and the gain map will be empty.'
	end
	return issues
end

function exportServiceProvider.processRenderedPhotos(functionContext, exportContext)
	local exportSession = exportContext.exportSession
	local settings = exportContext.propertyTable
	IsoSettings.applyDefaults(settings)
	-- Re-read the logging preference. IsoLogger caches it on first use, so a
	-- checkbox ticked after the plug-in loaded would otherwise not take effect
	-- until it was reloaded — which is why turning logging on produced no log.
	IsoLogger.refresh()

	local available, message = IsoEncoder.checkAvailable()
	if not available then
		LrDialogs.message(
			LOC '$$$/Iso21496/CannotExport=Cannot export ISO 21496-1 HDR JPEGs',
			message, 'critical')
		return
	end

	local valid, problem = IsoSettings.validate(settings)
	if not valid then
		LrDialogs.message(
			LOC '$$$/Iso21496/BadSettings=Check the export settings', problem, 'critical')
		return
	end

	-- updateExportSettings forces all of this, but a future Lightroom could
	-- rename a key. Warn rather than silently export a file with no HDR in it.
	local issues = intermediateProblems(settings)
	if #issues > 0 then
		local choice = LrDialogs.confirm(
			LOC '$$$/Iso21496/IssuesTitle=The export settings may limit HDR quality',
			LOC('$$$/Iso21496/IssuesBody=The plug-in could not force these:\n\n^1',
				table.concat(issues, '\n')),
			LOC '$$$/Iso21496/Continue=Continue anyway',
			LOC '$$$/Iso21496/Cancel=Cancel')
		if choice == 'cancel' then return end
	end

	local total = exportSession:countRenditions()

	local progress = exportContext:configureProgress {
		title = total > 1
			and LOC('$$$/Iso21496/ProgressMany=Exporting ^1 photos as ISO 21496-1 HDR JPEG', total)
			or LOC '$$$/Iso21496/ProgressOne=Exporting one photo as ISO 21496-1 HDR JPEG',
	}

	local exported, failures = {}, {}

	for i, rendition in exportContext:renditions { stopIfCanceled = true } do
		if progress:isCanceled() then break end
		progress:setPortionComplete(i - 1, total)
		progress:setCaption(LOC('$$$/Iso21496/ProgressPhoto=Rendering ^1',
			rendition.photo:getFormattedMetadata('fileName')))

		local success, pathOrMessage = rendition:waitForRender()
		if progress:isCanceled() then
			if success then LrFileUtils.delete(pathOrMessage) end
			break
		end

		if not success then
			failures[#failures + 1] = { name = '', reason = pathOrMessage }
		else
			local intermediate = pathOrMessage
			local finalPath = LrPathUtils.replaceExtension(intermediate, 'jpg')
			if LrFileUtils.exists(finalPath) and finalPath ~= intermediate then
				-- The intermediate got a unique name from Lightroom, but the
				-- .jpg we derive from it might still collide.
				finalPath = LrFileUtils.chooseUniqueFileName(finalPath)
			end

			progress:setCaption(LOC('$$$/Iso21496/ProgressEncode=Encoding gain map for ^1',
				LrPathUtils.leafName(finalPath)))

			-- Built per photo: the develop-settings hints differ for each one.
			local ok, report, err = IsoEncoder.run {
				input = intermediate,
				output = finalPath,
				arguments = IsoSettings.argumentsForPhoto(settings, rendition.photo),
			}

			if ok then
				if settings.iso_keep_intermediate ~= true then
					LrFileUtils.delete(intermediate)
				end
				exported[#exported + 1] = finalPath
				if report then
					IsoLogger.info(string.format(
						'%s: %d bytes (base %d + gain map %d), max boost %.2f stops',
						LrPathUtils.leafName(finalPath), report.totalBytes or 0,
						report.primaryBytes or 0, report.gainMapBytes or 0,
						report.maxBoostLog2 or 0))
				end
			else
				LrFileUtils.delete(finalPath)
				LrFileUtils.delete(intermediate)
				failures[#failures + 1] = {
					name = LrPathUtils.leafName(intermediate),
					reason = err or LOC '$$$/Iso21496/UnknownError=unknown error',
				}
				rendition:uploadFailed(err or LOC '$$$/Iso21496/EncodeFailed=Gain map encoding failed')
			end
		end
	end

	progress:setPortionComplete(total, total)

	if settings.iso_add_to_catalog == true and #exported > 0 then
		local catalog = LrApplication.activeCatalog()
		catalog:withWriteAccessDo(
			LOC '$$$/Iso21496/AddAction=Add ISO 21496-1 exports',
			function()
				for _, path in ipairs(exported) do
					local ok, err = pcall(function() catalog:addPhoto(path) end)
					if not ok then IsoLogger.warn('addPhoto failed: ' .. tostring(err)) end
				end
			end,
			{ timeout = 30 })
	end

	if #failures > 0 then
		local lines = {}
		for _, failure in ipairs(failures) do
			lines[#lines + 1] = (failure.name ~= '' and (failure.name .. ': ') or '') ..
				tostring(failure.reason)
		end
		LrDialogs.message(
			LOC('$$$/Iso21496/SomeFailed=^1 of ^2 photos could not be encoded',
				#failures, total),
			table.concat(lines, '\n'), 'warning')
	end
end

return exportServiceProvider
