This module adds a global autoincrementing http and stream counter variable, `$global_counter`.

# Building
Follow the [standard instructions on building Nginx modules](https://www.nginx.com/resources/wiki/extending/compiling/). The build generates `ngx_http_global_counter_var_module`, and if built with `--with-stream`, also `ngx_stream_global_counter_var_module`.

# Variables

`$global_counter` : Single autoimcrementing global counter variable, shared between `http` and `stream` modules. Due to the quirkiness of Nginx, the variable value is updated once per HTTP request, and once per string evaluation per stream connection."
