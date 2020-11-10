#include <ngx_http.h>
#include "ngx_global_counter_var.h"
#include <assert.h>

ngx_atomic_uint_t *ngx_global_counter = NULL;

static void set_varval(ngx_http_variable_value_t *v, u_char *data, size_t len) {
  v->valid = 1;
  v->no_cacheable = 1;
  v->not_found = 0;
  v->len = len;
  v->data = data;
}

static ngx_int_t ngx_http_global_counter_var_get_handler(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
  u_char                *buf = ngx_http_get_module_ctx(r, ngx_http_global_counter_var_module);
  if(!buf) {
    buf = ngx_palloc(r->pool, NGX_INT_T_LEN + 1);
    if(!buf) {
      v->valid = 0;
      return NGX_ERROR;
    }
    ngx_http_set_ctx(r, buf, ngx_http_global_counter_var_module);
  }
  
  ngx_atomic_uint_t     current_val = ngx_atomic_fetch_add(ngx_global_counter, 1);
  
  size_t len = ngx_sprintf(buf, "%ui", current_val) - buf;
  set_varval(v, buf, len);
  
  return NGX_OK;
}


static ngx_int_t ngx_http_global_counter_var_init_preconfig(ngx_conf_t *cf) {
  ngx_http_variable_t           *var;
  ngx_str_t                      name = ngx_string("global_counter");
  var = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_NOHASH);
  if(!var) {
    return NGX_ERROR;
  }
  var->get_handler = ngx_http_global_counter_var_get_handler;
  return NGX_OK;
  
}

static ngx_int_t initialize_shm(ngx_shm_zone_t *zone, void *data) {
  if(data) { //zone being passed after restart
    zone->data = data;
  }
  ngx_global_counter = zone->shm.addr;
  if(!data) {
    *ngx_global_counter = 0;
  }
  return NGX_OK;
}


static ngx_int_t ngx_http_global_counter_var_init_postconfig(ngx_conf_t *cf) {
  ngx_shm_zone_t    *zone;
  static ngx_str_t   name = ngx_string("global_counter_var_shmem");
  zone = ngx_shared_memory_add(cf, &name, ngx_pagesize, &ngx_http_global_counter_var_module);
  if (zone == NULL) {
    return NGX_ERROR;
  }

  zone->init = initialize_shm;
  zone->data = (void *) 1;
  return NGX_OK;
}
static ngx_int_t ngx_http_global_counter_var_init_module(ngx_cycle_t *cycle) {
  return NGX_OK;
}


static ngx_int_t ngx_http_global_counter_var_init_worker(ngx_cycle_t *cycle) {  
  return NGX_OK;
}

static void ngx_http_global_counter_var_exit_worker(ngx_cycle_t *cycle) { 
  
}

static void ngx_http_global_counter_var_exit_master(ngx_cycle_t *cycle) {
  
}

static ngx_command_t ngx_http_global_counter_var_commands[] = {
  ngx_null_command
};

static ngx_http_module_t ngx_http_global_counter_var_ctx = {
  ngx_http_global_counter_var_init_preconfig, /* preconfiguration */
  ngx_http_global_counter_var_init_postconfig, /* postconfiguration */
  NULL,                          /* create main configuration */
  NULL,                          /* init main configuration */
  NULL,                          /* create server configuration */
  NULL,                          /* merge server configuration */
  NULL,                          /* create location configuration */
  NULL,                          /* merge location configuration */
};

ngx_module_t ngx_http_global_counter_var_module = {
  NGX_MODULE_V1,
  &ngx_http_global_counter_var_ctx,           /* module context */
  ngx_http_global_counter_var_commands,       /* module directives */
  NGX_HTTP_MODULE,                            /* module type */
  NULL,                                       /* init master */
  ngx_http_global_counter_var_init_module,    /* init module */
  ngx_http_global_counter_var_init_worker,    /* init process */
  NULL,                                       /* init thread */
  NULL,                                       /* exit thread */
  ngx_http_global_counter_var_exit_worker,    /* exit process */
  ngx_http_global_counter_var_exit_master,    /* exit master */
  NGX_MODULE_V1_PADDING
};
