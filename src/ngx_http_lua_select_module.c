#include <ngx_http.h>
#include "ngx_lua_select.h"
#include <assert.h>

static ngx_int_t ngx_http_lua_select_init_preconfig(ngx_conf_t *cf) {
  return NGX_OK;
}

static ngx_int_t ngx_http_lua_select_init_postconfig(ngx_conf_t *cf) {
  return NGX_OK;
}
static ngx_int_t ngx_http_lua_select_init_module(ngx_cycle_t *cycle) {
  return NGX_OK;
}

static ngx_int_t ngx_http_lua_select_init_worker(ngx_cycle_t *cycle) {  
  return NGX_OK;
}

static void ngx_http_lua_select_exit_worker(ngx_cycle_t *cycle) { 
  
}

static void ngx_http_lua_select_exit_master(ngx_cycle_t *cycle) {
  
}

static ngx_command_t ngx_http_lua_select_commands[] = {
  ngx_null_command
};

static ngx_http_module_t ngx_http_lua_select_ctx = {
  ngx_http_lua_select_init_preconfig,  /* preconfiguration */
  ngx_http_lua_select_init_postconfig, /* postconfiguration */
  NULL,                                /* create main configuration */
  NULL,                                /* init main configuration */
  NULL,                                /* create server configuration */
  NULL,                                /* merge server configuration */
  NULL,                                /* create location configuration */
  NULL,                                /* merge location configuration */
};

ngx_module_t ngx_http_lua_select_module = {
  NGX_MODULE_V1,
  &ngx_http_lua_select_ctx,           /* module context */
  ngx_http_lua_select_commands,       /* module directives */
  NGX_HTTP_MODULE,                    /* module type */
  NULL,                               /* init master */
  ngx_http_lua_select_init_module,    /* init module */
  ngx_http_lua_select_init_worker,    /* init process */
  NULL,                               /* init thread */
  NULL,                               /* exit thread */
  ngx_http_lua_select_exit_worker,    /* exit process */
  ngx_http_lua_select_exit_master,    /* exit master */
  NGX_MODULE_V1_PADDING
};
