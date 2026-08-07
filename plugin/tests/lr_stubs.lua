--[[----------------------------------------------------------------------------
lr_stubs.lua — just enough of the Lightroom Classic SDK to run the plug-in's
non-UI modules under a plain Lua interpreter.

This is a test harness, not an emulator: it covers the handful of Lr* calls
IsoSettings and IsoEncoder make, so their behaviour (argument construction,
shell quoting, report parsing, error paths) can be checked against the real
native encoder in CI.
------------------------------------------------------------------------------]]

local stubs = {}

local modules = {}

--- LOC '$$$/Some/Key=Text with ^1' -> 'Text with ^1', substituting arguments.
function LOC(text, ...)
	local out = tostring(text):gsub('^%$%$%$/[%w/_%.]+=', '')
	local args = { ... }
	for i = 1, #args do
		out = out:gsub('%^' .. i, tostring(args[i]))
	end
	return out
end

local function pathSeparator() return '/' end

modules.LrPathUtils = {
	child = function(dir, name)
		if dir:sub(-1) == pathSeparator() then return dir .. name end
		return dir .. pathSeparator() .. name
	end,
	leafName = function(path) return (path:match('[^/\\]+$')) or path end,
	parent = function(path) return (path:match('^(.*)[/\\][^/\\]*$')) end,
	extension = function(path)
		return (path:match('%.([^%.\\/]+)$')) or ''
	end,
	removeExtension = function(path)
		local stripped = path:gsub('%.[^%.\\/]+$', '')
		return stripped
	end,
	replaceExtension = function(path, ext)
		return (path:gsub('%.[^%.\\/]+$', '')) .. '.' .. ext
	end,
	getStandardFilePath = function(which)
		if which == 'temp' then return os.getenv('TMPDIR') or '/tmp' end
		return os.getenv('HOME') or '/tmp'
	end,
}

modules.LrFileUtils = {
	exists = function(path)
		local f = io.open(path, 'rb')
		if f then
			f:close()
			return 'file'
		end
		return false
	end,
	readFile = function(path)
		local f = io.open(path, 'rb')
		if not f then error('cannot read ' .. path) end
		local contents = f:read('a')
		f:close()
		return contents
	end,
	delete = function(path) return os.remove(path) ~= nil end,
	move = function(from, to)
		local ok = os.rename(from, to)
		if ok then return true end
		return false, 'rename failed'
	end,
	chooseUniqueFileName = function(path)
		local n = 1
		local base = path:gsub('%.[^%.\\/]+$', '')
		local ext = path:match('%.([^%.\\/]+)$') or ''
		local candidate = path
		while io.open(candidate, 'rb') do
			candidate = string.format('%s-%d.%s', base, n, ext)
			n = n + 1
		end
		return candidate
	end,
}

modules.LrTasks = {
	execute = function(command)
		local ok, kind, code = os.execute(command)
		if ok == true then return 0 end
		if type(ok) == 'number' then return ok end
		return code or 1
	end,
	startAsyncTask = function(fn) fn() end,
	yield = function() end,
}

local logger = {
	enable = function() end,
	info = function(_, m) if stubs.verbose then print('[log] ' .. tostring(m)) end end,
	warn = function(_, m) if stubs.verbose then print('[warn] ' .. tostring(m)) end end,
	error = function(_, m) if stubs.verbose then print('[err] ' .. tostring(m)) end end,
}
modules.LrLogger = setmetatable({}, { __call = function() return logger end })

local prefs = { enableLogging = false }
modules.LrPrefs = { prefsForPlugin = function() return prefs end }

modules.LrDialogs = {
	message = function(title, detail) stubs.lastDialog = { title, detail } end,
}

modules.LrColor = setmetatable({}, { __call = function() return {} end })

modules.LrView = {
	bind = function(spec) return spec end,
	share = function(name) return name end,
}

modules.LrApplication = {
	activeCatalog = function()
		return { withWriteAccessDo = function(_, _, fn) fn() end, addPhoto = function() end }
	end,
}

function import(name)
	local m = modules[name]
	if m == nil then error('lr_stubs: no stub for ' .. tostring(name)) end
	return m
end

--- Installs the globals a plug-in module expects and points require at the
--- plug-in directory.
function stubs.install(pluginPath)
	_PLUGIN = { path = pluginPath, id = 'com.custom.lightroom.export.iso21496' }
	WIN_ENV = false
	MAC_ENV = true
	package.path = pluginPath .. '/?.lua;' .. package.path
end

stubs.modules = modules
return stubs
