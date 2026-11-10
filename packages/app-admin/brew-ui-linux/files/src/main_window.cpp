#include "main_window.h"

#include "brew_backend.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <csignal>
#include <cstdio>
#include <unistd.h>

MainWindow::MainWindow(QString executable, bool autoRefresh, bool checkInventory,
                       bool checkManagement, QWidget *parent)
    : QMainWindow(parent), brewExecutable(std::move(executable)), process(this),
      inventoryCheck(checkInventory), managementCheck(checkManagement) {
    setWindowTitle(QStringLiteral("Homebrew Manager for Linux (unofficial)"));
    resize(1100, 760);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *tabs = new QTabWidget(central);

    auto *formulaPage = new QWidget(tabs);
    auto *formulaLayout = new QVBoxLayout(formulaPage);
    auto *controls = new QHBoxLayout;
    packageName = new QLineEdit(formulaPage);
    packageName->setPlaceholderText(QStringLiteral("Exact formula name, e.g. b2sum"));
    controls->addWidget(packageName, 1);
    installButton = new QPushButton(QStringLiteral("Install"), formulaPage);
    uninstallButton = new QPushButton(QStringLiteral("Uninstall selected"), formulaPage);
    upgradeButton = new QPushButton(QStringLiteral("Upgrade selected"), formulaPage);
    controls->addWidget(installButton);
    controls->addWidget(uninstallButton);
    controls->addWidget(upgradeButton);
    formulaLayout->addLayout(controls);

    auto *commands = new QHBoxLayout;
    refreshButton = new QPushButton(QStringLiteral("Refresh all"), formulaPage);
    updateButton = new QPushButton(QStringLiteral("Update taps"), formulaPage);
    upgradeAllButton = new QPushButton(QStringLiteral("Upgrade all"), formulaPage);
    commands->addWidget(refreshButton);
    commands->addWidget(updateButton);
    commands->addWidget(upgradeAllButton);
    commands->addStretch(1);
    formulaLayout->addLayout(commands);

    packages = new QTableWidget(0, 4, formulaPage);
    packages->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Installed version"),
                                         QStringLiteral("Outdated"), QStringLiteral("Description")});
    packages->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    packages->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    packages->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    packages->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    packages->setSelectionBehavior(QTableWidget::SelectRows);
    packages->setSelectionMode(QTableWidget::SingleSelection);
    packages->setEditTriggers(QTableWidget::NoEditTriggers);
    formulaLayout->addWidget(packages);
    tabs->addTab(formulaPage, QStringLiteral("Formulae"));

    auto *servicesPage = new QWidget(tabs);
    auto *servicesLayout = new QVBoxLayout(servicesPage);
    auto *serviceControls = new QHBoxLayout;
    refreshServicesButton = new QPushButton(QStringLiteral("Refresh services"), servicesPage);
    startServiceButton = new QPushButton(QStringLiteral("Start selected"), servicesPage);
    stopServiceButton = new QPushButton(QStringLiteral("Stop selected"), servicesPage);
    restartServiceButton = new QPushButton(QStringLiteral("Restart selected"), servicesPage);
    serviceControls->addWidget(refreshServicesButton);
    serviceControls->addWidget(startServiceButton);
    serviceControls->addWidget(stopServiceButton);
    serviceControls->addWidget(restartServiceButton);
    serviceControls->addStretch(1);
    servicesLayout->addLayout(serviceControls);
    servicesTable = new QTableWidget(0, 4, servicesPage);
    servicesTable->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Status"),
                                              QStringLiteral("User"), QStringLiteral("Service file")});
    servicesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    servicesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    servicesTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    servicesTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    servicesTable->setSelectionBehavior(QTableWidget::SelectRows);
    servicesTable->setSelectionMode(QTableWidget::SingleSelection);
    servicesTable->setEditTriggers(QTableWidget::NoEditTriggers);
    servicesLayout->addWidget(servicesTable);
    tabs->addTab(servicesPage, QStringLiteral("Services"));

    auto *tapsPage = new QWidget(tabs);
    auto *tapsLayout = new QVBoxLayout(tapsPage);
    auto *tapControls = new QHBoxLayout;
    tapName = new QLineEdit(tapsPage);
    tapName->setPlaceholderText(QStringLiteral("Exact tap name, e.g. loongarch64/loong64"));
    tapControls->addWidget(tapName, 1);
    addTapButton = new QPushButton(QStringLiteral("Add tap"), tapsPage);
    removeTapButton = new QPushButton(QStringLiteral("Untap selected"), tapsPage);
    refreshTapsButton = new QPushButton(QStringLiteral("Refresh taps"), tapsPage);
    tapControls->addWidget(addTapButton);
    tapControls->addWidget(removeTapButton);
    tapControls->addWidget(refreshTapsButton);
    tapsLayout->addLayout(tapControls);
    tapsTable = new QTableWidget(0, 1, tapsPage);
    tapsTable->setHorizontalHeaderLabels({QStringLiteral("Tap")});
    tapsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tapsTable->setSelectionBehavior(QTableWidget::SelectRows);
    tapsTable->setSelectionMode(QTableWidget::SingleSelection);
    tapsTable->setEditTriggers(QTableWidget::NoEditTriggers);
    tapsLayout->addWidget(tapsTable);
    auto *linuxNote = new QLabel(QStringLiteral("Linux manages formulae, systemd services and taps here; macOS casks are not emulated."), tapsPage);
    linuxNote->setWordWrap(true);
    tapsLayout->addWidget(linuxNote);
    tabs->addTab(tapsPage, QStringLiteral("Taps"));

    layout->addWidget(tabs, 3);
    auto *bottom = new QHBoxLayout;
    bottom->addStretch(1);
    cancelButton = new QPushButton(QStringLiteral("Stop command"), central);
    cancelButton->setEnabled(false);
    bottom->addWidget(cancelButton);
    layout->addLayout(bottom);
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
    connect(refreshServicesButton, &QPushButton::clicked, this, [this] { refreshServices(); });
    connect(startServiceButton, &QPushButton::clicked, this, [this] { serviceAction(QStringLiteral("start")); });
    connect(stopServiceButton, &QPushButton::clicked, this, [this] { serviceAction(QStringLiteral("stop")); });
    connect(restartServiceButton, &QPushButton::clicked, this, [this] { serviceAction(QStringLiteral("restart")); });
    connect(addTapButton, &QPushButton::clicked, this, [this] { addTap(); });
    connect(tapName, &QLineEdit::returnPressed, this, [this] { addTap(); });
    connect(removeTapButton, &QPushButton::clicked, this, [this] { removeTap(); });
    connect(refreshTapsButton, &QPushButton::clicked, this, [this] { refreshTaps(); });
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
            if (inventoryCheck || managementCheck)
                QTimer::singleShot(0, this, [] { QCoreApplication::exit(1); });
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
    const bool hasBrew = !brewExecutable.isEmpty();
    refreshButton->setEnabled(!busy && hasBrew);
    installButton->setEnabled(!busy && hasBrew);
    uninstallButton->setEnabled(!busy && hasBrew);
    upgradeButton->setEnabled(!busy && hasBrew);
    upgradeAllButton->setEnabled(!busy && hasBrew);
    updateButton->setEnabled(!busy && hasBrew);
    refreshServicesButton->setEnabled(!busy && hasBrew);
    startServiceButton->setEnabled(!busy && hasBrew);
    stopServiceButton->setEnabled(!busy && hasBrew);
    restartServiceButton->setEnabled(!busy && hasBrew);
    refreshTapsButton->setEnabled(!busy && hasBrew);
    addTapButton->setEnabled(!busy && hasBrew);
    removeTapButton->setEnabled(!busy && hasBrew);
    packageName->setEnabled(!busy);
    tapName->setEnabled(!busy);
    cancelButton->setEnabled(busy);
}

