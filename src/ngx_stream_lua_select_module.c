#include <ngx_stream.h>
#include "ngx_lua_select.h"
#include <ngx_stream_lua_util.h>
#include <assert.h>
#include <sys/ioctl.h>

static void ngx_stream_lua_select_cleanup(void *data);
static void ngx_stream_lua_select_post_completion_handler(ngx_event_t *ev);
static void ngx_stream_lua_select_socket_read_request_handler(ngx_stream_lua_request_t *r);
static void ngx_stream_lua_select_socket_write_request_handler(ngx_stream_lua_request_t *r);
static void ngx_stream_lua_select_socket_read_upstream_handler(ngx_stream_lua_request_t *r, ngx_stream_lua_socket_tcp_upstream_t *up);
static void ngx_stream_lua_select_socket_write_upstream_handler(ngx_stream_lua_request_t *r, ngx_stream_lua_socket_tcp_upstream_t *up);
static ngx_int_t ngx_stream_lua_select_resume(ngx_stream_lua_request_t *r);
static void ngx_stream_lua_select_ctx_cleanup_and_discard(ngx_stream_lua_request_t *r);
static void ngx_stream_lua_select_timeout_handler(ngx_event_t *ev);

static void select_fail_cleanup(lua_State *L, ngx_lua_select_ctx_t *ctx, int selected_so_far) {
  if(ctx && ctx->ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->ref);
    ctx->ref = LUA_NOREF;
  }
  for(int i = 0; i<selected_so_far; i++) {
    ngx_connection_t *c;
    ngx_lua_select_socket_t *s = &ctx->socket[i];
    switch(s->type) {
      case NGX_LUA_SELECT_TCP_UPSTREAM:
      case NGX_LUA_SELECT_TCP_DOWNSTREAM:
        c = s->stream.tcp.up->peer.connection;
        break;
      case NGX_LUA_SELECT_UDP_UPSTREAM:
      case NGX_LUA_SELECT_UDP_DOWNSTREAM:
        c = s->stream.udp.up->udp_connection.connection;
        break;
      default:
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "unexpected socket type on select fail cleanup");
        c = NULL;
        break;
    }
    if((s->readwrite & NGX_LUA_SELECT_READ) && c && c->read->active) {
      //this may be going too far. could break future reads or something.
      //however it looks like all the other openresty API functions that use these events
      //call ngx_add_event as needed and never assume they're already added
      ngx_del_event(c->read, NGX_READ_EVENT, 0);
    }
    if((s->readwrite & NGX_LUA_SELECT_WRITE) && c && c->write->active) {
      //this may be going too far. could break future reads or something.
      //however it looks like all the other openresty API functions that use these events
      //call ngx_add_event as needed and never assume they're already added
      ngx_del_event(c->write, NGX_WRITE_EVENT, 0);
    }
  }
}

