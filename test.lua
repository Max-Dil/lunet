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