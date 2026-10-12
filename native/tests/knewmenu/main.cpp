// KNewFileMenu as the desktop's "Create New" submenu has it: what the menu holds after checkUpToDate(), and what the
// built-in template resource (kio's new_file_templates/templates.qrc) lists. Run with QT_QPA_PLATFORM=offscreen.
#include <KNewFileMenu>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QResource>
#include <QStandardPaths>
#include <cstdio>

int main(int argc, char **argv) {
	QApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("knew: resource roots:");
	for (const QString &e : QDir(QStringLiteral(":/")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden))
		std::printf(" %s", qPrintable(e));
	std::printf("\n");
	std::printf("knew: resource dir entries:");
	for (const QString &e : QDir(QStringLiteral(":/kio5/newfile-templates")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden))
		std::printf(" %s", qPrintable(e));
	std::printf("\n");
	std::printf("knew: .source/emptydir exists %d, Directory.desktop exists %d\n",
		QFile::exists(QStringLiteral(":/kio5/newfile-templates/.source/emptydir")),
		QFile::exists(QStringLiteral(":/kio5/newfile-templates/Directory.desktop")));
	std::printf("knew: templates dirs:");
	for (const QString &d : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("templates"), QStandardPaths::LocateDirectory))
		std::printf(" %s", qPrintable(d));
	std::printf("\n");
	KNewFileMenu menu(nullptr);
	menu.checkUpToDate();
	std::printf("knew: menu has %d actions, enabled %d, menu widget enabled %d\n", int(menu.menu()->actions().size()), menu.isEnabled(), menu.menu()->isEnabled());
	for (QAction *a : menu.menu()->actions())
		std::printf("knew:   %s%s\n", a->isSeparator() ? "---" : qPrintable(a->text()), a->menu() ? " (submenu)" : "");
	return 0;
}
