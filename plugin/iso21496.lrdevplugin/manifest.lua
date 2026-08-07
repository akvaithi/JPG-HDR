--[[----------------------------------------------------------------------------
manifest.lua — the name the build spec uses for the plugin manifest.

Lightroom Classic itself only ever reads Info.lua, so this module simply
re-exports it. Keeping both means `require 'manifest'` works from Lua code
without duplicating the descriptor.
------------------------------------------------------------------------------]]

return dofile(_PLUGIN.path .. '/Info.lua')
