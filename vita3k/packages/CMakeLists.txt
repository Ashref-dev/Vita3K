add_library(packages STATIC
            src/exfat.cpp
            src/license.cpp
            src/nonpdrm_pfs_read_only_mount.cpp
            src/nonpdrm_pfs_read_only_mount.h
            src/nonpdrm_zip_direct.cpp
            src/nonpdrm_zip_source.cpp
            src/nonpdrm_zip_source.h
            src/pkg.cpp
            src/pup.cpp
            src/sce_utils.cpp
            src/sfo.cpp
            src/vci.cpp
            include/packages/exfat.h
            include/packages/license.h
            include/packages/nonpdrm_zip_direct.h
            include/packages/functions.h
            include/packages/pkg.h
            include/packages/sce_types.h
            include/packages/sfo.h
            include/packages/vci.h
)
target_include_directories(packages PUBLIC include)
target_link_libraries(packages PUBLIC emuenv io util)
target_link_libraries(packages PRIVATE config crypto emuenv FAT16 miniz psvpfsparser vita-toolchain)

if(NOT ANDROID)
    add_executable(packages-nonpdrm-zip-tests
        tests/nonpdrm_zip_direct_tests.cpp
        tests/zip_test_builder.h
    )
    target_include_directories(packages-nonpdrm-zip-tests PRIVATE tests)
    target_link_libraries(packages-nonpdrm-zip-tests PRIVATE miniz packages psvpfsparser)
    add_test(NAME packages-nonpdrm-zip COMMAND packages-nonpdrm-zip-tests)

    add_executable(packages-nonpdrm-zip-tool EXCLUDE_FROM_ALL
        tests/nonpdrm_zip_direct_tool.cpp
    )
    target_link_libraries(packages-nonpdrm-zip-tool PRIVATE codec packages)
endif()
