// What the native KDE libraries read of the colour scheme: where kdeglobals is found, the scheme it names, and
// the colours KColorScheme gives (the Plasma panel's come from these: ksvg's ImageSet, libplasma's Theme).
#include <KColorScheme>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QCoreApplication>
#include <QStandardPaths>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("config home: %s\n", qPrintable(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)));
    for (const QString &p : QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals")))
        printf("kdeglobals: %s\n", qPrintable(p));
    KSharedConfigPtr c = KSharedConfig::openConfig();
    printf("default config: %s\n", qPrintable(c->name()));
    printf("ColorScheme: %s\n", qPrintable(KConfigGroup(c, QStringLiteral("General")).readEntry("ColorScheme", QString())));
    printf("Colors:Window BackgroundNormal: %s\n",
           qPrintable(KConfigGroup(c, QStringLiteral("Colors:Window")).readEntry("BackgroundNormal", QString())));
    KColorScheme w(QPalette::Active, KColorScheme::Window);
    printf("KColorScheme Window background %s foreground %s\n", qPrintable(w.background().color().name()),
           qPrintable(w.foreground().color().name()));
    return 0;
}
