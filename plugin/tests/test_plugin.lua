--[[----------------------------------------------------------------------------
test_plugin.lua — exercises the plug-in's non-UI logic against the real native
encoder.

  lua5.4 plugin/tests/test_plugin.lua <path-to-iso21496_encoder> [tmpdir]

The binary is copied into the plug-in's bin/macOS slot first, so the test
covers IsoEncoder's real path resolution, shell quoting and report parsing.
------------------------------------------------------------------------------]]

local scriptDir = arg[0]:match('^(.*)[/\\][^/\\]*$') or '.'
local sourcePlugin = scriptDir .. '/../iso21496.lrdevplugin'
package.path = scriptDir .. '/?.lua;' .. package.path

local encoderBinary = arg[1]
local tmpDir = arg[2] or (os.getenv('TMPDIR') or '/tmp')
if not encoderBinary then
	io.stderr:write('usage: test_plugin.lua <iso21496_encoder> [tmpdir]\n')
	os.exit(2)
end

-- Work against a copy of the bundle: the harness pretends to be macOS, so
-- installing the binary into the real tree would leave a host binary in the
-- macOS slot for scripts/package.sh to pick up.
local pluginPath = tmpDir .. '/iso21496-plugin-test/iso21496.lrdevplugin'
os.execute(string.format('rm -rf %q && mkdir -p %q && cp -R %q/. %q',
	tmpDir .. '/iso21496-plugin-test', pluginPath, sourcePlugin, pluginPath))

local stubs = require 'lr_stubs'
stubs.install(pluginPath)

local failures = 0
local function check(condition, description)
	if condition then
		print('ok   ' .. description)
	else
		print('FAIL ' .. description)
		failures = failures + 1
	end
end

local function checkEqual(got, want, description)
	check(got == want,
		description .. ' (got ' .. tostring(got) .. ', want ' .. tostring(want) .. ')')
end

-- Put the freshly built binary where the plug-in expects to find it.
local function installBinary()
	local binDir = pluginPath .. '/bin/macOS'
	local target = binDir .. '/iso21496_encoder'
	os.execute(string.format('mkdir -p %q && cp %q %q && chmod +x %q',
		binDir, encoderBinary, target, target))
	return target
end

local target = installBinary()

local IsoSettings = require 'IsoSettings'
local IsoEncoder = require 'IsoEncoder'

--------------------------------------------------------------------- settings

local function join(list) return table.concat(list, ' ') end

local function testDefaults()
	local p = IsoSettings.applyDefaults({})
	checkEqual(p.iso_target_headroom, 'match', 'headroom defaults to matching the render')
	checkEqual(p.iso_color_space, 'DisplayP3', 'default colour space is Display P3')
	checkEqual(p.iso_jpeg_quality, 90, 'default JPEG quality is 90')

	local fields = IsoSettings.exportPresetFields()
	local seen = {}
	for _, field in ipairs(fields) do seen[field.key] = true end
	for _, key in ipairs { 'iso_target_headroom', 'iso_color_space',
	                       'iso_jpeg_quality' } do
		check(seen[key], 'preset fields include ' .. key)
	end
end

