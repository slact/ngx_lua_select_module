#ifndef NGX_LUA_SELECT_H
#define NGX_LUA_SELECT_H
#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>

#include <lua.h>
#include <lauxlib.h>

#ifdef NGX_HTTP_MODULE
#include <ngx_http_lua_socket_tcp.h>
#include <ngx_http_lua_socket_udp.h>
extern ngx_module_t ngx_http_lua_select_module;
#endif
#ifdef NGX_STREAM_MODULE
#include <ngx_stream_lua_socket_tcp.h>
#include <ngx_stream_lua_socket_udp.h>
extern ngx_module_t ngx_stream_lua_select_module;
#endif

#define NGX_LUA_SELECT_READ 1
#define NGX_LUA_SELECT_WRITE 2

typedef enum {
  NGX_LUA_SELECT_TCP_UPSTREAM,
  NGX_LUA_SELECT_TCP_DOWNSTREAM,
  NGX_LUA_SELECT_UDP_UPSTREAM,
  NGX_LUA_SELECT_UDP_DOWNSTREAM,
} ngx_lua_select_socktype_t;

//copypasta from openresty
enum {
  SOCKET_CTX_INDEX = 1,
  SOCKET_KEY_INDEX = 3,
  SOCKET_CONNECT_TIMEOUT_INDEX = 2,
  SOCKET_SEND_TIMEOUT_INDEX = 4,
  SOCKET_READ_TIMEOUT_INDEX = 5,
};

typedef struct {

  union {
  #ifdef NGX_HTTP_MODULE
    struct {
      ngx_http_lua_socket_tcp_upstream_t *up;
    } http;
  #endif
  #ifdef NGX_STREAM_MODULE
    struct {
      union {
        struct {
          ngx_stream_lua_socket_tcp_upstream_t          *up;
        } tcp;
        struct {
          ngx_stream_lua_socket_udp_upstream_t          *up;
        } udp;
      };
      ngx_stream_lua_socket_tcp_upstream_handler_pt  prev_upstream_read_handler;
      ngx_stream_lua_socket_tcp_upstream_handler_pt  prev_upstream_write_handler;
    } stream;
  #endif
  };
  ngx_connection_t                      *connection;
  ngx_lua_select_socktype_t              type;
  int                                    lua_socket_ref;
  unsigned                               readwrite:2;
  unsigned                               read_ready:1;
  unsigned                               write_ready:1;
} ngx_lua_select_socket_t;

typedef struct {
  union {
  #ifdef NGX_HTTP_MODULE
    struct {
      ngx_http_request_t *r;
    } http;
  #endif
  #ifdef NGX_STREAM_MODULE
    struct {
      ngx_stream_lua_request_t             *r;
      ngx_stream_lua_co_ctx_t              *coctx;
      ngx_stream_lua_event_handler_pt prev_request_read_handler;
      ngx_stream_lua_event_handler_pt prev_request_write_handler;
    } stream;
  #endif
  };
  ngx_event_t             post_completion_ev; //added to ngx_post_event_queue when at least one event is activated
  ngx_event_t             timer;
  int                     ref; //registry reference
  size_t                  count;
  ngx_lua_select_socket_t socket[];
} ngx_lua_select_ctx_t;

ngx_lua_select_socktype_t ngx_lua_select_get_socket_type(lua_State *L, int lua_socket_ref, ngx_connection_t *c, ngx_connection_t *request_connection);
int ngx_lua_select_push_result(lua_State *L, ngx_lua_select_ctx_t *ctx);
int ngx_lua_select_sockets_ready(ngx_lua_select_ctx_t *ctx, char **err);

//debug funcs
char *ngx_lua_select_luaS_dbgval(lua_State *L, int n);
void ngx_lua_select_luaS_printstack_named(lua_State *L, const char *name);
int ngx_lua_select_module_sigstop(lua_State *L);

#endif //NGX_LUA_SELECT_H
