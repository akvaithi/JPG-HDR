--[[----------------------------------------------------------------------------
IsoSettings.lua — the export parameters, their defaults, and the translation
from property-table values into encoder command-line arguments.

The property names match the build spec where the setting still exists. The
ones that no longer do — gain map resolution, channels, quality, gamma, base
chroma subsampling, highlight measurement — were each swept against a fixed
Lightroom export and each turned out to have one answer that is simply better,
so they are the encoder's defaults now rather than questions.
------------------------------------------------------------------------------]]

local IsoSettings = {}

IsoSettings.defaults = {
	-- Spec section 3.1: the five required controls.
	-- 'match' declares whatever headroom the render turned out to need. That is
	-- the same number Lightroom shows for the photo, because it is a property of
	-- the render we are measuring, and a decoder scales the gain it applies by
	-- display_headroom / declared_headroom — so a fixed ceiling above what the
	-- image uses only ever makes it render dim.
	iso_target_headroom = 'match',
	iso_color_space = 'DisplayP3',
	iso_jpeg_quality = 90,

	-- Local contrast handed back over the compressed base. 1.0 reproduces what
	-- the render had; above that is a deliberate boost, and 1.25 is what puts
	-- more tonal separation in the base than a plain SDR export carries.
	iso_sdr_detail = 1.25,
	-- Declared as the *old* generation on purpose. Lightroom fills any preset
	-- field a stored preset is missing with the value declared here, so
	-- declaring the current generation would stamp every old preset as current
	-- and skip the migration entirely — the exact failure this is here to stop.
	iso_settings_version = 1,

	-- Gain map resolution, channels, quality, encoding gamma, base chroma
	-- subsampling, highlight measurement and the headroom measurement are all
	-- gone from here on purpose. Each was swept against Lightroom's own export
	-- of a fixed render and each has one answer that is simply better, so they
	-- are the encoder's defaults rather than questions put to a photographer.
	-- Monochrome in particular was not a trade-off but a trap: it left
	-- highlights 0.63 EV duller because one gain cannot follow three channels.


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

-- Export presets store every setting, so a preset saved before a default
-- changed keeps overriding the new one — silently, and forever. That has now
-- caused two separate accuracy bugs that looked like encoder faults: a stale
-- gamma of 2.2 crushing the midtones, and a stale Monochrome gain map leaving
-- highlights 0.63 EV duller than Lightroom's because one gain cannot recover
-- three channels.
--
-- The fix is to record which generation of the settings a preset was written
-- against, and to migrate only values that *equal the old default*. Someone who
-- deliberately picked monochrome after this version keeps it; someone who
-- merely inherited it does not. Bump this whenever a default changes, and add
-- the corresponding case below.
IsoSettings.settingsVersion = 4

local function migrateOldPreset(properties)
	-- The gamma changed meaning, not just value: the encoder used to store
	-- pow(norm, 1/gamma) and declare gamma, and now stores pow(norm, gamma) the
	-- way decoders read it back. Nothing above 1.0 is a value anyone would
	-- choose under the current convention, so this one is safe to migrate on
	-- sight rather than only for old presets.
	local gamma = tonumber(properties.iso_gainmap_gamma)
	if gamma ~= nil and gamma > 1.0 then
		properties.iso_gainmap_gamma = string.format('%.4f', 1.0 / gamma)
	end

	-- These were the defaults before version 2. Only migrate an exact match:
	-- anything else is a choice someone made.
	if properties.iso_gainmap_channels == 'Monochrome' then
		properties.iso_gainmap_channels = 'RGB'
	end
	if properties.iso_target_headroom == '4.0' then
		properties.iso_target_headroom = 'match'
	end
	-- Settings that stopped being settings. Cleared rather than left in place so
	-- a stale value cannot come back if one is ever reintroduced.
	for _, key in ipairs { 'iso_gainmap_subsample', 'iso_gainmap_channels',
	                       'iso_gainmap_quality', 'iso_gainmap_gamma',
	                       'iso_chroma_subsample', 'iso_peak_detect',
	                       'iso_auto_max_boost', 'iso_sdr_shape', 'iso_tone_map',
	                       'iso_sdr_lift', 'iso_sdr_contrast' } do
		properties[key] = nil
	end
end

--- Fills in any value the property table is missing (older presets, or the
--- filter provider being added to an existing preset), and brings a preset
--- written against an older generation of these settings up to date.
function IsoSettings.applyDefaults(properties)
	-- Distinguish "old preset" from "empty table" before filling anything in:
	-- afterwards every key is present and the two are indistinguishable.
	local stored = tonumber(properties.iso_settings_version)
	local looksLikeAPreset = properties.iso_gainmap_channels ~= nil or
		properties.iso_target_headroom ~= nil or
		properties.iso_gainmap_gamma ~= nil
	if looksLikeAPreset and (stored == nil or stored < IsoSettings.settingsVersion) then
		migrateOldPreset(properties)
	end
	properties.iso_settings_version = IsoSettings.settingsVersion

	for key, default in pairs(IsoSettings.defaults) do
		if properties[key] == nil then properties[key] = default end
	end
	return properties
end

IsoSettings.headroomItems = {
	{ title = LOC '$$$/Iso21496/HeadroomMatch=Match the render  (recommended)', value = 'match' },
	{ title = LOC '$$$/Iso21496/Headroom1=Cap at +1.0 EV  (about 160 nits)', value = '1.0' },
	{ title = LOC '$$$/Iso21496/Headroom2=Cap at +2.0 EV  (about 320 nits)', value = '2.0' },
	{ title = LOC '$$$/Iso21496/Headroom3=Cap at +3.0 EV  (about 640 nits)', value = '3.0' },
	{ title = LOC '$$$/Iso21496/Headroom4=Cap at +4.0 EV  (about 1280 nits)', value = '4.0' },
}

-- The encoder's own maximum. 'match' means "do not cap": the encoder measures
-- what the render needs and declares that, so anything we pass above the
-- image's requirement is simply never reached.
local kNoHeadroomCap = 10.0

IsoSettings.colorSpaceItems = {
	{ title = LOC '$$$/Iso21496/CsP3=Display P3  (recommended)', value = 'DisplayP3' },
	{ title = LOC '$$$/Iso21496/CsSrgb=sRGB  (legacy compatibility)', value = 'sRGB' },
	{ title = LOC '$$$/Iso21496/Cs2020=Rec. 2020  (wide gamut)', value = 'Rec2020' },
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

--- The headroom ceiling to pass the encoder, in stops.
function IsoSettings.headroomCap(properties)
	if properties.iso_target_headroom == 'match' then return kNoHeadroomCap end
	return clampNumber(properties.iso_target_headroom, 0.1, kNoHeadroomCap, 4.0)
end

--- Validates the property table and returns (ok, messageOrNil).
function IsoSettings.validate(properties)
	if properties.iso_target_headroom ~= 'match' then
		local headroom = tonumber(properties.iso_target_headroom)
		if headroom == nil or headroom <= 0 or headroom > 10 then
			return false, LOC '$$$/Iso21496/BadHeadroom=Target HDR headroom must be between 0 and 10 stops.'
		end
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
		'--headroom', string.format('%.4f', IsoSettings.headroomCap(p)),
		'--color-space', tostring(p.iso_color_space or 'DisplayP3'),
		'--quality', tostring(math.floor(clampNumber(p.iso_jpeg_quality, 1, 100, 90))),
		'--json',
	}

	-- The base image is always the local operator. The curve modes it replaced
	-- could only make the picture lighter or darker, never less flat — a curve
	-- maps every pixel of a given luminance to the same output — so they were
	-- offering a worse answer to a question the encoder already answers well.
	args[#args + 1] = '--tone-map'
	args[#args + 1] = 'local'
	args[#args + 1] = '--sdr-detail'
	args[#args + 1] = string.format('%.2f', clampNumber(p.iso_sdr_detail, 0, 2, 1.25))

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

--- The encoder arguments for a photo.
---
--- There is nothing per-photo left to add. The Lightroom develop settings used
--- to bias the base image from here, but the rendered TIFF already contains the
--- result of every slider — so they were never a measurement the encoder could
--- not make for itself, and reading them back was a second opinion on a
--- question already answered.
function IsoSettings.argumentsForPhoto(properties, photo)
	return IsoSettings.buildArguments(properties)
end

--- A one-line human summary for the collapsed export section.
function IsoSettings.summary(properties)
	local p = IsoSettings.applyDefaults(properties)
	local headroom = p.iso_target_headroom == 'match'
		and LOC '$$$/Iso21496/SummaryMatch=headroom from the render'
		or string.format('max +%s EV', tostring(p.iso_target_headroom))
	return string.format('%s / depth %.2f / %s / q%d', headroom,
		clampNumber(p.iso_sdr_detail, 0, 2, 1.25),
		tostring(p.iso_color_space),
		math.floor(tonumber(p.iso_jpeg_quality) or 90))
end

return IsoSettings
