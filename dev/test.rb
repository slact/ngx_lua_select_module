#!/usr/bin/env ruby
require_relative "test_helper.rb"
$tcp_server_bind_hostname = '127.0.0.1'
$openresty_hostname = '127.0.0.1'
$openresty_port = '8083'

class SelectModuleTest < Minitest::Test
  include TestHelp
  extend TestHelp::Sugar
  DEFAULT_TIMEOUT_SEC = 5
  
  def upstream(args = {})
    args[:clients]||= 0
    
    if args[:clients] > 0
      server = TestServer.new
      args[:upstream_host]=server.host
      args[:upstream_port]=server.port
    end
    resty_client = RestyTestClient.new(args)
    return resty_client unless server
    
    unless args[:monitor] == false
      resty_client.async_monitor
    end
    
    accepted_clients = []
    args[:clients].times do
      accepted_clients << server.wait(:client)
    end
    server.no_more_clients!
    return resty_client, server, accepted_clients
  end
  
  #test syntax: 
  # test "test_name: test description" timeout: 10 do
  #    ...
  # end
  #
  # when using the `upstream` helper func or the RestyTestClient, the test name 
  # _up to the ':'_ and user-provided test args are sent to openresty.
  # There, a corresponding test handler is called
  # "!FIN" should be treated as a special message that closes a client connection
  # 
  # the 'timeout' parameter is optional, and defaults to DEFAULT_TIMEOUT_SEC
  # notest skips a test
  
  test "client socket echo" do
    #runs client_socket_echo test in openresty
    echo_prefix="got_it."
    resty = upstream(echo_prefix: echo_prefix, clients: 0, monitor: false)
    (1..1000).each do |n|
      msg = "hello this is message number #{n}"
      resty.puts msg
      assert_equal "#{echo_prefix}#{msg}", resty.gets
    end
    resty.puts "!FIN"
    resty.wait.stop
  end
  
  
  test "upstream socket echo" do
    #runs upstream_socket_echo test in openresty
    
    resty, server, clients = upstream(clients: 20, exclude_resty_client: true)
    
    (1..20).each do |n|
      msg = "uptest #{n}"
      clients.each do |client|
        client.puts msg
      end
      clients.shuffle.each do |client|
        assert_equal msg, client.gets
      end
    end
    clients.each do |client|
      client.puts "!FIN"
      client.wait.stop
    end
    server.stop
    resty.stop
  end

  test "upstream socket receiveany: no timeouts" do
    resty, server, clients = upstream(clients: 10, exclude_resty_client: true)
    #resty.monitor_test!
    
    (1..100).each do |n|
      msg = "message #{n}"
      clients.each do |c|
        c.puts msg
      end
      clients.each do |c|
        assert_equal msg, c.gets
      end
    end
    clients.each do |c|
      c.puts "!FIN"
      c.wait.stop
    end
    server.stop
    resty.stop
  end
  
  test "upstream socket receiveany: large messages" do
    resty, server, clients = upstream(clients: 1, exclude_resty_client: true)
    #resty.monitor_test!
    
    (1..100).each do |n|
      msg = "0123456789" * n
      clients.each do |c|
        c.puts msg
      end
      clients.each do |c|
        assert_equal msg, c.gets
      end
    end
    clients.each do |c|
      c.puts "!FIN"
      c.wait.stop
    end
    server.stop
    resty.stop
  end
  
  notest "client socket echo: no read timeouts" do
    #runs upstream_socket_echo test in testnameopenresty
    #test name after colon is not sent to openresty
    resty = upstream(clients: 0)
  end
  
  notest "upstream socket echo: no read timeouts" do
    #runs upstream_socket_echo test in testnameopenresty
    #test name after colon is not sent to openresty
    resty, server, clients = upstream(exclude_resty_client: true, clients: 1)
  end
  
  notest "upstream readwrite select: no timeouts at all" do
    #TODO
  end
  
  notest "select invalid things: not a socket" do
    #TODO
  end
  notest "select invalid things: closed socket" do
    #TODO
  end
  notest "select invalid things: not a table" do
    #TODO
  end
  notest "select invalid things: bad mode string" do
    #TODO
  end
    
end

