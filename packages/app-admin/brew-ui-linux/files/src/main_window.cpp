#include "main_window.h"

#include "brew_backend.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <csignal>
#include <unistd.h>

MainWindow::MainWindow(QString executable, bool autoRefresh, bool checkInventory, QWidget *parent)
    : QMainWindow(parent), brewExecutable(std::move(executable)), process(this),
      inventoryCheck(checkInventory) {
    setWindowTitle(QStringLiteral("Homebrew Manager for Linux (unofficial)"));
    resize(1000, 700);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *controls = new QHBoxLayout;
    packageName = new QLineEdit(central);
    packageName->setPlaceholderText(QStringLiteral("Exact formula name, e.g. b2sum"));
    controls->addWidget(packageName, 1);
    installButton = new QPushButton(QStringLiteral("Install"), central);
    uninstallButton = new QPushButton(QStringLiteral("Uninstall selected"), central);
    upgradeButton = new QPushButton(QStringLiteral("Upgrade selected"), central);
    controls->addWidget(installButton);
    controls->addWidget(uninstallButton);
    controls->addWidget(upgradeButton);
    layout->addLayout(controls);

    auto *commands = new QHBoxLayout;
    refreshButton = new QPushButton(QStringLiteral("Refresh"), central);
    updateButton = new QPushButton(QStringLiteral("Update taps"), central);
    upgradeAllButton = new QPushButton(QStringLiteral("Upgrade all"), central);
    cancelButton = new QPushButton(QStringLiteral("Stop command"), central);
    cancelButton->setEnabled(false);
    commands->addWidget(refreshButton);
    commands->addWidget(updateButton);
    commands->addWidget(upgradeAllButton);
    commands->addStretch(1);
    commands->addWidget(cancelButton);
    layout->addLayout(commands);

    packages = new QTableWidget(0, 4, central);
    packages->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Installed version"),
                                         QStringLiteral("Outdated"), QStringLiteral("Description")});
    packages->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    packages->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    packages->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    packages->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    packages->setSelectionBehavior(QTableWidget::SelectRows);
    packages->setSelectionMode(QTableWidget::SingleSelection);
    packages->setEditTriggers(QTableWidget::NoEditTriggers);
    layout->addWidget(packages, 3);

    console = new QPlainTextEdit(central);
    console->setReadOnly(true);
    console->setPlaceholderText(QStringLiteral("Homebrew commands and their complete output appear here"));
    layout->addWidget(console, 2);
    setCentralWidget(central);

    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(installButton, &QPushButton::clicked, this, [this] { installTyped(); });
    connect(packageName, &QLineEdit::returnPressed, this, [this] { installTyped(); });
    connect(uninstallButton, &QPushButton::clicked, this, [this] { uninstallSelected(); });
    connect(upgradeButton, &QPushButton::clicked, this, [this] { upgradeSelected(); });
    connect(upgradeAllButton, &QPushButton::clicked, this, [this] { upgradeAll(); });
    connect(updateButton, &QPushButton::clicked, this, [this] { updateTaps(); });
    connect(cancelButton, &QPushButton::clicked, this, [this] { cancelCommand(); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray data = process.readAllStandardOutput();
        standardOutput.append(data);
        appendOutput(QString::fromUtf8(data));
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] {
        appendOutput(QString::fromUtf8(process.readAllStandardError()));
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) { finishCommand(code, status); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            statusBar()->showMessage(QStringLiteral("Could not start Homebrew"));
            appendOutput(process.errorString() + QLatin1Char('\n'));
            refreshWhenDone = false;
            mutationRunning = false;
            setBusy(false);
            if (inventoryCheck) QTimer::singleShot(0, this, [] { QCoreApplication::exit(1); });
        }
    });
    process.setChildProcessModifier([] { ::setsid(); });

    if (brewExecutable.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Homebrew executable not found; use --brew /path/to/brew"));
    } else {
        statusBar()->showMessage(QStringLiteral("Homebrew: %1").arg(brewExecutable));
        if (autoRefresh) refresh();
    }
}

void MainWindow::appendOutput(const QString &text) {
    console->moveCursor(QTextCursor::End);
    console->insertPlainText(text);
    console->moveCursor(QTextCursor::End);
}

void MainWindow::setBusy(bool busy) {
    refreshButton->setEnabled(busy ? false : !brewExecutable.isEmpty());
    installButton->setEnabled(!busy);
    uninstallButton->setEnabled(!busy);
    upgradeButton->setEnabled(!busy);
    upgradeAllButton->setEnabled(!busy);
    updateButton->setEnabled(!busy);
    cancelButton->setEnabled(busy);
}

