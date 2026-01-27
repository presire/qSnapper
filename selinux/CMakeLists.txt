# SELinux policy module build configuration for qSnapper

########################################
# Tool detection
########################################

find_program(CHECKMODULE checkmodule)
find_program(SEMODULE_PACKAGE semodule_package)
find_program(SEMODULE semodule)
find_program(RESTORECON restorecon)

if(NOT CHECKMODULE OR NOT SEMODULE_PACKAGE)
    message(WARNING "SELinux policy tools not found (checkmodule, semodule_package).")
    message(WARNING "SELinux policy module will not be built.")
    message(WARNING "To install tools on openSUSE: zypper install selinux-policy-devel")
    message(WARNING "To install tools on RHEL/Fedora: dnf install selinux-policy-devel")
    return()
endif()

########################################
# Policy module configuration
########################################

set(POLICY_NAME qsnapper)
set(POLICY_VERSION 1.0.0)

message(STATUS "Building SELinux policy module: ${POLICY_NAME} (version ${POLICY_VERSION})")

########################################
# Build pipeline: .te -> .mod -> .pp
########################################

# Step 1: Compile .te (Type Enforcement) to .mod (module object)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.mod
    COMMAND ${CHECKMODULE} -M -m
            -o ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.mod
            ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.te
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.te
    COMMENT "Compiling SELinux Type Enforcement policy: ${POLICY_NAME}.te -> ${POLICY_NAME}.mod"
    VERBATIM
)

# Step 2: Package .mod + .fc (File Contexts) + .if (Interface) into .pp (policy package)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.pp
    COMMAND ${SEMODULE_PACKAGE}
            -o ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.pp
            -m ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.mod
            -f ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.fc
    DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.mod
        ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.fc
        ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.if
    COMMENT "Packaging SELinux policy module: ${POLICY_NAME}.pp"
    VERBATIM
)

# Build target
add_custom_target(selinux-policy ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.pp
)

########################################
# Installation
########################################

# Install .pp to standard SELinux packages directory
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${POLICY_NAME}.pp
        DESTINATION ${CMAKE_INSTALL_DATADIR}/selinux/packages
        COMPONENT selinux
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# Install source files for reference (optional, useful for administrators)
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.te
        ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.fc
        ${CMAKE_CURRENT_SOURCE_DIR}/${POLICY_NAME}.if
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/selinux
        COMPONENT selinux
        OPTIONAL)

# Install documentation
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        ${CMAKE_CURRENT_SOURCE_DIR}/ADMIN.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/selinux
        COMPONENT selinux
        OPTIONAL)

########################################
# Post-installation: Load module and relabel files
########################################

if(SEMODULE AND RESTORECON)
    install(CODE "
        set(POLICY_PP \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/selinux/packages/${POLICY_NAME}.pp\")

        message(STATUS \"Installing SELinux policy module: ${POLICY_NAME}.pp\")
        message(STATUS \"Module location: \${POLICY_PP}\")

        # Load the policy module into the system
        execute_process(
            COMMAND ${SEMODULE} -i \${POLICY_PP}
            RESULT_VARIABLE SEMODULE_RESULT
            OUTPUT_VARIABLE SEMODULE_OUTPUT
            ERROR_VARIABLE SEMODULE_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE
        )

        if(NOT SEMODULE_RESULT EQUAL 0)
            message(WARNING \"Failed to install SELinux module: ${POLICY_NAME}\")
            message(WARNING \"Error: \${SEMODULE_ERROR}\")
            message(WARNING \"You may need to manually run:\")
            message(WARNING \"  sudo semodule -i \${POLICY_PP}\")
        else()
            message(STATUS \"SELinux module '${POLICY_NAME}' installed successfully\")

            # Relabel installed binaries
            message(STATUS \"Relabeling qSnapper binaries...\")

            execute_process(
                COMMAND ${RESTORECON} -R -v \${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/qsnapper
                OUTPUT_VARIABLE RESTORECON_OUTPUT1
                ERROR_VARIABLE RESTORECON_ERROR1
                RESULT_VARIABLE RESTORECON_RESULT1
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_STRIP_TRAILING_WHITESPACE
            )

            if(RESTORECON_RESULT1 EQUAL 0 AND RESTORECON_OUTPUT1)
                message(STATUS \"  \${RESTORECON_OUTPUT1}\")
            endif()

            execute_process(
                COMMAND ${RESTORECON} -R -v \${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBEXECDIR}/qsnapper-dbus-service
                OUTPUT_VARIABLE RESTORECON_OUTPUT2
                ERROR_VARIABLE RESTORECON_ERROR2
                RESULT_VARIABLE RESTORECON_RESULT2
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_STRIP_TRAILING_WHITESPACE
            )

            if(RESTORECON_RESULT2 EQUAL 0 AND RESTORECON_OUTPUT2)
                message(STATUS \"  \${RESTORECON_OUTPUT2}\")
            endif()

            # Relabel system directories (may fail if not running as root or directories don't exist)
            message(STATUS \"Relabeling system directories (non-fatal if missing)...\")

            execute_process(
                COMMAND ${RESTORECON} -R -v /etc/snapper
                OUTPUT_QUIET ERROR_QUIET
                RESULT_VARIABLE RESTORECON_RESULT3
            )

            execute_process(
                COMMAND ${RESTORECON} -R -v /.snapshots
                OUTPUT_QUIET ERROR_QUIET
                RESULT_VARIABLE RESTORECON_RESULT4
            )

            if(RESTORECON_RESULT3 EQUAL 0)
                message(STATUS \"  /etc/snapper relabeled\")
            endif()

            if(RESTORECON_RESULT4 EQUAL 0)
                message(STATUS \"  /.snapshots relabeled\")
            endif()

            message(STATUS \"File contexts updated successfully\")
            message(STATUS \"To verify SELinux policy: semodule -l | grep ${POLICY_NAME}\")
            message(STATUS \"To check file labels: ls -Z /usr/bin/qsnapper\")
        endif()
    ")
else()
    message(STATUS "SELinux tools (semodule, restorecon) not found at build time.")
    message(STATUS "Policy module will be installed but not automatically loaded.")
    message(STATUS "After installation, manually run:")
    message(STATUS "  sudo semodule -i ${CMAKE_INSTALL_FULL_DATADIR}/selinux/packages/${POLICY_NAME}.pp")
    message(STATUS "  sudo restorecon -R -v /usr/bin/qsnapper /usr/libexec/qsnapper-dbus-service")
endif()

########################################
# Uninstallation helper
########################################

# Note: CMake doesn't provide built-in uninstall target, but we can document the process
message(STATUS "To uninstall SELinux policy module later:")
message(STATUS "  sudo semodule -r ${POLICY_NAME}")
