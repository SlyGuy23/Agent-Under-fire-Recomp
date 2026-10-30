# Game Title (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/(game|launcher|driving)/")



# partitions as each config sets them
function(config_partitions config out)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config/${config})
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/config/${config} line REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" n "${line}")
    set(${out} ${n} PARENT_SCOPE)
endfunction()

# three images, each its own executable; the game relaunches between them as the console loads each xbe
# default.xbe: the EA Games and MGM intro films, then it starts the game
config_partitions(default.toml LAUNCHER_PARTITIONS)
add_xbe_image(AgentUnderFire default.xbe default.toml ${LAUNCHER_PARTITIONS} launcher)
# bond_eng.xbe: menus and on-foot missions
config_partitions(game.toml GAME_PARTITIONS)
add_xbe_image(AgentUnderFireGame bond_eng.xbe game.toml ${GAME_PARTITIONS} game)
# driving.xbe: the driving missions
config_partitions(driving.toml DRIVING_PARTITIONS)
add_xbe_image(AgentUnderFireDriving driving.xbe driving.toml ${DRIVING_PARTITIONS} driving)
