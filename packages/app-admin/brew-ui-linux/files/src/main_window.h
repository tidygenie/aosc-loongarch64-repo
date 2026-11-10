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
    explicit MainWindow(QString brewExecutable, bool autoRefresh, bool inventoryCheck = false,
                        bool managementCheck = false, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void refresh();
    void refreshServices();
    void refreshTaps();
    void installTyped();
    void uninstallSelected();
    void upgradeSelected();
    void upgradeAll();
    void updateTaps();
    void serviceAction(const QString &action);
    void addTap();
    void removeTap();
    void cancelCommand();
    void runCommand(const QStringList &arguments, bool refreshAfter = false);
    void finishCommand(int exitCode, QProcess::ExitStatus status);
    void appendOutput(const QString &text);
    void setBusy(bool busy);
    QString selectedPackage() const;
    QString selectedService() const;
    QString selectedTap() const;

    QString brewExecutable;
    QTableWidget *packages;
    QTableWidget *servicesTable;
    QTableWidget *tapsTable;
    QLineEdit *packageName;
    QLineEdit *tapName;
    QPlainTextEdit *console;
    QPushButton *refreshButton;
    QPushButton *installButton;
    QPushButton *uninstallButton;
    QPushButton *upgradeButton;
    QPushButton *upgradeAllButton;
    QPushButton *updateButton;
    QPushButton *refreshServicesButton;
    QPushButton *startServiceButton;
    QPushButton *stopServiceButton;
    QPushButton *restartServiceButton;
    QPushButton *refreshTapsButton;
    QPushButton *addTapButton;
    QPushButton *removeTapButton;
    QPushButton *cancelButton;
    QProcess process;
    QByteArray standardOutput;
    QStringList currentArguments;
    bool refreshWhenDone = false;
    bool mutationRunning = false;
    bool inventoryCheck = false;
    bool managementCheck = false;
    bool servicesCheckPassed = false;
};
