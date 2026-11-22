# Copyright (c) 2026 Daniel Tillett. Part of Klin; see the LICENSE file.
# Build LibU (https://github.com/koanlogic/libu) from the pinned checkout as
# the static library target 'libu', replacing LibU's MaKL configure/make.
#
# Inputs:  LIBU_SOURCE_DIR (checkout root), LIBU_ENABLE_SCTP (option)
# Outputs: target 'libu', with the generated u/libu_conf.h on its include path

include(CheckCSourceCompiles)
include(CheckIncludeFiles)
include(CheckStructHasMember)
include(CheckSymbolExists)

if(NOT EXISTS "${LIBU_SOURCE_DIR}/srcs/toolbox/memory.c")
    message(FATAL_ERROR
        "LibU sources not found in '${LIBU_SOURCE_DIR}'. Run "
        "'python3 build/build.py' once to fetch the pinned dependencies, or "
        "set -DLIBU_SOURCE_DIR=<path to a LibU checkout>.")
endif()

file(READ "${LIBU_SOURCE_DIR}/VERSION" LIBU_VERSION)
string(STRIP "${LIBU_VERSION}" LIBU_VERSION)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${LIBU_VERSION}")
set(LIBU_VERSION_MAJOR ${CMAKE_MATCH_1})
set(LIBU_VERSION_MINOR ${CMAKE_MATCH_2})
set(LIBU_VERSION_TEENY ${CMAKE_MATCH_3})

# platform
if(WIN32)
    set(OS_WIN 1)
    set(LIBU_PATH_SEPARATOR 0x5c)
    set(RB_INHIBIT_MMAP 1)
else()
    set(OS_UNIX 1)
    set(LIBU_PATH_SEPARATOR "'/'")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OS_LINUX 1)
elseif(APPLE)
    set(OS_DARWIN 1)
endif()

# Checks run with the same feature macros LibU is compiled with.
set(_libu_defs "")
if(OS_LINUX)
    list(APPEND _libu_defs -D_GNU_SOURCE)
endif()
set(CMAKE_REQUIRED_DEFINITIONS ${_libu_defs})
set(CMAKE_REQUIRED_QUIET ON)

# libu_check_type(<var> <type> [headers...]): <var> is set if <type> exists.
function(libu_check_type var type)
    set(_src "")
    foreach(_h IN LISTS ARGN)
        string(APPEND _src "#include <${_h}>\n")
    endforeach()
    string(APPEND _src "int main(void) { ${type} *v = 0; (void) v; return (int) sizeof(${type}); }\n")
    check_c_source_compiles("${_src}" ${var})
endfunction()

libu_check_type(HAVE_LONG_DOUBLE "long double")
libu_check_type(HAVE_LONG_LONG "long long")
libu_check_type(HAVE_INTMAX_T "intmax_t" stdint.h)
libu_check_type(HAVE__BOOL "_Bool" stdbool.h)
libu_check_type(HAVE_FLOAT__COMPLEX "float _Complex" complex.h)
libu_check_type(HAVE_DOUBLE__COMPLEX "double _Complex" complex.h)
libu_check_type(HAVE_LONG_DOUBLE__COMPLEX "long double _Complex" complex.h)
libu_check_type(HAVE_PID_T "pid_t" sys/types.h)
libu_check_type(HAVE_SSIZE_T "ssize_t" sys/types.h)
libu_check_type(HAVE_SOCKLEN_T "socklen_t" sys/types.h sys/socket.h)
libu_check_type(HAVE_IN_ADDR_T "in_addr_t" sys/types.h netinet/in.h)
libu_check_type(HAVE_STRUCT_RUSAGE "struct rusage" sys/types.h sys/resource.h)

check_symbol_exists(TCP_NODELAY "sys/types.h;netinet/tcp.h" HAVE_TCP_NODELAY)
check_symbol_exists(INET_ADDRSTRLEN "netinet/in.h" HAVE_INET_ADDRSTRLEN)
check_symbol_exists(SO_BROADCAST "sys/socket.h" HAVE_SO_BROADCAST)
check_symbol_exists(MAP_FIXED "sys/mman.h" HAVE_MAP_FIXED)

foreach(_f
        "daemon|stdlib.h;unistd.h"
        "setsockopt|sys/types.h;sys/socket.h"
        "getsockopt|sys/types.h;sys/socket.h"
        "getpid|unistd.h"
        "link|unistd.h"
        "unlink|unistd.h"
        "sleep|unistd.h"
        "mkstemps|stdlib.h;unistd.h"
        "setenv|stdlib.h"
        "mmap|sys/types.h;sys/mman.h"
        "fcntl|fcntl.h"
        "sysconf|unistd.h"
        "getpagesize|unistd.h"
        "fnmatch|fnmatch.h"
        "strtok_r|string.h"
        "strsep|string.h"
        "strlcpy|string.h"
        "strlcat|string.h"
        "strtod|stdlib.h"
        "gettimeofday|sys/time.h"
        "timegm|time.h"
        "syslog|syslog.h"
        "vsyslog|syslog.h"
        "strtoumax|inttypes.h"
        "getaddrinfo|sys/types.h;sys/socket.h;netdb.h"
        "uname|sys/utsname.h"
        "fork|unistd.h"
        "wait3|sys/types.h;sys/time.h;sys/resource.h;sys/wait.h"
        "wait|sys/wait.h"
        "sigaction|signal.h"
        "setitimer|sys/time.h"
        "isfinite|math.h")
    string(REPLACE "|" ";" _parts "${_f}")
    list(POP_FRONT _parts _name)
    string(TOUPPER "HAVE_${_name}" _var)
    check_symbol_exists(${_name} "${_parts}" ${_var})
