include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.cpp)
tactility_component_register(
    SRCS ${SOURCE_FILES}
    REQUIRES http-module cjson-module esp_http_client mbedtls
)
