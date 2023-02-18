local ngx_select = require "ngx.select"
local cjson = require "cjson"
local mm = require "mm"
local clientsock = assert(ngx.req.socket(true))
--local upsock = ngx.socket.tcp()
--assert(upsock:connect("127.0.0.1", 8092))

--upsock:settimeouts(2, 2, 1)
clientsock:settimeouts(100, 30, 1)

local DEFAULT_SELECT_TIMEOUT = 1000

local event, err

--mm({udp=udpsock, clientsock=clientsock, upsock=upsock, ngx_socket=ngx.socket})
--upsock:send("say hello please\n")
--local data, err = upsock:receiveany(1024)
--upsock:send(tostring(data) .. ":"..tostring(err))
--upsock:send("thanks!\n")

local function ngx_log(level, ...)
  local t = {}
  for _,v in ipairs{...} do
    table.insert(t, tostring(v))
  end
  ngx.log(level, table.concat(t, " "))
end
local function log_err(...)
  ngx_log(ngx.ERR, ...)
end

local function create_select_table(arg)
  local select_table = {}
  if not arg.exclude_resty_client then
    select_table[clientsock] = 'r'
  end
  local upsocks = {}
  for i=1,arg.clients do
    local upsock = ngx.socket.tcp()
    upsock:settimeouts(1000, 300, 1)
    local ok, err = upsock:connect(arg.upstream_host, arg.upstream_port)
    if not ok then
      local msg = "!ERR failed to connect to upstream at " .. arg.upstream_host .. ":" .. arg.upstream_port
      clientsock:send(msg)
      error(msg)
    end
    table.insert(upsocks, upsock)
    select_table[upsock]='r'
  end
  return select_table
end

local tests = {}
function tests.client_socket_echo(arg)
  local select_socks = create_select_table(arg)
  local socks, err
  while true do
    socks, err = ngx_select(select_socks, 50000)
    if err then
      log_err("got select() error:", err)
      return
    end
    local sockmsg = {}
    local res
    for _, sock in ipairs(socks) do
      --TODO: test that the following line doesn't block
      -- ASSUMING a line has actually been sent, of course
      --log_err("RECEIVING from", sock)
      sockmsg[sock], err = sock:receive("*l")
      --log_err("GOT MSG:", sockmsg[sock])
      if err then
        log_err("got receive() error for socket", sock, ":", err)
        return
      end
      
      if sockmsg[clientsock] == "!FIN" then
        clientsock:send("!FIN\n")
        return
      end
      
      for sock, msg in pairs(sockmsg) do
        res, err = sock:send(tostring(arg.echo_prefix)..msg.."\n")
        if err then
          log_err("failed to send response to", tostring(sock))
          return
        end
      end
    end
  end
end

function tests.upstream_socket_echo(arg)
  local select_read = create_select_table(arg)
  
  while true do
    local socks, err = ngx_select(select_read, arg.select_timeout)
    if err then
      log_err("got select() error:", err)
      return
    end
    for _, sock in ipairs(socks) do
      local msg, err = sock:receive("*l")
      if not msg then
        log_err("no message. ERR:", err)
      else
        if msg == "!FIN" then
          clientsock:send("!FIN\n")
          return
        end
        
        sock:send(msg.."\n")
      end
    end
  end
end

local Msgbuf = { __index = {
  chunksize=256,
  append = function(self, data)
    if not self.buf then
      self.buf = data
    else
      --no this isn't efficient, but frankly I don't give a damn
      self.buf = self.buf .. data
    end
  end,
  receive = function(self, socket)
    local data, err = socket:receiveany(self.chunksize)
    if err == "timeout" then
      if not self.ignore_timeout then
        error("unexpected socket timeout for " .. tostring(socket))
      end
    elseif err then
      assert(not data, "should have no data")
      log_err("ERR:", err)
      --return data, err
    end
    if data then
      self:append(data)
    end
    return self
  end,
  
  each_message = function(self)
    local messages = {}
    for line, newline in (self.buf or ""):gmatch("([^\n]*)(\n?)") do
      if line and newline then
        table.insert(messages, line)
      elseif not newline then
        assert(unfinished_line==nil)
        unfinished_line = line
      end
    end
    self.buf = unfinished_line
    local n = 0
    return function()
      n = n + 1
      return messages[n]
    end
  end
}}

local function run_test(clientsock)
  local handshake = assert(clientsock:receive("*l"))
  local testname = handshake:match("^!TEST (.*)$")
  
  if not testname then
    error("invalid test handshake: \"" .. handshake .. "\"")
  end
  
  local args = {}
  while true do
    local line = clientsock:receive("*l")
    if #line > 0 then
      table.insert(args, line)
    else
      args = cjson.decode(table.concat(args, "\n"))
      break
    end
  end
  mm(args)
  
  local test_handler_name, test_description = testname:match("^([^:]+):?%s*(.*)$")
  if test_description and #test_description > 0 then
    args.test_description = test_description
  end
  
  local test_handler = tests[test_handler_name]
  if not test_handler then
    clientsock:send("!ERR unknown test '" .. testname .. "'\n")
    error("no such test: " .. testname)
  end
  clientsock:send("!RUN ".. testname .."\n")
  test_handler(args)
end
run_test(clientsock)