# as_checkcheats — Plugify C++ plugin: port of the Pisex admin_system module "CheckCheats".
#
#   plugify-as_checkcheats  the plugin shared library (entry "bin/as_checkcheats")
#
# Consumer of the admin_system API: compiles the generated binding globals of
# plugins/admin_system/include (the __admin_system_* slots filled by the module) and uses the
# hand-written admin_api.hpp wrapper plus pisexcompat. The checkcheats_stats table is written
# through its own plgdb connection, so plgdb is linked in as well.

set(AS_CHECKCHEATS_VERSION "1.0.6")

# The consumer side of the admin API (API.md §5: the generated .cpp is compiled into every
# consumer plugin, like s2sdk_bindings / permissions_bindings).
set(AS_CHECKCHEATS_ADMIN_INCLUDE "${CMAKE_SOURCE_DIR}/plugins/admin_system/include")
set(AS_CHECKCHEATS_ADMIN_BINDINGS
		"${AS_CHECKCHEATS_ADMIN_INCLUDE}/external/plugify/include/admin_system/admin_system.cpp")

add_library(plugify-as_checkcheats SHARED
		src/plugin.cpp
		"${AS_CHECKCHEATS_ADMIN_BINDINGS}"
)
target_include_directories(plugify-as_checkcheats PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated admin_system/plg/s2sdk headers must not fail -Werror on
# our sources (src/plugin.cpp stays warning-clean).
target_include_directories(plugify-as_checkcheats SYSTEM PRIVATE
		"${CMAKE_SOURCE_DIR}/sdk/include"
		"${AS_CHECKCHEATS_ADMIN_INCLUDE}"
		"${AS_CHECKCHEATS_ADMIN_INCLUDE}/external/plugify/include")
target_link_libraries(plugify-as_checkcheats PRIVATE
		pisexcompat # pulls in the s2sdk binding globals and the plg headers
		plgdb       # own connection for checkcheats_stats
		-static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_checkcheats PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_checkcheats PRIVATE
		_GLIBCXX_USE_CXX11_ABI=1
		PLUGIFY_MODULE="as_checkcheats"
		PLUGIFY_IS_DEBUG=0
		PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_checkcheats PROPERTIES OUTPUT_NAME as_checkcheats)

if(UNIX AND NOT APPLE)
	target_link_options(plugify-as_checkcheats PRIVATE
			"-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/as_checkcheats.pplugin.in"
		"${CMAKE_CURRENT_BINARY_DIR}/as_checkcheats.pplugin"
)
