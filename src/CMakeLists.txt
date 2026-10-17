add_library(hisenseac_core STATIC
    chipcli.cpp
    chipcli.h
    controller.cpp
    controller.h
)

target_link_libraries(hisenseac_core
    PUBLIC
        Qt6::Core
        Qt6::Network
)

target_include_directories(hisenseac_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(hisense-matterd main.cpp)
target_link_libraries(hisense-matterd PRIVATE hisenseac_core Qt6::DBus)

add_executable(hisense-ac client_main.cpp)
target_link_libraries(hisense-ac PRIVATE Qt6::DBus Qt6::Core)

add_library(hisenseac_connectlife STATIC
    connectlifeapi.cpp
    connectlifeapi.h
    connectlifecontroller.cpp
    connectlifecontroller.h
)

target_link_libraries(hisenseac_connectlife
    PUBLIC
        Qt6::Core
        Qt6::Network
        OpenSSL::Crypto
)

target_include_directories(hisenseac_connectlife PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(hisense-connectlife connectlife_main.cpp)
target_link_libraries(hisense-connectlife PRIVATE hisenseac_connectlife Qt6::DBus)