endforeach()

check_include_files("complex.h" HAVE_COMPLEX)
check_include_files("unistd.h" HAVE_UNISTD)
check_include_files("stdlib.h" HAVE_STDLIB)
check_include_files("paths.h" HAVE_PATHS)
check_include_files("sys/types.h;sys/uio.h" HAVE_SYSUIO)
check_include_files("strings.h" HAVE_STRINGS)
check_include_files("sys/types.h;sys/socket.h" HAVE_SYS_SOCKET)
check_include_files("sys/types.h;netinet/in.h" HAVE_NETINET_IN)
check_include_files("sys/types.h;netinet/tcp.h" HAVE_NETINET_TCP)

check_struct_has_member("struct tm" tm_zone "time.h" HAVE_TMZONE)
check_c_source_compiles("#include <unistd.h>\nint main(void) { return optarg != 0; }" HAVE_OPTARG)
check_c_source_compiles("#include <unistd.h>\nint main(void) { return optind; }" HAVE_OPTIND)
check_c_source_compiles("static inline int f(void) { return 0; }\nint main(void) { return f(); }" HAVE_INLINE)

# networking
if(NOT OS_WIN)
    libu_check_type(HAVE_STRUCT_SOCKADDR_IN "struct sockaddr_in"
        sys/types.h sys/socket.h netinet/in.h)
    if(NOT HAVE_STRUCT_SOCKADDR_IN)
        message(FATAL_ERROR "LibU requires struct sockaddr_in")
    endif()
endif()
libu_check_type(HAVE_STRUCT_SOCKADDR_IN6 "struct sockaddr_in6"
    sys/types.h sys/socket.h netinet/in.h)
if(NOT HAVE_STRUCT_SOCKADDR_IN6)
    set(NO_IPV6 1)
endif()
libu_check_type(HAVE_STRUCT_SOCKADDR_UN "struct sockaddr_un"
    sys/types.h sys/socket.h netinet/in.h sys/un.h)
if(NOT HAVE_STRUCT_SOCKADDR_UN)
    set(NO_UNIXSOCK 1)
endif()
# SCTP is off unless requested: it also needs the SCTP library at link time.
if(LIBU_ENABLE_SCTP)
    check_include_files("sys/types.h;netinet/sctp.h" HAVE_NETINET_SCTP)
endif()
if(NOT HAVE_NETINET_SCTP)
    set(NO_SCTP 1)
endif()

unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_QUIET)

