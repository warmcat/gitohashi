cmake_minimum_required(VERSION 3.0)
include(CheckCSourceCompiles)
include(CheckIncludeFile)

set(PROJ gitohashi)
project (gitohashi C)

#
# You can override where gitohashi gets its dependent library and includes
# from, at the cmake commandline.  For libwebsockets, eg:
#
# cmake .. -DGOH_LWS_INC_PATH=/usr/local/include \
#          -DGOH_LWS_LIB_PATH=/usr/local/lib/libwebsockets.so
#
# For git2, eg
#
# cmake .. -DJG2_GIT2_INC_PATH=/usr/local/include \
#          -DJG2_GIT2_LIB_PATH=/usr/local/lib/libgit2.so
#
# and for libarchive, eg
#
# cmake .. -DJG2_ARCHIVE_INC_PATH=/usr/local/include \
#    

# enable this if you want to run gitohashi from another lws application via
# its protocol plugins alone
#
option(GOH_LWS_PLUGINS "Also build lws protocol plugins" OFF)
option(GOH_WITH_ASAN "Build with libasan" OFF)


set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Gitohashi")
set(CPACK_PACKAGE_VENDOR "Andy Green <andy@warmcat.com>")
set(CPACK_PACKAGE_DESCRIPTION "Advanced gitweb based on libwebsockets")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "2")
set(CPACK_PACKAGE_VERSION_PATCH "99")


set(GOH_DEPLIBS jsongit2)
set(JG2_DEPLIBS pthread) 

if (NOT GOH_LWS_LIB_PATH)
	set(GOH_DEPLIB ${GOH_DEPLIBS} websockets)
	set(JG2_DEPLIBS ${JG2_DEPLIBS} websockets)
endif()

#
# libgit2 paths
#
find_path(   JG2_GIT2_INC_PATH NAMES "git2.h")
find_library(JG2_GIT2_LIB_PATH NAMES "git2")

if (JG2_GIT2_INC_PATH AND JG2_GIT2_LIB_PATH)
	set(JG2_DEPLIBS ${JG2_GIT2_LIB_PATH} ${JG2_DEPLIBS})
	include_directories(BEFORE "${JG2_GIT2_INC_PATH}")
else()
	message(FATAL_ERROR " Unable to find libgit2")
endif()

#
# libarchive paths
#
find_path(   JG2_ARCHIVE_INC_PATH NAMES "archive.h")
find_library(JG2_ARCHIVE_LIB_PATH NAMES "archive")

if (JG2_ARCHIVE_INC_PATH AND JG2_ARCHIVE_LIB_PATH)
	set(JG2_DEPLIBS ${JG2_ARCHIVE_LIB_PATH} ${JG2_DEPLIBS})
	include_directories(BEFORE "${JG2_ARCHIVE_INC_PATH}")
	set(JG2_HAVE_ARCHIVE_H "Y")
endif()

find_package(Git)
if(GIT_EXECUTABLE)
        execute_process(
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                COMMAND "${GIT_EXECUTABLE}" describe --tags --always
                OUTPUT_VARIABLE GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )

        set(LWS_BUILD_HASH ${GIT_HASH})

        # append the build user and hostname
        if (NOT LWS_REPRODUCIBLE)
                execute_process(
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        COMMAND "whoami"
                        OUTPUT_VARIABLE GIT_USER
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        )
                execute_process(
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        COMMAND "hostname"
                        OUTPUT_VARIABLE GIT_HOST
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        )
                string(REGEX REPLACE "([^\\])[\\]([^\\])" "\\1\\\\\\\\\\2" GIT_USER ${GIT_USER})
                set(LWS_BUILD_HASH ${GIT_USER}@${GIT_HOST}-${GIT_HASH})
        endif()

        message("Git commit hash: ${LWS_BUILD_HASH}")
endif()

set(CPACK_PACKAGE_VERSION_PATCH "${CPACK_PACKAGE_VERSION_PATCH_NUMBER}-${LWS_BUILD_HASH}")


#
# libwebsockets paths
#
find_path(   GOH_LWS_INC_PATH NAMES "libwebsockets.h")
find_library(GOH_LWS_LIB_PATH NAMES "websockets")

if (GOH_LWS_INC_PATH AND GOH_LWS_LIB_PATH)
	set(GOH_DEPLIBS ${GOH_LWS_LIB_PATH} ${GOH_DEPLIBS})
	include_directories(BEFORE "${GOH_LWS_INC_PATH}")
