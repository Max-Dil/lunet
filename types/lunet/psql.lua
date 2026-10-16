---@meta

---@class psql
local psql = {}

---@class psql.PoolOptions
---@field host? string
---@field port? integer|string
---@field dbname? string
---@field user? string
---@field password? string
---@field max_conns? integer
---@field [string] string|number|nil

---@class psql.Pool
local pool = {}

---@class psql.Conn
local conn = {}

---@param opts psql.PoolOptions
---@return psql.Pool|nil pool
---@return string|nil error
function psql.pool(opts) end

---@param sql string
---@param ... any
---@return table<integer, table<string, any>>|table|nil rows
---@return integer|string|nil affected_or_error
---@return string|nil sqlstate
function pool:query(sql, ...) end

---@param name string
---@param ... any
---@return table<integer, table<string, any>>|table|nil rows
---@return integer|string|nil affected_or_error
---@return string|nil sqlstate
function pool:execute(name, ...) end

---@param name string
---@param sql string
---@return true|nil ok
---@return string|nil error
function pool:prepare(name, sql) end

---@return psql.Conn|nil conn
---@return string|nil error
function pool:acquire() end

function pool:close() end

---@param sql string
---@param ... any
---@return table<integer, table<string, any>>|table|nil rows
---@return integer|string|nil affected_or_error
---@return string|nil sqlstate
function conn:query(sql, ...) end

---@param name string
---@param ... any
---@return table<integer, table<string, any>>|table|nil rows
---@return integer|string|nil affected_or_error
---@return string|nil sqlstate
function conn:execute(name, ...) end

function conn:release() end

return psql