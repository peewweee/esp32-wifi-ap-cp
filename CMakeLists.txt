# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_SUPABASE_BASE_URL "https://pzkatqdyxexkevyccjzm.supabase.co" CACHE INTERNAL "")
set(PROJECT_SUPABASE_API_KEY "REPLACE_WITH_SUPABASE_SECRET_KEY" CACHE INTERNAL "")

set(PROJECT_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/.env")
if(EXISTS "${PROJECT_ENV_FILE}")
    file(STRINGS "${PROJECT_ENV_FILE}" PROJECT_ENV_LINES)
    foreach(PROJECT_ENV_LINE IN LISTS PROJECT_ENV_LINES)
        string(STRIP "${PROJECT_ENV_LINE}" PROJECT_ENV_LINE)
        if(PROJECT_ENV_LINE STREQUAL "" OR PROJECT_ENV_LINE MATCHES "^#")
            continue()
        endif()

        if(PROJECT_ENV_LINE MATCHES "^SUPABASE_BASE_URL=(.*)$")
            set(PROJECT_SUPABASE_BASE_URL "${CMAKE_MATCH_1}" CACHE INTERNAL "")
        elseif(PROJECT_ENV_LINE MATCHES "^SUPABASE_API_KEY=(.*)$")
            set(PROJECT_SUPABASE_API_KEY "${CMAKE_MATCH_1}" CACHE INTERNAL "")
        endif()
    endforeach()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_nat_router)

spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)
