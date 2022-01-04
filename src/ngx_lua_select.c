#include <ngx_stream.h>
#include "ngx_lua_select.h"
#include <ngx_stream_lua_util.h>
#include <assert.h>
#include <sys/ioctl.h>


int ngx_lua_select_push_result(lua_State *L, ngx_lua_select_ctx_t *ctx) {
  int ready_count = 0;
  lua_createtable(L, 1, 1); //chances are just 1 socket was ready. doesn't matter if there are more or fewer though
  int socktable_index = lua_gettop(L);
  
  for(unsigned i=0; i<ctx->count; i++) {
    ngx_lua_select_socket_t *s = &ctx->socket[i];
    int                      sockref = s->lua_socket_ref;

    if(! s->read_ready && !s->write_ready) {
      continue;
    }
    
    int sane_stack_top = lua_gettop(L);
    ready_count++;
    lua_checkstack(L, 3);
    lua_rawgeti(L, LUA_REGISTRYINDEX, sockref);
    lua_pushvalue(L, -1);
    if(s->read_ready && s->write_ready) {
      lua_pushliteral(L, "rw");
    }
    else if(s->read_ready) {
      lua_pushliteral(L, "r");
    }
    else {
      lua_pushliteral(L, "w");
    }
    lua_settable(L, socktable_index);
    lua_rawseti(L, socktable_index, ready_count);
    assert(lua_gettop(L) == sane_stack_top);
  }

  if(ready_count == 0 && ctx->timer.timedout) {
    //timeout error
    lua_pop(L, 1); //pop the empty ready-sockets table
    lua_pushnil(L);
    lua_pushliteral(L, "timeout");
    return 2;
  }
  return 1;
}

ngx_lua_select_socktype_t ngx_lua_select_get_socket_type(lua_State *L, int lua_socket_ref, ngx_connection_t *c, ngx_connection_t *request_connection) {
  int     type;
  
  //TODO: handle UDP sockets maybe?
  
  type = SOCK_STREAM;
  
  if(c == request_connection) {
    return type == SOCK_STREAM ? NGX_LUA_SELECT_TCP_DOWNSTREAM : NGX_LUA_SELECT_UDP_DOWNSTREAM;
  }
  else {
    return type == SOCK_STREAM ? NGX_LUA_SELECT_TCP_UPSTREAM : NGX_LUA_SELECT_UDP_UPSTREAM;
  }
}

int ngx_lua_select_sockets_ready(ngx_lua_select_ctx_t *ctx, char **err) {
  int        ready_count = 0;
  unsigned   i;
  for(i=0; i<ctx->count; i++) {
    int bytesready = 0;
    int fd = ctx->socket[i].connection->fd;
    if(ioctl(fd, FIONREAD,&bytesready) < 0) {
      *err = "ioctl error";
      return 0;
    }
    
    if(bytesready > 0) {
      ctx->socket[i].read_ready = 1;
      ready_count++;
    }
  }
  *err = NULL;
  return ready_count;
}

