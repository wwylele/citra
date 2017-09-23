set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

set(SRCS
            configuration/config.cpp
            configuration/configure_audio.cpp
            configuration/configure_debug.cpp
            configuration/configure_dialog.cpp
            configuration/configure_general.cpp
            configuration/configure_graphics.cpp
            configuration/configure_input.cpp
            configuration/configure_system.cpp
            configuration/configure_web.cpp
            debugger/graphics/graphics.cpp
            debugger/graphics/graphics_breakpoint_observer.cpp
            debugger/graphics/graphics_breakpoints.cpp
            debugger/graphics/graphics_cmdlists.cpp
            debugger/graphics/graphics_surface.cpp
            debugger/graphics/graphics_tracing.cpp
            debugger/graphics/graphics_vertex_shader.cpp
            debugger/profiler.cpp
            debugger/registers.cpp
            debugger/wait_tree.cpp
            updater/updater.cpp
            util/spinbox.cpp
            util/util.cpp
            aboutdialog.cpp
            bootmanager.cpp
            game_list.cpp
            hotkeys.cpp
            main.cpp
            ui_settings.cpp
            citra-qt.rc
            Info.plist
            )

set(HEADERS
            configuration/config.h
            configuration/configure_audio.h
            configuration/configure_debug.h
            configuration/configure_dialog.h
            configuration/configure_general.h
            configuration/configure_graphics.h
            configuration/configure_input.h
            configuration/configure_system.h
            configuration/configure_web.h
            debugger/graphics/graphics.h
            debugger/graphics/graphics_breakpoint_observer.h
            debugger/graphics/graphics_breakpoints.h
            debugger/graphics/graphics_breakpoints_p.h
            debugger/graphics/graphics_cmdlists.h
            debugger/graphics/graphics_surface.h
            debugger/graphics/graphics_tracing.h
            debugger/graphics/graphics_vertex_shader.h
            debugger/profiler.h
            debugger/registers.h
            debugger/wait_tree.h
            updater/updater.h
            updater/updater_p.h
            util/spinbox.h
            util/util.h
            aboutdialog.h
            bootmanager.h
            game_list.h
            game_list_p.h
            hotkeys.h
            main.h
            ui_settings.h
            )

set(UIS
            configuration/configure.ui
            configuration/configure_audio.ui
            configuration/configure_debug.ui
            configuration/configure_general.ui
            configuration/configure_graphics.ui
            configuration/configure_input.ui
            configuration/configure_system.ui
            configuration/configure_web.ui
            debugger/registers.ui
            aboutdialog.ui
            hotkeys.ui
            main.ui
            )

file(GLOB_RECURSE ICONS ${CMAKE_SOURCE_DIR}/dist/icons/*)
file(GLOB_RECURSE THEMES ${CMAKE_SOURCE_DIR}/dist/qt_themes/*)

create_directory_groups(${SRCS} ${HEADERS} ${UIS})

if (Qt5_FOUND)
    qt5_wrap_ui(UI_HDRS ${UIS})
else()
    qt4_wrap_ui(UI_HDRS ${UIS})
endif()

if (ENABLE_QT_TRANSLATION)

    # Update source TS file if enabled
    if (GENERATE_QT_TRANSLATION)
        # Note: with qt5_create_translation, en.ts becomes a build artifact and will be deleted in clean process
        qt5_create_translation(QM_FILES ${SRCS} ${HEADERS} ${UIS} ${CITRA_QT_LANGUAGES}/en.ts)
        add_custom_target(translation ALL DEPENDS ${CITRA_QT_LANGUAGES}/en.ts)
    endif()

    # Find all TS files except en.ts
    file(GLOB_RECURSE LANGUAGES_TS ${CITRA_QT_LANGUAGES}/*.ts)
    list(REMOVE_ITEM LANGUAGES_TS ${CITRA_QT_LANGUAGES}/en.ts)

    # Compile TS files to QM files
    qt5_add_translation(LANGUAGES_QM ${LANGUAGES_TS})

    # Build a QRC file from the QM file list
    SET(LANGUAGES_QRC ${CMAKE_CURRENT_BINARY_DIR}/languages.qrc)
    FILE(WRITE ${LANGUAGES_QRC} "<RCC><qresource prefix=\"languages\">\n")
    foreach (QM ${LANGUAGES_QM})
        get_filename_component(QM_FILE ${QM} NAME)
        FILE(APPEND ${LANGUAGES_QRC} "<file>${QM_FILE}</file>\n")
    endforeach (QM)
    FILE(APPEND ${LANGUAGES_QRC} "</qresource></RCC>")

    # Add the QRC file to package in all QM files
    qt5_add_resources(LANGUAGES ${LANGUAGES_QRC})
else()
    set(LANGUAGES)
endif()


if (APPLE)
    set(MACOSX_ICON "../../dist/citra.icns")
    set_source_files_properties(${MACOSX_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    add_executable(citra-qt MACOSX_BUNDLE ${SRCS} ${HEADERS} ${UI_HDRS} ${ICONS} ${THEMES} ${MACOSX_ICON} ${LANGUAGES})
    set_target_properties(citra-qt PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist)
else()
    add_executable(citra-qt ${SRCS} ${HEADERS} ${UI_HDRS} ${ICONS} ${THEMES} ${LANGUAGES})
endif()
target_link_libraries(citra-qt PRIVATE audio_core common core input_common network video_core)
target_link_libraries(citra-qt PRIVATE Boost::boost glad nihstro-headers Qt5::OpenGL Qt5::Widgets)
target_link_libraries(citra-qt PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)

if(UNIX AND NOT APPLE)
    install(TARGETS citra-qt RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif()

if (MSVC)
    include(CopyCitraQt5Deps)
    include(CopyCitraSDLDeps)
    copy_citra_Qt5_deps(citra-qt)
    copy_citra_SDL_deps(citra-qt)
endif()
