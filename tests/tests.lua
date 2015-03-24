
local Timer = require('timer')

local CrazySnail = require('crazy-snail')

snail = CrazySnail.new({path = "/var/run/redis/redis.sock"})
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

  snail:subscribe(50, "b", "c", function(err, res)
    if not err then
      -- Do something every 1000ms or on Key1, Key4, Key5, expire events
      --for key,value in pairs(res) do print(key,value) end
      snail:command("set", "d", 52, function(err, res)
        assert(err == nil)
        assert(res == "OK")
      end)
    end
  end)

  snail:subscribe("a", "d", function(err, res)
    --for key,value in pairs(res) do print(key,value) end
    snail:command("get", res[1], function(err, res)
      assert(type(res) == "string")
      print(tonumber(res))
      assert(tonumber(res) == i)
      if i >= 10 then
        t:stop()
        snail:disconnect()
      end
    end)
  end)


  --snail:exit()
end):on('error', function(err)
  p(err)
end):on('disconnect', function()
  p("disconnect")
end)
