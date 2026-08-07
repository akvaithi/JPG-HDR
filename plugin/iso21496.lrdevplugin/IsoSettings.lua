--[[----------------------------------------------------------------------------
IsoSettings.lua — the export parameters, their defaults, and the translation
from property-table values into encoder command-line arguments.

The property names match the build spec (iso_target_headroom, iso_color_space,
iso_gainmap_subsample, iso_gainmap_channels, iso_jpeg_quality) so that presets
written against the spec keep working.
------------------------------------------------------------------------------]]

local IsoSettings = {}

IsoSettings.defaults = {
	-- Spec section 3.1: the five required controls.
	iso_target_headroom = '4.0',
	iso_color_space = 'DisplayP3',
	iso_gainmap_subsample = '2',
	iso_gainmap_channels = 'Monochrome',
	iso_jpeg_quality = 90,

	-- Advanced controls, all with spec-conformant defaults.
	iso_gainmap_quality = 85,
	iso_gainmap_gamma = '2.2',
	iso_tone_map = 'reinhard',
	iso_auto_max_boost = true,
	iso_chroma_subsample = true,

	-- How to read what Lightroom rendered.
	iso_input_transfer = 'auto',
	iso_input_primaries = 'auto',
	iso_pq_diffuse_white = 203,

	-- Housekeeping.
	iso_copy_metadata = true,
	iso_add_to_catalog = false,
	iso_keep_intermediate = false,
}

