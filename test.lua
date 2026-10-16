local thread = require("lunet.thread")
local channel = require("lunet.channel")
local lunet = require("lunet")

local th, err = thread.create(function (name, age)
    local channel = require("lunet.channel")
    local lunet = require("lunet")
    print(name, age)
    
    local main_sh = channel.create("main:thread:1")
    local my_sh = channel.create(":1")
    lunet.spawn(function ()
        while true do
            channel.push(main_sh, "Hello, world. Time: " .. os.time())
            lunet.sleep(1000)
        end
    end)
end, "Max", 70)
if th then
    print("start")
    local sh = channel.create("main:thread:1")
    local thread1_sh = channel.create("thread:1")
    lunet.spawn(function ()
        while true do
            local message = channel.demand(sh)
            print(message)
            lunet.sleep(0)
        end
    end)
else
    print(err)
end

-- local lunet = require("lunet")
-- local psql = require("lunet.psql")

-- local pool = assert(psql.pool({
--     host = "127.0.0.1",
--     port = 5432,
--     dbname = "game",
--     user = "game",
--     password = "secret",
--     max_conns = 16,
-- }))

-- assert(pool:prepare("player_by_id", "SELECT id, name, gold FROM players WHERE id = $1"))

-- lunet.spawn(function()
--     local rows, err = pool:execute("player_by_id", 42)
--     if not rows then
--         print("error:", err)
--         return
--     end
--     print(rows[1] and rows[1].name)

--     local conn = pool:acquire()
--     conn:query("BEGIN")
--     conn:query("UPDATE players SET gold = gold - $1 WHERE id = $2", 100, 42)
--     conn:query("UPDATE players SET gold = gold + $1 WHERE id = $2", 100, 43)
--     conn:query("COMMIT")
--     conn:release()
-- end)