set(LIBU_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/libu/include")
configure_file("${CMAKE_CURRENT_LIST_DIR}/libu_conf.h.in"
    "${LIBU_GENERATED_INCLUDE_DIR}/u/libu_conf.h")

# LibU's makefiles symlink include/u/{toolbox,missing} inside the checkout.
# Copy the headers into that layout in the build tree instead, which leaves
# the checkout untouched and works where symlinks are unavailable.
file(GLOB_RECURSE _libu_headers CONFIGURE_DEPENDS
    RELATIVE "${LIBU_SOURCE_DIR}/include" "${LIBU_SOURCE_DIR}/include/*.h")
foreach(_h IN LISTS _libu_headers)
    if(_h STREQUAL "u/libu_conf.h")
        continue()  # stale output of a legacy in-place configure
    endif()
    set(_dests "${_h}")
    if(_h MATCHES "^(toolbox|missing)/")
        list(APPEND _dests "u/${_h}")
    endif()
    foreach(_d IN LISTS _dests)
        configure_file("${LIBU_SOURCE_DIR}/include/${_h}"
            "${LIBU_GENERATED_INCLUDE_DIR}/${_d}" COPYONLY)
    endforeach()
endforeach()

# Modules used by Klin and their LibU dependencies (uri.c uses lexer.c).
set(_libu_srcs
    toolbox/memory.c toolbox/misc.c toolbox/str.c toolbox/buf.c
    toolbox/log.c toolbox/lexer.c toolbox/hmap.c
    toolbox/config.c toolbox/config_fs.c toolbox/net.c
    toolbox/uri.c toolbox/fs.c toolbox/b64.c toolbox/pwd.c toolbox/list.c)
# replacements for functions the platform lacks
foreach(_m vsyslog syslog strtok_r unlink getpid fnmatch timegm strsep
        strlcpy strlcat gettimeofday daemon mkstemps setenv)
    string(TOUPPER "HAVE_${_m}" _var)
    # Windows has no fork(2): nothing calls daemon(3) there, and LibU's
    # stand-in does not compile (it misses its own headers)
    if(OS_WIN AND _m STREQUAL "daemon")
        continue()
    endif()
    if(NOT ${_var})
        list(APPEND _libu_srcs missing/${_m}.c)
    endif()
endforeach()
list(TRANSFORM _libu_srcs PREPEND "${LIBU_SOURCE_DIR}/srcs/")

# LibU's u_atol() and u_atoumax() test errno after strtol(3)/strtoumax(3)
# without clearing it first, so "0" fails to parse whenever an earlier call
# left EINVAL behind, and a setting of 0 silently keeps its default (seen
# with worker_max_procs 0 on macOS). Build a copy of misc.c that clears it;
# stop if the pinned LibU no longer has that code.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${LIBU_SOURCE_DIR}/srcs/toolbox/misc.c")
file(READ "${LIBU_SOURCE_DIR}/srcs/toolbox/misc.c" _libu_misc)
foreach(_call "tmp = strtol(nptr, &endptr, 10);"
        "tmp = strtoumax(nptr, (char **) NULL, 10);")
    string(FIND "${_libu_misc}" "\n    ${_call}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "LibU's srcs/toolbox/misc.c no longer has '${_call}': "
            "review the errno fix in cmake/LibU.cmake")
    endif()
    string(REPLACE "\n    ${_call}" "\n    errno = 0;\n    ${_call}"
        _libu_misc "${_libu_misc}")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/libu/misc.c.new" "${_libu_misc}")
configure_file("${CMAKE_BINARY_DIR}/libu/misc.c.new" "${CMAKE_BINARY_DIR}/libu/misc.c"
    COPYONLY)   # rewritten only when it changes
list(FIND _libu_srcs "${LIBU_SOURCE_DIR}/srcs/toolbox/misc.c" _at)
list(REMOVE_AT _libu_srcs ${_at})
list(INSERT _libu_srcs ${_at} "${CMAKE_BINARY_DIR}/libu/misc.c")

# LibU's configuration parser keeps a key with no value for the '{' that
# should follow, and drops it without a word when another key, a '}' or the
# end of the file comes instead: a bare "max_size" or "required" vanished,
# leaving its default. Build a copy of config.c that stops on such a key;
# stop if the pinned LibU no longer has that code.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${LIBU_SOURCE_DIR}/srcs/toolbox/config.c")
file(READ "${LIBU_SOURCE_DIR}/srcs/toolbox/config.c" _libu_config)
set(_libu_dangling "if(u_string_len(lastkey))\n            crit_err(\"config error \
[line %d]: '%s' has no value (nor a { on the next line)\", lineno, \
u_string_c(lastkey));\n")
foreach(_anchor "\n        /* find the end of the key string */"
        "\n    u_string_free(lastkey);\n    u_string_free(value);\n    u_string_free(key);")
    string(FIND "${_libu_config}" "${_anchor}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "LibU's srcs/toolbox/config.c has changed: review "
            "the no-value key fix in cmake/LibU.cmake")
    endif()
endforeach()
string(REPLACE "\n        /* find the end of the key string */"
    "\n        ${_libu_dangling}\n        /* find the end of the key string */"
    _libu_config "${_libu_config}")
string(REPLACE "\n    u_string_free(lastkey);\n    u_string_free(value);\n    u_string_free(key);"
    "\n    ${_libu_dangling}\n    u_string_free(lastkey);\n    u_string_free(value);\n    u_string_free(key);"
    _libu_config "${_libu_config}")
file(WRITE "${CMAKE_BINARY_DIR}/libu/config.c.new" "${_libu_config}")
configure_file("${CMAKE_BINARY_DIR}/libu/config.c.new" "${CMAKE_BINARY_DIR}/libu/config.c"
    COPYONLY)
list(FIND _libu_srcs "${LIBU_SOURCE_DIR}/srcs/toolbox/config.c" _at)
list(REMOVE_AT _libu_srcs ${_at})
list(INSERT _libu_srcs ${_at} "${CMAKE_BINARY_DIR}/libu/config.c")

add_library(libu STATIC ${_libu_srcs})
set_target_properties(libu PROPERTIES OUTPUT_NAME u)
target_include_directories(libu PUBLIC "${LIBU_GENERATED_INCLUDE_DIR}")
target_compile_definitions(libu PUBLIC HAVE_CONF_H)
target_compile_definitions(libu PRIVATE ${_libu_defs})
# LibU's u/missing.h leaves out the setenv(3) stand-in it compiles where the
# system has none (MinGW): declare it for LibU's own sources
if(OS_WIN AND NOT HAVE_SETENV AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(libu PRIVATE "SHELL:-include u/missing/setenv.h")
endif()
if(LIBU_DEBUG)
    target_compile_definitions(libu PRIVATE DEBUG)
endif()
