--[[----------------------------------------------------------------------------
IsoEncoder.lua — locating and running the bundled native encoder.

Everything platform specific lives here: which binary to use, how to quote a
command line for the shell Lightroom hands to LrTasks.execute, and how to get
the encoder's stdout and stderr back out of it.
------------------------------------------------------------------------------]]

local LrFileUtils = import 'LrFileUtils'
local LrPathUtils = import 'LrPathUtils'
local LrTasks = import 'LrTasks'

local IsoLogger = require 'IsoLogger'

local IsoEncoder = {}

local isWindows = WIN_ENV == true

--- Absolute path of the platform binary inside the plugin bundle.
function IsoEncoder.binaryPath()
	local dir = LrPathUtils.child(_PLUGIN.path, 'bin')
	if isWindows then
		return LrPathUtils.child(LrPathUtils.child(dir, 'windows'), 'iso21496_encoder.exe')
	end
	return LrPathUtils.child(LrPathUtils.child(dir, 'macOS'), 'iso21496_encoder')
end

local executableChecked = false

--- Confirms the binary is present and runnable. Returns (ok, message).
function IsoEncoder.checkAvailable()
	local path = IsoEncoder.binaryPath()
	if not LrFileUtils.exists(path) then
		return false, LOC('$$$/Iso21496/NoBinary=The ISO 21496-1 encoder is missing from the plug-in: ^1', path)
	end
	if not isWindows and not executableChecked then
		-- Zip archives and some download paths drop the executable bit.
		LrTasks.execute(string.format('chmod +x %q', path))
		executableChecked = true
	end
	return true
end

local function quote(value)
	if isWindows then
		-- cmd.exe: double quotes, and a literal " cannot appear in a path.
		return '"' .. tostring(value):gsub('"', '') .. '"'
	end
	return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function buildCommand(binary, args, stdoutPath, stderrPath)
	local parts = { quote(binary) }
	for _, a in ipairs(args) do parts[#parts + 1] = quote(a) end
	parts[#parts + 1] = '>'
	parts[#parts + 1] = quote(stdoutPath)
	parts[#parts + 1] = '2>'
	parts[#parts + 1] = quote(stderrPath)
	local command = table.concat(parts, ' ')
	if isWindows then
		-- cmd /c strips the outermost pair of quotes, so add one.
		command = '"' .. command .. '"'
	end
	return command
end

local function readAll(path)
	if not LrFileUtils.exists(path) then return '' end
	local ok, contents = pcall(LrFileUtils.readFile, path)
	return ok and contents or ''
end

--- Parses the flat one-line JSON object the encoder prints with --json.
function IsoEncoder.parseReport(text)
	if not text or text == '' then return nil end
	local body = text:match('%b{}')
	if not body then return nil end
	local report = {}
	for key, value in body:gmatch('"([%w_]+)"%s*:%s*"([^"]*)"') do
		report[key] = value
	end
	for key, value in body:gmatch('"([%w_]+)"%s*:%s*(-?[%d%.eE+]+)') do
		report[key] = tonumber(value)
	end
	for key, value in body:gmatch('"([%w_]+)"%s*:%s*(%a+)') do
		if value == 'true' then report[key] = true
		elseif value == 'false' then report[key] = false end
	end
	return report
end

--- Runs the encoder.
--- @param params table  { input, output, arguments }
--- @return boolean ok, table|nil report, string|nil errorMessage
function IsoEncoder.run(params)
	local ok, message = IsoEncoder.checkAvailable()
	if not ok then return false, nil, message end

	local binary = IsoEncoder.binaryPath()
	local stdoutPath = LrPathUtils.replaceExtension(params.output, 'encoder-out.tmp')
	local stderrPath = LrPathUtils.replaceExtension(params.output, 'encoder-err.tmp')

	local args = { '--input', params.input, '--output', params.output }
	for _, a in ipairs(params.arguments or {}) do args[#args + 1] = a end

	local command = buildCommand(binary, args, stdoutPath, stderrPath)
	IsoLogger.info('running: ' .. command)

	local exitCode = LrTasks.execute(command)
	local out = readAll(stdoutPath)
	local err = readAll(stderrPath)
	LrFileUtils.delete(stdoutPath)
	LrFileUtils.delete(stderrPath)

	if exitCode ~= 0 then
		local detail = err:match('error:%s*(.-)%s*$') or err
		if detail == nil or detail == '' then
			detail = LOC('$$$/Iso21496/ExitCode=the encoder exited with code ^1', tostring(exitCode))
		end
		IsoLogger.error('encoder failed: ' .. tostring(detail))
		return false, nil, detail
	end

	if not LrFileUtils.exists(params.output) then
		return false, nil, LOC '$$$/Iso21496/NoOutput=The encoder reported success but wrote no file.'
	end

	local report = IsoEncoder.parseReport(out)
	IsoLogger.info('encoded ' .. params.output ..
		(report and (' (' .. tostring(report.totalBytes) .. ' bytes)') or ''))
	return true, report
end

--- Version string of the bundled binary, or nil.
function IsoEncoder.version()
	local ok = IsoEncoder.checkAvailable()
	if not ok then return nil end
	local tmp = LrPathUtils.child(LrPathUtils.getStandardFilePath('temp'),
		'iso21496_version.txt')
	local command = buildCommand(IsoEncoder.binaryPath(), { '--version' }, tmp, tmp .. '.err')
	if LrTasks.execute(command) ~= 0 then return nil end
	local text = readAll(tmp)
	LrFileUtils.delete(tmp)
	LrFileUtils.delete(tmp .. '.err')
	return (text:gsub('%s+$', ''))
end

return IsoEncoder
