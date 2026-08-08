--[[----------------------------------------------------------------------------
ExportDialogSections.lua — the custom controls Lightroom draws inside the
Export dialog, shared by the export service and the export filter.

Section 3.1 of the build spec defines the five required controls; everything
else lives in a collapsed "Advanced" section so the common case stays simple.
------------------------------------------------------------------------------]]

local LrView = import 'LrView'
local LrColor = import 'LrColor'
local LrTasks = import 'LrTasks'

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
				'iso_target_headroom', 'iso_sdr_detail', 'iso_color_space',
				'iso_jpeg_quality',
			},
			operation = function() return IsoSettings.summary(properties) end,
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/HeadroomLabel=HDR headroom:',
				alignment = 'right',
				width = share 'iso_label_width',
			},
			f:popup_menu {
				value = bind 'iso_target_headroom',
				items = IsoSettings.headroomItems,
				width_in_chars = 28,
			},
		},
		f:row {
			spacing = f:label_spacing(),
			f:static_text { title = '', width = share 'iso_label_width' },
			f:static_text {
				title = LOC '$$$/Iso21496/HeadroomHint=Files declare the headroom the photo actually needs, which is the figure Lightroom shows for it. Cap it only to hold a set down deliberately.',
				width_in_chars = 46,
				height_in_lines = 3,
				text_color = LrColor(0.4, 0.4, 0.4),
			},
		},

		f:row {
			spacing = f:label_spacing(),
			f:static_text { title = '', width = share 'iso_label_width' },
			f:static_text {
				title = LOC '$$$/Iso21496/ShapeHint=Brightness and contrast of the rendition seen without an HDR display, solved from each photo. The HDR result is unaffected either way.',
				width_in_chars = 46,
				height_in_lines = 3,
				text_color = LrColor(0.4, 0.4, 0.4),
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

	-- Asking the encoder its version means running it, which yields, and this
	-- section is built on the main thread — doing it inline fails with
	-- "Yielding is not allowed within a C or metamethod call" and Lightroom
	-- refuses to draw the plug-in's part of the Export dialog. The key is
	-- deliberately not an export preset field: it is display state, not a
	-- setting, and must not be written into presets.
	properties.iso_encoder_version = LOC '$$$/Iso21496/EncoderChecking=checking…'
	LrTasks.startAsyncTask(function()
		properties.iso_encoder_version = IsoEncoder.version()
			or LOC '$$$/Iso21496/EncoderMissing=not found'
	end)

	return {
		title = LOC '$$$/Iso21496/AdvancedTitle=ISO 21496-1 Advanced',
		synopsis = LOC '$$$/Iso21496/AdvancedSynopsis=Colour space, base image look, intermediate handling',

		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/ColorSpaceLabel=Base colour space:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:popup_menu {
				value = bind 'iso_color_space',
				items = IsoSettings.colorSpaceItems,
				width_in_chars = 26,
			},
		},

		f:separator { fill_horizontal = 1 },


		f:row {
			f:static_text {
				title = LOC '$$$/Iso21496/SdrHeading=SDR base image look',
				font = '<system/bold>',
			},
		},
		f:row {
			f:static_text {
				title = LOC '$$$/Iso21496/SdrHint=The rendition seen without full HDR headroom. Every display short of the file\'s own headroom shows a blend anchored on it, so this is not only the SDR fallback. The HDR rendition itself is unaffected.',
				width_in_chars = 58,
				height_in_lines = 2,
				text_color = LrColor(0.4, 0.4, 0.4),
			},
		},
		f:row {
			spacing = f:label_spacing(),
			f:static_text {
				title = LOC '$$$/Iso21496/DetailLabel=Depth:',
				alignment = 'right',
				width = share 'iso_adv_label_width',
			},
			f:slider {
				value = bind 'iso_sdr_detail',
				min = 0,
				max = 2,
				width = 160,
				enabled = automaticShaping,
			},
			f:edit_field {
				value = bind 'iso_sdr_detail',
				min = 0,
				max = 2,
				precision = 2,
				width_in_digits = 5,
				enabled = automaticShaping,
			},
		},
		f:row {
			f:static_text { title = '', width = share 'iso_adv_label_width' },
			f:static_text {
				title = LOC '$$$/Iso21496/DetailHint=How much local contrast the base image keeps. 1.0 is what the render had; higher trades a flatter fallback for one that carries the separation an HDR display shows.',
				width_in_chars = 58,
				height_in_lines = 3,
				text_color = LrColor(0.4, 0.4, 0.4),
			},
		},

		f:separator { fill_horizontal = 1 },




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
				title = bind {
					key = 'iso_encoder_version',
					transform = function(value)
						return LOC('$$$/Iso21496/EncoderVersion=Bundled encoder: ^1',
							tostring(value))
					end,
				},
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
