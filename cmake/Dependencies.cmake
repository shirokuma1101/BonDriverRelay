include(FetchContent)

set(BDR_DEPENDENCY_ARCHIVE_DIR "${CMAKE_SOURCE_DIR}/out/downloads" CACHE PATH
    "Optional directory of the exact dependency archives (SHA-256 is always checked)")

function(bdr_dependency_url output filename remote_url)
    if(EXISTS "${BDR_DEPENDENCY_ARCHIVE_DIR}/${filename}")
        set(${output} "${BDR_DEPENDENCY_ARCHIVE_DIR}/${filename}" PARENT_SCOPE)
    else()
        set(${output} "${remote_url}" PARENT_SCOPE)
    endif()
endfunction()

bdr_dependency_url(spdlog_url spdlog-1.15.3.tar.gz
    https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v1.15.3)
bdr_dependency_url(tomlplusplus_url tomlplusplus-3.4.0.tar.gz
    https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0)

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)
set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog SYSTEM EXCLUDE_FROM_ALL
    URL "${spdlog_url}"
    URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67)
FetchContent_Declare(tomlplusplus SYSTEM EXCLUDE_FROM_ALL
    URL "${tomlplusplus_url}"
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155)
FetchContent_MakeAvailable(spdlog tomlplusplus)

install(FILES "${spdlog_SOURCE_DIR}/LICENSE" DESTINATION licenses/spdlog)
install(FILES "${spdlog_SOURCE_DIR}/include/spdlog/fmt/bundled/fmt.license.rst" DESTINATION licenses/fmt)
install(FILES "${tomlplusplus_SOURCE_DIR}/LICENSE" DESTINATION licenses/tomlplusplus)
