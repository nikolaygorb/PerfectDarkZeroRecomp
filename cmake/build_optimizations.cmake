# build_optimizations.cmake
#
# Определяет флаги оптимизации для каждого build type.
#
#   Debug           — по умолчанию CMake (без оптимизации, полные debug-информации)
#   RelWithDebInfo  — по умолчанию CMake (-O2 -g -DNDEBUG)
#   Release         — агрессивные оптимизации:
#                     -O3          максимум оптимизаций
#                     -flto=thin   link-time optimization (быстрее полного LTO, совместим с Ninja)
#                     -march=native (только AMD64) — коды под локальный CPU
#                     -DNDEBUG     отключить assert и debug-проверки
#
# Примечание: флаги задаются через CMAKE_CXX_FLAGS_<CONFIG>, поэтому применяются
# ко всем целям проекта (включая генерируемый код и host app).

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    # Release
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto=thin -march=native -DNDEBUG")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto=thin -DNDEBUG")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto=thin -DNDEBUG")
    endif()

    # RelWithDebInfo
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

    # Debug
    set(CMAKE_CXX_FLAGS_DEBUG "-g")
endif()
