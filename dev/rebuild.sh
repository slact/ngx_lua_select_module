#!/usr/bin/env zsh
DEV_PATH=$(realpath "`dirname \"$0\"`")
BASE_PATH=`realpath ${DEV_PATH}/..`
BUILD_PATH=${DEV_PATH}/build
BUILD_PREFIX=${DEV_PATH}/build/out

SRC_PATH=${BASE_PATH}/src

if command -v nproc >/dev/null; then
  _num_cores=`nproc`
else
  _num_cores=4
fi

OPENRESTY_VERSION=1.19.3.1
LUAJIT_USE_APICHECK=1
OPTIMIZE_LEVEL=0


_clang="clang -Qunused-arguments -fcolor-diagnostics"

#clang_memcheck="-fsanitize=address,undefined -fno-omit-frame-pointer"
clang_sanitize_memory="-use-gold-plugins -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer -fsanitize-blacklist=bl.txt"
clang_sanitize_addres="-fsanitize=address,undefined -fno-omit-frame-pointer"


CONFIGURE_WITH_DEBUG=0
_extra_config_opt=()

#export WITH_LUA_MODULE=1

for opt in $*; do
  case $opt in
    clang)
      CC=$_clang;;
    coverage)
      CC="$_clang -fprofile-instr-generate -fcoverage-mapping"
      ;;
    clang-sanitize|sanitize|sanitize-memory)
      CC="CMAKE_LD=llvm-link $_clang -Xclang -cc1 $clang_sanitize_memory "
      CLINKER=$clang
      ;;
    gcc-sanitize-undefined)
      SANITIZE_UNDEFINED=1
      ;;
    sanitize-address)
      CC="$_clang $clang_sanitize_addres";;
    gcc)
      CC="gcc";;
    gcc6)
      CC="gcc-6";;
    gcc5)
      CC="gcc-5";;
    gcc4|gcc47|gcc4.7)
      CC="gcc-4.7";;
    nopool|no-pool|nop) 
      NO_POOL=1;;
    dynamic)
      DYNAMIC=1;;
    re|remake|continue|c)
      REMAKE="-B"
      NO_CONFIGURE=1
      NO_EXTRACT_SOURCE=1
      ;;
    noextract)
      NO_EXTRACT_SOURCE=1;;
    noconfigure)
      NO_EXTRACT_SOURCE=1;;
    nomake)
      NO_MAKE=1;;
    nodebug)
      NO_DEBUG=1;;
    echo_module)
      WITH_NGX_ECHO_MODULE=1;;
    O0)
      optimize_level=0;;
    O1)
      optimize_level=1;;
    O2)
      optimize_level=2;;
    O3)
      optimize_level=3;;
    Og)
      optimize_level=g;;
    release=*)
      RELEASE="${opt:8}";;
    withdebug)
      CONFIGURE_WITH_DEBUG=1;;
    clang-analyzer|analyzer|scan|analyze)
      CC="clang"
      CLANG_ANALYZER=$DEV_PATH/clang-analyzer
      mkdir $CLANG_ANALYZER 2>/dev/null
      ;;
    stub_status)
      WITH_STUB_STATUS_MODULE=1
      ;;
    default_prefix)
      DEFAULT_PREFIX=1;;
    prefix=*)
      CUSTOM_PREFIX="${opt:7}";;
    openresty=*)
      OPENRESTY_VERSION="${opt:10}"
      ;;
    lua_no_jit)
      LUAJIT_DISABLE_JIT=1;;
    lua_no_apicheck|lua_no_debug)
      LUAJIT_USE_APICHECK="";;
    lua_internal_debug)
      LUAJIT_USE_ASSERT=1;;
    lua_valgrind)
      LUAJIT_USE_VALGRIND=1;;
    valgrind)
      NO_POOL=1
      LUAJIT_USE_VALGRIND=1
      ;;
    clean_source)
      CLEAN_SOURCE=1;;
    clean)
      CLEAN=1;;
    --*)
      _extra_config_opt+=( "$opt" )
  esac
done

NO_WITH_DEBUG=$NO_WITH_DEBUG;
EXTRA_CONFIG_OPT="`echo $_extra_config_opt`"

