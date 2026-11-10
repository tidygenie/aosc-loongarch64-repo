#include "brew_backend.h"

#include <QDebug>
#include <QFileInfo>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qCritical() << message;
        std::exit(1);
    }
}
}

int main() {
    const QByteArray json = R"({"formulae":[{"name":"b2sum","desc":"Portable checksum utility","outdated":true,"installed":[{"version":"20190424"}],"versions":{"stable":"20260101"},"unknown_new_field":42},{"name":"zsh","installed":[{"version":"6.0"}]}]})";
    QString error;
    const auto packages = parseInstalledPackages(json, &error);
    require(error.isEmpty(), "valid JSON returned an error");
    require(packages.size() == 2, "wrong formula count");
    require(packages[0].name == QStringLiteral("b2sum"), "wrong formula name");
    require(packages[0].version == QStringLiteral("20190424"), "installed version must win over stable");
    require(packages[0].description == QStringLiteral("Portable checksum utility"), "description lost");
    require(packages[0].outdated, "outdated state lost");
    require(packages[1].version == QStringLiteral("6.0"), "second installed version lost");

    require(parseInstalledPackages("not-json", &error).isEmpty() && !error.isEmpty(), "invalid JSON accepted");
    require(parseInstalledPackages("{}", &error).isEmpty() && !error.isEmpty(), "missing formulae accepted");
    require(displayCommand(QStringLiteral("/bin/brew"), {QStringLiteral("install"), QStringLiteral("bad name")}) ==
                QStringLiteral("/bin/brew install 'bad name'"), "display quoting failed");
    require(discoverBrewExecutable(QStringLiteral("/definitely/not/brew")).isEmpty() ||
                discoverBrewExecutable(QStringLiteral("/definitely/not/brew")) != QStringLiteral("/definitely/not/brew"),
            "invalid explicit brew path accepted");
    qInfo() << "BREW_BACKEND_TESTS=PASS";
    return 0;
}
