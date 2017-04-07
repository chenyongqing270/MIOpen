
function(install_symlink_subdir SUBDIR)
    # TODO: Check if SUBDIR is relative path
    install(CODE "
        file(GLOB_RECURSE FILES RELATIVE ${SUBDIR} ${SUBDIR}/*)
        foreach(FILE \${FILES})
            set(SRC ${SUBDIR}/\${FILE})
            set(DEST ${CMAKE_INSTALL_PREFIX}/\${FILE})
            get_filename_component(DEST_DIR \${DEST} DIRECTORY)
            file(MAKE_DIRECTORY \${DEST_DIR})
            file(RELATIVE_PATH SRC_REL \${DEST_DIR} \${SRC})
            message(STATUS \"symlink: \${SRC_REL} -> \${DEST}\")
            execute_process(COMMAND ln -s \${SRC_REL} \${DEST})
        endforeach()
    ")
endfunction()