static int lua_select_module_select(lua_State *L) {
  ngx_stream_lua_request_t            *r;
  
  ngx_stream_lua_ctx_t                *streamctx;
  ngx_stream_lua_co_ctx_t             *coctx;
  
  ngx_lua_select_ctx_t                *ctx = NULL;
  int                                  n = lua_gettop(L);
  int                                  i;
  
  if(n == 0) {
    return luaL_error(L, "expected more than 0 arguments");
  }
  if(n > 2) {
    return luaL_error(L, "expected no more than 3 arguments");
  }
  if(!lua_istable(L, 1)) {
    return luaL_argerror(L, 1, "expected a table of sockets");
  }
  if(n == 2 && !lua_isnumber(L, 2) && !lua_isnil(L, 2)) {
    return luaL_argerror(L, 2, "expected a timeout value or nil");
  }
  
  lua_Number                          timeout = 0;
  
  if(n >= 2 && lua_isnumber(L, 2)) {
    timeout = lua_tonumber(L, n);
    if(timeout < 0) {
      return luaL_argerror(L, 2, "timeout cannot be <0");
    }
  }
  
  if((r = ngx_stream_lua_get_req(L)) == NULL) {
    return luaL_error(L, "no request found");
  }
  if((streamctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_module)) == NULL) {
    return luaL_error(L, "no request ctx found");
  }
  if ((coctx = streamctx->cur_co_ctx) == NULL) {
      return luaL_error(L, "no co ctx found");
  }
  int socketcount = 0; 
  lua_pushnil(L);  /* first key */
  while(lua_next(L, 1) != 0) {
    socketcount ++;
    lua_pop(L, 1);
  }
  
  ctx = lua_newuserdata(L, sizeof(*ctx) + sizeof(*ctx->socket) * socketcount);
  if(ctx == NULL) {
    return luaL_error(L, "unable to allocate ctx");
  }
  ctx->count = socketcount;
  ctx->ref = luaL_ref(L, LUA_REGISTRYINDEX);
  
  ngx_memzero(&ctx->timer, sizeof(ctx->timer));
  
  ctx->stream.prev_request_write_handler = NULL;
  ctx->stream.prev_request_read_handler = NULL;
  
  lua_pushnil(L);
  for(i=0; lua_next(L, 1) != 0; i++) {
    int readwrite = 0;
    
    // TODO: figure out if this is a TCP or UDP socket. they need slightly different treatment,
    //  but OpenResty doesn't make it easy to know. 
    
    //For now, assume it's a TCP socket
    
    ngx_stream_lua_socket_tcp_upstream_t        *u;
    if(lua_isnumber(L, -2)) { //numeric key, assume it's strictly for reading
      // { socket }
      if(!lua_istable(L, -1)) {
        select_fail_cleanup(L, ctx, i);
        return luaL_argerror(L, 1, "expected to have a table of sockets, but there's... something else there. something... wrong");
      }
      lua_rawgeti(L, -1, SOCKET_CTX_INDEX);
      u = lua_touserdata(L, -1);
      lua_pop(L, 1);
      readwrite = NGX_LUA_SELECT_READ;
    }
    else if(lua_isstring(L, -1) && lua_istable(L, -2)) {
      //{ [socket] = "rw" }
      const char *rw_string = lua_tostring(L, -1);
      if(strchr(rw_string, 'r')) {
        readwrite |= NGX_LUA_SELECT_READ;
      }
      if(strchr(rw_string, 'w')) {
        readwrite |= NGX_LUA_SELECT_WRITE;
      }
      if(readwrite == 0) {
        select_fail_cleanup(L, ctx, i);
        return luaL_argerror(L, 1, "invalid read-write string in socket table, must be one of 'r', 'w', or 'rw'");
      }
      lua_rawgeti(L, -2, SOCKET_CTX_INDEX);
      u = lua_touserdata(L, -1);
      lua_pop(L, 2); //pop the readwrite string
      lua_pushvalue(L, -1); //push the socket tabke for the luaL_ref further down
    }
    else {
      select_fail_cleanup(L, ctx, i);
      return luaL_argerror(L, 1, "invalid entry in socket table");
    }
    if(u == NULL) {
      select_fail_cleanup(L, ctx, i);
      return luaL_argerror(L, 1, "expected to have a table of sockets, but there's something amiss in that table");
    }
    
    if (u->peer.connection == NULL || u->read_closed) {
      select_fail_cleanup(L, ctx, i);
      return luaL_argerror(L, 1, "expected to have a table of active sockets, but at least one of them is closed");
    }
    
    if (u->request != r) {
      select_fail_cleanup(L, ctx, i);
      return luaL_argerror(L, 1, "expected to have a table of valid sockets, but at least one has a bad 'request'");
    }
    
    if(u->conn_waiting) {
      select_fail_cleanup(L, ctx, i);
      return luaL_argerror(L, 1, "expected to have a table of active sockets, but at least one is busy connective");
    }
    
    if((readwrite & NGX_LUA_SELECT_READ) && u->read_waiting) {
      select_fail_cleanup(L, ctx, i);
      return luaL_argerror(L, 1, "expected to have a table of active sockets, but at least one is busy waiting for read event");
    }
    
    if((readwrite & NGX_LUA_SELECT_WRITE) &&
      (u->write_waiting || (u->raw_downstream && (r->connection->buffered)))) {
      select_fail_cleanup(L, ctx, i);
      return luaL_argerror(L, 1, "expected to have a table of active sockets, but at least one is busy waiting for write event");
    }
    
    ctx->socket[i].readwrite = readwrite;
    ctx->socket[i].stream.tcp.up = u;
    
    //store the socket lua ref 'cause I don't know where openresty keeps it, nor do I want to rely on whatever that place is remaining the same throughout time
    ctx->socket[i].lua_socket_ref = luaL_ref(L, LUA_REGISTRYINDEX); //yeah luaL_ref pops that value for ya
    
    ngx_connection_t *c = u->peer.connection;
    ctx->socket[i].connection = c;
    
    ctx->socket[i].type = ngx_lua_select_get_socket_type(L, ctx->socket[i].lua_socket_ref, c, r->connection);
  }

  char *err;
  int sockets_already_ready = ngx_lua_select_sockets_ready(ctx, &err);
  if(err) {
    select_fail_cleanup(L, ctx, i);
    return luaL_error(L, err);
  }
  if(sockets_already_ready) {
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "Detected bytes already waiting to be read on socket when using the select module");
    int retcount = ngx_lua_select_push_result(L, ctx);
    ngx_stream_lua_select_ctx_cleanup_and_discard(r);
    return retcount;
  }
  
  for(i=0; i < socketcount; i++) {
    int rw = ctx->socket[i].readwrite;
    ngx_lua_select_socket_t *s = &ctx->socket[i];
    
    switch(s->type) {
      case NGX_LUA_SELECT_TCP_DOWNSTREAM:
        //this is the originating request's connection
        //it's handled differently than "upstreams" created with ngx.socket.tcp()
        //fuckin' openresty.. creating an ngx.req.socket() wraps the downstream connection in an upstream struct, but still completely bypasses this fake upstream's handlers
        //I don't blame you agentzh, you did what you had to do.
        
        if(rw & NGX_LUA_SELECT_READ) {
          ctx->stream.prev_request_read_handler = r->read_event_handler;
          r->read_event_handler = ngx_stream_lua_select_socket_read_request_handler;
          s->stream.prev_upstream_read_handler = NULL;
        }
        
        if(rw & NGX_LUA_SELECT_WRITE) {
          ctx->stream.prev_request_write_handler = r->write_event_handler;
          r->write_event_handler = ngx_stream_lua_select_socket_write_request_handler;
          s->stream.prev_upstream_write_handler = NULL;
        }
        break;
      
      case NGX_LUA_SELECT_TCP_UPSTREAM:
        if(rw & NGX_LUA_SELECT_READ) {
          s->stream.prev_upstream_read_handler = s->stream.tcp.up->read_event_handler;
          s->stream.tcp.up->read_event_handler = ngx_stream_lua_select_socket_read_upstream_handler;
        }
        else {
          s->stream.prev_upstream_read_handler = NULL;
        }
        
        if(rw & NGX_LUA_SELECT_WRITE) {
          s->stream.prev_upstream_write_handler = s->stream.tcp.up->write_event_handler;
          s->stream.tcp.up->write_event_handler = ngx_stream_lua_select_socket_write_upstream_handler;
        }
        else {
          s->stream.prev_upstream_write_handler = NULL;
        }
        break;
        
      case NGX_LUA_SELECT_UDP_UPSTREAM:
      case NGX_LUA_SELECT_UDP_DOWNSTREAM:
        select_fail_cleanup(L, ctx, i);
        return luaL_argerror(L, 1, "UDP sockets not yet supported");
        break;
    }
    
    ctx->socket[i].read_ready = 0;
    ctx->socket[i].write_ready = 0;
  }
  
  if(timeout > 0) {
    ctx->timer.cancelable = 1;
    ctx->timer.handler = ngx_stream_lua_select_timeout_handler;
    ctx->timer.data = r;
    ctx->timer.log = r->connection->log;
    ngx_add_timer(&ctx->timer, timeout);
  }
  
  ctx->post_completion_ev.posted = 0;
  ctx->post_completion_ev.data = r;
  ctx->post_completion_ev.handler = ngx_stream_lua_select_post_completion_handler;
  
  ctx->stream.r = r;
  ctx->stream.coctx = coctx;
  
  ngx_stream_lua_set_ctx(r, ctx, ngx_stream_lua_select_module);
  
  ngx_stream_lua_cleanup_pending_operation(coctx);
  coctx->cleanup = ngx_stream_lua_select_cleanup;
  coctx->data = r;
  
  
  return lua_yield(L, 0);
}

