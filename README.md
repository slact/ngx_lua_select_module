This module adds a global http and stream counter variable, `$global_counter`.

# Variables

`$global_counter` : Single global counter variable, shared between `http` and `stream` modules. Due to the quirkiness of Nginx, the variable value is updated once per HTTP request.