local function testArguments()
	local args = join(IsoSettings.buildArguments({}))
	check(args:find('--headroom 10%.0'), 'matching the render passes no real cap')
	check(args:find('--color%-space DisplayP3'), 'arguments carry the colour space')
	check(args:find('--quality 90'), 'arguments carry the quality')
	check(args:find('--json'), 'arguments request the JSON report')

	-- Automatic means the local operator. A curve cannot give the base depth, so
	-- the default must not silently be one: passing --tone-map reinhard here
	-- would quietly turn the whole feature off.
	check(args:find('--tone%-map local'), 'the automatic base asks for local tone mapping')
	check(args:find('--sdr%-detail 1%.25'), 'the automatic base carries the depth amount')
	check(not args:find('--sdr%-lift'), 'no lift is passed: the operator has none')
	check(not args:find('--sdr%-contrast'), 'no contrast is passed either')
	check(not args:find('--tone%-map reinhard'), 'the base image is never a curve')

	-- Everything with one measured right answer is the encoder's default now.
	-- Passing it from here would let a preset put a worse value back.
	-- --subsample is no longer on this list: it travels with --apple-compatible,
	-- which pairs the metadata with the half resolution map that was carried
	-- through every platform rather than leaving the two to drift apart.
	for _, flag in ipairs { '--channels', '--gainmap%-quality',
	                       '--gamma', '--peak%-detect', '--no%-chroma%-subsample',
	                       '--no%-auto%-max%-boost', '--sdr%-lift', '--sdr%-contrast' } do
		check(not args:find(flag), 'the plug-in does not pass ' .. flag:gsub('%%', ''))
	end

	-- Not a setting any more: this is the only shape confirmed to render on iOS,
	-- Android and Google Photos and to survive an iMessage send, so no preset may
	-- turn it off. Passing iso_apple_compatible = false must change nothing.
	check(args:find('--apple%-compatible'), 'every export carries Apple\'s description')
	check(args:find('--subsample 2'), 'and a half resolution gain map')
	local forced = join(IsoSettings.buildArguments { iso_apple_compatible = false })
	check(forced:find('--apple%-compatible'),
		'a stored preset cannot turn iMessage compatibility off')
	check(forced:find('--subsample 2'), 'nor the half resolution map')

	-- The depth amount is clamped to what the encoder accepts.
	local deep = join(IsoSettings.buildArguments { iso_sdr_detail = 99 })
	check(deep:find('--sdr%-detail 2%.00'), 'the depth amount is clamped to 2')

	local custom = IsoSettings.buildArguments {
		iso_target_headroom = '2.0',
		iso_color_space = 'sRGB',
		iso_jpeg_quality = 75,
		iso_copy_metadata = false,
		iso_input_transfer = 'pq',
		iso_pq_diffuse_white = 100,
	}
	local text = join(custom)
	check(text:find('--headroom 2%.0'), 'a headroom cap is passed as the cap')
	check(text:find('--color%-space sRGB'), 'custom colour space')
	check(text:find('--quality 75'), 'custom quality')
	check(text:find('--no%-exif'), 'metadata copying can be disabled')
	check(text:find('--input%-transfer pq'), 'input transfer override')
	check(text:find('--pq%-diffuse%-white 100'), 'diffuse white accompanies PQ')

	-- Out-of-range values are clamped rather than passed through.
	local clamped = join(IsoSettings.buildArguments {
		iso_jpeg_quality = 5000,
	})
	check(clamped:find('--quality 100'), 'quality is clamped to 100')
end

