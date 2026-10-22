# File Associations (kcm_filetypes) from kde-cli-tools 6.3.6's keditfiletype: the module only, not the keditfiletype program.
add_definitions(-DTRANSLATION_DOMAIN=\"kcm_filetypes\")
set(KFT ${CLI_SRC}/keditfiletype)
set(libfiletypes_SRCS ${KFT}/filetypedetails.cpp ${KFT}/filegroupdetails.cpp ${KFT}/kservicelistwidget.cpp ${KFT}/typeslistitem.cpp
    ${KFT}/typeslistitembase.cpp ${KFT}/typeslistproxyitem.cpp ${KFT}/typeslisttreewidget.cpp ${KFT}/mimetypedata.cpp
    ${KFT}/mimetypewriter.cpp ${KFT}/multiapplydialog.cpp ${KFT}/newtypedlg.cpp ${KFT}/kserviceselectdlg.cpp)
kcoreaddons_add_plugin(kcm_filetypes SOURCES ${KFT}/filetypesview.cpp ${KFT}/filetypesview.h ${libfiletypes_SRCS}
    INSTALL_NAMESPACE "plasma/kcms/systemsettings_qwidgets")
target_include_directories(kcm_filetypes PRIVATE ${KFT})
kcmutils_generate_desktop_file(kcm_filetypes)
target_link_libraries(kcm_filetypes PRIVATE KF6::ConfigCore KF6::IconThemes KF6::IconWidgets KF6::I18n KF6::KCMUtils KF6::KIOWidgets KF6::Parts Qt::DBus)
