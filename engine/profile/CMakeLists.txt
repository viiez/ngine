add_library(ngine_profile)
add_library(ngine::profile ALIAS ngine_profile)

target_sources(ngine_profile
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            profile.cppm
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/ngine/profile.hpp)

target_link_libraries(ngine_profile PUBLIC ngine::core)

# Tracy steckt nur in profile_tracy.cpp. Alle anderen sehen ausschließlich NGINE_WITH_TRACY.
if(NGINE_WITH_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_sources(ngine_profile PRIVATE profile_tracy.cpp)
    target_link_libraries(ngine_profile PRIVATE Tracy::TracyClient)
    target_compile_definitions(ngine_profile PUBLIC NGINE_WITH_TRACY=1)
else()
    target_sources(ngine_profile PRIVATE profile_null.cpp)
    target_compile_definitions(ngine_profile PUBLIC NGINE_WITH_TRACY=0)
endif()

ngine_set_compiler_options(ngine_profile)