static void ngx_stream_lua_select_socket_event_activated(ngx_stream_lua_request_t *r) {
  ngx_lua_select_ctx_t                 *ctx;

  ctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_select_module);
  
  //don't do anything, just queue up the post-event-handler
  if(!ctx->post_completion_ev.posted) {
    ngx_post_event((&ctx->post_completion_ev), &ngx_posted_events);
  }
}


static void ngx_stream_lua_select_timeout_handler(ngx_event_t *ev) {
  ngx_stream_lua_request_t             *r = ev->data;
  ngx_stream_lua_select_socket_event_activated(r);
}
static void ngx_stream_lua_select_socket_write_upstream_handler(ngx_stream_lua_request_t *r, ngx_stream_lua_socket_tcp_upstream_t *up) {
  ngx_stream_lua_select_socket_event_activated(r);
}
static void ngx_stream_lua_select_socket_read_upstream_handler(ngx_stream_lua_request_t *r, ngx_stream_lua_socket_tcp_upstream_t *up) {
  ngx_stream_lua_select_socket_event_activated(r);
}
static void ngx_stream_lua_select_socket_write_request_handler(ngx_stream_lua_request_t *r) {
  ngx_stream_lua_select_socket_event_activated(r);
}
static void ngx_stream_lua_select_socket_read_request_handler(ngx_stream_lua_request_t *r) {
  ngx_stream_lua_select_socket_event_activated(r);
}