-- A preset written before a default changed keeps overriding it, silently and
-- forever. Only values that equal the old default may be migrated: a deliberate
-- choice has to survive.
local function testPresetMigration()
	local old = IsoSettings.applyDefaults {
		iso_gainmap_channels = 'Monochrome',
		iso_target_headroom = '4.0',
		iso_gainmap_gamma = '2.2',
		iso_sdr_shape = 'manual',
		iso_sdr_lift = 0.9,
	}
	checkEqual(old.iso_sdr_shape, nil, 'a stale base image mode is cleared')
	checkEqual(old.iso_sdr_lift, nil, 'a stale manual lift is cleared')
	checkEqual(old.iso_gainmap_channels, nil,
		'a stale gain map channel setting is cleared, not honoured')
	checkEqual(old.iso_gainmap_gamma, nil, 'a stale gamma is cleared')
	-- Was a checkbox in version 5. A preset holding it false would otherwise keep
	-- producing files that arrive flat over iMessage.
	local wasOff = IsoSettings.applyDefaults {
		iso_target_headroom = 'match', iso_apple_compatible = false,
		iso_settings_version = 5,
	}
	checkEqual(wasOff.iso_apple_compatible, nil,
		'a preset that turned off iMessage compatibility is cleared')
	checkEqual(old.iso_target_headroom, 'match', 'a stale +4 EV cap migrates to matching the render')
	checkEqual(old.iso_settings_version, IsoSettings.settingsVersion,
		'the preset is stamped with the current generation')

	-- Lightroom fills a missing preset field with the value declared in the
	-- defaults, so that declared value must be the *old* generation or every
	-- stale preset would be stamped current and skip migration entirely.
	check(IsoSettings.defaults.iso_settings_version < IsoSettings.settingsVersion,
		'the declared version default is older than the current generation')
	local viaLightroom = IsoSettings.applyDefaults {
		iso_gainmap_channels = 'Monochrome',
		iso_settings_version = IsoSettings.defaults.iso_settings_version,
	}
	checkEqual(viaLightroom.iso_gainmap_channels, nil,
		'a preset carrying the declared version default still migrates')

	-- A preset already at this generation is left alone, even where its values
	-- match what the old defaults happened to be.
	local deliberate = IsoSettings.applyDefaults {
		iso_settings_version = IsoSettings.settingsVersion,
		iso_target_headroom = '4.0',
	}
	checkEqual(deliberate.iso_sdr_shape, nil, 'the base image mode is gone entirely')
	checkEqual(deliberate.iso_target_headroom, '4.0', 'a deliberate cap survives')

	-- Values that were never the old default are choices, and are kept.
	local chosen = IsoSettings.applyDefaults { iso_target_headroom = '2.0' }
	checkEqual(chosen.iso_target_headroom, '2.0', 'a non-default cap is not migrated')

	-- A fresh export is not a preset and must simply get the defaults.
	local fresh = IsoSettings.applyDefaults({})
	checkEqual(fresh.iso_target_headroom, 'match', 'a fresh export matches the render')

	-- Migration runs on every export, so it has to be idempotent.
	local twice = IsoSettings.applyDefaults(IsoSettings.applyDefaults {
		iso_gainmap_channels = 'Monochrome', iso_target_headroom = '4.0',
	})
	checkEqual(twice.iso_gainmap_channels, nil, 'migrating twice is stable')
	checkEqual(twice.iso_target_headroom, 'match', 'and does not migrate twice')
end

local function testValidation()
	local ok = IsoSettings.validate(IsoSettings.applyDefaults({}))
	check(ok, 'defaults validate')
	ok = IsoSettings.validate(IsoSettings.applyDefaults { iso_target_headroom = 'match' })
	check(ok, 'matching the render validates')
	ok = IsoSettings.validate(IsoSettings.applyDefaults { iso_target_headroom = '0' })
	check(not ok, 'zero headroom is rejected')
	ok = IsoSettings.validate(IsoSettings.applyDefaults { iso_jpeg_quality = 0 })
	check(not ok, 'a quality of 0 is rejected')
end

local function testSummary()
	local s = IsoSettings.summary({})
	check(s:find('headroom from the render'), 'summary shows where the headroom comes from')
	check(s:find('depth 1%.25'), 'summary shows the depth amount')
	check(IsoSettings.summary { iso_target_headroom = '2.0' }:find('max %+2%.0 EV'),
		'summary shows a headroom cap when one is set')
end

---------------------------------------------------------------------- encoder

local function testEncoderPlumbing()
	checkEqual(IsoEncoder.binaryPath(), target, 'binary path resolves into the bundle')
	local ok = IsoEncoder.checkAvailable()
	check(ok, 'the bundled binary is detected')
	local version = IsoEncoder.version()
	check(version ~= nil and version:match('^%d+%.%d+%.%d+$') ~= nil,
		'reported encoder version looks like a version (' .. tostring(version) .. ')')
end

