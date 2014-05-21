require("luvit-test/helper")
local Timer = require('timer')

local CrazySnail = require('crazy-snail')

snail = CrazySnail.new({path = "/tmp/redis.sock"})

snail:connect()

local i = 0

snail:on('connect', function()
  snail:command("set", "a", 0, function(err, res)
    assert(err == nil)
    assert(res == "OK")
  end)

  local t = Timer.setInterval(100, function()
    i = i + 1
    snail:command("set", "a", i, function(err, res)
      assert(err == nil)
      assert(res == "OK")
    end)
  end)

  snail:subscribe("a", "b", function(err, res)
    for key,value in pairs(res) do print(key,value) end
    snail:command("get", res[1], function(err, res)
      assert(type(res) == "string")
      assert(tonumber(res) == i)
      if i > 10 then
        t:stop()
        snail:disconnect()
      end
    end)
  end)


  --snail:exit()
end)

