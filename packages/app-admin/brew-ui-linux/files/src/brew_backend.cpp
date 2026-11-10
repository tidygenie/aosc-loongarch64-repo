#include "brew_backend.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {
bool executableFile(const QString &path) {
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && info.isExecutable();
}
}

QList<BrewPackage> parseInstalledPackages(const QByteArray &json, QString *error) {
    QList<BrewPackage> result;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Invalid Homebrew JSON: %1").arg(parseError.errorString());
        return result;
    }
    const QJsonValue formulae = document.object().value(QStringLiteral("formulae"));
    if (!formulae.isArray()) {
        if (error) *error = QStringLiteral("Homebrew JSON has no formulae array");
        return result;
    }
    int malformed = 0;
    for (const QJsonValue &value : formulae.toArray()) {
        if (!value.isObject()) { ++malformed; continue; }
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) { ++malformed; continue; }
        BrewPackage package;
        package.name = name;
        package.description = object.value(QStringLiteral("desc")).toString();
        package.outdated = object.value(QStringLiteral("outdated")).toBool();
        const QJsonArray installed = object.value(QStringLiteral("installed")).toArray();
        if (!installed.isEmpty()) {
            package.version = installed.last().toObject().value(QStringLiteral("version")).toString();
        }
        if (package.version.isEmpty())
            package.version = object.value(QStringLiteral("versions")).toObject()
                                  .value(QStringLiteral("stable")).toString();
        result.append(package);
    }
    if (malformed && result.isEmpty()) {
        if (error) *error = QStringLiteral("All %1 Homebrew formulae were malformed").arg(malformed);
        return {};
    }
    if (error) error->clear();
    return result;
}

QString discoverBrewExecutable(const QString &explicitPath) {
    if (executableFile(explicitPath)) return QFileInfo(explicitPath).absoluteFilePath();
    const QString fromEnvironment = QProcessEnvironment::systemEnvironment()
                                        .value(QStringLiteral("HOMEBREW_BREW_FILE"));
    if (executableFile(fromEnvironment)) return QFileInfo(fromEnvironment).absoluteFilePath();
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("brew"));
    if (executableFile(fromPath)) return fromPath;
    const QStringList candidates = {
        QStringLiteral("/home/linuxbrew/.linuxbrew/bin/brew"),
        QStringLiteral("/opt/homebrew/bin/brew"),
        QStringLiteral("/usr/local/bin/brew"),
        QStringLiteral("/home/houge/infra/brew-build/bin/brew"),
    };
    for (const QString &candidate : candidates)
        if (executableFile(candidate)) return candidate;
    return {};
}

QString displayCommand(const QString &program, const QStringList &arguments) {
    QStringList words{program};
    for (QString word : arguments) {
        if (word.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9_./:=@+-]")))) {
            word.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            word = QLatin1Char('\'') + word + QLatin1Char('\'');
        }
        words.append(word);
    }
    return words.join(QLatin1Char(' '));
}
