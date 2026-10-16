---@meta

---@class channel
local channel = {}

---@param name string
---@return lightuserdata|nil ch
---@return string|nil error
function channel.create(name) end

---@param ch lightuserdata
---@return string|nil error
function channel.close(ch) end

---@param ch lightuserdata
---@return string|nil error
function channel.clear(ch) end

---@param ch lightuserdata
---@param data any
---@return string|nil error
function channel.push(ch, data) end

---@param ch lightuserdata
---@return any|nil data
---@return string|nil error
function channel.pop(ch) end

---@param ch lightuserdata
---@return any|nil data
---@return string|nil error
function channel.demand(ch) end

---@param ch lightuserdata
---@param data any
---@return string|nil error
function channel.supply(ch, data) end

return channel