static void ngx_stream_lua_select_post_completion_handler(ngx_event_t *ev) {
  //modded copypasta from ngx_stream_lua_sleep.c ngx_stream_lua_sleep_handler()

  ngx_stream_lua_request_t        *r = ev->data;
  ngx_lua_select_ctx_t            *ctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_select_module);
  
  assert(r == ctx->stream.r);
  
  ngx_stream_lua_ctx_t            *streamctx;
  ngx_stream_lua_co_ctx_t         *coctx;

  coctx = ctx->stream.coctx;

  streamctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_module);

  if (streamctx == NULL) {
    return;
  }
  
  coctx->cleanup = NULL;

  streamctx->cur_co_ctx = coctx;

  if (streamctx->entered_content_phase) {
    ngx_stream_lua_select_resume(r);
  }
  else {
    streamctx->resume_handler = ngx_stream_lua_select_resume;
    ngx_stream_lua_core_run_phases(r);
  }
}

static ngx_int_t ngx_stream_lua_select_resume(ngx_stream_lua_request_t *r) {
  ngx_lua_select_ctx_t                 *ctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_select_module);
  ngx_stream_lua_ctx_t                 *streamctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_module);
  lua_State                            *L = ngx_stream_lua_get_lua_vm(r, streamctx);
  lua_State                            *coroL = streamctx->cur_co_ctx->co;
  
  //seems like this is the resume handler everyone else in ngx_stream_lua-land sets after theirs is finished
  streamctx->resume_handler = ngx_stream_lua_wev_handler;
  
  lua_createtable(coroL, 1, 1); //chances are just 1 socket was ready. doesn't matter if there are more or fewer though
  for(unsigned i=0; i<ctx->count; i++) {
    ngx_lua_select_socket_t   *s = &ctx->socket[i];
    ngx_connection_t          *c = s->connection;
    
    if((s->readwrite & NGX_LUA_SELECT_READ) && c->read->ready) {
      s->read_ready = 1;
    }
    if((s->readwrite & NGX_LUA_SELECT_WRITE) && c->write->ready) {
      s->write_ready = 1;
    }
  }
  int retcount = ngx_lua_select_push_result(coroL, ctx);
  ngx_stream_lua_select_ctx_cleanup_and_discard(r);
  
  //this is how the Lua thread is supposed to be resumed... this whole block of copypasta from EVERY resume handler inside Openresty.
  //you'd think this would be wrapped in a function or at least  macro or something...
  //nope.
  
  ngx_int_t           rc;
  ngx_connection_t   *c = r->connection;
  ngx_uint_t          nreqs = c->requests;

  rc = ngx_stream_lua_run_thread(L, r, streamctx, retcount);
  
  ngx_log_debug1(NGX_LOG_DEBUG_STREAM, r->connection->log, 0, "lua run thread returned %d", rc);

  if (rc == NGX_AGAIN) {
    return ngx_stream_lua_run_posted_threads(c, L, r, streamctx, nreqs);
  }

  if (rc == NGX_DONE) {
    ngx_stream_lua_finalize_request(r, NGX_DONE);
    return ngx_stream_lua_run_posted_threads(c, L, r, streamctx, nreqs);
  }

  if (streamctx->entered_content_phase) {
    ngx_stream_lua_finalize_request(r, rc);
    return NGX_DONE;
  }
  
  return rc;
}