-- Lightroom builds dialog sections on the main thread, and the stub raises the
-- way Lightroom does if anything shells out there. Both providers used to, and
-- the Plug-in Manager logged "We can only wait from within a task" / "Yielding
-- is not allowed within a C or metamethod call" instead of drawing the UI.
local function testDialogsBuildOutsideATask()
	IsoEncoder.forget()
	stubs.inTask = false

	check(IsoEncoder.checkAvailable(), 'the binary is still detected outside a task')
	check(IsoEncoder.version() == nil, 'the version is not read outside a task')

	local f = stubs.viewFactory()

	local ExportDialogSections = require 'ExportDialogSections'
	local properties = IsoSettings.applyDefaults({})
	local built, err = pcall(ExportDialogSections.sectionsForBottomOfDialog, f, properties)
	check(built, 'the export dialog sections build outside a task'
		.. (built and '' or ': ' .. tostring(err)))

	local PluginInfoProvider = require 'PluginInfoProvider'
	local infoTable = {}
	local infoBuilt, infoErr = pcall(PluginInfoProvider.sectionsForTopOfDialog, f, infoTable)
	check(infoBuilt, 'the Plug-in Manager section builds outside a task'
		.. (infoBuilt and '' or ': ' .. tostring(infoErr)))

	-- Building the sections starts tasks, which is where the encoder is allowed
	-- to run. A started task has not necessarily finished — that is the whole
	-- point of one — so drain them before asking what they produced.
	stubs.drainTasks()
	-- the values they were waiting on must actually have arrived.
	check(properties.iso_encoder_version ~= nil and
		properties.iso_encoder_version:match('^%d+%.%d+%.%d+$') ~= nil,
		'the export dialog resolves the encoder version from a task ('
			.. tostring(properties.iso_encoder_version) .. ')')
	-- Match a version rather than a particular one: pinning the major number
	-- here made a release bump look like a broken Plug-in Manager.
	check(infoTable.isoStatus ~= nil and
		infoTable.isoStatus:find('%d+%.%d+%.%d+') ~= nil,
		'the Plug-in Manager resolves the encoder status from a task ('
			.. tostring(infoTable.isoStatus) .. ')')

	-- The version is display state, not a setting: writing it into a preset
	-- would put a machine-specific string in every exported preset file.
	local presetKeys = {}
	for _, field in ipairs(IsoSettings.exportPresetFields()) do
		presetKeys[field.key] = true
	end
	check(not presetKeys.iso_encoder_version,
		'the encoder version is not an export preset field')

	-- Cached now, so the main thread can read it without shelling out again.
	check(IsoEncoder.version() ~= nil, 'the version is cached for the main thread')

	stubs.inTask = true
end

local function testReportParsing()
	local report = IsoEncoder.parseReport(
		'{"ok":true,"width":100,"height":50,"inputTransfer":"linear","seconds":0.25}')
	check(report ~= nil, 'a JSON report parses')
	checkEqual(report.width, 100, 'report width')
	checkEqual(report.inputTransfer, 'linear', 'report string field')
	checkEqual(report.ok, true, 'report boolean field')
	check(math.abs(report.seconds - 0.25) < 1e-9, 'report float field')
	check(IsoEncoder.parseReport('') == nil, 'an empty report yields nil')
	check(IsoEncoder.parseReport('not json') == nil, 'garbage yields nil')
end

