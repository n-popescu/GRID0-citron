# SPDX-License-Identifier: GPL-2.0-or-later
set(CMAKE_Swift_FLAGS "${CMAKE_Swift_FLAGS} -module-cache-path ${CMAKE_BINARY_DIR}/swift-module-cache")
enable_language(Swift)
add_library(citrosis_native_bridge STATIC bridge.cpp ../ios/native.cpp ../ios/emu_window/emu_window.cpp)
set_target_properties(citrosis_native_bridge PROPERTIES COMPILE_OPTIONS "")
target_compile_options(citrosis_native_bridge PRIVATE -fno-rtti -fwrapv -fno-strict-aliasing)
target_link_libraries(citrosis_native_bridge PUBLIC core common input_common frontend_common network web_service nlohmann_json::nlohmann_json)
add_executable(citrosis-mac MACOSX_BUNDLE CitrosisApp.swift)
set_target_properties(citrosis-mac PROPERTIES COMPILE_OPTIONS "" Swift_LANGUAGE_VERSION 5
 MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/src/citron/Info.plist"
 MACOSX_BUNDLE_BUNDLE_NAME Citrosis MACOSX_BUNDLE_SHORT_VERSION_STRING 0.2.0 MACOSX_BUNDLE_BUNDLE_VERSION 0.2.0)
target_compile_options(citrosis-mac PRIVATE -parse-as-library -target "${ARCHITECTURE}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
target_link_libraries(citrosis-mac PRIVATE citrosis_native_bridge "-lc++" "-framework SwiftUI" "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework GameController")
set_source_files_properties(${PROJECT_SOURCE_DIR}/dist/citron.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_sources(citrosis-mac PRIVATE ${PROJECT_SOURCE_DIR}/dist/citron.icns)
add_custom_command(TARGET citrosis-mac POST_BUILD COMMAND ${CMAKE_COMMAND} -E rm -f
 "$<TARGET_BUNDLE_DIR:citrosis-mac>/Contents/MacOS/main.d")
if(NOT USE_SYSTEM_MOLTENVK)
 download_moltenvk_external("macOS" "v1.4.0")
endif()
find_library(MOLTENVK_LIBRARY MoltenVK REQUIRED)
if(MOLTENVK_LIBRARY)
 add_custom_command(TARGET citrosis-mac POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:citrosis-mac>/Contents/Frameworks"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOLTENVK_LIBRARY}"
  "$<TARGET_BUNDLE_DIR:citrosis-mac>/Contents/Frameworks/libMoltenVK.dylib")
endif()
