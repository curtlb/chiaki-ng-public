
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(APPLE)
	enable_language(OBJCXX)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Concurrent Svg Qml Quick Widgets Network)
find_package(Qt6 COMPONENTS WebEngineQuick)
find_package(Qt6 COMPONENTS DBus)

# Try to find Qt6::GuiPrivate for private API access
if(TARGET Qt6::GuiPrivate)
	set(QT_GUI_PRIVATE_TARGET Qt6::GuiPrivate)
	message(STATUS "Qt6::GuiPrivate target found")
else()
	# Fallback for MSYS2 or other systems where GuiPrivate target is not available
	set(QT_GUI_PRIVATE_TARGET "")
	message(STATUS "Qt6::GuiPrivate target not found, setting up include directories manually")
	
	# Get Qt6Gui version and include directories
	get_target_property(Qt6Gui_INCLUDE_DIRS Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
	message(STATUS "Qt6Gui include dirs: ${Qt6Gui_INCLUDE_DIRS}")
	message(STATUS "Qt6Gui version: ${Qt6Gui_VERSION}")
	
	# Build private include paths for MSYS2 and other systems
	set(QT_PRIVATE_INCLUDE_DIRS "")
	foreach(dir ${Qt6Gui_INCLUDE_DIRS})
		if(dir MATCHES "QtGui$")
			# Add versioned private include directories
			list(APPEND QT_PRIVATE_INCLUDE_DIRS 
				"${dir}/${Qt6Gui_VERSION}"
				"${dir}/${Qt6Gui_VERSION}/QtGui")
			message(STATUS "Qt private include dirs: ${QT_PRIVATE_INCLUDE_DIRS}")
			break()
		endif()
	endforeach()
	
	# Fallback: try to construct path from Qt6_DIR for MSYS2
	if(NOT QT_PRIVATE_INCLUDE_DIRS AND Qt6_DIR)
		get_filename_component(QT_INSTALL_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
		list(APPEND QT_PRIVATE_INCLUDE_DIRS
			"${QT_INSTALL_PREFIX}/include/qt6/QtGui/${Qt6Gui_VERSION}"
			"${QT_INSTALL_PREFIX}/include/qt6/QtGui/${Qt6Gui_VERSION}/QtGui")
		message(STATUS "Using fallback Qt private include dirs: ${QT_PRIVATE_INCLUDE_DIRS}")
	endif()
endif()

if(WIN32)
	add_definitions(-DWIN32_LEAN_AND_MEAN)
endif()

set(SOURCE_FILES
	include/exception.h
	src/main.cpp
	include/discoverymanager.h
	src/discoverymanager.cpp
	include/streamsession.h
	src/streamsession.cpp
	include/gamelauncher.h
	src/gamelauncher.cpp
	include/macrorecorder.h
	src/macrorecorder.cpp
	include/sessionlog.h
	src/sessionlog.cpp
	include/cloudlog.h
	src/cloudlog.cpp
	include/settings.h
	src/settings.cpp
	include/host.h
	src/host.cpp
	res/resources.qrc
	include/controllermanager.h
	src/controllermanager.cpp
	include/psnaccountid.h
	src/psnaccountid.cpp
	include/psntoken.h
	src/psntoken.cpp
	include/cloudstreamingbackend.h
	src/cloudstreamingbackend.cpp
	include/cloudbillingclient.h
	src/cloudbillingclient.cpp
	include/cloudcatalogbackend.h
	src/cloudcatalogbackend.cpp
	include/jsonrequester.h
	src/jsonrequester.cpp
	src/qml/qml.qrc
	include/qmlmainwindow.h
	src/qmlmainwindow.cpp
	include/qmlbackend.h
	src/qmlbackend.cpp
	include/qmlcontroller.h
	src/qmlcontroller.cpp
	include/qmlsettings.h
	src/qmlsettings.cpp
	include/qmlsvgprovider.h
	src/qmlsvgprovider.cpp
	include/systemdinhibit.h
	src/systemdinhibit.cpp
	include/yandexocr.h
	src/yandexocr.cpp
	include/textoverlay.h
	src/textoverlay.cpp
	include/crashreporter.h
	src/crashreporter.cpp
	)
set(RESOURCE_FILES "")

if(APPLE)
	list(APPEND RESOURCE_FILES "chiaking.icns")
elseif(WIN32)
	configure_file(
		${CMAKE_CURRENT_SOURCE_DIR}/chiaking.rc.in
		${CMAKE_CURRENT_SOURCE_DIR}/chiaking.rc
		@ONLY)
	list(APPEND RESOURCE_FILES "chiaking.rc")
endif()

if(CHIAKI_ENABLE_STEAMDECK_NATIVE AND LINUX)
	list(APPEND SOURCE_FILES
		include/sdinputcontext.h
		src/sdinputcontext.cpp
	)
endif()

if(APPLE)
	list(APPEND SOURCE_FILES
		include/macMicPermission.h
		src/macMicPermission.m
		include/macWakeSleep.h
		src/macWakeSleep.cpp
	)
elseif(WIN32)
	list(APPEND SOURCE_FILES
		include/windowsWakeSleep.h
		src/windowsWakeSleep.cpp
	)
endif()

add_executable(chiaki WIN32
		${RESOURCE_FILES}
		${SOURCE_FILES}
)
target_include_directories(chiaki PRIVATE include)

add_library(placebo-libav-impl src/libav_impl.c)
target_link_libraries(placebo-libav-impl PkgConfig::LIBPLACEBO FFMPEG::avcodec FFMPEG::avutil FFMPEG::avformat FFMPEG::swscale)
target_link_libraries(chiaki PkgConfig::LIBPLACEBO placebo-libav-impl FFMPEG::swscale)

target_link_libraries(chiaki chiaki-lib)
if(NOT CHIAKI_LIB_ENABLE_MBEDTLS)
	find_package(OpenSSL REQUIRED)
	target_link_libraries(chiaki OpenSSL::Crypto)
endif()
if(CHIAKI_ENABLE_CLI)
	add_definitions(-DCHIAKI_ENABLE_CLI)
	target_link_libraries(chiaki chiaki-cli-lib)
endif()

target_link_libraries(chiaki Qt::Core Qt::Gui ${QT_GUI_PRIVATE_TARGET} Qt::Concurrent Qt::Svg Qt::Qml Qt::Quick Qt::Widgets Qt::Network)
# Windows-specific libraries for crash reporting
if(WIN32)
	target_link_libraries(chiaki dbghelp psapi)
endif()
# For systems without GuiPrivate target, include private headers directly
if(NOT TARGET Qt6::GuiPrivate)
	if(QT_PRIVATE_INCLUDE_DIRS)
		target_include_directories(chiaki PRIVATE ${QT_PRIVATE_INCLUDE_DIRS})
	endif()
	# Also try standard Qt private include directories variable
	if(Qt6Gui_PRIVATE_INCLUDE_DIRS)
		target_include_directories(chiaki PRIVATE ${Qt6Gui_PRIVATE_INCLUDE_DIRS})
	endif()
endif()
if (${Qt6DBus_FOUND} AND NOT WIN32)
	target_link_libraries(chiaki Qt::DBus)
if (NOT APPLE)
	target_compile_definitions(chiaki PRIVATE CHIAKI_HAVE_DBUS)
endif()
endif()
if (${Qt6WebEngineQuick_FOUND})
	target_link_libraries(chiaki Qt::WebEngineQuick)
	target_compile_definitions(chiaki PRIVATE CHIAKI_HAVE_WEBENGINE)
endif()
target_link_libraries(chiaki SDL2::SDL2)
if(CHIAKI_ENABLE_SPEEX)
  target_link_libraries(chiaki PkgConfig::SpeexDSP)
  target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_SPEEX)
endif()
if(CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER)
	target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER)
