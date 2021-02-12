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
      ngx_stream_lua_socket_tcp_upstream_t          *up;
      ngx_stream_lua_socket_tcp_upstream_handler_pt  prev_upstream_read_handler;
    } stream;
  #endif
  };
  int                                    lua_socket_ref;
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
      ngx_stream_lua_request_t *r;
      ngx_stream_lua_co_ctx_t  *coctx;
      ngx_stream_lua_event_handler_pt prev_request_read_handler;
    } stream;
  #endif
  };
  ngx_event_t             post_completion_ev; //added to ngx_post_event_queue when at least one event is activated
  int                     ref; //registry reference
  size_t                  count;
  ngx_lua_select_socket_t socket[];
} ngx_lua_select_ctx_t;

#endif //NGX_LUA_SELECT_H
