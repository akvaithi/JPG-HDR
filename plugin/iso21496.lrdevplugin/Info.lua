--[[----------------------------------------------------------------------------
Info.lua — plugin manifest for the ISO 21496-1 Gain Map HDR JPEG exporter.

Lightroom Classic loads a plugin by reading Info.lua from the .lrdevplugin
bundle; it is the file the build spec calls `manifest.lua`. A `manifest.lua`
shim sits alongside it so either name can be required from Lua code.
------------------------------------------------------------------------------]]

return {
	LrSdkVersion = 13.0,
	LrSdkMinimumVersion = 13.0,

	LrToolkitIdentifier = 'com.custom.lightroom.export.iso21496',
	LrPluginName = LOC '$$$/Iso21496/PluginName=ISO 21496-1 HDR JPEG',
	LrPluginInfoUrl = 'https://github.com/akvaithi/JPG-HDR',

	-- A full export destination: owns file naming, so exports land as .jpg.
	LrExportServiceProvider = {
		title = LOC '$$$/Iso21496/ServiceTitle=ISO 21496-1 HDR JPEG',
		file = 'ExportServiceProvider.lua',
	},

	-- A post-process filter, for slotting the encoder into an existing export
	-- preset. See docs/INSTALL.md for the file-naming caveat.
	LrExportFilterProvider = {
		title = LOC '$$$/Iso21496/FilterTitle=Encode as ISO 21496-1 HDR JPEG',
		file = 'ExportFilterProvider.lua',
		id = 'com.custom.lightroom.export.iso21496.filter',
	},

	LrPluginInfoProvider = 'PluginInfoProvider.lua',

	VERSION = { major = 1, minor = 0, revision = 0, build = 0 },
}
