# CMake toolchain for native linux-wasm programs (wasm64, musl). See docs/native-wasm.md.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR wasm64)
set(NQ_SYSROOT $ENV{NQ_SYSROOT})
get_filename_component(_nq_bin "${CMAKE_CURRENT_LIST_DIR}/bin" ABSOLUTE)
set(CMAKE_C_COMPILER ${_nq_bin}/nq-clang)
set(CMAKE_CXX_COMPILER ${_nq_bin}/nq-clang++)
set(CMAKE_AR $ENV{NQ_LLVM}/llvm-ar)
set(CMAKE_RANLIB $ENV{NQ_LLVM}/llvm-ranlib)
set(CMAKE_NM $ENV{NQ_LLVM}/llvm-nm)
set(CMAKE_OBJDUMP $ENV{NQ_LLVM}/llvm-objdump)
set(CMAKE_STRIP $ENV{NQ_LLVM}/llvm-strip)
set(CMAKE_SYSROOT ${NQ_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${NQ_SYSROOT} ${NQ_SYSROOT}/usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_COMPILER_LAUNCHER $ENV{NQ_LAUNCHER})
set(CMAKE_CXX_COMPILER_LAUNCHER $ENV{NQ_LAUNCHER})
# Everything is static: one module per program (docs/native-wasm.md, "Linking").
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${NQ_SYSROOT})
set(ENV{PKG_CONFIG_LIBDIR} ${NQ_SYSROOT}/usr/lib/pkgconfig:${NQ_SYSROOT}/usr/share/pkgconfig)
# Debian's Qt patch turns off the automatic inclusion of plugin packages (right for shared Qt); a static
# program needs them to link its plugins (qt_import_plugins finds no plugin target otherwise).
set(QT_SKIP_AUTO_PLUGIN_INCLUSION OFF CACHE BOOL "")
set(QT_SKIP_AUTO_QML_PLUGIN_INCLUSION OFF CACHE BOOL "")   # the same for QML modules' plugins
# No rpaths: Wasm modules have none (and CMake refuses to rewrite a non-ELF executable's at install).
set(CMAKE_SKIP_RPATH ON)

# A static build has no loadable modules nor shared libraries: add_library(X MODULE) and add_library(X SHARED)
# (KDE's plugins: KWindowSystem's Wayland platform, KIdleTime's, KAuth's backend, LayerShellQt's shell
# integration) become static libraries, which
# the program that uses it links (Q_IMPORT_PLUGIN / kcoreaddons static plugins). Once per CMake run: a second
# override would call itself.
get_property(_nq_add_library_done GLOBAL PROPERTY NQ_ADD_LIBRARY_OVERRIDDEN)
if(NOT _nq_add_library_done)
	set_property(GLOBAL PROPERTY NQ_ADD_LIBRARY_OVERRIDDEN TRUE)
	function(add_library name)
		set(args ${ARGN})
		if(args AND NOT BUILD_SHARED_LIBS)
			list(GET args 0 type)
			# (list(FIND), not IN_LIST: projects with an old cmake_minimum_required have no IN_LIST)
			list(FIND args IMPORTED _nq_imported)
			if((type STREQUAL "MODULE" OR type STREQUAL "SHARED") AND _nq_imported EQUAL -1)
				list(REMOVE_AT args 0)
				_add_library(${name} STATIC ${args})
				# A plugin's moc output then names its instance qt_static_plugin_<Class>() (registered by
				# whoever links it), not qt_plugin_instance(), which every plugin would define.
				target_compile_definitions(${name} PRIVATE QT_STATICPLUGIN)
				# An old-style QML plugin (a qmldir next to it naming it, no qt_add_qml_module): a static one needs
				# its module's URI in its metadata (without one, Qt refuses every QML import while it is linked:
				# "static plugin ... has no metadata URI"), and its qmldir in the resources, where the engine finds
				# it (/qt/qml/...): an object library <name>_resources_98 (native/static-plugins.py links those).
				if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/qmldir)
					file(READ ${CMAKE_CURRENT_SOURCE_DIR}/qmldir _nq_qmldir)
					string(REGEX MATCH "module[ \t]+([A-Za-z0-9_.]+)" _nq_m "${_nq_qmldir}")
					set(_nq_uri ${CMAKE_MATCH_1})
					if(_nq_uri AND _nq_qmldir MATCHES "plugin[ \t]+${name}([ \t\n]|$)")
						set_property(TARGET ${name} APPEND PROPERTY AUTOMOC_MOC_OPTIONS -Muri=${_nq_uri})
						string(REPLACE "." "/" _nq_path ${_nq_uri})
						set(_nq_qrc ${CMAKE_CURRENT_BINARY_DIR}/nq_auto_qmldir_${name}.qrc)
						file(WRITE ${_nq_qrc} "<RCC><qresource prefix=\"/qt/qml/${_nq_path}\"><file alias=\"qmldir\">${CMAKE_CURRENT_SOURCE_DIR}/qmldir</file></qresource></RCC>\n")
						_add_library(${name}_resources_98 OBJECT ${_nq_qrc})
						set_target_properties(${name}_resources_98 PROPERTIES AUTORCC ON AUTOMOC OFF AUTOUIC OFF)
					endif()
				endif()
				return()
			endif()
		endif()
		_add_library(${name} ${ARGN})
	endfunction()
endif()

# Qt's versionless macros (qt_add_dbus_interface, qt_add_shaders, ...) are not made when a module's CMake
# package is first loaded while QT_NO_CREATE_VERSIONLESS_FUNCTIONS is on. ECMQmlModule loads Qt that way,
# and in a static Qt that load pulls QtDBus and QtShaderTools in as dependencies: KF6/Plasma projects then
# found no qt_add_dbus_interface. Macros (not functions), so that the Qt 6 ones set their results in the
# caller's scope; Qt's own definitions replace these when they are made.
foreach(_nq_f add_dbus_interface add_dbus_interfaces add_dbus_adaptor generate_dbus_interface add_shaders)
	if(NOT COMMAND qt_${_nq_f})
		cmake_language(EVAL CODE "macro(qt_${_nq_f})\n qt6_${_nq_f}(\${ARGV})\nendmacro()")
	endif()
endforeach()
# The host's rcc compresses with zstd when it can, and the target Qt has no zstd (qResourceFeatureZstd() would be
# missing): every AUTORCC resource without it.
set(CMAKE_AUTORCC_OPTIONS --no-zstd)
