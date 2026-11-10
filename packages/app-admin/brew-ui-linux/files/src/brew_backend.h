#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

struct BrewPackage {
    QString name;
    QString version;
    QString description;
    bool outdated = false;
};

QList<BrewPackage> parseInstalledPackages(const QByteArray &json, QString *error);
QString discoverBrewExecutable(const QString &explicitPath = {});
QString displayCommand(const QString &program, const QStringList &arguments);