local function testRealEncode(fixtureTiff)
	local out = tmpDir .. '/plugin_test_output.jpg'
	os.remove(out)
	local ok, report, err = IsoEncoder.run {
		input = fixtureTiff,
		output = out,
		arguments = IsoSettings.buildArguments({}),
	}
	check(ok, 'encoding a real TIFF succeeds' .. (ok and '' or ': ' .. tostring(err)))
	if not ok then return end
	check(report ~= nil, 'the encoder returned a report')

	-- The default path is the local operator, which has no lift or contrast to
	-- solve — reporting that it did would be a lie, and the gain map floor is
	-- exactly zero because the operator never brightens the base.
	checkEqual(report.autoShaped, false, 'no curve solver runs')
	checkEqual(report.sdrLiftEV, 0, 'local tone mapping applies no lift')
	checkEqual(report.sdrContrast, 1, 'local tone mapping applies no contrast')
	checkEqual(report.minBoostLog2, 0, 'the local operator never needs a negative gain')

	-- One channel, because the plug-in's default is the shareable file and
	-- Apple's pipeline accepts nothing else. The encoder on its own still
	-- defaults to three; this is the plug-in's choice, made because a file that
	-- arrives flat over iMessage is worse than one 0.25 EV short in saturated
	-- highlights. Unchecking the box gets the three-channel map back.
	checkEqual(report.gainChannels, 1, 'the plug-in default is a single-channel gain map')
	check(report.totalBytes > 0, 'report says bytes were written')

	-- The file must declare the headroom it needs, not the 4 EV ceiling the
	-- default settings allow: a decoder scales the gain it applies by
	-- display_headroom / declared_headroom.
	-- The declared headroom is what the picture needs; the gain map's maximum is
	-- what the base needs to get back there. They are related but not equal, and
	-- forcing them equal is what truncated saturated highlights.
	check(report.declaredHeadroom ~= nil and
		math.abs(report.declaredHeadroom - report.measuredHeadroom) < 0.001,
		'declared headroom is the measured headroom ('
			.. tostring(report.declaredHeadroom) .. ')')
	check(report.maxBoostLog2 > 0, 'the gain map spans a real range')
	check(report.declaredHeadroom < 4.0, 'declared headroom is below the ceiling')
	-- The local operator never brightens the base, so nothing needs darkening.
	checkEqual(report.minBoostLog2, 0, 'the local operator needs no negative gain')

	local f = io.open(out, 'rb')
	local data = f:read('a')
	f:close()
	check(data:sub(1, 2) == '\255\216', 'the output starts with a JPEG SOI marker')
	check(data:find('urn:iso:std:iso:ts:21496:-1', 1, true) ~= nil,
		'the output carries the ISO 21496-1 URN')
	check(data:find('MPF\0', 1, true) ~= nil, 'the output carries an MPF index')
	checkEqual(#data, report.totalBytes, 'the file size matches the report')

	-- A path with spaces and quotes must survive the shell quoting.
	local awkward = tmpDir .. "/plugin test's output.jpg"
	os.remove(awkward)
	local ok2 = IsoEncoder.run {
		input = fixtureTiff,
		output = awkward,
		arguments = IsoSettings.buildArguments({}),
	}
	check(ok2, 'a path containing spaces and a quote is handled')
	os.remove(awkward)
	os.remove(out)
end

local function testEncoderFailureIsReported()
	local ok, _, err = IsoEncoder.run {
		input = tmpDir .. '/definitely_not_here.tif',
		output = tmpDir .. '/never_written.jpg',
		arguments = IsoSettings.buildArguments({}),
	}
	check(not ok, 'a missing input is reported as a failure')
	check(err ~= nil and err:find('cannot open input file') ~= nil,
		'the encoder error message reaches the caller (' .. tostring(err) .. ')')
end

-- The export loop, driven end to end against fake renditions.
--
-- It encodes on a pool of tasks rather than one photo at a time, which means
-- output names are claimed before a worker starts and the loop has to drain
-- what is still in flight when the renditions run out. Neither is visible from
-- the outside until an export drops a photo or two workers write to the same
-- file, so it is worth a test even though the concurrency itself is
-- Lightroom's.
local function testExportLoop()
	local ExportServiceProvider = require 'ExportServiceProvider'
	-- An export runs inside a task; an earlier test leaves this false.
	stubs.inTask = true

	local rendered, failed = {}, {}
	local renditions = {}
	local intermediates = {}

	-- Three ordinary ones, then the collision. Lightroom hands out unique
	-- intermediates, so two .jpg names can only clash after uniquifying: dup.jpg
	-- already exists on disk, so dup.tif becomes dup-1.jpg — which is exactly
	-- the name dup-1.tif derives for itself. Serially that is invisible, because
	-- the first file exists by the time the second is named. On a pool it is a
	-- lost photo.
	local names = { 'pool_1', 'pool_2', 'pool_3', 'dup', 'dup-1' }
	local existing = io.open(tmpDir .. '/dup.jpg', 'wb')
	existing:write('an earlier export')
	existing:close()

	for i, name in ipairs(names) do
		local path = tmpDir .. '/' .. name .. '.tif'
		intermediates[i] = path
		local f = io.open(path, 'wb')
		f:write('not really a tiff')
		f:close()
		renditions[i] = {
			photo = { getFormattedMetadata = function() return 'photo.dng' end },
			waitForRender = function() return true, path end,
			uploadFailed = function(_, reason) failed[#failed + 1] = reason end,
		}
	end

	local captions = {}
	local exportContext = {
		exportSession = { countRenditions = function() return #renditions end },
		-- Enough that intermediateProblems finds nothing and the loop is not
		-- interrupted by a confirmation dialog.
		propertyTable = { iso_workers = 3, iso_keep_intermediate = false,
		                  iso_add_to_catalog = false, LR_format = 'TIFF',
		                  LR_export_bitDepth = 16, LR_export_useHDR = true },
		configureProgress = function(_, _)
			return {
				setPortionComplete = function() end,
				setCaption = function(_, c) captions[#captions + 1] = c end,
				isCanceled = function() return false end,
			}
		end,
		renditions = function()
			local i = 0
			return function()
				i = i + 1
				if renditions[i] then return i, renditions[i] end
			end
		end,
	}

	-- The encoder is not the subject here; record the calls and succeed.
	local realRun = IsoEncoder.run
	IsoEncoder.run = function(spec)
		rendered[#rendered + 1] = spec.output
		local f = io.open(spec.output, 'wb')
		f:write('jpeg')
		f:close()
		return true, { totalBytes = 4, primaryBytes = 3, gainMapBytes = 1,
		               maxBoostLog2 = 1.0 }
	end

	local ok, err = pcall(function()
		ExportServiceProvider.processRenderedPhotos(nil, exportContext)
	end)
	IsoEncoder.run = realRun

	check(ok, 'the export loop runs to completion (' .. tostring(err) .. ')')
	checkEqual(#rendered, #renditions, 'every rendition was encoded')

	local seen, duplicate = {}, nil
	for _, path in ipairs(rendered) do
		if seen[path] then duplicate = path end
		seen[path] = true
	end
	check(duplicate == nil,
		'no two workers were given the same output path (' ..
		tostring(duplicate) .. ')')

	check(not seen[tmpDir .. '/dup.jpg'],
		'an existing .jpg from an earlier export is not overwritten')

	local leftOver = 0
	for _, path in ipairs(intermediates) do
		local f = io.open(path, 'rb')
		if f then f:close(); leftOver = leftOver + 1 end
	end
	checkEqual(leftOver, 0, 'every intermediate was deleted')

	for _, path in ipairs(rendered) do os.remove(path) end
	os.remove(tmpDir .. '/dup.jpg')
end

testDefaults()
testArguments()
testValidation()
testPresetMigration()
testSummary()
testEncoderPlumbing()
testDialogsBuildOutsideATask()
testReportParsing()
testExportLoop()

local fixture = arg[3]
if fixture then
	testRealEncode(fixture)
else
	print('skip fixture-based encode test (no TIFF given)')
end
testEncoderFailureIsReported()

if failures > 0 then
	io.stderr:write(string.format('%d check(s) failed\n', failures))
	os.exit(1)
end
print('all plug-in checks passed')