_prepare_build() {
  mkdir -p $BUILD_PATH 2>/dev/null
  cd $BUILD_PATH
  
  if [[ $CLEAN_SOURCE == 1 ]]; then
    rm -rf openresty-${OPENRESTY_VERSION}
  fi
  
  if [[ $NO_EXTRACT_SOURCE == 1 ]]; then
    echo "skipped extracting Openresty source, as requested"
    return 0
  fi
  
  openresty_source="https://openresty.org/download/openresty-${OPENRESTY_VERSION}.tar.gz"
  wget --no-clobber "$openresty_source" || exit 1

  if [[ ! -d "./openresty-${OPENRESTY_VERSION}" ]]; then
    echo "tar -xf openresty-${OPENRESTY_VERSION}.tar.gz"
    tar -xf openresty-${OPENRESTY_VERSION}.tar.gz
  fi
  
  ln -sf "openresty-${OPENRESTY_VERSION}" "openresty"
  ln -sf "openresty-${OPENRESTY_VERSION}/nginx" "nginx"
  return 0
}

_run_configure_step() {
  if [[ $NO_CONFIGURE == 1 ]]; then
    echo "skipping ./configure, as requested"
    return 0
  fi
  
  if [[ -z $NO_DEBUG ]]; then
    #debug build. clear cflags
    CFLAGS=" -ggdb -fvar-tracking-assignments -O$OPTIMIZE_LEVEL"
  fi

  CFLAGS="$CFLAGS -Wno-error -Wall -Wextra -Wno-unused-parameter -Wpointer-sign -Wpointer-arith -Wshadow -Wnested-externs -Wsign-compare"
  if [[ $SANITIZE_UNDEFINED == 1 ]]; then
      CFLAGS="$CFLAGS -fsanitize=undefined -fsanitize=shift -fsanitize=integer-divide-by-zero -fsanitize=unreachable -fsanitize=vla-bound -fsanitize=null -fsanitize=return -fsanitize=bounds -fsanitize=alignment -fsanitize=object-size -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fsanitize=nonnull-attribute -fsanitize=returns-nonnull-attribute -fsanitize=enum -lubsan"
  fi
  
  
  cd $BUILD_PATH/openresty
  CFLAGS="${CFLAGS/-Werror/}" #no warning-as-error
  CONFIGURE=()
  LUAJIT_XCFLAGS=()
  
  if [[ $LUAJIT_DISABLE_JIT == 1 ]]; then
    LUAJIT_XCFLAGS+=( -DLUAJIT_DISABLE_JIT )
  fi
  if [[ $LUAJIT_USE_APICHECK == 1 ]]; then
    LUAJIT_XCFLAGS+=( -DLUA_USE_APICHECK )
  fi
  if [[ $LUAJIT_USE_ASSERT == 1 ]]; then
    LUAJIT_XCFLAGS+=( -DLUA_USE_ASSERT )
  fi
  if [[ $LUAJIT_USE_VALGRIND == 1 ]]; then
    LUAJIT_XCFLAGS+=( -DLUAJIT_USE_VALGRIND )
  fi
  
  CC=${CC:=cc}
  
  _tmp_path="/tmp"
  _pid_path="/run"
  _lock_path="/var/lock"
  _access_log="/dev/stdout"
  _error_log="errors.log"

  if [[ -n $NO_POOL ]]; then
    CONFIGURE+=( --with-no-pool-patch )
  fi
  
  if [[ -n $EXTRA_CONFIG_OPT ]]; then
    if [[ $0 == "/usr/bin/makepkg" ]]; then
      CONFIGURE+=${EXTRA_CONFIG_OPT}
    else
      CONFIGURE+=( ${=EXTRA_CONFIG_OPT} )
    fi    
  fi

  if [[ -z $DEFAULT_PREFIX ]]; then
    if [[ -n $CUSTOM_PREFIX ]]; then
      _prefix_path=$CUSTOM_PREFIX
    else
      _prefix_path="/etc/$_name"
    fi
  else
    _prefix_path="/usr/local/$_name"
  fi
  
  if [[ -z $DEFAULT_PREFIX ]]; then
    CONFIGURE+=( --prefix=$_prefix_path )
  fi
  
    CONFIGURE+=(
    --prefix=${BUILD_PREFIX}
    --sbin-path=${BUILD_PREFIX}/bin/nginx
    --pid-path=${_pid_path}/nginx.pid
    --lock-path=${_pid_path}/nginx.lock
    --http-client-body-temp-path=${_tmp_path}/client_body_temp
    --http-proxy-temp-path=${_tmp_path}/proxy_temp
    --http-fastcgi-temp-path=${_tmp_path}/fastcgi_temp
    --http-uwsgi-temp-path=${_tmp_path}/uwsgi_temp
    --http-log-path=${_access_log}
    --error-log-path=${_error_log}
    -j${_num_cores}
  )

  if [[ -n $WITH_STUB_STATUS_MODULE ]]; then
    CONFIGURE+=( --with-http_stub_status_module )
  fi
  
  if [[ $DYNAMIC == 1 ]]; then
    CONFIGURE+=( --add-dynamic-module=${BASE_PATH} )
  else
    CONFIGURE+=( --add-module=${BASE_PATH} )
  fi
  
  if [[ $CONFIGURE_WITH_DEBUG == 1 ]]; then
    CONFIGURE+=( "--with-debug" )
  fi
  
  if [[ -z $NO_NGINX_USER ]]; then
    CONFIGURE+=( "--user=${_user}" "--group=${_group}" )
  fi
  
  if [[ $SANITIZE_UNDEFINED == 1 ]]; then
    LDFLAGS="$LDFLAGS -lubsan"
  fi
  
  if [[ $CC == *clang* ]] || [[ "$OSTYPE" == "darwin"* ]]; then
    #not a valid clang parameter
    CFLAGS="${CFLAGS/-fvar-tracking-assignments/}"
    CFLAGS="${CFLAGS/-fvar-tracking-assignments/}"
    if [[ -z $CLANG_ANALYZER ]]; then
      CFLAGS="-ferror-limit=5 $CFLAGS -Wconditional-uninitialized"
    fi
  elif [[ $CC == "cc" ]] || [[ $CC == "gcc" ]] || [[ -z $CC ]] && [[ -z $NO_GCC_COLOR ]]; then
    CFLAGS="-fdiagnostics-color=always  -Wmaybe-uninitialized $CFLAGS"
  fi
  
  if [[ ! -z $OPTIMIZE_LEVEL ]]; then
    CFLAGS="-O${OPTIMIZE_LEVEL} $CFLAGS"
  fi
  
  export CFLAGS=$CFLAGS
  export CC=$CC
  export LDFLAGS=$LDFLAGS
  
  if ! [[ -z $CLANG_ANALYZER ]]; then
    scan-build -o "$CLANG_ANALYZER" ./configure "--with-cc=${CC}" "--with-cc-opt=${CFLAGS}" "--with-ld-opt=${LDFLAGS}" "--with-luajit-ldflags=${LDFLAGS}" "--with-luajit-xcflags=${LUAJIT_XCFLAGS}" ${CONFIGURE[@]}
  else
    if command -v ccache >/dev/null && [[ ! $CC == ccache* ]]; then
      CC="ccache ${CC}"
    fi
    pwd;
    echo ./configure --with-cc="${CC}" "--with-cc-opt=${CFLAGS}" "--with-ld-opt=${LDFLAGS}" "--with-luajit-ldflags=${LDFLAGS}" "--with-luajit-xcflags=${LUAJIT_XCFLAGS}" ${CONFIGURE[@]}
    ./configure "--with-cc=${CC}" "--with-cc-opt=${CFLAGS}" "--with-ld-opt=${LDFLAGS}" "--with-luajit-ldflags=${LDFLAGS}" "--with-luajit-xcflags=${LUAJIT_XCFLAGS}" ${CONFIGURE[@]}
  fi
  
  return $?
}

_run_build_step() {
  cd ${BUILD_PATH}/openresty
  if ! [[ -z $CLANG_ANALYZER ]]; then
    CFLAGS=$CFLAGS scan-build -o "$CLANG_ANALYZER" make -j${_num_cores}
  else
    make $REMAKE -j${_num_cores}
  fi
  return $?
}

_run_install_step() {
  ln -sf "${BUILD_PATH}"/openresty/bundle/nginx-1* "${DEV_PATH}/src"

  if ! [[ -z $CLANG_ANALYZER ]]; then
    cd $CLANG_ANALYZER >/dev/null
    latest_scan=`ls -c |head -n1`
    echo "run 'scan-view ${CLANG_ANALYZER}/${latest_scan}' for static analysis."
    scan-view $latest_scan 2>/dev/null
  else
    echo make install -j${_num_cores} 
    cd ${BUILD_PATH}/openresty
    make install -j${_num_cores} >/dev/null
  fi
  return $?
}

_prepare_build && _run_configure_step && _run_build_step && _run_install_step
