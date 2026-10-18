function(semcache_set_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -Wno-sign-conversion -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC false positive on coroutine frames allocated by Asio's awaitable_frame_base
    # (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=109224).
    target_compile_options(${target} PRIVATE -Wno-mismatched-new-delete)
  endif()
  if(SEMCACHE_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
