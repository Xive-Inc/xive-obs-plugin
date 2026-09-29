cmake_minimum_required(VERSION 3.22)

project(xive-obs-plugin VERSION 0.1.6)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Q_OBJECT classes live in the headers below, so moc has to run over them.
set(CMAKE_AUTOMOC ON)

# ---------------------------------------------------------------------------
# Dependencies
#
# Linux distributions ship libobs development packages with CMake config files, so the normal
# find_package path works. macOS has no such package: OBS.app bundles the libraries but no
# headers, so there the headers come from a checkout of the obs-studio source at the MATCHING
# version and nothing is linked at all (see the macOS block further down).
# ---------------------------------------------------------------------------
if(APPLE)
  if(NOT DEFINED XIVE_OBS_SOURCE_DIR)
    message(
      FATAL_ERROR
        "On macOS, set -DXIVE_OBS_SOURCE_DIR=/path/to/obs-studio (checked out at the same "
        "version as the installed OBS.app) so the libobs headers can be found.")
  endif()

  find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

  #
  # libobs/util/sse-intrin.h includes <simde/x86/sse2.h> — on Apple Silicon there are no real
  # SSE intrinsics, so libobs emulates them through SIMDe. Linux distributions pull simde in as a
  # dependency of the libobs devel package; on macOS it has to be installed separately
  # (`brew install simde`).
  #
  # Deliberately the Cellar path and NOT /opt/homebrew/include: this machine has qt@5 linked,
  # which puts Qt 5 headers in /opt/homebrew/include, and putting that on the include path would
  # let Qt 5's QtCore shadow the Qt 6 framework headers we actually want.
  #
  if(NOT DEFINED XIVE_SIMDE_INCLUDE_DIR)
    set(XIVE_SIMDE_INCLUDE_DIR "/opt/homebrew/opt/simde/include")
  endif()
  if(NOT EXISTS "${XIVE_SIMDE_INCLUDE_DIR}/simde/x86/sse2.h")
    message(FATAL_ERROR "SIMDe headers not found at ${XIVE_SIMDE_INCLUDE_DIR}. "
                        "Run `brew install simde`, or pass -DXIVE_SIMDE_INCLUDE_DIR=...")
  endif()
else()
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
endif()

add_library(xive-obs-plugin MODULE)

# The gift emoji table and the "·" in the gift pattern are UTF-8 string literals. GCC and clang
# assume a UTF-8 source file; MSVC assumes the system code page unless told otherwise, and would
# silently mis-encode every one of them.
if(MSVC)
  target_compile_options(xive-obs-plugin PRIVATE /utf-8)
endif()

target_sources(
  xive-obs-plugin
  PRIVATE src/plugin-main.cpp
          src/xive-config.cpp
          src/xive-config.h
          src/xive-auth.cpp
          src/xive-auth.h
          src/xive-api.cpp
          src/xive-api.h
          src/xive-channel.cpp
          src/xive-channel.h
          src/xive-chat-feed.cpp
          src/xive-chat-feed.h
          src/xive-realtime.cpp
          src/xive-realtime.h
          src/xive-activity.cpp
          src/xive-activity.h
          src/xive-activity-feed.cpp
          src/xive-activity-feed.h
          src/xive-chat-render.cpp
          src/xive-chat-render.h
          src/xive-surge.cpp
          src/xive-surge.h
          src/xive-surge-feed.cpp
          src/xive-surge-feed.h
          src/xive-quick-actions.cpp
          src/xive-quick-actions.h
          src/xive-overlay-source.cpp
          src/xive-overlay-source.h
          src/xive-service.cpp
          src/xive-service.h
          src/xive-service-catalog.cpp
          src/xive-service-catalog.h
          src/xive-release.cpp
          src/xive-release.h
          src/xive-update.cpp
          src/xive-update.h
          src/xive-log.h
          src/ui/xive-theme.cpp
          src/ui/xive-theme.h
          src/ui/xive-dock.cpp
          src/ui/xive-dock.h
          src/ui/xive-chat-dock.cpp
          src/ui/xive-chat-dock.h
          src/ui/xive-blocked-dialog.cpp
          src/ui/xive-blocked-dialog.h
          src/ui/xive-manager-dock.cpp
          src/ui/xive-manager-dock.h
          src/ui/xive-category-picker.cpp
          src/ui/xive-category-picker.h
          src/ui/xive-alerts-dock.cpp
          src/ui/xive-alerts-dock.h
          src/ui/xive-activity-dock.cpp
          src/ui/xive-activity-dock.h
          src/ui/xive-browser-dock.cpp
          src/ui/xive-browser-dock.h
          # Listed so AUTOMOC runs over it: QCefWidget carries Q_OBJECT, and without the generated
          # moc the vtable it declares has no definition to link against. The file is a VERBATIM
          # copy of obs-browser's panel/browser-panel.hpp — do not edit it; re-copy it to update.
          src/ui/browser-panel.hpp)

