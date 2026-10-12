# Helpers for KDE projects built for one static program (included by plasma-workspace-extra.cmake and
# plasma-desktop-parts/CMakeLists.txt).
#
# nq_old_qml_plugin(TARGET URI CLASS QMLDIR): a QML plugin made the old way (add_library SHARED and a qmldir file
# installed next to it: the task manager's, kicker's, show desktop's, the image wallpaper's) as a static plugin:
# compiled with QT_STATICPLUGIN, its metadata saying its module (moc -Muri: Qt's QML import finds a static plugin
# by that), and its qmldir, with the class named, a resource where Qt looks (:/qt/qml/URI/). The plugin is
# remembered (global property NQ_OLD_QML_PLUGINS, TARGET:CLASS) for whoever imports it (Q_IMPORT_PLUGIN).
function(nq_old_qml_plugin target uri class qmldir)
    target_compile_definitions(${target} PRIVATE QT_STATICPLUGIN)
    set_property(TARGET ${target} APPEND PROPERTY AUTOMOC_MOC_OPTIONS -Muri=${uri})
    string(REPLACE "." "/" path ${uri})
    file(READ ${qmldir} text)
    if(NOT text MATCHES "classname")
        string(APPEND text "\nclassname ${class}\n")
    endif()
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/nq_qmldir_${target})
    file(WRITE ${dir}/qmldir "${text}")
    # (an object library of its own, named as Qt names a target's resource objects, so that it is linked whole by
    # whoever links the plugin: static-plugins.py, plasma-workspace-end.cmake; qt_add_resources on the plugin's
    # own target would use the keyword form of target_link_libraries, which these targets do not)
    file(WRITE ${dir}/nq_qmldir_${target}.qrc "<RCC><qresource prefix=\"/qt/qml/${path}\"><file>qmldir</file></qresource></RCC>\n")
    qt_add_resources(res_src ${dir}/nq_qmldir_${target}.qrc)
    add_library(${target}_resources_99 OBJECT ${res_src})
    target_link_libraries(${target}_resources_99 PRIVATE Qt6::Core)
    set_property(GLOBAL APPEND PROPERTY NQ_OLD_QML_PLUGINS "${target}:${class}")
endfunction()
