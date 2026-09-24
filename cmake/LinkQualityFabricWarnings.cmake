# Strict, first-party-only warning configuration for every Link Quality Fabric
# target.
#
# Goal: zero first-party diagnostics under /W4 /WX (MSVC) and
# -Wall -Wextra -Werror (GCC/Clang). Third-party headers are isolated through
# system include directories so their diagnostics are never attributed here.

include_guard(GLOBAL)

function(lqf_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /Zc:__cplusplus
      /Zc:preprocessor
      /Zc:inline
      /utf-8
      /EHsc
      /external:anglebrackets
      /external:W0
      /w14640 # threadsafe static local initialization: reviewed, not applicable to first-party statics
    )
    if(LQF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(LQF_ENABLE_ANALYZE)
      target_compile_options(${target} PRIVATE /analyze /analyze:external- /wd6326)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
      -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
      -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference
      -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
      -Wzero-as-null-pointer-constant -Wno-unknown-warning-option)
    if(LQF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(LQF_SANITIZE)
    if(MSVC)
      target_compile_options(${target} PRIVATE "/fsanitize=${LQF_SANITIZE}")
    else()
      target_compile_options(${target} PRIVATE "-fsanitize=${LQF_SANITIZE}" -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE "-fsanitize=${LQF_SANITIZE}")
    endif()
  endif()
endfunction()
