add_subdirectory(core)
add_subdirectory(memory)
add_subdirectory(jobs)
add_subdirectory(rhi)
add_subdirectory(shaders)

if(NGINE_WITH_WINDOW)
    add_subdirectory(platform)
endif()