else()
	message(FATAL_ERROR " Unable to find libwebsockets")
endif()

unset(ASAN_LIBS)
if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX OR (CMAKE_C_COMPILER_ID MATCHES "Clang") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    include (CheckCCompilerFlag)
   
   	if (GOH_WITH_ASAN)
		set(ASAN_FLAGS "-fsanitize=address -fsanitize=pointer-compare -fsanitize=pointer-subtract -fsanitize=leak -fsanitize=undefined -fsanitize-address-use-after-scope -fsanitize-undefined-trap-on-error")
		set(ASAN_LIBS "asan")
		message("Enabling ASAN")
	endif()
   
    set(CMAKE_C_FLAGS "-Wall -Wsign-compare -Wignored-qualifiers -Wtype-limits -Wuninitialized -Werror -Wundef ${CMAKE_C_FLAGS} ${ASAN_FLAGS}" )
endif()

set(CMAKE_REQUIRED_INCLUDES "include")
CHECK_C_SOURCE_COMPILES(
	"#include <git2.h>
	int main(int argc, char **argv) { return GIT_BLAME_USE_MAILMAP; }
	" JG2_HAVE_BLAME_MAILMAP)

set(CMAKE_REQUIRED_FLAGS "-pthread")
CHECK_C_SOURCE_COMPILES("#define _GNU_SOURCE
	#include <pthread.h> 
	void *dummy(void *arg) { pthread_exit(NULL); return NULL; } 
	int main(int argc, char *argv[]) {
		pthread_t th;

		if (pthread_create(&th, NULL, dummy, NULL))
			pthread_setname_np(th, \"test\");
 
		return 0;
	}" JG2_HAS_PTHREAD_SETNAME_NP)

set(JG2_SOURCES lib/cache.c
	    lib/main.c
	    lib/repostate.c
	    lib/util.c

	    lib/job/job.c
	    lib/job/reflist.c
	    lib/job/log.c
	    lib/job/commit.c
	    lib/job/tree.c
	    lib/job/plain.c
	    lib/job/repos.c
	    lib/job/blame.c
	    lib/job/blog.c
	    lib/job/search.c

	    lib/conf/gitolite/gitolite3.c
	    lib/conf/gitolite/common.c
	    lib/conf/scan-repos.c

	    lib/email/md5.c
	    lib/email/email.c
)

if (JG2_HAVE_ARCHIVE_H)
	set(JG2_SOURCES ${JG2_SOURCES} lib/job/snapshot.c)
	set(JG2_DEPLIBS ${JG2_DEPLIBS} archive)
else()
	set(JG2_SOURCES ${JG2_SOURCES} lib/job/no-snapshot.c)
endif()

configure_file("cmake/config.h.in" "${PROJECT_BINARY_DIR}/jg2-config.h")
add_library(jsongit2 SHARED ${JG2_SOURCES})
set(HDR_PUBLIC "include/libjsongit2.h" "${PROJECT_BINARY_DIR}/jg2-config.h")
set_target_properties(jsongit2 PROPERTIES PUBLIC_HEADER "${HDR_PUBLIC}")
target_include_directories(jsongit2 PRIVATE "${PROJECT_BINARY_DIR}"
					      "${PROJECT_SOURCE_DIR}/include")      
target_link_libraries(jsongit2 ${ASAN_LIBS} ${GOH_LWS_LIB_PATH} ${JG2_DEPLIBS})

set(CPACK_COMPONENT_LIBRARIES_DISPLAY_NAME "Libraries")
set(CPACK_COMPONENT_DEV_DISPLAY_NAME "Development files")

# jsongit2 examples 

add_executable(jg2-example examples/minimal/jg2-example.c)
target_link_libraries(jg2-example ${ASAN_LIBS} ${GOH_LWS_LIB_PATH} jsongit2)
target_include_directories(jg2-example PRIVATE "${PROJECT_SOURCE_DIR}/include")

add_executable(jg2-threadchurn examples/threadchurn/threadchurn.c)
target_link_libraries(jg2-threadchurn ${ASAN_LIBS} ${GOH_LWS_LIB_PATH} jsongit2 pthread)
target_include_directories(jg2-threadchurn PRIVATE "${PROJECT_SOURCE_DIR}/include")


message("----------------------------- dependent libs -----------------------------")
message(" libgit2:    include: ${JG2_GIT2_INC_PATH}, lib: ${JG2_GIT2_LIB_PATH}")
message(" libgit2:    has GIT_BLAME_USE_MAILMAP: ${JG2_HAVE_BLAME_MAILMAP}")
if (NOT JG2_HAVE_ARCHIVE_H)
	message(" libarchive: not found")
else()
	message(" libarchive: include: ${JG2_ARCHIVE_INC_PATH}, lib: ${JG2_ARCHIVE_LIB_PATH}")
endif()
message(" libwebsockets: include: ${GOH_LWS_INC_PATH}, lib: ${GOH_LWS_LIB_PATH}")
message(" lws plugins:   ${GOH_LWS_PLUGINS}")

set(LIB_DIR       lib     CACHE PATH "Install dir for libraries")
set(BIN_DIR       bin     CACHE PATH "Install dir for executables")
set(INCLUDE_DIR   include CACHE PATH "Install dir for header files")
set(DATA_DIR      share   CACHE PATH "Install dir for data files")

install(TARGETS jsongit2
		jg2-example
		jg2-threadchurn

	LIBRARY DESTINATION "${LIB_DIR}${LIB_SUFFIX}" COMPONENT libraries
	ARCHIVE DESTINATION "${LIB_DIR}${LIB_SUFFIX}" COMPONENT libraries
	RUNTIME DESTINATION "${BIN_DIR}" COMPONENT libraries
	PUBLIC_HEADER DESTINATION "${INCLUDE_DIR}" COMPONENT dev)



install(FILES assets/github.css
	      assets/logo.css
	      assets/gitohashi-custom.css
	      assets/highlight.pack.js
	      assets/jg2.css
	      assets/jg2.js
	      assets/favicon.ico
	      assets/inconsolata.ttf
	      assets/showdown.min.js
	      assets/showdown.min.js.map
			DESTINATION "${DATA_DIR}/gitohashi/assets")
			
install(FILES example-cert/localhost-100y.cert.pem
	      example-cert/localhost-100y.key.pem
			DESTINATION "${DATA_DIR}/gitohashi/example-cert")
			
install(FILES templates/gitohashi-example.html
			DESTINATION "${DATA_DIR}/gitohashi/templates")
			
install(FILES system/gitohashi-selinux.pp
			DESTINATION "${DATA_DIR}/gitohashi")
			
install(FILES system/gitohashi.service
			DESTINATION "${LIB_DIR}/systemd/system")

add_executable(${PROJ}
	src/main.c
)

target_link_libraries(${PROJ} ${ASAN_LIBS} ${GOH_LWS_LIB_PATH} jsongit2)
target_include_directories(${PROJ} PRIVATE "${PROJECT_SOURCE_DIR}/include")

install(TARGETS ${PROJ} RUNTIME DESTINATION ${BIN_DIR})


if (GOH_LWS_PLUGINS)
	add_library(protocol_gitohashi SHARED src/protocol_gitohashi.c)
	
       target_link_libraries(protocol_gitohashi ${ASAN_LIBS} websockets jsongit2)

	set_property(TARGET protocol_gitohashi
		     PROPERTY COMPILE_DEFINITIONS
		     INSTALL_DATADIR="${CMAKE_INSTALL_PREFIX}/plugins"
	)
	
	list(APPEND PLUGINS_LIST protocol_gitohashi)
	
	
	add_library(protocol_avatar_proxy SHARED src/protocol_avatar-proxy.c)
	
       target_link_libraries(protocol_avatar_proxy ${ASAN_LIBS} websockets jsongit2)

	set_property(TARGET protocol_avatar_proxy
		     PROPERTY COMPILE_DEFINITIONS
		     INSTALL_DATADIR="${CMAKE_INSTALL_PREFIX}/plugins"
	)
	
	list(APPEND PLUGINS_LIST protocol_avatar_proxy)
	
	install(TARGETS ${PLUGINS_LIST}
		PERMISSIONS  OWNER_WRITE OWNER_EXECUTE GROUP_EXECUTE WORLD_EXECUTE OWNER_READ GROUP_READ WORLD_READ
		DESTINATION share/libwebsockets-test-server/plugins
		COMPONENT plugins)

endif()

include(CPack)
