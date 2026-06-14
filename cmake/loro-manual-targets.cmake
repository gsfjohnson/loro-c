# loro-manual-targets.cmake — installed by the manual (non-Corrosion) build path.
#
# Recreates the Loro Rust static archive as a relocatable STATIC IMPORTED target, mirroring
# the snippet Corrosion generates for its own builds. Included by loroConfig.cmake, which
# then appends the platform system libs and links it into loro::loro.
#
# This file lives at <prefix>/lib*/cmake/loro/ and the archive at <prefix>/lib*/ — i.e. two
# directory levels up — so the path holds regardless of the GNUInstallDirs lib dir name
# (lib, lib64, lib/<triple>, …).

if(NOT TARGET loro_c_api-static)
    add_library(loro_c_api-static STATIC IMPORTED)
    get_filename_component(_loro_libdir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set_target_properties(loro_c_api-static PROPERTIES
        IMPORTED_LOCATION "${_loro_libdir}/libloro_c_api.a")
    unset(_loro_libdir)
endif()
