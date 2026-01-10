# Инструмент для конвертации INI конфига в JSON формат

if(CHIAKI_ENABLE_GUI)
	find_package(Qt6 REQUIRED COMPONENTS Core)
	
	
	add_executable(chiaki-config-converter
		convert_config.cpp
		${CMAKE_SOURCE_DIR}/gui/src/host.cpp
	)
	
	target_link_libraries(chiaki-config-converter
		PRIVATE
		Qt6::Core
		chiaki-lib
	)
	
	
	target_include_directories(chiaki-config-converter
		PRIVATE
		${CMAKE_SOURCE_DIR}/gui/include
		${CMAKE_SOURCE_DIR}/lib/include
		${CMAKE_SOURCE_DIR}
	)
	
	# Используем AUTOMOC для Qt
	set_target_properties(chiaki-config-converter PROPERTIES
		AUTOMOC ON
	)
	
	# Копирование определений из gui/CMakeLists.txt если нужно
	if(WIN32)
		target_compile_definitions(chiaki-config-converter PRIVATE WIN32_LEAN_AND_MEAN)
	endif()
	
	# Установка (опционально)
	install(TARGETS chiaki-config-converter
		RUNTIME DESTINATION bin
	)
endif()