void MainWindow::runCommand(const QStringList &arguments, bool refreshAfter) {
    if (process.state() != QProcess::NotRunning || brewExecutable.isEmpty()) return;
    currentArguments = arguments;
    standardOutput.clear();
    refreshWhenDone = refreshAfter;
    const QString operation = arguments.value(0);
    mutationRunning = operation == QStringLiteral("install") ||
                      operation == QStringLiteral("uninstall") ||
                      operation == QStringLiteral("upgrade") ||
                      operation == QStringLiteral("update") ||
                      operation == QStringLiteral("untap") ||
                      (operation == QStringLiteral("tap") && arguments.size() > 1) ||
                      (operation == QStringLiteral("services") && arguments.value(1) != QStringLiteral("list"));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("HOMEBREW_NO_INSTALL_FROM_API"), QStringLiteral("1"));
    if (operation != QStringLiteral("update"))
        environment.insert(QStringLiteral("HOMEBREW_NO_AUTO_UPDATE"), QStringLiteral("1"));
    process.setProgram(brewExecutable);
    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    appendOutput(QStringLiteral("$ %1\n").arg(displayCommand(brewExecutable, arguments)));
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("Running brew %1").arg(operation));
    process.start(QIODevice::ReadWrite);
}

void MainWindow::refresh() {
    runCommand({QStringLiteral("info"), QStringLiteral("--json=v2"), QStringLiteral("--installed")});
}

void MainWindow::refreshServices() {
    runCommand({QStringLiteral("services"), QStringLiteral("list"), QStringLiteral("--json")});
}

