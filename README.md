crazy-snail
===========

```
migration to luvit 2 ongoing, a 0.0.1 version has been published on lit for linux x64
dependencies = {
    "gsick/crazy-snail@0.0.1"
  },

```

Real-Time Luvit module based on [Redis](http://redis.io/).<br />
This module is optimized for [Redis Keyspace Notifications](http://redis.io/topics/notifications)
via Unix Domain Socket.<br />
For basic Redis usage you may use [luvit-redis](https://github.com/tadeuszwojcik/luvit-redis)<br />
It assume that your redis.conf is well configured.<br />

Motivations:<br />
Hiredis doesn't allow many subscription callback on the same key for the same client.<br />
Have timer interval event.<br />
Launch Redis LUA script on key notification.<br />

## Table of Contents

* [Status](#status)
* [Dependencies](#dependencies)
* [Synopsis](#synopsis)
* [API](#api)
    * [new](#new)
    * [connect](#connect)
    * [on](#on)
    * [subscribe](#subscribe)
    * [command](#command)
    * [disconnect](#disconnect)
    * [exit](#exit)
* [Installation](#installation)
* [Tests](#tests)
* [Authors](#authors)
* [Contributing](#contributing)
* [Licence](#licence)

## Status

Beta version

## Dependencies

* [Luvit](http://luvit.io/)

## Synopsis

Redis configuration:
```
unixsocket /var/run/redis/redis.sock
unixsocketperm 755
notify-keyspace-events KE$
```

```lua
local Timer = require('timer')
local CrazySnail = require("crazy-snail")

snail = CrazySnail.new({path = "/var/run/redis/redis.sock"})

snail:connect()

snail:on('connect', function()
  print("connected")

  -- Basic Key-space and Key-event subscription
  snail:subscribe("Key1", "Key2", "Key3", "del", "set", function(err, res)
    if not err then
      -- Do something on Key1, Key2, Key3, del or set event
      -- Two connections are automatically managed
      -- Others commands can easily be sent in a subscribed context
      snail:command("set", "Key4", "123", function(err, res)
        if not err then
          print(res)
        end
      end)
    end
  end)

  -- With Timer-event subscription
  snail:subscribe(1000, "Key1", "Key4", "Key5", "expire", function(err, res)
    if not err then
      -- Do something every 1000ms or on Key1, Key4, Key5, expire events
    end
  end)

   -- Script subscription
  local script
  snail:command("script", "load", [[ return {KEYS[1],KEYS[2],ARGV[1],ARGV[2]} ]], function(err, res)
    if not err then
      script = res
    end
  end)

  snail:subscribe(10000, "Key1", function(err, res)
    snail:command("EVALSHA", script, 2, "a", "b", 2, 3, function(err, res)
      if err then
        print(err)
      end
    end)
  end)

end):on('error', function(err)
  print(err)
end)

snail:on('disconnect', function()
  print("disconnected")
  Timer.setTimeout(1000, function()
    snail:connect()
  end)
end)

```

## API

### new

```lua
snail = CrazySnail.new(options)
```

Instanciate a new CrazySnail object.

* `options`: LUA_TTABLE
    * `path`: LUA_TSTRING, path to the Redis Unix Domain Socket
    * `ignore_sub_cmd_reply`: LUA_TBOOLEAN, ignore the subscription command response, default `true`

### connect

```lua
snail:connect()
```

Connect to the Redis instance. Send a `connect` or `error` event.

### on

```lua
snail:on(event, callback)
```

Subscribe to a CrazySnail instance event.

* `event`: LUA_TSTRING, `connect`, `disconnect` or `error`
* `callback`: LUA_TFUNCTION

### subscribe

```lua
snail:subscribe(key, ..., callback)
```

Subscribe to a Key-space, Key-event or Timer-event notification

* `key`: LUA_TSTRING or LUA_TNUMBER
    * if LUA_TNUMBER, it assume it's a timer interval event subscription
    * if LUA_TSTRING, it assume it's a Key-event or Key-space subscription
* `callback`: LUA_TFUNCTION

List of Redis event:<br />
`append`, `del`,
`expire`, `evicted`,
`incrby`, `incrbyfloat`,
`hdel`, `hincrby`, `hincrbyfloat`, `hset`,
`linsert`, `lpop`, `lpush`, `lset`, `ltrim`,
`rename_from`, `rename_to`, `rpop`, `rpush`,
`sadd`, `sdiffstore`, `set`, `setrange`,
`sinterstore`, `sortstore`, `spop`, `srem`, `sunionostore`,
`zadd`, `zincr`, `zinterstore`, `zrem`, `zrembyrank`,
`zrembyscore`, `zunionstore`

Examples:<br />
```lua
snail:subscribe(1000, "a", "b", "set", callback)
```

is same as

```lua
snail:subscribe("__timer@0__:1000", "__keyspace@0__:a", "__keyspace@0__:b", "__keyevent@0__:set", callback)
```

or

```lua
snail:command("subscribe", "__timer@0__:1000", "__keyspace@0__:a", "__keyspace@0__:b", "__keyevent@0__:set", callback)
```

### command

```lua
snail:command(cmd, args, ..., callback)
```

Execute a Redis command.

* `cmd`: LUA_TSTRING, Redis command
* `args`: LUA_TSTRING, command args
* `callback`: LUA_TFUNCTION

### disconnect

```lua
snail:disconnect()
```

Disconnect the client. Send a `disconnect` or `error` event.
`connect` can be call safely after.

### exit

```lua
snail:exit()
```

Sync disconnect (without event).
`connect` cannot be call after.

## Installation

## Tests

## Authors

Gamaliel Sick

## Contributing

  * Fork it
  * Create your feature branch (git checkout -b my-new-feature)
  * Commit your changes (git commit -am 'Add some feature')
  * Push to the branch (git push origin my-new-feature)
  * Create new Pull Request

## License

```
The MIT License (MIT)

Copyright (c) 2014 gsick

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