endif()
if(CHIAKI_ENABLE_SETSU)
	target_link_libraries(chiaki setsu)
	target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_SETSU)
endif()
if(CHIAKI_ENABLE_STEAMDECK_NATIVE)
	target_link_libraries(chiaki sdeck)
	target_compile_definitions(chiaki PRIVATE QT_STATICPLUGIN CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE)
endif()
if(CHIAKI_ENABLE_STEAM_SHORTCUT)
	target_link_libraries(chiaki cpp-steam-tools)
	target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_STEAM_SHORTCUT)
endif()
if(NOT CHIAKI_ENABLE_FFMPEG_DECODER)
	message(FATAL_ERROR "Chiaki GUI requires CHIAKI_ENABLE_FFMPEG_DECODER=ON")
endif()
if(APPLE)
	target_link_libraries(chiaki "-framework AVFoundation" "-framework IOKit")
endif()

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fanalyzer")
set_target_properties(chiaki PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/MacOSXBundleInfo.plist.in"
		MACOSX_BUNDLE_BUNDLE_NAME chiaki-ng
		MACOSX_BUNDLE_BUNDLE_VERSION ${CHIAKI_VERSION}
		MACOSX_BUNDLE_COPYRIGHT "thestr4ng3r (AGPLv3)"
		MACOSX_BUNDLE_GUI_IDENTIFIER "org.streetpea.chiaking"
		MACOSX_BUNDLE_ICON_FILE chiaking.icns
		RESOURCE "${RESOURCE_FILES}")

install(TARGETS chiaki
		RUNTIME DESTINATION bin
		BUNDLE DESTINATION bin)
install(FILES chiaking.desktop DESTINATION share/applications)
install(FILES chiaking.png DESTINATION share/icons/hicolor/512x512/apps)
install(FILES io.github.streetpea.Chiaki4deck.appdata.xml DESTINATION share/metainfo)