void MainWindow::refreshTaps() {
    runCommand({QStringLiteral("tap")});
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

QString MainWindow::selectedService() const {
    const auto selected = servicesTable->selectedItems();
    return selected.isEmpty() ? QString{} : servicesTable->item(selected.constFirst()->row(), 0)->text();
}

QString MainWindow::selectedTap() const {
    const auto selected = tapsTable->selectedItems();
    return selected.isEmpty() ? QString{} : tapsTable->item(selected.constFirst()->row(), 0)->text();
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

void MainWindow::serviceAction(const QString &action) {
    const QString name = selectedService();
    static const QRegularExpression validService(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_+@.-]*$"));
    if (name.isEmpty() || !validService.match(name).hasMatch()) return;
    if (QMessageBox::question(this, QStringLiteral("Manage Homebrew service"),
                              QStringLiteral("Run brew services %1 %2?").arg(action, name)) != QMessageBox::Yes)
        return;
    runCommand({QStringLiteral("services"), action, name}, true);
}

void MainWindow::addTap() {
    const QString name = tapName->text().trimmed();
    static const QRegularExpression validTap(QStringLiteral("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$"));
    if (!validTap.match(name).hasMatch()) {
        statusBar()->showMessage(QStringLiteral("Enter one exact user/repository tap name"));
        return;
    }
    runCommand({QStringLiteral("tap"), name}, true);
}

void MainWindow::removeTap() {
    const QString name = selectedTap();
    if (name.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("Remove tap"),
                              QStringLiteral("Run brew untap %1?").arg(name)) != QMessageBox::Yes)
        return;
    runCommand({QStringLiteral("untap"), name}, true);
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
    const bool servicesList = operation == QStringLiteral("services") &&
                              currentArguments == QStringList({QStringLiteral("services"), QStringLiteral("list"), QStringLiteral("--json")});
    const bool tapList = operation == QStringLiteral("tap") && currentArguments.size() == 1;
    bool inventoryCheckPassed = false;
    bool inventoryParsed = false;
    bool servicesParsed = false;
    bool tapsParsed = false;
    bool checkStepFailed = false;
    if (succeeded && operation == QStringLiteral("info")) {
        QString error;
        const auto inventory = parseInstalledPackages(standardOutput, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error);
            checkStepFailed = true;
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
            inventoryParsed = true;
        }
    } else if (succeeded && servicesList) {
        QString error;
        const auto services = parseServices(standardOutput, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error);
            servicesCheckPassed = false;
            checkStepFailed = true;
        } else {
            servicesTable->setRowCount(services.size());
            int row = 0;
            for (const auto &service : services) {
                servicesTable->setItem(row, 0, new QTableWidgetItem(service.name));
                servicesTable->setItem(row, 1, new QTableWidgetItem(service.status));
                servicesTable->setItem(row, 2, new QTableWidgetItem(service.user));
                servicesTable->setItem(row, 3, new QTableWidgetItem(service.file));
                ++row;
            }
            statusBar()->showMessage(QStringLiteral("%1 Homebrew services").arg(services.size()));
            servicesCheckPassed = true;
            servicesParsed = true;
        }
    } else if (succeeded && tapList) {
        QString error;
        const auto taps = parseTapList(standardOutput, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error);
            checkStepFailed = true;
        } else {
            tapsTable->setRowCount(taps.size());
            for (int row = 0; row < taps.size(); ++row)
                tapsTable->setItem(row, 0, new QTableWidgetItem(taps.at(row)));
            statusBar()->showMessage(QStringLiteral("%1 Homebrew taps").arg(taps.size()));
            tapsParsed = true;
        }
    } else if (!succeeded) {
        statusBar()->showMessage(QStringLiteral("brew %1 failed (%2); inspect output").arg(operation).arg(exitCode));
        checkStepFailed = true;
    } else {
        statusBar()->showMessage(QStringLiteral("brew %1 completed").arg(operation));
    }
    setBusy(false);
    mutationRunning = false;

    if (inventoryParsed && !inventoryCheck) refreshServices();
    if (servicesParsed) refreshTaps();

    const bool doRefresh = succeeded && refreshWhenDone && operation != QStringLiteral("info");
    refreshWhenDone = false;
    if (doRefresh) {
        if (operation == QStringLiteral("services")) refreshServices();
        else if (operation == QStringLiteral("tap") || operation == QStringLiteral("untap")) refreshTaps();
        else refresh();
    }
    if (inventoryCheck && operation == QStringLiteral("info")) {
        QTimer::singleShot(0, this, [inventoryCheckPassed] {
            QCoreApplication::exit(inventoryCheckPassed ? 0 : 1);
        });
    }
    if (managementCheck && checkStepFailed) {
        QTimer::singleShot(0, this, [] { QCoreApplication::exit(1); });
    } else if (managementCheck && tapList && succeeded) {
        const bool passed = tapsParsed && servicesCheckPassed;
        if (passed) std::fputs("BREW_MANAGEMENT_CHECK=PASS\n", stderr);
        QTimer::singleShot(0, this, [passed] { QCoreApplication::exit(passed ? 0 : 1); });
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
