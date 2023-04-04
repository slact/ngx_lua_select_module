require 'rubygems'
require 'bundler/setup'
require 'uri'
require 'async/io'
require 'async/io/protocol/line'
require 'async/semaphore'
require 'async/queue'
require 'minitest'                                                                                                                     
require 'minitest/reporters'                                                                                                           
require "minitest/autorun"     
require "pry"
require "binding_of_caller"
require "json"
require "ostruct"

class BetterSpecReporter < Minitest::Reporters::SpecReporter                                                                           
  def before_test(test)                                                                                                                
    test_name = test.name.gsub(/^test_: /, 'test:')                                                                                    
    print pad_test(test_name)                                                                                                          
    print yellow("...")                                                                                                                
  end                                                                                                                                  
  def record_print_status(test)  
    print ANSI::Code.left(4)                                                                                                           
    print_colored_status(test)                                                                                                         
    print(" (%.2fs)" % test.time) unless test.time.nil?
    print(" (#{test.assertions} assert#{test.assertions == 1 ? "" : "s"})")
    puts
  end                                                                                                                                  
end   
Minitest::Reporters.use! BetterSpecReporter.new

module TestHelp
  class SemaphoreWait < Async::Semaphore
    public :wait
  end
  class LockSet
    def initialize
      @locks={}
    end
    def lock(*names)
      names.each do |name|
        l = get_lock(name)
        raise "Lock #{name} is already locked" if l.blocking?
        l.acquire
      end
      self
    end
    def unlock(*names)
      names.each do |name|
        l = get_lock(name)
        raise "Lock #{name} is already unlocked" unless l.blocking?
        l.release
      end
      self
    end
    def locked?(*names)
      names.each do |name|
        return true if get_lock(name).blocking?
      end
      false
    end
    def wait(*names)
      names.each do |name|
        get_lock(name).wait
      end
      self
    end
    private def get_lock(name)
      return @locks[name] if @locks[name]
      @locks[name]=SemaphoreWait.new(1)
    end
  end
  
  class TCPServer
    attr_accessor :port, :host, :task, :server
    def initialize(host, port)
      @endpoint = Async::IO::Endpoint.tcp(host, port)
      @locks = LockSet.new
      @locks.lock :ready, :finished
      @accept_connections = true
      @client_queue = Async::Queue.new
    end

    def no_more_clients!
      @accept_connections = false
    end
    
    def run
      Async do |task|
        @task = task
        @endpoint.bind do |server|
          #Console.logger.info(server) {"Accepting connections on #{server.local_address.inspect}"}
          @server = server
          @host = server.local_address.ip_address
          @port = server.local_address.ip_port
          @server.listen 128
          @locks.unlock :ready
          @server.accept_each(task: @task) do |socket|
            if @no_more_clients then
              @error = "A client tried to connect when the server isn't expecting any more"
              close
            else
              client = TCPClient.new(socket)
              @client_queue << client
              client.wait :finished
            end
          end
          @locks.unlock :finished
        rescue Async::Wrapper::Cancelled => e
          @locks.unlock(:finished) if @locks.locked?(:finished)
        end
      end
      self
    end
    
    def stop
      @server.close unless @server.closed?
      @task.stop if @task.alive?
      self
    end
    
    def each_client(max_clients=nil)
      clients_accepted=0
      while client = @client_queue.dequeue
        clients_accepted += 1
        yield client
        if max_clients and clients_accepted >= max_clients
          return self
        end
      end
      self
    end
    
    def wait(until_state=:ready)
      case until_state
      when :ready
        @locks.wait :ready
        raise @error if @error
        return self
      when :client
        next_client = @client_queue.dequeue
        raise @error if @error
        return next_client
      when :close, nil
        @locks.wait :finished
        raise @error if @error
        return self
      else
        raise Exception, "don't know how to wait until server is #{until_state}"
      end
    end
  end
  
  class TestServer < TCPServer
    def initialize(autorun: true)
      super($tcp_server_bind_hostname, 0)
      self.run.wait(:ready) if autorun
    end
  end
  
  class TCPClient
    attr_reader :stream, :socket
    def initialize(host, port=nil)
      @locks = LockSet.new.lock(:ready, :finished)
      if host.respond_to?(:host) && host.respond_to(:port)
        @host = host.host
        @port = port.port
      elsif Async::IO::Socket === host
        return initialize_from_socket host
      else
        @host = host
        @port = port
      end
      @endpoint = Async::IO::Endpoint.tcp(@host, @port)
      @running = false
    end
    
    private def initialize_from_socket(sock)
      #binding.pry  unless sock.ready?
      #raise "Refusing to make a client out of a disconnected socket" unless sock.ready?
      
      @host = sock.remote_address.ip_address
      @port = sock.remote_address.ip_port
      @endpoint = Async::IO::Endpoint.tcp(@host, @port)
      @running = true
      @socket = sock
      @stream = Async::IO::Stream.new(@socket)
      handshake
      @locks.unlock :ready
    end
    
    def run
      return self if @running
      Async do |task|
        @client_task = task
        begin
          @socket = @endpoint.connect
          @stream = Async::IO::Stream.new(@socket)
          handshake
          @locks.unlock :ready
        rescue SystemCallError, RuntimeError => e
          @error = e
          @locks.unlock :ready, :finished
          raise e
        end
      end
      @running = true
      self
    end
    private def handshake
      #do nothing
    end
    
    def write(msg)
      @stream.write(msg)
    end
    def read(n)
      @stream.read(n)
    end
    def read_exactly(n)
      @stream.read_exactly(n)
    end
    def gets
      @stream.gets
    end
    def puts(msg)
      @stream.puts msg
    end

    def stop
      @socket.close
      @client_task.stop if @client_task
      @locks.unlock(:finished) if @locks.locked?(:finished)
    end
    
    def wait(until_state=:ready)
      case until_state
      when :ready
        @locks.wait :ready
      when :finished, nil
        @locks.wait :finished
      end
      raise @error if @error
      self
    end
  end

  class RestyTestClient < TCPClient
    def initialize(args={})
      begin
        test_name = args.delete(:test)
      rescue Exception => e
        binding.pry
      end
      if test_name.nil?
        begin
          n=1
          while true do
            caller_test_name = binding.of_caller(n).eval("respond_to?(:current_test_name) && current_test_name")
            if caller_test_name
              test_name = caller_test_name
              break
            end
            n+=1
          end
        rescue RuntimeError => e
          raise "no test name given to RestyTestclient, and couldn't figure it out from the call stack"
        end
      end
      @test_name = test_name
      @test_args = args
      
      super($openresty_hostname, $openresty_port)
      self.run.wait(:ready)
    end
    
    def handshake
      @stream.puts "!TEST #{@test_name}"
      @stream.puts JSON.dump(@test_args)
      @stream.puts ""
      reply, err = @stream.gets
      if reply.nil?
        raise "Disconnected while doing handshake for test #{@test_name}"
      end
      m = reply.match(/^!(\w+) (.*)\n?/)
      case m[1]
      when "RUN"
        unless m[2] == @test_name
          raise "invalid handshake reply for test #{@test_name}: #{m[2]}" 
        end
      when "ERR"
        raise "Server error during test handshake: #{m[2]}"
      else
        raise "Bad handshake reply #{reply}"
      end
    end
    
    def async_monitor
      @monitor_thread = @client_task.async do
        monitor
      rescue Async::Wrapper::Cancelled
      end
    end
    
    private def receive_status_message
      line = self.gets
      if not line then
        raise "failed to receive status message line"
      end
      m = line.match(/^!(?<type>\w+)(\[(?<length>\d+)\])?(?<space>\s)?(?<data>.*)$/)
      if m[:length]
        data = self.read_exactly(m[:length].to_i + 1)
        data = data[0..-2]
      elsif m[:space].length == 0
        buf = []
        while true do
          line = self.gets
          break if line.length == 0
          buf << line
        end
        data = buf.join("\n")
      else
        data = m[:data]
      end
      OpenStruct.new(type: m[:type].to_sym, data: data)
    end
    
    def monitor
      while true do
        status = receive_status_message
        case status.type
        when :ERR
          raise "openresty error: #{status.data}"
        when :DEBUG
          puts "DEBUG: #{status.data}"
        when :FAIL
          e = Minitest::Assertion.new(status.data)
          e.set_backtrace []
          raise e
        else
          raise "Unexpected monitor message: #{status}"
        end
      end
    end
  end
  
  attr_reader :task, :current_test_name
    
  module Sugar
    #sugar for defining async tests with timeouts
    private def regularize_test_name(testname)
      m = testname.match(/^([^:]+):?(.*)$/)
      name = m[1].gsub(" ", "_")
      unless name.match('^[\w\d_:]+$')
        raise "Invalid test name: \"#{testname}\". Only letters and numbers and underscores and spaces please"
      end
      if m[2].length > 0
        "#{name}:#{m[2]}"
      else
        name
      end
    end
    
    def test(name, timeout: nil, &block)
      name = regularize_test_name name
      define_method "test_#{name}" do
        Async do |task|
          @current_test_name=name
          begin
            task.with_timeout(timeout || self.class::DEFAULT_TIMEOUT_SEC) do |t|
              @task = t
              instance_exec(t, &block)
            end
          rescue Async::Stop => e
            #that's ok
          rescue Async::TimeoutError => e
            flunk "timeout: #{e.message}"
          rescue Minitest::Assertion => e
            raise e
          rescue SystemCallError => e
            flunk "#{e.class}: #{e.message}"
          rescue Exception => e
            Console.logger.warn(self, e)
            failure = MiniTest::Assertion.new("#{e.class}: #{e.message}")
            failure.set_backtrace e.backtrace
            raise failure
          end
          @current_test_name=nil
        end
      end
    end
    def notest(name, ...)
      name = regularize_test_name name
      define_method "test_#{name}" do
        skip
      end
    end
    
  end
end