target_include_directories(xive-obs-plugin PRIVATE src)

if(APPLE)
  # Headers only — see the note below on why nothing is linked.
  target_include_directories(
    xive-obs-plugin
    PRIVATE "${XIVE_OBS_SOURCE_DIR}/libobs"
            "${XIVE_OBS_SOURCE_DIR}/frontend/api"
            "${XIVE_OBS_SOURCE_DIR}/UI/obs-frontend-api")

  target_include_directories(xive-obs-plugin SYSTEM PRIVATE "${XIVE_SIMDE_INCLUDE_DIR}")

  # Supplies the obsconfig.h that OBS's own build would have generated. See the header comment
  # in that file for why hardcoded values are safe here.
  target_include_directories(xive-obs-plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos-shim")

  # Qt headers without Qt libraries: pull the include paths and compile definitions off the
  # imported targets, but do not link them.
  get_target_property(_qt_widgets_inc Qt6::Widgets INTERFACE_INCLUDE_DIRECTORIES)
  get_target_property(_qt_network_inc Qt6::Network INTERFACE_INCLUDE_DIRECTORIES)
  get_target_property(_qt_core_inc Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
  get_target_property(_qt_gui_inc Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(
    xive-obs-plugin SYSTEM PRIVATE ${_qt_core_inc} ${_qt_gui_inc} ${_qt_widgets_inc}
                                   ${_qt_network_inc})

  #
  # Link NOTHING, and resolve libobs and Qt symbols from the host process at load time.
  #
  # This is not laziness — it is the only safe option here. OBS.app ships its own Qt 6.11
  # frameworks and loads them itself. If this plugin also linked a Homebrew Qt, the process
  # would end up with two separate copies of QtCore, each with its own metatype registry and
  # event dispatcher, which crashes rather than degrades.
  #
  # The tradeoff is that a misspelled symbol becomes a load-time failure instead of a link
  # error. That is covered by the Linux build, which links everything properly and therefore
  # catches undefined symbols before macOS ever sees them. Build Linux first.
  #
  target_link_options(xive-obs-plugin PRIVATE -undefined dynamic_lookup)

  # OBS expects a macOS plugin as a bundle directory named <name>.plugin.
  set_target_properties(
    xive-obs-plugin
    PROPERTIES BUNDLE TRUE
               BUNDLE_EXTENSION plugin
               PREFIX ""
               SUFFIX ""
               MACOSX_BUNDLE_BUNDLE_NAME "xive-obs-plugin"
               MACOSX_BUNDLE_GUI_IDENTIFIER "com.thexive.obs.plugin"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
else()
  target_link_libraries(xive-obs-plugin PRIVATE OBS::libobs OBS::obs-frontend-api
                                               Qt6::Widgets Qt6::Network)

  # OBS loads modules by bare name, so strip the "lib" prefix CMake adds to MODULE targets.
  set_target_properties(xive-obs-plugin PROPERTIES PREFIX "")
endif()

target_compile_definitions(
  xive-obs-plugin PRIVATE XIVE_PLUGIN_VERSION="${PROJECT_VERSION}")

# browser-panel.hpp only performs its Wayland check when this is defined — undefined, it happily
# hands back a panel that obs-browser cannot draw, and the docks come up as black rectangles.
# Linux only: the header's #include <obs-nix-platform.h> exists nowhere else.
if(UNIX AND NOT APPLE)
  target_compile_definitions(xive-obs-plugin PRIVATE ENABLE_WAYLAND)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xive-obs-plugin PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Install
#
# Linux: a user-scope OBS plugin is a bare .so plus a data/ dir, both under
# ~/.config/obs-studio/plugins/<name>/. Installing there rather than into
# /usr/lib64/obs-plugins keeps the dev loop free of sudo.
# ---------------------------------------------------------------------------
# macOS: a self-contained <name>.plugin bundle goes straight into the user plugins dir, with the
# locale data inside the bundle's Resources rather than beside it.
# ---------------------------------------------------------------------------
if(APPLE)
  if(NOT DEFINED XIVE_USER_PLUGIN_DIR)
    set(XIVE_USER_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
  endif()

  # Locale files live in the bundle, so OBS's default-locale lookup finds them via
  # obs_module_config/data paths inside Contents/Resources.
  set_source_files_properties(data/locale/en-US.ini PROPERTIES MACOSX_PACKAGE_LOCATION
                                                              Resources/locale)
  target_sources(xive-obs-plugin PRIVATE data/locale/en-US.ini)

  # Both destinations: CMake classifies a MODULE target as LIBRARY unless it is certain the
  # bundle properties apply, and errors out if only BUNDLE is given.
  install(TARGETS xive-obs-plugin BUNDLE DESTINATION "${XIVE_USER_PLUGIN_DIR}"
                                  LIBRARY DESTINATION "${XIVE_USER_PLUGIN_DIR}")
elseif(WIN32)
  # 🔴 %ProgramData%, NOT %APPDATA%. Windows has no per-user plugin directory: OBS's
  # AddExtraModulePaths() calls GetProgramDataPath() here where the other platforms call
  # GetAppConfigPath(), so %APPDATA%\obs-studio is the CONFIG directory (profiles, logs,
  # plugin_config) and is never scanned for modules. Installing there succeeds and then OBS
  # silently loads nothing. The bin/64bit + data layout below is the same as Linux; only the root
  # differs. $ENV{HOME} is not usually set on Windows, so the Linux default further down would
  # resolve to a bare relative path.
  if(NOT DEFINED XIVE_USER_PLUGIN_DIR)
    # ProgramData comes back with backslashes, which would leave the install() destinations below
    # as non-normalized mixed-separator paths (CMP0177).
    file(TO_CMAKE_PATH "$ENV{ProgramData}" XIVE_PROGRAMDATA_DIR)
    set(XIVE_USER_PLUGIN_DIR "${XIVE_PROGRAMDATA_DIR}/obs-studio/plugins/xive-obs-plugin")
  endif()

  # A MODULE target is classified as LIBRARY on every platform, Windows included — RUNTIME alone
  # leaves the .dll with no destination.
  install(TARGETS xive-obs-plugin LIBRARY DESTINATION "${XIVE_USER_PLUGIN_DIR}/bin/64bit"
                                  RUNTIME DESTINATION "${XIVE_USER_PLUGIN_DIR}/bin/64bit")
  install(DIRECTORY data/ DESTINATION "${XIVE_USER_PLUGIN_DIR}/data")

  # Qt reaches TLS only through a backend plugin, and the Qt that OBS ships on Windows contains no
  # tls/ plugin directory at all — every HTTPS request dies with "No functional TLS backend was
  # found" before it leaves the process. Ship the backend ourselves, next to the module;
  # registerTlsBackendPath() in plugin-main.cpp puts this directory on Qt's plugin search path.
  #
  # Schannel and not OpenSSL: the obs-deps Qt archive builds only qschannelbackend and
  # qcertonlybackend (which cannot do TLS, hence the message above), and Schannel is the Windows
  # native stack, so it needs no libssl/libcrypto shipped beside it.
  find_file(
    XIVE_QT_TLS_BACKEND qschannelbackend.dll
    PATH_SUFFIXES plugins/tls
    DOC "Qt Schannel TLS backend plugin, installed beside the module" REQUIRED)
  install(FILES "${XIVE_QT_TLS_BACKEND}" DESTINATION "${XIVE_USER_PLUGIN_DIR}/bin/64bit/tls")
else()
  if(NOT DEFINED XIVE_USER_PLUGIN_DIR)
    set(XIVE_USER_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/xive-obs-plugin")
  endif()

  install(TARGETS xive-obs-plugin LIBRARY DESTINATION "${XIVE_USER_PLUGIN_DIR}/bin/64bit")
  install(DIRECTORY data/ DESTINATION "${XIVE_USER_PLUGIN_DIR}/data")
endif()
