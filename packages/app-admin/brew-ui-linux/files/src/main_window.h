#pragma once

#include <QMainWindow>
#include <QProcess>
#include <QStringList>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QString brewExecutable, bool autoRefresh, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void refresh();
    void installTyped();
    void uninstallSelected();
    void upgradeSelected();
    void upgradeAll();
    void updateTaps();
    void cancelCommand();
    void runCommand(const QStringList &arguments, bool refreshAfter = false);
    void finishCommand(int exitCode, QProcess::ExitStatus status);
    void appendOutput(const QString &text);
    void setBusy(bool busy);
    QString selectedPackage() const;

    QString brewExecutable;
    QTableWidget *packages;
    QLineEdit *packageName;
    QPlainTextEdit *console;
    QPushButton *refreshButton;
    QPushButton *installButton;
    QPushButton *uninstallButton;
    QPushButton *upgradeButton;
    QPushButton *upgradeAllButton;
    QPushButton *updateButton;
    QPushButton *cancelButton;
    QProcess process;
    QByteArray standardOutput;
    QStringList currentArguments;
    bool refreshWhenDone = false;
    bool mutationRunning = false;
};
