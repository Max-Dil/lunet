---@meta

---@class socket
local socket = {}

---Listen for incoming connections
---@param protocol string Protocol type, only "tcp" is supported
---@param host string Host address to bind to (e.g., "127.0.0.1", "0.0.0.0")
---@param port integer Port number to listen on (1-65535)
---@param reuse_port? boolean Enable SO_REUSEPORT (default false)
---@return lightuserdata|nil listener The listener handle or nil on error
---@return string|nil error Error message if failed
---@usage
---```lua
---local socket = require('lunet.socket')
---local listener, err = socket.listen("tcp", "127.0.0.1", 8080)
---if not listener then
---    error("Failed to listen: " .. err)
---end
---```
function socket.listen(protocol, host, port, reuse_port) end

---Accept an incoming connection (must be called from coroutine)
---@param listener lightuserdata The listener handle from socket.listen()
---@return lightuserdata|nil client The client handle or nil on error
---@return string|nil error Error message if failed
---@usage
---```lua
---local socket = require('lunet.socket')
---lunet.spawn(function()
---    local client, err = socket.accept(listener)
---    if not client then
---        error("Accept failed: " .. err)
---    end
---    -- Handle client connection
---end)
---```
function socket.accept(listener) end

---Get the peer address of a connected socket (must be called from coroutine)
---@param client lightuserdata The client handle from socket.accept()
---@return string|nil address The peer IP address or nil on error
---@return integer|string|nil port_or_error The peer port, or error message if address is nil
---@usage
---```lua
---local socket = require('lunet.socket')
---local ip, port = socket.getpeername(client)
---if ip then
---    print("Client from: " .. ip .. ":" .. port)
---end
---```
function socket.getpeername(client) end

---Read data from a socket.
---
---Without `callback` it must be called from a coroutine: the coroutine yields
---until data arrives, EOF, an error or the timeout.
---
---With `callback` the call returns immediately (works outside of coroutines)
---and `callback(data, err)` is invoked later from the event loop. The callback
---is NOT run inside a coroutine, use `lunet.spawn` inside it if you need
---blocking functions.
---@param client lightuserdata The client handle
---@param timeout? integer Timeout in milliseconds (nil or 0 = wait forever). On timeout `err` is "timeout"
---@param callback? fun(data: string|nil, err: string|nil) Called with the result instead of yielding
---@return string|boolean|nil data Received data (coroutine mode); `true` if callback mode started; nil on error/EOF
---@return string|nil error Error message ("timeout", "socket closed", ...); nil on success or EOF
---@usage
---```lua
---local socket = require('lunet.socket')
---
----- coroutine mode, 5 second timeout
---lunet.spawn(function()
---    local data, err = socket.read(client, 5000)
---    if data then
---        print("Received: " .. data)
---    elseif err == "timeout" then
---        print("No data for 5s")
---    elseif err then
---        print("Read error: " .. err)
---    else
---        print("Connection closed")
---    end
---end)
---
----- callback mode, no coroutine needed
---socket.read(client, 5000, function(data, err)
---    print(data, err)
---end)
---```
function socket.read(client, timeout, callback) end

---Write data to a socket.
---
---By default (`wait` = true, no callback) it must be called from a coroutine and
---yields until the write completes.
---
---With `wait` = false or with `callback` the call returns immediately and works
---outside of coroutines. If `callback` is given, `wait` is ignored and
---`callback(err)` is invoked when the write completes.
---@param client lightuserdata The client handle
---@param data string The data to send
---@param wait? boolean Wait for write completion (default true). false = fire-and-forget
---@param callback? fun(err: string|nil) Called when the write completes (err is nil on success)
---@return string|nil error Error message if the write failed (or could not be started); nil otherwise
---@usage
---```lua
---local socket = require('lunet.socket')
---
----- coroutine mode, waits for completion
---lunet.spawn(function()
---    local err = socket.write(client, "Hello, client!")
---    if err then
---        print("Write error: " .. err)
---    end
---end)
---
----- do not wait, no coroutine needed
---socket.write(client, "Hello", false)
---
----- callback mode, no coroutine needed
---socket.write(client, "Hello", true, function(err)
---    if err then print("Write error: " .. err) end
---end)
---```
function socket.write(client, data, wait, callback) end

---Close a socket or listener.
---Pending read (coroutine or callback) is completed with nil, "socket closed".
---@param handle lightuserdata The socket handle to close
---@return string|nil error Error message if failed
---@usage
---```lua
---local socket = require('lunet.socket')
---socket.close(client)
---socket.close(listener)
---```
function socket.close(handle) end

---Set the read buffer size (bytes) used for subsequent reads
---@param size integer The new read buffer size
---@return nil
---@usage
---```lua
---local socket = require('lunet.socket')
---socket.set_read_buffer_size(1024)
---```
function socket.set_read_buffer_size(size) end

---Connect to a server (must be called from coroutine)
---@param host string The server host (IPv4 address)
---@param port integer The server port
---@return lightuserdata|nil conn The connection handle or nil on error
---@return string|nil error Error message if failed
function socket.connect(host, port) end

return socket