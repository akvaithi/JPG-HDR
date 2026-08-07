--[[----------------------------------------------------------------------------
ExportDialogSections.lua — the custom controls Lightroom draws inside the
Export dialog, shared by the export service and the export filter.

Section 3.1 of the build spec defines the five required controls; everything
else lives in a collapsed "Advanced" section so the common case stays simple.
------------------------------------------------------------------------------]]

local LrView = import 'LrView'
local LrColor = import 'LrColor'

local IsoSettings = require 'IsoSettings'
local IsoEncoder = require 'IsoEncoder'

local ExportDialogSections = {}

--- The main settings section.
function ExportDialogSections.mainSection(f, properties)
	IsoSettings.applyDefaults(properties)

	local bind = LrView.bind
	local share = LrView.share

	return {
		title = LOC '$$$/Iso21496/SectionTitle=ISO 21496-1 HDR Gain Map',
		-- Every key the summary reads has to be listed, or the collapsed
		-- header goes stale when one of the others changes.
		synopsis = bind {
			keys = {
				'iso_target_headroom', 'iso_color_space',
				'iso_gainmap_subsample', 'iso_gainmap_channels',
				'iso_jpeg_quality',
			},
			operation = function() return IsoSettings.summary(properties) end,
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/HeadroomLabel=Target HDR headroom:',
				alignment = 'right',
				width = share 'iso_label_width',
			},
			f:popup_menu {
				value = bind 'iso_target_headroom',
				items = IsoSettings.headroomItems,
				width_in_chars = 24,
			},
		},
		f:row {
			spacing = f:label_spacing(),
			f:static_text { title = '', width = share 'iso_label_width' },
			f:static_text {
				title = LOC '$$$/Iso21496/HeadroomHint=Peak brightness the gain map may reach on an HDR display. Files still look correct on SDR screens.',
				width_in_chars = 46,
				height_in_lines = 2,
				text_color = LrColor(0.4, 0.4, 0.4),
			},
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/ColorSpaceLabel=Base colour space:',
				alignment = 'right',
				width = share 'iso_label_width',
			},
			f:popup_menu {
				value = bind 'iso_color_space',
				items = IsoSettings.colorSpaceItems,
				width_in_chars = 24,
			},
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/SubsampleLabel=Gain map resolution:',
				alignment = 'right',
				width = share 'iso_label_width',
			},
			f:popup_menu {
				value = bind 'iso_gainmap_subsample',
				items = IsoSettings.subsampleItems,
				width_in_chars = 24,
			},
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/ChannelsLabel=Gain map channels:',
				alignment = 'right',
				width = share 'iso_label_width',
			},
			f:popup_menu {
				value = bind 'iso_gainmap_channels',
				items = IsoSettings.channelItems,
				width_in_chars = 24,
			},
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/QualityLabel=Baseline JPEG quality:',
				alignment = 'right',
				width = share 'iso_label_width',
			},
			f:slider {
				value = bind 'iso_jpeg_quality',
				min = 60,
				max = 100,
				integral = true,
				width = 200,
			},
			f:edit_field {
				value = bind 'iso_jpeg_quality',
				min = 60,
				max = 100,
				precision = 0,
				width_in_digits = 4,
			},
		},
	}
end

--- The collapsed advanced section.
function ExportDialogSections.advancedSection(f, properties)
	local bind = LrView.bind
	local share = LrView.share

	return {
		title = LOC '$$$/Iso21496/AdvancedTitle=ISO 21496-1 Advanced',
		synopsis = LOC '$$$/Iso21496/AdvancedSynopsis=Tone mapping, gain map encoding, intermediate handling',

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/ToneMapLabel=Tone mapping:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:popup_menu {
				value = bind 'iso_tone_map',
				items = IsoSettings.toneMapItems,
				width_in_chars = 22,
			},
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/GainQualityLabel=Gain map quality:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:slider {
				value = bind 'iso_gainmap_quality',
				min = 50,
				max = 100,
				integral = true,
				width = 160,
			},
			f:edit_field {
				value = bind 'iso_gainmap_quality',
				min = 50,
				max = 100,
				precision = 0,
				width_in_digits = 4,
			},
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/GammaLabel=Gain map gamma:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:popup_menu {
				value = bind 'iso_gainmap_gamma',
				items = IsoSettings.gammaItems,
				width_in_chars = 14,
			},
		},

		f:row {
			f:static_text { title = '', width = share 'iso_adv_label_width' },
			f:checkbox {
				title = LOC '$$$/Iso21496/AutoMaxBoost=Fit the gain map range to the image',
				value = bind 'iso_auto_max_boost',
			},
		},
		f:row {
			f:static_text { title = '', width = share 'iso_adv_label_width' },
			f:checkbox {
				title = LOC '$$$/Iso21496/ChromaSubsample=Chroma subsample the base image (4:2:0)',
				value = bind 'iso_chroma_subsample',
			},
		},
		f:row {
			f:static_text { title = '', width = share 'iso_adv_label_width' },
			f:checkbox {
				title = LOC '$$$/Iso21496/CopyMetadata=Copy Exif, GPS and copyright from the render',
				value = bind 'iso_copy_metadata',
			},
		},

		f:separator { fill_horizontal = 1 },

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/TransferLabel=Rendered encoding:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:popup_menu {
				value = bind 'iso_input_transfer',
				items = IsoSettings.transferItems,
				width_in_chars = 26,
			},
		},
		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/PrimariesLabel=Rendered primaries:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:popup_menu {
				value = bind 'iso_input_primaries',
				items = IsoSettings.primariesItems,
				width_in_chars = 26,
			},
		},
		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/DiffuseWhiteLabel=PQ/HLG diffuse white:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:edit_field {
				value = bind 'iso_pq_diffuse_white',
				min = 10,
				max = 10000,
				precision = 0,
				width_in_digits = 6,
				enabled = bind {
					key = 'iso_input_transfer',
					transform = function(value) return value == 'pq' or value == 'hlg' end,
				},
			},
			f:static_text { title = LOC '$$$/Iso21496/Nits=nits' },
		},

		f:separator { fill_horizontal = 1 },

		f:row {
			f:static_text { title = '', width = share 'iso_adv_label_width' },
			f:checkbox {
				title = LOC '$$$/Iso21496/AddToCatalog=Add the exported JPEGs to this catalogue',
				value = bind 'iso_add_to_catalog',
			},
		},
		f:row {
			f:static_text { title = '', width = share 'iso_adv_label_width' },
			f:checkbox {
				title = LOC '$$$/Iso21496/KeepIntermediate=Keep the intermediate TIFF (for troubleshooting)',
				value = bind 'iso_keep_intermediate',
			},
		},

		f:row {
			f:static_text { title = '', width = share 'iso_adv_label_width' },
			f:static_text {
				title = LOC('$$$/Iso21496/EncoderVersion=Bundled encoder: ^1',
					IsoEncoder.version() or LOC '$$$/Iso21496/EncoderMissing=not found'),
				text_color = LrColor(0.4, 0.4, 0.4),
			},
		},
	}
end

--- Both sections, in display order.
function ExportDialogSections.sectionsForBottomOfDialog(f, properties)
	return {
		ExportDialogSections.mainSection(f, properties),
		ExportDialogSections.advancedSection(f, properties),
	}
end

return ExportDialogSections
