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

struct BrewService {
    QString name;
    QString status;
    QString user;
    QString file;
};

QList<BrewPackage> parseInstalledPackages(const QByteArray &json, QString *error);
QList<BrewService> parseServices(const QByteArray &json, QString *error);
QStringList parseTapList(const QByteArray &text, QString *error);
QString discoverBrewExecutable(const QString &explicitPath = {});
QString displayCommand(const QString &program, const QStringList &arguments);
