set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

add_executable(main main.cpp http_conn.cpp)
target_include_directories(main
	PRIVATE
		${PROJECT_SOURCE_DIR}/include)
