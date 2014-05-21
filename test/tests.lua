require("luvit-test/helper")

local CrazySnail = require('crazy-snail')

snail = CrazySnail.new({path = "/tmp/redis.sock"})

snail:connect()

snail:on('connect', function()
  snail:command("set", "a", "999", function(err, res)
    assert(err == nil)
    assert(res == "OK")
  end)
  
  --snail:exit()
end)

