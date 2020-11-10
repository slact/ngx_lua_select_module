#ifndef NGX_GLOBAL_COUNTER_VAR_H
#define NGX_GLOBAL_COUNTER_VAR_H
#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>

extern ngx_module_t ngx_http_global_counter_var_module;
#ifdef NGX_STREAM_MODULE
extern ngx_module_t ngx_stream_global_counter_var_module;
#endif

extern ngx_atomic_uint_t *ngx_global_counter;

#endif //NGX_GLOBAL_COUNTER_VAR_H
