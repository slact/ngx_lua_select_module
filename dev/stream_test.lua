local select = require "ngx.select"
local mm = require "mm"
local clientsock = assert(ngx.req.socket(true))
local upsock = ngx.socket.tcp()
assert(upsock:connect("127.0.0.1", 8092))


upsock:settimeouts(100, 30, 1)
clientsock:settimeouts(100, 30, 1)

--mm({udp=udpsock, clientsock=clientsock, upsock=upsock, ngx_socket=ngx.socket})
upsock:send("say hello please\n")
upsock:receive()
upsock:send("thanks!\n")
while true do
  local socks, err = select({[clientsock]="r", [upsock]="r"}, 5000)
  if socks == nil then
    ngx.log(ngx.NOTICE, "got select() error:" .. tostring(err))
    break
  end
  for _, sock in ipairs(socks) do
    local data, err = sock:receive()
    if not data then
      ngx.log(ngx.NOTICE, "got receive error:" .. tostring(err))
      break
    end
    sock:send("you said: " .. data.."\n")
    data, err = sock:receive()
    ngx.log(ngx.NOTICE, "extra receive:" .. tostring(data).. ", " .. tostring(err))
  end
end
