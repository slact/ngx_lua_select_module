non-blocking `select()` inside the Nginx event loop for Openresty cosockets. Good if you want to `receive()` from more than one socket at a time.

# API

```lua
local select = require "ngx.select"
```

### `select({sockets}, [timeout])`

Waits until at least one socket is ready for reading or writing, or timeout is reached.

**First argument** is a table of cosockets from `ngx.req.socket()` or `ngx.socket.tcp()` or `ngx.socket.udp()`.

```lua
select({[sock1]="r", [sock2]="w", [sock3]="rw"})
--wait until sock1 is read-ready OR sock2 is write-ready OR sock3 is read-or-write-ready
```

```lua
select({sock1, sock2, sock3})
--wait until sock1 OR sock2 OR sock3 are read-ready. equivalent to
select({[sock1]="r", [sock2]="r", [sock3]="r"})
```
`select()` will error out if any of the sockets passed in are closed or aren't sockets.


**Second argument** is an optional wait timeout, in milliseconds.

```lua
select({socket1, socket2}, 1000)
```

**Returns** a table of ready sockets, or `nil, error_string` on error (such as `"timeout"`)

Ready sockets table has numerically indexed sockets, and socket-indexed event-types:

```lua
local ready, err = select({[sock1]="rw", [sock2]="rw"})
--let's say sock1 is read-ready, and sock2 is write-ready

--ready looks like this:
ready = {
  sock1,
  sock2,
  [sock1]="r",
  [sock2]="w"
}
```

### Caveats
 - If you plan on being absolutely sure your socket reads and writes don't yield the Lua thread after the `select()` reports the sockets as ready, set their timeouts to `1` in Openresty. `0` would be nice but Openresty ignores it, so `1` is good enough. (The thread actually yields with this timeout but then resumes immediately after)
 - Only implemented for the Lua stream module, not HTTP. (ask me if you want this done)
 - Only supported for TCP sockets, not UDP. Openresty makes it hard to guess what type a given socket is without making syscalls. (Can be implemented as an ugly hack. I'm not opposed to ugly hacks, mind you, so ask me if you want this done)
 
### Example
```nginx
# a simple 2-way echo server with very little error handling

stream {
  server {
    listen 8083;
    content_by_lua_block {
      local select = require "ngx.select"
      local clientsock = assert(ngx.req.socket(true))
      local upsock = ngx.socket.tcp()
      assert(upsock:connect("127.0.0.1", 8092))
      
      --setting read timeouts to 1 to ensure we do not yield for more than 1ms
      --on socket:receive() after select()
      upsock:settimeouts(100, 30, 1)
      clientsock:settimeouts(100, 30, 1)

      while true do
        --select clientsock and upsock for read-readiness, but wait no more than 5000 milliseconds
        local socks, err = select({clientsock, upsock}, 5000)
        
        if socks == nil then
          ngx.log(ngx.NOTICE, "got select() error:" .. tostring(err))
          break
        end
        
        local receive_error
        for _, sock in ipairs(socks) do
          --note that ipairs() is used here and not pairs().
          --read the return value documentation to understand why
          
          local data, err = sock:receive()
          if not data then
            ngx.log(ngx.NOTICE, "got receive error:" .. tostring(err))
            receive_error = true
            break
          end
          sock:send("you said: " .. data.."\n")
          data, err = sock:receive()
          ngx.log(ngx.NOTICE, "extra receive:" .. tostring(data).. ", " .. tostring(err))
        end
        
        if receive_error then
          ngx.log(ngx.NOTICE, "a socket failed to receive data. we're done here")
          break
        end
      end
    
    }
  }
}

```

### Building and Installing

To build this module, follow the standard Nginx module building procedure, but pass the module argument to Openresty's `configure` script instead:


```bash
tar -xvf openresty-VERSION.tar.gz
cd openresty-VERSION/
#build static module into openresty
./configure --add-module=path/to/ngx_lua_select_module_source
make

#--OR--

#build dynamic module
./configure --add-dynamic-module=path/to/ngx_lua_select_module_source
make
# this creates 2 modules - ngx_http_lua_select_module.so and ngx_stream_lua_select_module.so,
#unless Openresty is explicitly built without stream or http


sudo make install
```
