# SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
if (LINUX)
    set(vfs_fuse_SRCS
        ../cfapi/hydrationjob.h
        ../cfapi/hydrationjob.cpp
        protocol.hpp
        vfs_fuse.h
        vfs_fuse.cpp
    )

    add_library(nextcloudsync_vfs_fuse SHARED
        ${vfs_fuse_SRCS}
    )

    target_link_libraries(nextcloudsync_vfs_fuse PRIVATE Nextcloud::sync)

    set_target_properties(nextcloudsync_vfs_fuse
      PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY
          ${BIN_OUTPUT_DIRECTORY}
        RUNTIME_OUTPUT_DIRECTORY
          ${BIN_OUTPUT_DIRECTORY}
        PREFIX
          ""
        AUTOMOC
          TRUE
        LIBRARY_OUTPUT_NAME
          ${APPLICATION_EXECUTABLE}sync_vfs_fuse
        RUNTIME_OUTPUT_NAME
          ${APPLICATION_EXECUTABLE}sync_vfs_fuse
    )

    set(vfs_installdir "${PLUGINDIR}")

    install(TARGETS nextcloudsync_vfs_fuse
      LIBRARY DESTINATION "${vfs_installdir}"
      RUNTIME DESTINATION "${vfs_installdir}"
    )
endif()
