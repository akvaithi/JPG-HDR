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
	checkEqual(p.iso_target_headroom, '4.0', 'default headroom is +4 EV')
	checkEqual(p.iso_color_space, 'DisplayP3', 'default colour space is Display P3')
	checkEqual(p.iso_gainmap_subsample, '2', 'default gain map subsampling is 1:2')
	checkEqual(p.iso_gainmap_channels, 'Monochrome', 'default gain map is monochrome')
	checkEqual(p.iso_jpeg_quality, 90, 'default JPEG quality is 90')
	checkEqual(p.iso_gainmap_quality, 50, 'default gain map quality is 50')
	checkEqual(p.iso_sdr_lift, 0.43, 'default SDR lift is 0.43 EV')
	checkEqual(p.iso_sdr_contrast, 1.14, 'default SDR contrast is 1.14')
	checkEqual(p.iso_peak_detect, 'softened', 'highlight measurement is softened')

	local fields = IsoSettings.exportPresetFields()
	local seen = {}
	for _, field in ipairs(fields) do seen[field.key] = true end
	for _, key in ipairs { 'iso_target_headroom', 'iso_color_space',
	                       'iso_gainmap_subsample', 'iso_gainmap_channels',
	                       'iso_jpeg_quality' } do
		check(seen[key], 'preset fields include ' .. key)
	end
end

local function testArguments()
	local args = join(IsoSettings.buildArguments({}))
	check(args:find('--headroom 4%.0'), 'arguments carry the headroom')
	check(args:find('--color%-space DisplayP3'), 'arguments carry the colour space')
	check(args:find('--subsample 2'), 'arguments carry the subsample factor')
	check(args:find('--channels mono'), 'arguments carry the channel mode')
	check(args:find('--quality 90'), 'arguments carry the quality')
	check(args:find('--json'), 'arguments request the JSON report')
	check(not args:find('--no%-auto%-max%-boost'), 'auto max boost is on by default')
	check(args:find('--gainmap%-quality 50'), 'arguments carry the gain map quality')
	check(args:find('--sdr%-lift 0%.430'), 'arguments carry the SDR lift')
	check(args:find('--sdr%-contrast 1%.140'), 'arguments carry the SDR contrast')
	check(args:find('--peak%-detect softened'), 'arguments carry the peak detection mode')

	-- A neutral base must be reachable, and must stay neutral on the wire.
	local neutral = join(IsoSettings.buildArguments {
		iso_sdr_lift = 0, iso_sdr_contrast = 1.0,
	})
	check(neutral:find('--sdr%-lift 0%.000'), 'the lift can be switched off')
	check(neutral:find('--sdr%-contrast 1%.000'), 'the contrast can be switched off')

	-- Out-of-range shaping values must be clamped to what the encoder accepts,
	-- which rejects anything outside 0-3 EV and 0.5-2.0.
	local wild = join(IsoSettings.buildArguments {
		iso_sdr_lift = 99, iso_sdr_contrast = 9,
	})
	check(wild:find('--sdr%-lift 3%.000'), 'the lift is clamped to 3 EV')
	check(wild:find('--sdr%-contrast 2%.000'), 'the contrast is clamped to 2.0')

	local custom = IsoSettings.buildArguments {
		iso_target_headroom = '2.0',
		iso_color_space = 'sRGB',
		iso_gainmap_subsample = '4',
		iso_gainmap_channels = 'RGB',
		iso_jpeg_quality = 75,
		iso_auto_max_boost = false,
		iso_copy_metadata = false,
		iso_input_transfer = 'pq',
		iso_pq_diffuse_white = 100,
	}
	local text = join(custom)
	check(text:find('--headroom 2%.0'), 'custom headroom')
	check(text:find('--color%-space sRGB'), 'custom colour space')
	check(text:find('--subsample 4'), 'custom subsampling')
	check(text:find('--channels rgb'), 'RGB gain map selected')
	check(text:find('--quality 75'), 'custom quality')
	check(text:find('--no%-auto%-max%-boost'), 'auto max boost can be disabled')
	check(text:find('--no%-exif'), 'metadata copying can be disabled')
	check(text:find('--input%-transfer pq'), 'input transfer override')
	check(text:find('--pq%-diffuse%-white 100'), 'diffuse white accompanies PQ')

	-- Out-of-range values are clamped rather than passed through.
	local clamped = join(IsoSettings.buildArguments {
		iso_jpeg_quality = 5000, iso_gainmap_subsample = '9',
	})
	check(clamped:find('--quality 100'), 'quality is clamped to 100')
	check(clamped:find('--subsample 4'), 'subsampling is clamped to 4')
end

local function testValidation()
	local ok = IsoSettings.validate(IsoSettings.applyDefaults({}))
	check(ok, 'defaults validate')
	ok = IsoSettings.validate(IsoSettings.applyDefaults { iso_target_headroom = '0' })
	check(not ok, 'zero headroom is rejected')
	ok = IsoSettings.validate(IsoSettings.applyDefaults { iso_gainmap_subsample = '3' })
	check(not ok, 'a subsample factor of 3 is rejected')
	ok = IsoSettings.validate(IsoSettings.applyDefaults { iso_jpeg_quality = 0 })
	check(not ok, 'a quality of 0 is rejected')
end

local function testSummary()
	local s = IsoSettings.summary({})
	check(s:find('%+4%.0 EV'), 'summary shows the headroom')
	check(s:find('mono'), 'summary shows the gain map channels')
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
	checkEqual(report.gainChannels, 1, 'report says the gain map is monochrome')
	check(report.totalBytes > 0, 'report says bytes were written')

	-- The file must declare the headroom it needs, not the 4 EV ceiling the
	-- default settings allow: a decoder scales the gain it applies by
	-- display_headroom / declared_headroom.
	check(report.declaredHeadroom ~= nil and
		math.abs(report.declaredHeadroom - report.maxBoostLog2) < 0.001,
		'declared headroom matches the measured gain map maximum ('
			.. tostring(report.declaredHeadroom) .. ')')
	check(report.declaredHeadroom < 4.0, 'declared headroom is below the ceiling')
	check(report.minBoostLog2 < 0, 'the lifted base gives the gain map a negative floor')

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

testDefaults()
testArguments()
testValidation()
testSummary()
testEncoderPlumbing()
testReportParsing()

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