static void ngx_stream_lua_select_cleanup(void *data) {
  ngx_stream_lua_co_ctx_t         *coctx = data;
  ngx_stream_lua_request_t        *r = coctx->data;
  ngx_stream_lua_select_ctx_cleanup_and_discard(r);
}

static void ngx_stream_lua_select_ctx_cleanup_and_discard(ngx_stream_lua_request_t *r) {
  ngx_lua_select_ctx_t            *ctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_select_module);
  ngx_stream_lua_ctx_t            *streamctx = ngx_stream_lua_get_module_ctx(r, ngx_stream_lua_module);
  lua_State                       *L = ngx_stream_lua_get_lua_vm(r, streamctx);
  
  if(ctx->timer.timer_set) {
    ngx_del_timer(&ctx->timer);
  }
  
  for(unsigned i=0; i<ctx->count; i++) {
    switch(ctx->socket[i].type) {
      case NGX_LUA_SELECT_TCP_UPSTREAM:
        assert(ctx->socket[i].stream.prev_upstream_read_handler);
        //restore upstream read event handler
        ctx->socket[i].stream.tcp.up->read_event_handler = ctx->socket[i].stream.prev_upstream_read_handler;
        break;
      
      case NGX_LUA_SELECT_TCP_DOWNSTREAM:
        //restore request handler if needed
        assert(ctx->stream.prev_request_read_handler);
        r->read_event_handler = ctx->stream.prev_request_read_handler;
        break;
      
      case NGX_LUA_SELECT_UDP_UPSTREAM:
      case NGX_LUA_SELECT_UDP_DOWNSTREAM:
        //NOT IMPLEMENTED
        raise(SIGABRT);
        break;
      default:
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "unexpected socket type on select cleanup");
        break;
    }
    /*
    if(c->read->active) {
      ngx_del_event(c->read, NGX_READ_EVENT, 0);
    }
    */
    
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->socket[i].lua_socket_ref);
  }
  
  luaL_unref(L, LUA_REGISTRYINDEX, ctx->ref);
  ngx_stream_lua_set_ctx(r, NULL, ngx_stream_lua_select_module); //for debugging purposes, really
}

static int ngx_stream_lua_select_init_code(lua_State * L) {
  
  lua_pushcfunction(L, lua_select_module_select);
  
  return 1;
}

static ngx_int_t ngx_stream_lua_select_init_preconfig(ngx_conf_t *cf) {
  return NGX_OK;
}

static ngx_int_t ngx_stream_lua_select_init_postconfig(ngx_conf_t *cf) {
  if (ngx_stream_lua_add_package_preload(cf, "ngx.select", ngx_stream_lua_select_init_code) != NGX_OK) {
    return NGX_ERROR;
  }
  return NGX_OK;
}
static ngx_int_t ngx_stream_lua_select_init_module(ngx_cycle_t *cycle) {
  
  return NGX_OK;
}


static ngx_int_t ngx_stream_lua_select_init_worker(ngx_cycle_t *cycle) {  
  return NGX_OK;
}

static void ngx_stream_lua_select_exit_worker(ngx_cycle_t *cycle) {
}

static void ngx_stream_lua_select_exit_master(ngx_cycle_t *cycle) {
  
}

static ngx_command_t ngx_stream_lua_select_commands[] = {
  ngx_null_command
};

static ngx_stream_module_t ngx_stream_lua_select_ctx = {
  ngx_stream_lua_select_init_preconfig,  /* preconfiguration */
  ngx_stream_lua_select_init_postconfig, /* postconfiguration */
  NULL,                          /* create main configuration */
  NULL,                          /* init main configuration */
  NULL,                          /* create server configuration */
  NULL,                          /* merge server configuration */
};

ngx_module_t ngx_stream_lua_select_module = {
  NGX_MODULE_V1,
  &ngx_stream_lua_select_ctx,         /* module context */
  ngx_stream_lua_select_commands,     /* module directives */
  NGX_STREAM_MODULE,                  /* module type */
  NULL,                               /* init master */
  ngx_stream_lua_select_init_module,  /* init module */
  ngx_stream_lua_select_init_worker,  /* init process */
  NULL,                               /* init thread */
  NULL,                               /* exit thread */
  ngx_stream_lua_select_exit_worker,  /* exit process */
  ngx_stream_lua_select_exit_master,  /* exit master */
  NGX_MODULE_V1_PADDING
};
