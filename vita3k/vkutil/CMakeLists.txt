add_library(
	vkutil
	STATIC
	include/vkutil/native_surface.h
	include/vkutil/objects.h
	include/vkutil/upload_buffer_allocator.h
	include/vkutil/vkutil.h

	src/objects.cpp
	src/vkutil.cpp
)

target_include_directories(vkutil PUBLIC include)
target_link_libraries(vkutil PUBLIC vulkan vma)
target_link_libraries(vkutil PRIVATE util)

if(NOT ANDROID)
	add_executable(vkutil-upload-tests tests/upload_buffer_tests.cpp)
	target_include_directories(vkutil-upload-tests PRIVATE include)
	target_link_libraries(vkutil-upload-tests PRIVATE googletest)
	add_test(NAME vkutil-upload COMMAND vkutil-upload-tests)
endif()
