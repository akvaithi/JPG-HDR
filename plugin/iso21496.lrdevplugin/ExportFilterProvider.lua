--[[----------------------------------------------------------------------------
ExportFilterProvider.lua — the post-process render filter form of the encoder,
for dropping ISO 21496-1 encoding into an existing export preset.

This is the `postProcessRenderedPhotos` bridge from section 2.1.3 of the build
spec: the filter forces Lightroom to render a 16-bit ProPhoto TIFF, feeds it to
the native encoder, and satisfies the rendition with the resulting JPEG bytes.

Caveat, documented in docs/INSTALL.md: an export filter cannot change the file
extension Lightroom derived from the export format, so the file that lands in
the destination folder is named .tif while containing an ISO 21496-1 JPEG. The
filter renames it afterwards unless "Add to This Catalog" is on, in which case
it leaves the name alone so the catalogue reference stays valid. Use the
"ISO 21496-1 HDR JPEG" export destination instead if you want .jpg names with
no caveats.
------------------------------------------------------------------------------]]

local LrFileUtils = import 'LrFileUtils'
local LrPathUtils = import 'LrPathUtils'

local ExportDialogSections = require 'ExportDialogSections'
local IsoEncoder = require 'IsoEncoder'
local IsoLogger = require 'IsoLogger'
local IsoSettings = require 'IsoSettings'

local exportFilterProvider = {}

exportFilterProvider.exportPresetFields = IsoSettings.exportPresetFields()

function exportFilterProvider.sectionForFilterInDialog(f, propertyTable)
	IsoSettings.applyDefaults(propertyTable)
	local sections = ExportDialogSections.sectionsForBottomOfDialog(f, propertyTable)
	-- A filter contributes exactly one section, so fold the advanced controls
	-- into the main one.
	local main = sections[1]
	local advanced = sections[2]
	for i = 1, #advanced do main[#main + 1] = advanced[i] end
	main.title = LOC '$$$/Iso21496/FilterSection=ISO 21496-1 HDR Gain Map'
	return main
end

function exportFilterProvider.updateExportSettings(exportSettings)
	IsoSettings.applyDefaults(exportSettings)
	exportSettings.LR_format = 'TIFF'
	exportSettings.LR_export_bitDepth = 16
	exportSettings.LR_export_colorSpace = 'ProPhotoRGB'
	exportSettings.LR_tiff_compressionMethod = 'compressionMethod_None'
	-- HDR Output on, no baked SDR-compatible copy. Without these the render
	-- carries no headroom for the gain map to describe.
	exportSettings.LR_export_useHDR = true
	exportSettings.LR_export_maximizeCompatibility = false
end

function exportFilterProvider.shouldRenderPhoto(exportSettings, photo)
	return true
end

function exportFilterProvider.postProcessRenderedPhotos(functionContext, filterContext)
	local settings = filterContext.propertyTable
	IsoSettings.applyDefaults(settings)

	local available, message = IsoEncoder.checkAvailable()
	local valid, problem = IsoSettings.validate(settings)
	local arguments = IsoSettings.buildArguments(settings)

	local session = filterContext.sourceExportSession
	local total = session and session:countRenditions() or 0
	local renamed = {}

	for sourceRendition, renditionToSatisfy in filterContext:renditions { stopIfCanceled = true } do
		local success, pathOrMessage = sourceRendition:waitForRender()
		if not success then
			renditionToSatisfy:renditionIsDone(false, pathOrMessage)
		elseif not available then
			renditionToSatisfy:renditionIsDone(false, message)
		elseif not valid then
			renditionToSatisfy:renditionIsDone(false, problem)
		else
			local intermediate = pathOrMessage
			-- Encode beside the rendition, then move onto the path Lightroom
			-- is waiting for. renditionToSatisfy.destinationPath is the same
			-- file for the last filter in the chain, hence the temp name.
			local temp = LrPathUtils.replaceExtension(intermediate, 'iso21496.tmp')
			local ok, report, err = IsoEncoder.run {
				input = intermediate,
				output = temp,
				arguments = arguments,
			}

			if not ok then
				LrFileUtils.delete(temp)
				renditionToSatisfy:renditionIsDone(false,
					err or LOC '$$$/Iso21496/EncodeFailed=Gain map encoding failed')
			else
				local destination = renditionToSatisfy.destinationPath
				LrFileUtils.delete(destination)
				local moved, moveError = LrFileUtils.move(temp, destination)
				if moved == false then
					LrFileUtils.delete(temp)
					renditionToSatisfy:renditionIsDone(false, tostring(moveError))
				else
					if report then
						IsoLogger.info(string.format('%s: %d bytes total',
							LrPathUtils.leafName(destination), report.totalBytes or 0))
					end
					renditionToSatisfy:renditionIsDone(true)
					if LrPathUtils.extension(destination):lower() ~= 'jpg' then
						renamed[#renamed + 1] = destination
					end
				end
			end
		end
	end

	-- Lightroom has finished with the files now, so the extension can be
	-- corrected. Skipped when the export re-imports them, because the
	-- catalogue would then point at a path that no longer exists.
	if settings.iso_add_to_catalog ~= true then
		for _, path in ipairs(renamed) do
			local target = LrPathUtils.replaceExtension(path, 'jpg')
			if not LrFileUtils.exists(target) then
				LrFileUtils.move(path, target)
			end
		end
	end
	IsoLogger.info(string.format('filter finished: %d rendition(s)', total))
end

return exportFilterProvider