char *ngx_lua_select_luaS_dbgval(lua_State *L, int n) {
  static char buf[512];
  int         type = lua_type(L, n);
  const char *typename = lua_typename(L, type);
  const char *str;
  lua_Number  num;
  
  char *cur = buf;
  int top = lua_gettop(L);
  switch(type) {
    case LUA_TNUMBER:
      num = lua_tonumber(L, n);
      sprintf(cur, "%s: %f", typename, num);
      break;
    case LUA_TBOOLEAN:
      sprintf(cur, "%s: %s", typename, lua_toboolean(L, n) ? "true" : "false");
      break;
    case LUA_TSTRING:
      str = lua_tostring(L, n);
      sprintf(cur, "%s: \"%.50s%s\"", typename, str, strlen(str) > 50 ? "..." : "");
      break;
    case LUA_TTABLE:
      luaL_checkstack(L, 8, NULL);
      if(lua_status(L) == LUA_OK) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, n);
        lua_call(L, 1, 1);
        str = lua_tostring(L, -1);
        cur += sprintf(cur, "%s", str);
        lua_pop(L, 1);
      }
      else {
        cur += sprintf(cur, "table: %p", lua_topointer(L, n));
      }
      
      
      //is it a global?
      if(lua_equal(L, -1, LUA_GLOBALSINDEX)) {
        //it's the globals table
        sprintf(cur, "%s", " _G");
        lua_pop(L, 1);
        break;
      }
      lua_pushnil(L);
      while(lua_next(L, LUA_GLOBALSINDEX)) {
        if(lua_equal(L, -1, n)) {
          cur += sprintf(cur, " _G[\"%s\"]", lua_tostring(L, -2));
          lua_pop(L, 2);
          break;
        }
        lua_pop(L, 1);
      }
      
      //is it a loaded module?
      lua_getglobal(L, "package");
      lua_getfield(L, -1, "loaded");
      lua_remove(L, -2);
      lua_pushnil(L);  // first key
      while(lua_next(L, -2) != 0) {
        
        if(lua_equal(L, -1, n)) {
        //it's the globals table
          sprintf(cur, " module \"%s\"", lua_tostring(L, -2));
          lua_pop(L, 2);
          break;
        }
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
      break;
    case LUA_TLIGHTUSERDATA:
      luaL_checkstack(L, 2, NULL);
      if(lua_status(L) == LUA_OK) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, n);
        lua_call(L, 1, 1);
        sprintf(cur, "light %s", lua_tostring(L, -1));
        lua_pop(L, 1);
      }
      else {
        sprintf(cur, "light userdata: %p", lua_topointer(L, n));
      }
      break;
    case LUA_TFUNCTION: {
      luaL_checkstack(L, 3, NULL);
      lua_Debug dbg;
      lua_pushvalue(L, n);
      lua_getinfo(L, ">nSlu", &dbg);
      
      if(lua_status(L) == LUA_OK) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, n);
        lua_call(L, 1, 1);
      }
      else {
        lua_pushfstring(L, "function: %p", lua_topointer(L, n));
      }
      
      sprintf(cur, "%s%s%s%s%s%s %s:%d", lua_iscfunction(L, n) ? "c " : "", lua_tostring(L, -1), strlen(dbg.namewhat)>0 ? " ":"", dbg.namewhat, dbg.name?" ":"", dbg.name?dbg.name:"", dbg.short_src, dbg.linedefined);
      lua_pop(L, 1);
      
      break;
    }
    case LUA_TTHREAD: {
      lua_State *coro = lua_tothread(L, n);
      luaL_checkstack(L, 4, NULL);
      luaL_checkstack(coro, 1, NULL);
      
      if(lua_status(L) == LUA_OK) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, n);
        lua_call(L, 1, 1);
      }
      else {
        lua_pushfstring(L, "thread: %p", lua_topointer(L, n));
      }
      
      char *status = NULL;
      switch(lua_status(L)) {
        case LUA_OK: {
          lua_Debug ar;
          if (lua_getstack(L, 0, &ar) > 0) {  // does it have frames? 
            status = "normal";
          }
          else if (lua_gettop(L) == 0) {
            status = "dead";
          }
          else {
            status ="suspended";  // initial state 
          }
          break;
        }
        case LUA_YIELD:
          status = "suspended";
          break;
        default:
          status = "dead";
          break;
      }
      lua_pushstring(L, status);
      
      luaL_where(coro, 1);
      if(L == coro) {
        sprintf(cur, "%s (self) (%s) @ %s", lua_tostring(L, -3), lua_tostring(L, -2), lua_tostring(coro, -1));
        lua_pop(L, 3);
      }
      else {
        sprintf(cur, "%s (%s) @ %s", lua_tostring(L, -2), lua_tostring(L, -1), lua_tostring(coro, -1));
        lua_pop(L, 2);
        lua_pop(coro, 1);
      }
      break;
    }
    
    case LUA_TNIL:
      sprintf(cur, "%s", "nil");
      break;
    
    default:
      if(lua_status(L) == LUA_OK) {
        luaL_checkstack(L, 2, NULL);
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, n);
        lua_call(L, 1, 1);
        str = lua_tostring(L, -1);
        
        sprintf(cur, "%s", str);
        lua_pop(L, 1);
      }
      else {
        sprintf(cur, "%s: %p", lua_typename(L, type), lua_topointer(L, n));
      }
      break;
  }
  assert(lua_gettop(L)==top);
  return buf;
}
void ngx_lua_select_luaS_printstack_named(lua_State *L, const char *name) {
  int        top = lua_gettop(L);
  const char line[256];
  luaL_Buffer buf;
  luaL_buffinit(L, &buf);
  
  sprintf((char *)line, "lua stack %s:", name);
  luaL_addstring(&buf, line);
  
  for(int n=top; n>0; n--) {
    snprintf((char *)line, 256, "\n                               [%-2i  %i]: %s", -(top-n+1), n, ngx_lua_select_luaS_dbgval(L, n));
    luaL_addstring(&buf, line);
  }
  luaL_checkstack(L, 1, NULL);
  luaL_pushresult(&buf);
  const char *stack = lua_tostring(L, -1);
  ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "%s", stack);
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
}

int ngx_lua_select_module_sigstop(lua_State *L) {
  raise(SIGSTOP);
  return 1;
}
