#ifndef NGX_LUA_SELECT_H
#define NGX_LUA_SELECT_H
#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>

extern ngx_module_t ngx_http_lua_select_module;
#ifdef NGX_STREAM_MODULE
extern ngx_module_t ngx_stream_lua_select_module;
#endif

#endif //NGX_LUA_SELECT_H
