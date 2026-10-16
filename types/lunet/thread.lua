---@meta

---@class thread
local thread = {}

---@param func function
---@param ... any
---@return lightuserdata|nil th
---@return string|nil error
function thread.create(func, ...) end

---@param th lightuserdata
---@return string|nil error
function thread.close(th) end

---@param th lightuserdata
---@return string|nil error
function thread.wait(th) end

return thread