void MainWindow::runCommand(const QStringList &arguments, bool refreshAfter) {
    if (process.state() != QProcess::NotRunning || brewExecutable.isEmpty()) return;
    currentArguments = arguments;
    standardOutput.clear();
    refreshWhenDone = refreshAfter;
    mutationRunning = !arguments.isEmpty() &&
                      (arguments.at(0) == QStringLiteral("install") ||
                       arguments.at(0) == QStringLiteral("uninstall") ||
                       arguments.at(0) == QStringLiteral("upgrade") ||
                       arguments.at(0) == QStringLiteral("update"));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("HOMEBREW_NO_INSTALL_FROM_API"), QStringLiteral("1"));
    if (arguments.value(0) != QStringLiteral("update"))
        environment.insert(QStringLiteral("HOMEBREW_NO_AUTO_UPDATE"), QStringLiteral("1"));
    process.setProgram(brewExecutable);
    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    appendOutput(QStringLiteral("$ %1\n").arg(displayCommand(brewExecutable, arguments)));
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("Running brew %1").arg(arguments.value(0)));
    process.start(QIODevice::ReadWrite);
}

void MainWindow::refresh() {
    runCommand({QStringLiteral("info"), QStringLiteral("--json=v2"), QStringLiteral("--installed")});
}

void MainWindow::installTyped() {
    const QString name = packageName->text().trimmed();
    static const QRegularExpression validName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_+@./-]*$"));
    if (!validName.match(name).hasMatch()) {
        statusBar()->showMessage(QStringLiteral("Enter one exact formula name"));
        return;
    }
    runCommand({QStringLiteral("install"), name}, true);
}

QString MainWindow::selectedPackage() const {
    const auto selected = packages->selectedItems();
    return selected.isEmpty() ? QString{} : packages->item(selected.constFirst()->row(), 0)->text();
}

void MainWindow::uninstallSelected() {
    const QString name = selectedPackage();
    if (name.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("Uninstall formula"),
                              QStringLiteral("Run brew uninstall %1?").arg(name)) != QMessageBox::Yes)
        return;
    runCommand({QStringLiteral("uninstall"), name}, true);
}

void MainWindow::upgradeSelected() {
    const QString name = selectedPackage();
    if (!name.isEmpty()) runCommand({QStringLiteral("upgrade"), name}, true);
}

void MainWindow::upgradeAll() {
    if (QMessageBox::question(this, QStringLiteral("Upgrade all"),
                              QStringLiteral("Run brew upgrade for all outdated formulae?")) != QMessageBox::Yes)
        return;
    runCommand({QStringLiteral("upgrade")}, true);
}

void MainWindow::updateTaps() {
    runCommand({QStringLiteral("update")}, true);
}

void MainWindow::cancelCommand() {
    if (process.state() == QProcess::NotRunning) return;
    const qint64 pid = process.processId();
    if (pid > 0) ::kill(-static_cast<pid_t>(pid), SIGTERM);
    statusBar()->showMessage(QStringLiteral("Cancellation requested; waiting for Homebrew to exit"));
}

void MainWindow::finishCommand(int exitCode, QProcess::ExitStatus status) {
    const QString operation = currentArguments.value(0);
    const bool succeeded = status == QProcess::NormalExit && exitCode == 0;
    bool inventoryCheckPassed = false;
    if (succeeded && operation == QStringLiteral("info")) {
        QString error;
        const auto inventory = parseInstalledPackages(standardOutput, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error);
        } else {
            packages->setRowCount(inventory.size());
            int row = 0;
            for (const auto &package : inventory) {
                packages->setItem(row, 0, new QTableWidgetItem(package.name));
                packages->setItem(row, 1, new QTableWidgetItem(package.version));
                packages->setItem(row, 2, new QTableWidgetItem(package.outdated ? QStringLiteral("yes") : QStringLiteral("no")));
                packages->setItem(row, 3, new QTableWidgetItem(package.description));
                ++row;
            }
            statusBar()->showMessage(QStringLiteral("%1 installed formulae").arg(inventory.size()));
            inventoryCheckPassed = true;
        }
    } else if (!succeeded) {
        statusBar()->showMessage(QStringLiteral("brew %1 failed (%2); inspect output").arg(operation).arg(exitCode));
    } else {
        statusBar()->showMessage(QStringLiteral("brew %1 completed").arg(operation));
    }
    setBusy(false);
    mutationRunning = false;
    const bool doRefresh = succeeded && refreshWhenDone && operation != QStringLiteral("info");
    refreshWhenDone = false;
    if (doRefresh) refresh();
    if (inventoryCheck && operation == QStringLiteral("info")) {
        QTimer::singleShot(0, this, [inventoryCheckPassed] {
            QCoreApplication::exit(inventoryCheckPassed ? 0 : 1);
        });
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (process.state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Command still running"),
                                 QStringLiteral("Stop the Homebrew command before closing the window."));
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}
