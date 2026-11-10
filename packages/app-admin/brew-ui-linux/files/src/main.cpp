#include "brew_backend.h"
#include "main_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 ||
                      std::strcmp(argv[1], "-v") == 0)) {
        std::puts("brew-ui-linux 0.1.0");
        return 0;
    }
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("brew-ui-linux"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Transparent Homebrew package management for Linux"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption brewOption(QStringLiteral("brew"), QStringLiteral("Homebrew executable"), QStringLiteral("path"));
    const QCommandLineOption smokeOption(QStringLiteral("smoke-test"), QStringLiteral("Construct the GUI and exit without running brew"));
    const QCommandLineOption inventoryOption(QStringLiteral("check-inventory"), QStringLiteral("Run one real read-only inventory query and exit 0 only on success"));
    parser.addOption(brewOption);
    parser.addOption(smokeOption);
    parser.addOption(inventoryOption);
    parser.process(application);
    if (parser.isSet(smokeOption) && parser.isSet(inventoryOption)) parser.showHelp(2);
    const QString brewExecutable = discoverBrewExecutable(parser.value(brewOption));
    if ((parser.isSet(brewOption) && brewExecutable.isEmpty()) ||
        (parser.isSet(inventoryOption) && brewExecutable.isEmpty())) {
        parser.showHelp(2);
    }
    MainWindow window(brewExecutable, !parser.isSet(smokeOption), parser.isSet(inventoryOption));
    window.show();
    if (parser.isSet(smokeOption)) QTimer::singleShot(100, &application, &QCoreApplication::quit);
    if (parser.isSet(inventoryOption)) QTimer::singleShot(60000, &application, [] { QCoreApplication::exit(124); });
    return QApplication::exec();
}
