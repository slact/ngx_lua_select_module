local ngx_select = require "ngx.select"
local cjson = require "cjson"
local mm = require "mm"
local clientsock = assert(ngx.req.socket(true))
local Msgbuf
--upsock:settimeouts(2, 2, 1)
clientsock:settimeouts(100, 30, 1)

local DEFAULT_SELECT_TIMEOUT = 1000

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
      error("failed to connect to upstream at " .. arg.upstream_host .. ":" .. arg.upstream_port)
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
        error("got receive() error for socket " .. tostring(sock) .. ":" .. err)
      end
      
      if sockmsg[clientsock] == "!FIN" then
        return
      end
      
      for sock, msg in pairs(sockmsg) do
        res, err = sock:send(tostring(arg.echo_prefix)..msg.."\n")
        if err then
          error("failed to send response to" .. tostring(sock))
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
      error("got select() error:", err)
    end
    for _, sock in ipairs(socks) do
      local msg, err = sock:receive("*l")
      if not msg then
        if err ~= "timeout" then
          error("no message. sock:" .. tostring(sock).. "err:" .. tostring(err))
        end
      elseif msg == "!FIN" then
        return
      else
        sock:send(msg.."\n")
      end
    end
  end
end


function tests.upstream_socket_receiveany(arg)
  local select_read = create_select_table(arg)
  
  local buf = {}
  for sock, mode in pairs(select_read) do
    buf[sock] = Msgbuf.new()
  end
  
  while true do
    local socks, err = ngx_select(select_read, arg.select_timeout)
    if not socks then
      error("ngx_select error: " .. tostring(err))
    end
    for _, sock in ipairs(socks) do
      local data, err = buf[sock]:receiveany(sock)
      if err then
        mm(sock)
        error("socket " .. tostring(sock) .. " receiveany error: " .. tostring(err))
      end
      for msg in buf[sock]:each_message() do
        mm(msg)
        if msg == "!FIN" then
          return
        end
        sock:send(msg.."\n")
      end
    end
  end
end

Msgbuf = { 
  new = function()
    return setmetatable({}, Msgbuf)
  end,
  __index = {
  chunksize=256,
  append = function(self, data)
    if not self.buf then
      self.buf = data
    else
      --no this isn't efficient, but frankly I don't give a damn
      self.buf = self.buf .. data
    end
  end,
  receiveany = function(self, socket)
    local data, err = socket:receiveany(self.chunksize)
    if err == "timeout" then
      if not self.ignore_timeout then
        return nil, err
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
      if line and newline and #newline > 0 then
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
  if not args.select_timeout then
    args.select_timeout = DEFAULT_SELECT_TIMEOUT
  end
  
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
  xpcall(function() test_handler(args) end, function(err)
    local err_status = "!FAIL["..#tostring(err).."]\n"..err.."\n"
    clientsock:send(err_status)
    ngx.log(ngx.ERR, "Error in test " .. testname .. ": " .. debug.traceback(err))
    sleep(5)
  end)
end
run_test(clientsock)