--- The list Lightroom needs so these values are saved in export presets.
function IsoSettings.exportPresetFields()
	local fields = {}
	for key, default in pairs(IsoSettings.defaults) do
		fields[#fields + 1] = { key = key, default = default }
	end
	table.sort(fields, function(a, b) return a.key < b.key end)
	return fields
end

--- Fills in any value the property table is missing (older presets, or the
--- filter provider being added to an existing preset).
function IsoSettings.applyDefaults(properties)
	for key, default in pairs(IsoSettings.defaults) do
		if properties[key] == nil then properties[key] = default end
	end
	return properties
end

IsoSettings.headroomItems = {
	{ title = LOC '$$$/Iso21496/Headroom1=+1.0 EV  (about 160 nits)', value = '1.0' },
	{ title = LOC '$$$/Iso21496/Headroom2=+2.0 EV  (about 320 nits)', value = '2.0' },
	{ title = LOC '$$$/Iso21496/Headroom3=+3.0 EV  (about 640 nits)', value = '3.0' },
	{ title = LOC '$$$/Iso21496/Headroom4=+4.0 EV  (about 1280 nits)', value = '4.0' },
}

IsoSettings.colorSpaceItems = {
	{ title = LOC '$$$/Iso21496/CsP3=Display P3  (recommended)', value = 'DisplayP3' },
	{ title = LOC '$$$/Iso21496/CsSrgb=sRGB  (legacy compatibility)', value = 'sRGB' },
	{ title = LOC '$$$/Iso21496/Cs2020=Rec. 2020  (wide gamut)', value = 'Rec2020' },
}

IsoSettings.subsampleItems = {
	{ title = LOC '$$$/Iso21496/Sub1=Full resolution  (1:1)', value = '1' },
	{ title = LOC '$$$/Iso21496/Sub2=Half resolution  (1:2)', value = '2' },
	{ title = LOC '$$$/Iso21496/Sub4=Quarter resolution  (1:4)', value = '4' },
}

IsoSettings.channelItems = {
	{ title = LOC '$$$/Iso21496/ChMono=Monochrome  (single channel)', value = 'Monochrome' },
	{ title = LOC '$$$/Iso21496/ChRgb=RGB  (three channels)', value = 'RGB' },
}

IsoSettings.toneMapItems = {
	{ title = LOC '$$$/Iso21496/TmReinhard=Reinhard  (default)', value = 'reinhard' },
	{ title = LOC '$$$/Iso21496/TmFilmic=Filmic', value = 'filmic' },
	{ title = LOC '$$$/Iso21496/TmClip=Hard clip', value = 'clip' },
}

IsoSettings.gammaItems = {
	{ title = '1.0', value = '1.0' },
	{ title = '2.2  (default)', value = '2.2' },
	{ title = '2.4', value = '2.4' },
}

IsoSettings.transferItems = {
	{ title = LOC '$$$/Iso21496/TfAuto=Detect from the rendered file', value = 'auto' },
	{ title = LOC '$$$/Iso21496/TfRomm=ProPhoto RGB  (ROMM curve)', value = 'romm' },
	{ title = LOC '$$$/Iso21496/TfLinear=Linear', value = 'linear' },
	{ title = LOC '$$$/Iso21496/TfSrgb=sRGB curve', value = 'srgb' },
	{ title = LOC '$$$/Iso21496/TfPq=Rec. 2100 PQ', value = 'pq' },
	{ title = LOC '$$$/Iso21496/TfHlg=Rec. 2100 HLG', value = 'hlg' },
}

IsoSettings.primariesItems = {
	{ title = LOC '$$$/Iso21496/PrAuto=Detect from the rendered file', value = 'auto' },
	{ title = 'ProPhoto RGB', value = 'prophoto' },
	{ title = 'Display P3', value = 'p3' },
	{ title = 'Rec. 2020', value = 'rec2020' },
	{ title = 'Adobe RGB', value = 'adobergb' },
	{ title = 'sRGB', value = 'srgb' },
}

local function clampNumber(value, low, high, fallback)
	local n = tonumber(value)
	if n == nil then return fallback end
	if n < low then return low end
	if n > high then return high end
	return n
end

--- Validates the property table and returns (ok, messageOrNil).
function IsoSettings.validate(properties)
	local headroom = tonumber(properties.iso_target_headroom)
	if headroom == nil or headroom <= 0 or headroom > 10 then
		return false, LOC '$$$/Iso21496/BadHeadroom=Target HDR headroom must be between 0 and 10 stops.'
	end
	local subsample = tonumber(properties.iso_gainmap_subsample)
	if subsample ~= 1 and subsample ~= 2 and subsample ~= 4 then
		return false, LOC '$$$/Iso21496/BadSubsample=Gain map subsampling must be 1, 2 or 4.'
	end
	local quality = tonumber(properties.iso_jpeg_quality)
	if quality == nil or quality < 1 or quality > 100 then
		return false, LOC '$$$/Iso21496/BadQuality=Baseline JPEG quality must be between 1 and 100.'
	end
	return true
end

--- Builds the encoder argument list (excluding --input/--output).
function IsoSettings.buildArguments(properties)
	local p = IsoSettings.applyDefaults(properties)
	local args = {
		'--headroom', tostring(clampNumber(p.iso_target_headroom, 0.1, 10, 4.0)),
		'--color-space', tostring(p.iso_color_space or 'DisplayP3'),
		'--subsample', tostring(math.floor(clampNumber(p.iso_gainmap_subsample, 1, 4, 2))),
		'--channels', (p.iso_gainmap_channels == 'RGB') and 'rgb' or 'mono',
		'--quality', tostring(math.floor(clampNumber(p.iso_jpeg_quality, 1, 100, 90))),
		'--gainmap-quality', tostring(math.floor(clampNumber(p.iso_gainmap_quality, 1, 100, 85))),
		'--gamma', tostring(clampNumber(p.iso_gainmap_gamma, 0.1, 8, 2.2)),
		'--tone-map', tostring(p.iso_tone_map or 'reinhard'),
		'--json',
	}

	if p.iso_auto_max_boost == false then
		args[#args + 1] = '--no-auto-max-boost'
	end
	if p.iso_chroma_subsample == false then
		args[#args + 1] = '--no-chroma-subsample'
	end
	if p.iso_copy_metadata == false then
		args[#args + 1] = '--no-exif'
	end
	if p.iso_input_transfer and p.iso_input_transfer ~= 'auto' then
		args[#args + 1] = '--input-transfer'
		args[#args + 1] = tostring(p.iso_input_transfer)
	end
	if p.iso_input_primaries and p.iso_input_primaries ~= 'auto' then
		args[#args + 1] = '--input-primaries'
		args[#args + 1] = tostring(p.iso_input_primaries)
	end
	if p.iso_input_transfer == 'pq' or p.iso_input_transfer == 'hlg' then
		args[#args + 1] = '--pq-diffuse-white'
		args[#args + 1] = tostring(clampNumber(p.iso_pq_diffuse_white, 10, 10000, 203))
	end
	return args
end

--- A one-line human summary for the collapsed export section.
function IsoSettings.summary(properties)
	local p = IsoSettings.applyDefaults(properties)
	return string.format('+%s EV / %s / gain map 1:%s %s / q%d',
		tostring(p.iso_target_headroom),
		tostring(p.iso_color_space),
		tostring(p.iso_gainmap_subsample),
		(p.iso_gainmap_channels == 'RGB') and 'RGB' or 'mono',
		math.floor(tonumber(p.iso_jpeg_quality) or 90))
end

return IsoSettings
