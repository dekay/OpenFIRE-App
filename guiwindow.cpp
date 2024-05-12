/*  OpenFIRE App: a configuration utility for the OpenFIRE light gun system.
    Copyright (C) 2024  Team OpenFIRE

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "guiwindow.h"
#include "constants.h"
#include "ui_guiwindow.h"
#include "ui_about.h"
#include <QGraphicsScene>
#include <QMessageBox>
#include <QRadioButton>
#include <QSvgRenderer>
#include <QSvgWidget>
#include <QSerialPortInfo>
#include <QtDebug>
#include <QProgressBar>
#include <QProcess>
#include <QStorageInfo>
#include <QThread>

// Currently loaded board object
boardInfo_s board;

// Currently loaded board's TinyUSB identifier info
tinyUSBtable_s tinyUSBtable;
// TinyUSB ident, as loaded from the board
tinyUSBtable_s tinyUSBtable_orig;

// Current calibration profiles
QVector<profilesTable_s> profilesTable(4);
// Calibration profiles, as loaded from the board
QVector<profilesTable_s> profilesTable_orig(4);

// Indexed array map of the current physical layout of the board.
// Key = pin number, Value = pin function
// Values: -2 = N/A, -1 = reserved, 0 = available, unused
QMap<uint8_t, int8_t> currentPins;

#define INPUTS_COUNT 29
// Map of what inputs are put where,
// Key = button/output, Value = pin number occupying, if any.
// Value of -1 means unmapped.
// Key order based on boardInputs_e, minus 1
// Map functions used in deduplication
QMap<uint8_t, int8_t> inputsMap;
// Inputs map, as loaded from the board
QMap<uint8_t, int8_t> inputsMap_orig;

// ^^^-----Typedefs up there:----^^^
//
// vvv---UI Objects down here:---vvv

// Guess I'll have to do the dynamic layout spawning/destroying stuff to make things work.
// How else do I hide things lol?
QVBoxLayout *PinsCenter;
QGridLayout *PinsCenterSub;
QGridLayout *PinsLeft;
QGridLayout *PinsRight;

QComboBox *pinBoxes[30];
QLabel *pinLabel[30];
QWidget *padding[30];

QRadioButton *selectedProfile[4];
QLabel *xScale[4];
QLabel *yScale[4];
QLabel *xCenter[4];
QLabel *yCenter[4];
QComboBox *irSens[4];
QComboBox *runMode[4];
QSvgWidget *centerPic;

QGraphicsScene *testScene;

//
// ^^^-------GLOBAL VARS UP THERE----------^^^
//
// vvv-------GUI METHODS DOWN HERE---------vvv
//

void guiWindow::PortsSearch()
{
    serialFoundList = QSerialPortInfo::availablePorts();
    if(serialFoundList.isEmpty()) {
        //statusBar()->showMessage("FATAL: No COM devices detected!");
        PopupWindow("No devices detected!", "Is the microcontroller board currently running OpenFIRE and is currently plugged in? Make sure it's connected and recognized by the PC.\n\nThis app will now close.", "ERROR", 4);
        exit(1);
    } else {
        // Yeah, sue me, we reading this backwards to make stack management easier.
        for(int i = serialFoundList.length() - 1; i >= 0; --i) {
            if(serialFoundList[i].vendorIdentifier() == 2336) {
                usbName.prepend(serialFoundList[i].systemLocation());
                qDebug() << "Found device @" << serialFoundList[i].systemLocation();
            } else {
                qDebug() << "Deleting dummy device" << serialFoundList[i].systemLocation();
                serialFoundList.removeAt(i);
            }
        }
        if(!usbName.length()) {
            PopupWindow("No OpenFIRE devices detected!", "Is the microcontroller board currently running OpenFIRE and is currently plugged in? Make sure it's connected and recognized by the PC.\n\nThis app will now close.", "ERROR", 4);
            exit(1);
        }
    }
}

guiWindow::guiWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::guiWindow)
{
    ui->setupUi(this);

#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
    if(qEnvironmentVariable("USER") != "root") {
        QProcess *externalProg = new QProcess;
        QStringList args;
        externalProg->start("/usr/bin/groups", args);
        externalProg->waitForFinished();
        if(!externalProg->readAllStandardOutput().contains("dialout")) {
            PopupWindow("User doesn't have serial permissions!", QString("Currently, your user is not allowed to have access to serial devices.\n\nTo add yourself to the right group, run this command in a terminal and then re-login to your session: \n\nsudo usermod -aG dialout %1").arg(qEnvironmentVariable("USER")), "Permission error", 2);
            exit(0);
        }
    } else {
        PopupWindow("Running as root is not allowed!", "Please run the OpenFIRE app as a normal user.", "ERROR", 4);
        exit(2);
    }
#endif

    connect(&serialPort, &QSerialPort::readyRead, this, &guiWindow::serialPort_readyRead);

    // just to be sure, init the inputsMap hashes
    for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
        inputsMap[i] = -1;
        inputsMap_orig[i] = -1;
    }

    // sending all these children to die upon comPortSelector->on_currentIndexChanged
    // (which gets fired immediately after ui->comPortSelector->addItems).
    PinsCenter = new QVBoxLayout();
    PinsCenterSub = new QGridLayout();
    PinsLeft = new QGridLayout();
    PinsRight = new QGridLayout();

    ui->PinsTopHalf->addLayout(PinsLeft);
    ui->PinsTopHalf->addLayout(PinsCenter);
    ui->PinsTopHalf->addLayout(PinsRight);

    for(uint8_t i = 0; i < 30; i++) {
        pinBoxes[i] = new QComboBox();
        connect(pinBoxes[i], SIGNAL(activated(int)), this, SLOT(pinBoxes_activated(int)));
        pinLabel[i] = new QLabel();
        pinLabel[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        padding[i] = new QWidget();
        padding[i]->setMinimumHeight(25);
    }

    // These can actually stay, tho.
    for(uint8_t i = 0; i < 4; i++) {
        selectedProfile[i] = new QRadioButton(QString("%1.").arg(i+1));
        connect(selectedProfile[i], SIGNAL(toggled(bool)), this, SLOT(selectedProfile_isChecked(bool)));
        xScale[i] = new QLabel("0");
        yScale[i] = new QLabel("0");
        xCenter[i] = new QLabel("0");
        yCenter[i] = new QLabel("0");
        irSens[i] = new QComboBox();
        runMode[i] = new QComboBox();
        xScale[i]->setAlignment(Qt::AlignHCenter);
        yScale[i]->setAlignment(Qt::AlignHCenter);
        xCenter[i]->setAlignment(Qt::AlignHCenter);
        yCenter[i]->setAlignment(Qt::AlignHCenter);
        irSens[i]->addItem("Default");
        irSens[i]->addItem("Higher");
        irSens[i]->addItem("Highest");
        connect(irSens[i], SIGNAL(activated(int)), this, SLOT(irBoxes_activated(int)));
        runMode[i]->addItem("Normal");
        runMode[i]->addItem("1-Frame Avg");
        runMode[i]->addItem("2-Frame Avg");
        connect(runMode[i], SIGNAL(activated(int)), this, SLOT(runModeBoxes_activated(int)));
        ui->profilesArea->addWidget(selectedProfile[i], i+1, 0, 1, 1);
        ui->profilesArea->addWidget(xScale[i], i+1, 1, 1, 1);
        ui->profilesArea->addWidget(yScale[i], i+1, 3, 1, 1);
        ui->profilesArea->addWidget(xCenter[i], i+1, 5, 1, 1);
        ui->profilesArea->addWidget(yCenter[i], i+1, 7, 1, 1);
        ui->profilesArea->addWidget(irSens[i], i+1, 9, 1, 1);
        ui->profilesArea->addWidget(runMode[i], i+1, 11, 1, 1);
    }

    // Setup Test Mode screen colors
    testPointTLPen.setColor(Qt::green);
    testPointTRPen.setColor(Qt::green);
    testPointBLPen.setColor(Qt::blue);
    testPointBRPen.setColor(Qt::blue);
    testPointMedPen.setColor(Qt::gray);
    testPointDPen.setColor(Qt::red);
    testPointTLPen.setWidth(3);
    testPointTRPen.setWidth(3);
    testPointBLPen.setWidth(3);
    testPointBRPen.setWidth(3);
    testPointMedPen.setWidth(3);
    testPointDPen.setWidth(3);
    testPointTL.setPen(testPointTLPen);
    testPointTR.setPen(testPointTRPen);
    testPointBL.setPen(testPointBLPen);
    testPointBR.setPen(testPointBRPen);
    testPointMed.setPen(testPointMedPen);
    testPointD.setPen(testPointDPen);

    // Actually setup the Test Mode scene
    testScene = new QGraphicsScene();
    testScene->setSceneRect(0, 0, 1024, 768);
    testScene->setBackgroundBrush(Qt::darkGray);
    ui->testView->setScene(testScene);
    testScene->addItem(&testBox);
    testScene->addItem(&testPointTL);
    testScene->addItem(&testPointTR);
    testScene->addItem(&testPointBL);
    testScene->addItem(&testPointBR);
    testScene->addItem(&testPointMed);
    testScene->addItem(&testPointD);
    // TODO: is there a way of dynamically scaling QGraphicsViews?
    ui->testView->scale(0.5, 0.5);

    // Finally get to the thing!
    statusBar()->showMessage("Welcome to the OpenFIRE app!", 3000);
    PortsSearch();
    usbName.prepend("[No device]");
    ui->productIdConverted->setEnabled(false);
    ui->productIdInput->setValidator(new QIntValidator());
    // TODO: what's a good validator to only accept character values within the range of an unsigned char?
    //ui->productNameInput->setValidator(new QRegExpValidator(QRegExp("[A-Za-z0-9_]+"), this));
    ui->comPortSelector->addItems(usbName);
}

guiWindow::~guiWindow()
{
    if(serialPort.isOpen()) {
        statusBar()->showMessage("Sending undock request to board...");
        serialPort.write("XE");
        serialPort.waitForBytesWritten(2000);
        serialPort.waitForReadyRead(2000);
        serialPort.close();
    }
    delete ui;
}


void guiWindow::PopupWindow(QString errorTitle, QString errorMessage, QString windowTitle, int errorType)
{
    QMessageBox messageBox;
    messageBox.setText(errorTitle);
    messageBox.setInformativeText(errorMessage);
    messageBox.setWindowTitle(windowTitle);
    switch(errorType) {
    case 0:
        // lol nothing here
        break;
    case 1:
        messageBox.setIcon(QMessageBox::Question);
        break;
    case 2:
        messageBox.setIcon(QMessageBox::Information);
        break;
    case 3:
        messageBox.setIcon(QMessageBox::Warning);
        break;
    case 4:
        messageBox.setIcon(QMessageBox::Critical);
        break;
    }
    messageBox.exec();
    // TODO: maybe we should be using Serial Port errors instead of assuming,
    // but for now just clear it here for cleanliness.
    serialPort.clearError();
}

// TODO: Copy loaded values to use for comparison to determine state of save button.
void guiWindow::SerialLoad()
{
    serialActive = true;
    serialPort.write("Xlb");
    if(serialPort.waitForBytesWritten(2000)) {
        if(serialPort.waitForReadyRead(2000)) {
            // booleans
            QString buffer;
            for(uint8_t i = 0; i < sizeof(boolSettings); i++) {
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                boolSettings[i] = buffer.toInt();
                boolSettings_orig[i] = boolSettings[i];
            }
            // pins
            if(boolSettings[customPins]) {
                serialPort.write("Xlp");
                serialPort.waitForReadyRead(1000);
                for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
                    buffer = serialPort.readLine();
                    inputsMap_orig[i] = buffer.toInt();
                    // For some reason, QTSerial drops output shortly after this.
                    // So we send a ping to refill the buffer.
                    if(i == 14) {
                        serialPort.write(".");
                        serialPort.waitForReadyRead(1000);
                    }
                }
                inputsMap = inputsMap_orig;
            }
            // settings
            serialPort.write("Xls");
            serialPort.waitForBytesWritten(2000);
            serialPort.waitForReadyRead(2000);
            for(uint8_t i = 0; i < 8; i++) {
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                settingsTable[i] = buffer.toInt();
                settingsTable_orig[i] = settingsTable[i];
            }
            // profiles
            for(uint8_t i = 0; i < 4; i++) {
                QString genString = QString("XlP%1").arg(i);
                serialPort.write(genString.toLocal8Bit());
                serialPort.waitForBytesWritten(2000);
                serialPort.waitForReadyRead(2000);
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                xScale[i]->setText(buffer);
                profilesTable[i].xScale = buffer.toInt();
                profilesTable_orig[i].xScale = profilesTable[i].xScale;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                yScale[i]->setText(buffer);
                profilesTable[i].yScale = buffer.toInt();
                profilesTable_orig[i].yScale = profilesTable[i].yScale;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                xCenter[i]->setText(buffer);
                profilesTable[i].xCenter = buffer.toInt();
                profilesTable_orig[i].xCenter = profilesTable[i].xCenter;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                yCenter[i]->setText(buffer);
                profilesTable[i].yCenter = buffer.toInt();
                profilesTable_orig[i].yCenter = profilesTable[i].yCenter;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                profilesTable[i].irSensitivity = buffer.toInt();
                profilesTable_orig[i].irSensitivity = profilesTable[i].irSensitivity;
                irSens[i]->setCurrentIndex(profilesTable[i].irSensitivity);
                irSensOldIndex[i] = profilesTable[i].irSensitivity;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                profilesTable[i].runMode = buffer.toInt();
                profilesTable_orig[i].runMode = profilesTable[i].runMode;
                runMode[i]->setCurrentIndex(profilesTable[i].runMode);
                runModeOldIndex[i] = profilesTable[i].runMode;
            }
            serialActive = false;
        } else {
            PopupWindow("Data hasn't arrived!", "Device was detected, but settings request wasn't received in time!\nThis can happen if the app was closed in the middle of an operation.\n\nTry selecting the device again.", "Oops!", 4);
            //qDebug() << "Didn't receive any data in time! Dammit Seong, you jiggled the cable too much again!";
        }
    } else {
        qDebug() << "Couldn't send any data in time! Does the port even exist??? Fucking dammit Seong!?!?!?";
    }
}

// Bool returns success (false if failed)
// TODO: copy TinyUSB values to a backup for comparison to determine availability of save btn functionality
bool guiWindow::SerialInit(int portNum)
{
    serialPort.setPort(serialFoundList[portNum]);
    serialPort.setBaudRate(QSerialPort::Baud9600);
    if(serialPort.open(QIODevice::ReadWrite)) {
        qDebug() << "Opened port successfully!";
        serialActive = true;
        // windows needs DTR enabled to actually read responses.
        serialPort.setDataTerminalReady(true);
        serialPort.write("XP");
        if(serialPort.waitForBytesWritten(2000)) {
            if(serialPort.waitForReadyRead(2000)) {
                QString buffer = serialPort.readLine();
                if(buffer.contains("OpenFIRE")) {
                    qDebug() << "OpenFIRE gun detected!";
                    buffer = serialPort.readLine();
                    buffer = buffer.trimmed();
                    board.versionNumber = buffer.toFloat();
                    qDebug() << "Version number:" << board.versionNumber;
                    buffer = serialPort.readLine();
                    board.versionCodename = buffer.trimmed();
                    qDebug() << "Version codename:" << board.versionCodename;
                    buffer = serialPort.readLine();
                    buffer = buffer.trimmed();
                    if(buffer == "rpipico") {
                        board.type = rpipico;
                    } else if(buffer == "adafruitItsyRP2040") {
                        board.type = adafruitItsyRP2040;
                    } else if(buffer == "adafruitKB2040") {
                        board.type = adafruitKB2040;
                    } else if(buffer == "arduinoNanoRP2040") {
                        board.type = arduinoNanoRP2040;
                    } else {
                        board.type = generic;
                    }
                    //qDebug() << "Selected profile number:" << buffer;
                    buffer = serialPort.readLine();
                    buffer = buffer.trimmed();
                    board.selectedProfile = buffer.toInt();
                    board.previousProfile = board.selectedProfile;
                    selectedProfile[board.selectedProfile]->setChecked(true);
                    //qDebug() << "Board type:" << buffer;
                    serialPort.write("Xln");
                    serialPort.waitForReadyRead(1000);
                    buffer = serialPort.readLine();
                    if(buffer.trimmed() == "SERIALREADERR01") {
                        tinyUSBtable.tinyUSBname = "";
                    } else {
                        tinyUSBtable.tinyUSBname = buffer.trimmed();
                    }
                    tinyUSBtable_orig.tinyUSBname = tinyUSBtable.tinyUSBname;
                    serialPort.write("Xli");
                    serialPort.waitForReadyRead(1000);
                    buffer = serialPort.readLine();
                    tinyUSBtable.tinyUSBid = buffer.trimmed();
                    tinyUSBtable_orig.tinyUSBid = tinyUSBtable.tinyUSBid;
                    SerialLoad();
                    return true;
                } else {
                    qDebug() << "Port did not respond with expected response! Seong fucked this up again.";
                    return false;
                }
            } else {
                PopupWindow("Data hasn't arrived!", "Device was detected, but initial settings request wasn't received in time!\nThis can happen if the app was unexpectedly closed and the gun is in a stale docked state.\n\nTry selecting the device again.", "Oops!", 3);
                qDebug() << "Didn't receive any data in time! Dammit Seong, you jiggled the cable too much again!";
                return false;
            }
        } else {
            qDebug() << "Couldn't send any data in time! Does the port even exist??? Fucking dammit Seong!?!?!?";
            return false;
        }
    } else {
        PopupWindow("Couldn't open port!", "This usually indicates that the port is being used by something else, e.g. Arduino IDE's serial monitor, or another command line app (stty, screen).\n\nPlease close the offending application and try selecting this port again.", "Oops!", 3);
        return false;
    }
}


void guiWindow::BoxesUpdate()
{
    for(uint8_t i = 0; i < 30; i++) {
        pinBoxes[i]->setCurrentIndex(btnUnmapped);
        pinBoxesOldIndex[i] = btnUnmapped;
    }
    if(boolSettings[customPins]) {
        currentPins.clear();
        for(uint8_t i = 0; i < 30; i++) {
            pinBoxes[i]->setEnabled(true);
            currentPins[i] = btnUnmapped;
        }
        inputsMap = inputsMap_orig;
        for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
            if(inputsMap.value(i) >= 0) {
                currentPins[inputsMap.value(i)] = i+1;
                pinBoxes[inputsMap.value(i)]->setCurrentIndex(currentPins[inputsMap.value(i)]);
                pinBoxesOldIndex[inputsMap.value(i)] = currentPins[inputsMap.value(i)];
            }
        }
        return;
    } else {
        switch(board.type) {
        case rpipico:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = rpipicoLayout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        case adafruitItsyRP2040:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = adafruitItsyRP2040Layout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        case adafruitKB2040:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = adafruitKB2040Layout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        case arduinoNanoRP2040:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = arduinoNanoRP2040Layout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        }
    }
}


void guiWindow::DiffUpdate()
{
    settingsDiff = 0;
    if(boolSettings_orig[customPins] != boolSettings[customPins]) {
        //settingsDiff++;
    }
    if(boolSettings[customPins]) {
        if(inputsMap_orig != inputsMap) {
            settingsDiff++;
        }
    }
    for(uint8_t i = 1; i < sizeof(boolSettings); i++) {
        if(boolSettings_orig[i] != boolSettings[i]) {
            settingsDiff++;
        }
    }
    for(uint8_t i = 0; i < sizeof(settingsTable) / 2; i++) {
        if(settingsTable_orig[i] != settingsTable[i]) {
            settingsDiff++;
        }
    }
    if(tinyUSBtable_orig.tinyUSBid != tinyUSBtable.tinyUSBid) {
        settingsDiff++;
    }
    if(tinyUSBtable_orig.tinyUSBname != tinyUSBtable.tinyUSBname) {
        settingsDiff++;
    }
    if(board.selectedProfile != board.previousProfile) {
        settingsDiff++;
    }
    for(uint8_t i = 0; i < 4; i++) {
        if(profilesTable_orig[i].xScale != profilesTable[i].xScale) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].yScale != profilesTable[i].yScale) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].xCenter != profilesTable[i].xCenter) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].yCenter != profilesTable[i].yCenter) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].irSensitivity != profilesTable[i].irSensitivity) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].runMode != profilesTable[i].runMode) {
            settingsDiff++;
        }
    }
    if(settingsDiff) {
        ui->confirmButton->setText("Save and Send Settings");
        ui->confirmButton->setEnabled(true);
    } else {
        ui->confirmButton->setText("[Nothing To Save]");
        ui->confirmButton->setEnabled(false);
    }
}


void guiWindow::SyncSettings()
{
    for(uint8_t i = 0; i < sizeof(boolSettings); i++) {
        boolSettings_orig[i] = boolSettings[i];
    }
    if(boolSettings_orig[customPins]) {
        inputsMap_orig = inputsMap;
    } else {
        for(uint8_t i = 0; i < INPUTS_COUNT; i++)
        inputsMap_orig[i] = -1;
    }
    for(uint8_t i = 0; i < sizeof(settingsTable) / 2; i++) {
        settingsTable_orig[i] = settingsTable[i];
    }
    tinyUSBtable_orig.tinyUSBid = tinyUSBtable.tinyUSBid;
    tinyUSBtable_orig.tinyUSBname = tinyUSBtable.tinyUSBname;
    board.previousProfile = board.selectedProfile;
    for(uint8_t i = 0; i < 4; i++) {
        profilesTable_orig[i].irSensitivity = profilesTable[i].irSensitivity;
        profilesTable_orig[i].runMode = profilesTable[i].runMode;
    }
}


QString PrettifyName()
{
    QString name;
    if(!tinyUSBtable.tinyUSBname.isEmpty()) {
        name = tinyUSBtable.tinyUSBname;
    } else {
        name = "Unnamed Device";
    }
    switch(board.type) {
    case nothing:
        name = "";
        break;
    case rpipico:
        name = name + " | Raspberry Pi Pico";
        break;
    case adafruitItsyRP2040:
        name = name + " | Adafruit ItsyBitsy RP2040";
        break;
    case adafruitKB2040:
        name = name + " | Adafruit KB2040";
        break;
    case arduinoNanoRP2040:
        name = name + " | Arduino Nano RP2040 Connect";
        break;
    case waveshareZero:
        name = name + " | Waveshare RP2040 Zero";
        break;
    case generic:
        name = name + " | Generic RP2040 Board";
        break;
    }
    return name;
}


void guiWindow::on_confirmButton_clicked()
{
    QMessageBox messageBox;
    messageBox.setText("Are these settings okay?");
    messageBox.setInformativeText("These settings will be committed to your lightgun. Is that okay?");
    messageBox.setWindowTitle("Commit Confirmation");
    messageBox.setIcon(QMessageBox::Information);
    messageBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    messageBox.setDefaultButton(QMessageBox::Yes);
    int value = messageBox.exec();
    if(value == QMessageBox::Yes) {
        if(serialPort.isOpen()) {
            serialActive = true;
            // send a signal so the gun pauses its test outputs for the save op.
            serialPort.write("Xm");
            serialPort.waitForBytesWritten(1000);

            QProgressBar *statusProgressBar = new QProgressBar();
            ui->statusBar->addPermanentWidget(statusProgressBar);
            ui->tabWidget->setEnabled(false);
            ui->comPortSelector->setEnabled(false);
            ui->confirmButton->setEnabled(false);

            QStringList serialQueue;
            for(uint8_t i = 0; i < sizeof(boolSettings); i++) {
                QString genString = QString("Xm.0.%1.%2").arg(i).arg(boolSettings[i]);
                serialQueue.append(genString);
            }

            if(boolSettings[customPins]) {
                serialQueue.append("Xm.1.0.1");
                for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
                    QString genString = QString("Xm.1.%1.%2").arg(i).arg(inputsMap.value(i));
                    serialQueue.append(genString);
                }
            }

            for(uint8_t i = 0; i < sizeof(settingsTable) / 2; i++) {
                QString genString = QString("Xm.2.%1.%2").arg(i).arg(settingsTable[i]);
                serialQueue.append(genString);
            }

            serialQueue.append(QString("Xm.3.0.%1").arg(tinyUSBtable.tinyUSBid));
            if(!tinyUSBtable.tinyUSBname.isEmpty()) {
                serialQueue.append(QString("Xm.3.1.%1").arg(tinyUSBtable.tinyUSBname));
            }
            for(uint8_t i = 0; i < 4; i++) {
                serialQueue.append(QString("Xm.P.i.%1.%2").arg(i).arg(profilesTable[i].irSensitivity));
                serialQueue.append(QString("Xm.P.r.%1.%2").arg(i).arg(profilesTable[i].runMode));
            }
            serialQueue.append("XS");

            statusProgressBar->setRange(0, serialQueue.length()-1);
            bool success = true;

            // throw out whatever's in the buffer if there's anything there.
            while(!serialPort.atEnd()) {
                serialPort.readLine();
            }

            for(uint8_t i = 0; i < serialQueue.length(); i++) {
                serialPort.write(serialQueue[i].toLocal8Bit());
                serialPort.waitForBytesWritten(2000);
                if(serialPort.waitForReadyRead(2000)) {
                    QString buffer = serialPort.readLine();
                    if(buffer.contains("OK:") || buffer.contains("NOENT:")) {
                        statusProgressBar->setValue(statusProgressBar->value() + 1);
                    } else if(i == serialQueue.length() - 1 && buffer.contains("Saving preferences...")) {
                        buffer = serialPort.readLine();
                        if(buffer.contains("Settings saved to")) {
                            success = true;
                            // because there's probably some leftover bytes that might congest things:
                            while(!serialPort.atEnd()) {
                                serialPort.readLine();
                            }
                        } else {
                            qDebug() << "Sent save command, but didn't save successfully! What the fuck happened???";
                        }
                    } else {
                        success = false;
                        break;
                    }
                }
            }
            ui->statusBar->removeWidget(statusProgressBar);
            delete statusProgressBar;
            ui->tabWidget->setEnabled(true);
            ui->comPortSelector->setEnabled(true);
            if(!success) {
                qDebug() << "Ah shit, it failed! What did you do, Seong?";
            } else {
                statusBar()->showMessage("Sent settings successfully!", 5000);
                SyncSettings();
                DiffUpdate();
                ui->boardLabel->setText(PrettifyName());
            }
            serialActive = false;
            serialQueue.clear();
            if(!serialPort.atEnd()) {
                serialPort.readAll();
            }
        } else {
            qDebug() << "Wait, this port wasn't open to begin with!!! WTF SEONG!?!?";
        }
    } else {
        statusBar()->showMessage("Save operation canceled.", 3000);
    }
}


void guiWindow::on_comPortSelector_currentIndexChanged(int index)
{
    // Indiscriminately clears the board layout views.
    // yes, every time. goddammit QT.
    // fuck it, it works until QT provides a better mechanism to remove widgets without deleting them.
    if(pinBoxes[0]->count() > 0) {
        for(uint8_t i = 0; i < 30; i++) {
            pinBoxes[i]->clear();
            delete pinBoxes[i];
            delete padding[i];
            delete pinLabel[i];
        }
        delete centerPic;
    }

    delete PinsCenter;
    delete PinsLeft;
    delete PinsRight;

    PinsCenter = new QVBoxLayout();
    PinsCenterSub = new QGridLayout();
    PinsLeft = new QGridLayout();
    PinsRight = new QGridLayout();

    ui->PinsTopHalf->addLayout(PinsLeft);
    ui->PinsTopHalf->addLayout(PinsCenter);
    ui->PinsTopHalf->addLayout(PinsRight);

    for(uint8_t i = 0; i < 30; i++) {
        pinBoxes[i] = new QComboBox();
        pinBoxes[i]->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        connect(pinBoxes[i], SIGNAL(activated(int)), this, SLOT(pinBoxes_activated(int)));
        padding[i] = new QWidget();
        padding[i]->setMinimumHeight(25);
        pinLabel[i] = new QLabel(QString("<GPIO%1>").arg(i));
        pinLabel[i]->setEnabled(false);
        pinLabel[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    if(index > 0) {
        qDebug() << "COM port set to" << ui->comPortSelector->currentIndex();
        // Clear stale states if any, and unmount old board if mounted.
        if(testMode) {
            testMode = false;
            ui->testView->setEnabled(false);
            ui->buttonsTestArea->setEnabled(true);
            ui->testBtn->setText("Enable IR Test Mode");
            ui->pinsTab->setEnabled(true);
            ui->settingsTab->setEnabled(true);
            ui->profilesTab->setEnabled(true);
            ui->feedbackTestsBox->setEnabled(true);
            ui->dangerZoneBox->setEnabled(true);
            serialActive = false;
        }
        if(serialPort.isOpen()) {
            serialActive = true;
            serialPort.write("XE");
            serialPort.waitForBytesWritten(2000);
            serialPort.waitForReadyRead(2000);
            serialPort.readAll();
            serialPort.close();
            serialActive = false;
        }
        if(!SerialInit(index - 1)) {
            ui->comPortSelector->setCurrentIndex(0);
        } else {
            ui->versionLabel->setText(QString("v%1 - \"%2\"").arg(board.versionNumber).arg(board.versionCodename));
            BoxesFill();

            switch(board.type) {
                case rpipico:
                {
                    centerPic = new QSvgWidget(":/boardPics/pico.svg");
                    QSvgRenderer *picRenderer = centerPic->renderer();
                    picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
                    ui->boardLabel->setText(PrettifyName());

                    // left side
                    PinsLeft->addWidget(padding[0],    0,  0);   // padding
                    PinsLeft->addWidget(pinBoxes[0],   1,  0), PinsLeft->addWidget(pinLabel[0],  1,  1);
                    PinsLeft->addWidget(pinBoxes[1],   2,  0), PinsLeft->addWidget(pinLabel[1],  2,  1);
                    PinsLeft->addWidget(padding[1],    3,  0);   // gnd
                    PinsLeft->addWidget(pinBoxes[2],   4,  0), PinsLeft->addWidget(pinLabel[2],  4,  1);
                    PinsLeft->addWidget(pinBoxes[3],   5,  0), PinsLeft->addWidget(pinLabel[3],  5,  1);
                    PinsLeft->addWidget(pinBoxes[4],   6,  0), PinsLeft->addWidget(pinLabel[4],  6,  1);
                    PinsLeft->addWidget(pinBoxes[5],   7,  0), PinsLeft->addWidget(pinLabel[5],  7,  1);
                    PinsLeft->addWidget(padding[2],    8,  0);   // gnd
                    PinsLeft->addWidget(pinBoxes[6],   9,  0), PinsLeft->addWidget(pinLabel[6],  9,  1);
                    PinsLeft->addWidget(pinBoxes[7],   10, 0), PinsLeft->addWidget(pinLabel[7],  10, 1);
                    PinsLeft->addWidget(pinBoxes[8],   11, 0), PinsLeft->addWidget(pinLabel[8],  11, 1);
                    PinsLeft->addWidget(pinBoxes[9],   12, 0), PinsLeft->addWidget(pinLabel[9],  12, 1);
                    PinsLeft->addWidget(padding[3],    13, 0);   // gnd
                    PinsLeft->addWidget(pinBoxes[10],  14, 0), PinsLeft->addWidget(pinLabel[10], 14, 1);
                    PinsLeft->addWidget(pinBoxes[11],  15, 0), PinsLeft->addWidget(pinLabel[11], 15, 1);
                    PinsLeft->addWidget(pinBoxes[12],  16, 0), PinsLeft->addWidget(pinLabel[12], 16, 1);
                    PinsLeft->addWidget(pinBoxes[13],  17, 0), PinsLeft->addWidget(pinLabel[13], 17, 1);
                    PinsLeft->addWidget(padding[4],    18, 0);   // gnd
                    PinsLeft->addWidget(pinBoxes[14],  19, 0), PinsLeft->addWidget(pinLabel[14], 19, 1);
                    PinsLeft->addWidget(pinBoxes[15],  20, 0), PinsLeft->addWidget(pinLabel[15], 20, 1);

                    // right side
                    PinsRight->addWidget(padding[5],   0,  1);   // padding
                    PinsRight->addWidget(padding[6],   1,  1);   // VBUS
                    PinsRight->addWidget(padding[7],   2,  1);   // VSYS
                    PinsRight->addWidget(padding[8],   3,  1);   // gnd
                    PinsRight->addWidget(padding[9],   4,  1);   // 3V3 EN
                    PinsRight->addWidget(padding[10],  5,  1);   // 3V3 OUT
                    PinsRight->addWidget(padding[11],  6,  1);   // ADC VREF
                    PinsRight->addWidget(pinBoxes[28], 7,  1), PinsRight->addWidget(pinLabel[28], 7,  0);
                    PinsRight->addWidget(padding[12],  8,  1);   // gnd
                    PinsRight->addWidget(pinBoxes[27], 9,  1), PinsRight->addWidget(pinLabel[27], 9,  0);
                    PinsRight->addWidget(pinBoxes[26], 10, 1), PinsRight->addWidget(pinLabel[26], 10, 0);
                    PinsRight->addWidget(padding[13],  11, 1);   // RUN
                    PinsRight->addWidget(pinBoxes[22], 12, 1), PinsRight->addWidget(pinLabel[22], 12, 0);
                    PinsRight->addWidget(padding[14],  13, 1);   // gnd
                    PinsRight->addWidget(pinBoxes[21], 14, 1), PinsRight->addWidget(pinLabel[21], 14, 0);
                    PinsRight->addWidget(pinBoxes[20], 15, 1), PinsRight->addWidget(pinLabel[20], 15, 0);
                    PinsRight->addWidget(pinBoxes[19], 16, 1), PinsRight->addWidget(pinLabel[19], 16, 0);
                    PinsRight->addWidget(pinBoxes[18], 17, 1), PinsRight->addWidget(pinLabel[18], 17, 0);
                    PinsRight->addWidget(padding[17],  18, 1);   // gnd
                    PinsRight->addWidget(pinBoxes[17], 19, 1), PinsRight->addWidget(pinLabel[17], 19, 0);
                    PinsRight->addWidget(pinBoxes[16], 20, 1), PinsRight->addWidget(pinLabel[16], 20, 0);

                    // center
                    PinsCenter->addWidget(centerPic);
                    centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
                    break;
                }
                case adafruitItsyRP2040:
                {
                    centerPic = new QSvgWidget(":/boardPics/adafruitItsy2040.svg");
                    QSvgRenderer *picRenderer = centerPic->renderer();
                    picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
                    ui->boardLabel->setText(PrettifyName());

                    // left side
                    PinsLeft->addWidget(padding[0],    0,  0);   // reset
                    PinsLeft->addWidget(padding[1],    1,  0);   // 3v3_1
                    PinsLeft->addWidget(padding[3],    2,  0);   // 3v3_2
                    PinsLeft->addWidget(padding[4],    3,  0);   // VHi
                    PinsLeft->addWidget(pinBoxes[26],  4,  0), PinsLeft->addWidget(pinLabel[26], 4,  1);
                    PinsLeft->addWidget(pinBoxes[27],  5,  0), PinsLeft->addWidget(pinLabel[27], 5,  1);
                    PinsLeft->addWidget(pinBoxes[28],  6,  0), PinsLeft->addWidget(pinLabel[28], 6,  1);
                    PinsLeft->addWidget(pinBoxes[29],  7,  0), PinsLeft->addWidget(pinLabel[29], 7,  1);
                    PinsLeft->addWidget(pinBoxes[24],  8,  0), PinsLeft->addWidget(pinLabel[24], 8,  1);
                    PinsLeft->addWidget(pinBoxes[25],  9,  0), PinsLeft->addWidget(pinLabel[25], 9,  1);
                    PinsLeft->addWidget(pinBoxes[18],  10, 0), PinsLeft->addWidget(pinLabel[18], 10, 1);
                    PinsLeft->addWidget(pinBoxes[19],  11, 0), PinsLeft->addWidget(pinLabel[19], 11, 1);
                    PinsLeft->addWidget(pinBoxes[20],  12, 0), PinsLeft->addWidget(pinLabel[20], 12, 1);
                    PinsLeft->addWidget(pinBoxes[12],  13, 0), PinsLeft->addWidget(pinLabel[12], 13, 1);
                    PinsLeft->addWidget(padding[5],    14, 0);   // bottom padding
                    PinsLeft->addWidget(padding[6],    14, 0);

                    // right side
                    PinsRight->addWidget(padding[8],   0,  1);   // battery
                    PinsRight->addWidget(padding[9],   1,  1);   // gnd
                    PinsRight->addWidget(padding[10],  2,  1);   // USB power in
                    PinsRight->addWidget(pinBoxes[11], 3,  1), PinsRight->addWidget(pinLabel[11], 3,  0);
                    PinsRight->addWidget(pinBoxes[10], 4,  1), PinsRight->addWidget(pinLabel[10], 4,  0);
                    PinsRight->addWidget(pinBoxes[9],  5,  1), PinsRight->addWidget(pinLabel[9],  5,  0);
                    PinsRight->addWidget(pinBoxes[8],  6,  1), PinsRight->addWidget(pinLabel[8],  6,  0);
                    PinsRight->addWidget(pinBoxes[7],  7,  1), PinsRight->addWidget(pinLabel[7],  7,  0);
                    PinsRight->addWidget(pinBoxes[6],  8,  1), PinsRight->addWidget(pinLabel[6],  8,  0);
                    PinsRight->addWidget(padding[11],  9,  1);   // 5!
                    PinsRight->addWidget(pinBoxes[2],  10, 1), PinsRight->addWidget(pinLabel[2],  10, 0);
                    PinsRight->addWidget(pinBoxes[3],  11, 1), PinsRight->addWidget(pinLabel[3],  11, 0);
                    PinsRight->addWidget(pinBoxes[0],  12, 1), PinsRight->addWidget(pinLabel[0],  12, 0);
                    PinsRight->addWidget(pinBoxes[1],  13, 1), PinsRight->addWidget(pinLabel[1],  13, 0);
                    PinsRight->addWidget(padding[12],  14, 1);   // bottom padding
                    PinsRight->addWidget(padding[13],  15, 1);

                    // center
                    PinsCenter->addWidget(centerPic);
                    PinsCenter->addLayout(PinsCenterSub);
                    centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
                    PinsCenterSub->addWidget(pinBoxes[4], 1, 3), PinsCenterSub->addWidget(pinLabel[4], 0, 3);
                    PinsCenterSub->addWidget(pinBoxes[5], 1, 2), PinsCenterSub->addWidget(pinLabel[5], 0, 2);
                    break;
                }
                case adafruitKB2040:
                {
                    centerPic = new QSvgWidget(":/boardPics/adafruitKB2040.svg");
                    QSvgRenderer *picRenderer = centerPic->renderer();
                    picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
                    ui->boardLabel->setText(PrettifyName());

                    // left side
                    PinsLeft->addWidget(padding[0],    0,  0);   // padding
                    PinsLeft->addWidget(padding[1],    1,  0);   // D+
                    PinsLeft->addWidget(pinBoxes[0],   2,  0), PinsLeft->addWidget(pinLabel[0],   2,  1);
                    PinsLeft->addWidget(pinBoxes[1],   3,  0), PinsLeft->addWidget(pinLabel[1],   3,  1);
                    PinsLeft->addWidget(padding[2],    4,  0);   // gnd
                    PinsLeft->addWidget(padding[3],    5,  0);   // gnd
                    PinsLeft->addWidget(pinBoxes[2],   6,  0), PinsLeft->addWidget(pinLabel[2],   6,  1);
                    PinsLeft->addWidget(pinBoxes[3],   7,  0), PinsLeft->addWidget(pinLabel[3],   7,  1);
                    PinsLeft->addWidget(pinBoxes[4],   8,  0), PinsLeft->addWidget(pinLabel[4],   8,  1);
                    PinsLeft->addWidget(pinBoxes[5],   9,  0), PinsLeft->addWidget(pinLabel[5],   9,  1);
                    PinsLeft->addWidget(pinBoxes[6],   10, 0), PinsLeft->addWidget(pinLabel[6],   10, 1);
                    PinsLeft->addWidget(pinBoxes[7],   11, 0), PinsLeft->addWidget(pinLabel[7],   11, 1);
                    PinsLeft->addWidget(pinBoxes[8],   12, 0), PinsLeft->addWidget(pinLabel[8],   12, 1);
                    PinsLeft->addWidget(pinBoxes[9],   13, 0), PinsLeft->addWidget(pinLabel[9],   13, 1);

                    // right side
                    PinsRight->addWidget(padding[4],   0,  1);   // padding
                    PinsRight->addWidget(padding[5],   1,  1);   // D-
                    PinsRight->addWidget(padding[6],   2,  1);   // RAW
                    PinsRight->addWidget(padding[7],   3,  1);   // gnd
                    PinsRight->addWidget(padding[8],   4,  1);   // reset
                    PinsRight->addWidget(padding[9],   5,  1);   // 3.3v
                    PinsRight->addWidget(pinBoxes[29], 6,  1), PinsRight->addWidget(pinLabel[29], 6,  0);
                    PinsRight->addWidget(pinBoxes[28], 7,  1), PinsRight->addWidget(pinLabel[28], 7,  0);
                    PinsRight->addWidget(pinBoxes[27], 8,  1), PinsRight->addWidget(pinLabel[27], 8,  0);
                    PinsRight->addWidget(pinBoxes[26], 9,  1), PinsRight->addWidget(pinLabel[26], 9,  0);
                    PinsRight->addWidget(pinBoxes[18], 10, 1), PinsRight->addWidget(pinLabel[18], 10, 0);
                    PinsRight->addWidget(pinBoxes[20], 11, 1), PinsRight->addWidget(pinLabel[20], 11, 0);
                    PinsRight->addWidget(pinBoxes[19], 12, 1), PinsRight->addWidget(pinLabel[19], 12, 0);
                    PinsRight->addWidget(pinBoxes[10], 13, 1), PinsRight->addWidget(pinLabel[10], 13, 0);

                    // center
                    PinsCenter->addWidget(centerPic);
                    centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
                    break;
                }
                case arduinoNanoRP2040:
                {
                    centerPic = new QSvgWidget(":/boardPics/arduinoNano2040.svg");
                    QSvgRenderer *picRenderer = centerPic->renderer();
                    picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
                    PinsCenter->addWidget(centerPic);
                    ui->boardLabel->setText(PrettifyName());

                    // left side
                    PinsLeft->addWidget(padding[0],    0,  0);   // top padding
                    PinsLeft->addWidget(padding[1],    1,  0);
                    PinsLeft->addWidget(padding[2],    2,  0);
                    PinsLeft->addWidget(pinBoxes[6],   3,  0), PinsLeft->addWidget(pinLabel[6],   3,  1);
                    PinsLeft->addWidget(padding[3],    4,  0);   // 3V3 Out
                    PinsLeft->addWidget(padding[4],    5,  0);   // AREF
                    PinsLeft->addWidget(pinBoxes[26],  6,  0), PinsLeft->addWidget(pinLabel[26],  6,  1);
                    PinsLeft->addWidget(pinBoxes[27],  7,  0), PinsLeft->addWidget(pinLabel[27],  7,  1);
                    PinsLeft->addWidget(pinBoxes[28],  8,  0), PinsLeft->addWidget(pinLabel[28],  8,  1);
                    PinsLeft->addWidget(pinBoxes[29],  9,  0), PinsLeft->addWidget(pinLabel[29],  9,  1);
                    PinsLeft->addWidget(pinBoxes[12],  10, 0), PinsLeft->addWidget(pinLabel[12],  10, 1);
                    PinsLeft->addWidget(pinBoxes[13],  11, 0), PinsLeft->addWidget(pinLabel[13],  11, 1);
                    PinsLeft->addWidget(padding[5],    12, 0);   // A6 - unused
                    PinsLeft->addWidget(padding[6],    13, 0);   // A7 - unused
                    PinsLeft->addWidget(padding[7],    14, 0);   // 5V OUT
                    PinsLeft->addWidget(padding[8],    15, 0);   // REC?
                    PinsLeft->addWidget(padding[9],    16, 0);   // gnd
                    PinsLeft->addWidget(padding[10],   17, 0);   // 5V IN
                    PinsLeft->addWidget(padding[11],   18, 0);   // bottom padding
                    PinsLeft->addWidget(padding[12],   19, 0);

                    // right side
                    PinsRight->addWidget(padding[13],  0,  1);   // top padding
                    PinsRight->addWidget(padding[14],  1,  1);   // top padding
                    PinsRight->addWidget(padding[15],  2,  1);   // top padding
                    PinsRight->addWidget(pinBoxes[4],  3,  1), PinsRight->addWidget(pinLabel[4],  3,  0);
                    PinsRight->addWidget(pinBoxes[7],  4,  1), PinsRight->addWidget(pinLabel[7],  4,  0);
                    PinsRight->addWidget(pinBoxes[5],  5,  1), PinsRight->addWidget(pinLabel[5],  5,  0);
                    PinsRight->addWidget(pinBoxes[21], 6,  1), PinsRight->addWidget(pinLabel[21], 6,  0);
                    PinsRight->addWidget(pinBoxes[20], 7,  1), PinsRight->addWidget(pinLabel[20], 7,  0);
                    PinsRight->addWidget(pinBoxes[19], 8,  1), PinsRight->addWidget(pinLabel[19], 8,  0);
                    PinsRight->addWidget(pinBoxes[18], 9,  1), PinsRight->addWidget(pinLabel[18], 9,  0);
                    PinsRight->addWidget(pinBoxes[17], 10, 1), PinsRight->addWidget(pinLabel[17], 10, 0);
                    PinsRight->addWidget(pinBoxes[16], 11, 1), PinsRight->addWidget(pinLabel[16], 11, 0);
                    PinsRight->addWidget(pinBoxes[15], 12, 1), PinsRight->addWidget(pinLabel[15], 12, 0);
                    PinsRight->addWidget(pinBoxes[25], 13, 1), PinsRight->addWidget(pinLabel[25], 13, 0);
                    PinsRight->addWidget(padding[16],  14, 1);   // gnd
                    PinsRight->addWidget(padding[17],  15, 1);   // RESET
                    PinsRight->addWidget(pinBoxes[1],  16, 1), PinsRight->addWidget(pinLabel[1],  16, 0);
                    PinsRight->addWidget(pinBoxes[0],  17, 1), PinsRight->addWidget(pinLabel[0],  17, 0);
                    PinsRight->addWidget(padding[18],  18, 1);   // bottom padding
                    PinsRight->addWidget(padding[19],  19, 1);

                    // center
                    PinsCenter->addWidget(centerPic);
                    break;
                }
                case waveshareZero:
                {
                    centerPic = new QSvgWidget(":/boardPics/waveshareZero.svg");
                    QSvgRenderer *picRenderer = centerPic->renderer();
                    picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
                    ui->boardLabel->setText(PrettifyName());

                    // left side
                    PinsLeft->addWidget(padding[0],   0,  0);    // 5V OUT
                    PinsLeft->addWidget(padding[1],   1,  0);    // gnd
                    PinsLeft->addWidget(padding[2],   2,  0);    // 3V3 OUT
                    PinsLeft->addWidget(pinBoxes[29], 3,  0),  PinsLeft->addWidget(pinLabel[29], 3,  1);
                    PinsLeft->addWidget(pinBoxes[28], 4,  0),  PinsLeft->addWidget(pinLabel[28], 4,  1);
                    PinsLeft->addWidget(pinBoxes[27], 5,  0),  PinsLeft->addWidget(pinLabel[27], 5,  1);
                    PinsLeft->addWidget(pinBoxes[26], 6,  0),  PinsLeft->addWidget(pinLabel[26], 6,  1);
                    PinsLeft->addWidget(pinBoxes[15], 7,  0),  PinsLeft->addWidget(pinLabel[15], 7,  1);
                    PinsLeft->addWidget(pinBoxes[14], 8,  0),  PinsLeft->addWidget(pinLabel[14], 8,  1);
                    PinsLeft->addWidget(pinBoxes[13], 9,  0),  PinsLeft->addWidget(pinLabel[13], 9,  1);
                    PinsLeft->addWidget(pinBoxes[12], 10, 0),  PinsLeft->addWidget(pinLabel[12], 10, 1);

                    // right side
                    PinsRight->addWidget(padding[3],  0,  1);    // padding
                    PinsRight->addWidget(pinBoxes[0], 1,  1),  PinsRight->addWidget(pinLabel[0], 1,  0);
                    PinsRight->addWidget(pinBoxes[1], 2,  1),  PinsRight->addWidget(pinLabel[1], 2,  0);
                    PinsRight->addWidget(pinBoxes[2], 3,  1),  PinsRight->addWidget(pinLabel[2], 3,  0);
                    PinsRight->addWidget(pinBoxes[3], 4,  1),  PinsRight->addWidget(pinLabel[3], 4,  0);
                    PinsRight->addWidget(pinBoxes[4], 5,  1),  PinsRight->addWidget(pinLabel[4], 5,  0);
                    PinsRight->addWidget(pinBoxes[5], 6,  1),  PinsRight->addWidget(pinLabel[5], 6,  0);
                    PinsRight->addWidget(pinBoxes[6], 7,  1),  PinsRight->addWidget(pinLabel[6], 7,  0);
                    PinsRight->addWidget(pinBoxes[7], 8,  1),  PinsRight->addWidget(pinLabel[7], 8,  0);
                    PinsRight->addWidget(pinBoxes[8], 9,  1),  PinsRight->addWidget(pinLabel[8], 9,  0);
                    PinsRight->addWidget(pinBoxes[9], 10, 1),  PinsRight->addWidget(pinLabel[9], 10, 0);

                    // center
                    PinsCenter->addWidget(centerPic);
                    PinsCenter->addLayout(PinsCenterSub);
                    centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
                    PinsCenterSub->addWidget(pinBoxes[10], 1, 3), PinsCenterSub->addWidget(pinLabel[10], 0, 3);
                    PinsCenterSub->addWidget(pinBoxes[11], 1, 2), PinsCenterSub->addWidget(pinLabel[11], 0, 2);
                    break;
                }
                case generic:
                {
                    centerPic = new QSvgWidget(":/boardPics/unknown.svg");
                    QSvgRenderer *picRenderer = centerPic->renderer();
                    picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
                    ui->boardLabel->setText(PrettifyName());

                    // left side
                    PinsLeft->addWidget(padding[0],    0,  0);   // padding
                    PinsLeft->addWidget(pinBoxes[0],   1,  0), PinsLeft->addWidget(pinLabel[0],  1,  1);
                    PinsLeft->addWidget(pinBoxes[1],   2,  0), PinsLeft->addWidget(pinLabel[1],  2,  1);
                    PinsLeft->addWidget(padding[1],    3,  0);   // gnd
                    PinsLeft->addWidget(pinBoxes[2],   4,  0), PinsLeft->addWidget(pinLabel[2],  4,  1);
                    PinsLeft->addWidget(pinBoxes[3],   5,  0), PinsLeft->addWidget(pinLabel[3],  5,  1);
                    PinsLeft->addWidget(pinBoxes[4],   6,  0), PinsLeft->addWidget(pinLabel[4],  6,  1);
                    PinsLeft->addWidget(pinBoxes[5],   7,  0), PinsLeft->addWidget(pinLabel[5],  7,  1);
                    PinsLeft->addWidget(padding[2],    8,  0);   // gnd
                    PinsLeft->addWidget(pinBoxes[6],   9,  0), PinsLeft->addWidget(pinLabel[6],  9,  1);
                    PinsLeft->addWidget(pinBoxes[7],   10, 0), PinsLeft->addWidget(pinLabel[7],  10, 1);
                    PinsLeft->addWidget(pinBoxes[8],   11, 0), PinsLeft->addWidget(pinLabel[8],  11, 1);
                    PinsLeft->addWidget(pinBoxes[9],   12, 0), PinsLeft->addWidget(pinLabel[9],  12, 1);
                    PinsLeft->addWidget(padding[3],    13, 0);   // gnd
                    PinsLeft->addWidget(pinBoxes[10],  14, 0), PinsLeft->addWidget(pinLabel[10], 14, 1);
                    PinsLeft->addWidget(pinBoxes[11],  15, 0), PinsLeft->addWidget(pinLabel[11], 15, 1);
                    PinsLeft->addWidget(pinBoxes[12],  16, 0), PinsLeft->addWidget(pinLabel[12], 16, 1);
                    PinsLeft->addWidget(pinBoxes[13],  17, 0), PinsLeft->addWidget(pinLabel[13], 17, 1);
                    PinsLeft->addWidget(padding[4],    18, 0);   // gnd
                    PinsLeft->addWidget(pinBoxes[14],  19, 0), PinsLeft->addWidget(pinLabel[14], 19, 1);
                    PinsLeft->addWidget(pinBoxes[15],  20, 0), PinsLeft->addWidget(pinLabel[15], 20, 1);

                    // right side
                    PinsRight->addWidget(padding[5],   0,  1);   // padding
                    PinsRight->addWidget(padding[6],   1,  1);
                    PinsRight->addWidget(padding[7],   2,  1);
                    PinsRight->addWidget(padding[8],   3,  1);   // gnd
                    PinsRight->addWidget(padding[9],   4,  1);
                    PinsRight->addWidget(padding[10],  5,  1);
                    PinsRight->addWidget(padding[11],  6,  1);
                    PinsRight->addWidget(pinBoxes[28], 7,  1), PinsRight->addWidget(pinLabel[28], 7,  0);
                    PinsRight->addWidget(padding[12],  8,  1);   // gnd
                    PinsRight->addWidget(pinBoxes[27], 9,  1), PinsRight->addWidget(pinLabel[27], 9,  0);
                    PinsRight->addWidget(pinBoxes[26], 10, 1), PinsRight->addWidget(pinLabel[26], 10, 0);
                    PinsRight->addWidget(padding[13],  11, 1);
                    PinsRight->addWidget(pinBoxes[22], 12, 1), PinsRight->addWidget(pinLabel[22], 12, 0);
                    PinsRight->addWidget(padding[14],  13, 1);   // gnd
                    PinsRight->addWidget(pinBoxes[21], 14, 1), PinsRight->addWidget(pinLabel[21], 14, 0);
                    PinsRight->addWidget(pinBoxes[20], 15, 1), PinsRight->addWidget(pinLabel[20], 15, 0);
                    PinsRight->addWidget(pinBoxes[19], 16, 1), PinsRight->addWidget(pinLabel[19], 16, 0);
                    PinsRight->addWidget(pinBoxes[18], 17, 1), PinsRight->addWidget(pinLabel[18], 17, 0);
                    PinsRight->addWidget(padding[17],  18, 1);   // gnd
                    PinsRight->addWidget(pinBoxes[17], 19, 1), PinsRight->addWidget(pinLabel[17], 19, 0);
                    PinsRight->addWidget(pinBoxes[16], 20, 1), PinsRight->addWidget(pinLabel[16], 20, 0);

                    // center
                    PinsCenter->addWidget(centerPic);
                    centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
                    break;
                }
            }

            ui->tabWidget->setEnabled(true);
            ui->customPinsEnabled->setChecked(boolSettings[customPins]);

            ui->rumbleToggle->setChecked(boolSettings[rumble]);
            ui->solenoidToggle->setChecked(boolSettings[solenoid]);
            ui->autofireToggle->setChecked(boolSettings[autofire]);
            ui->simplePauseToggle->setChecked(boolSettings[simplePause]);
            ui->holdToPauseToggle->setChecked(boolSettings[holdToPause]);
            ui->commonAnodeToggle->setChecked(boolSettings[commonAnode]);
            ui->lowButtonsToggle->setChecked(boolSettings[lowButtonsMode]);
            ui->rumbleIntensityBox->setValue(settingsTable[rumbleStrength]);
            ui->rumbleLengthBox->setValue(settingsTable[rumbleInterval]);
            ui->holdToPauseLengthBox->setValue(settingsTable[holdToPauseLength]);
            ui->neopixelStrandLengthBox->setValue(settingsTable[customLEDcount]);
            ui->solenoidNormalIntervalBox->setValue(settingsTable[solenoidNormalInterval]);
            ui->solenoidFastIntervalBox->setValue(settingsTable[solenoidFastInterval]);
            ui->solenoidHoldLengthBox->setValue(settingsTable[solenoidHoldLength]);
            ui->autofireWaitFactorBox->setValue(settingsTable[autofireWaitFactor]);
            ui->productIdInput->setText(tinyUSBtable.tinyUSBid);
            ui->productNameInput->setText(tinyUSBtable.tinyUSBname);
        }
    } else {
        ui->boardLabel->clear();
        ui->versionLabel->clear();

        if(serialPort.isOpen()) {
            serialActive = true;
            serialPort.write("XE");
            serialPort.waitForBytesWritten(2000);
            serialPort.waitForReadyRead(2000);
            serialPort.readAll();
            serialPort.close();
            if(testMode) {
                testMode = false;
                ui->testView->setEnabled(false);
                ui->buttonsTestArea->setEnabled(true);
                ui->testBtn->setText("Enable IR Test Mode");
                ui->pinsTab->setEnabled(true);
                ui->settingsTab->setEnabled(true);
                ui->profilesTab->setEnabled(true);
                ui->feedbackTestsBox->setEnabled(true);
                ui->dangerZoneBox->setEnabled(true);
                serialActive = false;
            }
            serialActive = false;
        }
        qDebug() << "COM port disabled!";
        ui->tabWidget->setEnabled(false);
    }
}

void guiWindow::BoxesFill()
{
    // update box types
    for(uint8_t i = 0; i < 30; i++) {
        pinBoxes[i]->addItems(valuesNameList);
        // clear out analog options for digital pins (< GPIO26)
        if(i < 26) {
            pinBoxes[i]->removeItem(tempPin);
            pinBoxes[i]->removeItem(analogY);
            pinBoxes[i]->removeItem(analogX);
        }
        // filter out SCL/SDA if possible.
        if(i & 1) {
            pinBoxes[i]->removeItem(camSDA);
            pinBoxes[i]->insertSeparator(camSDA);
            pinBoxes[i]->removeItem(periphSDA);
            pinBoxes[i]->insertSeparator(periphSDA);
        } else {
            pinBoxes[i]->removeItem(camSCL);
            pinBoxes[i]->insertSeparator(camSCL);
            pinBoxes[i]->removeItem(periphSCL);
            pinBoxes[i]->insertSeparator(periphSCL);
        }
    }
    BoxesUpdate();
}

void guiWindow::pinBoxes_activated(int index)
{
    // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
    uint8_t pin;
    QObject* obj = sender();
    for(uint8_t i = 0;;i++) {
        if(obj == pinBoxes[i]) {
                pin = i;
                break;
            }
    }

    if(!index) {
        inputsMap[currentPins.value(pin) - 1] = -1;
        currentPins[pin] = btnUnmapped;
    } else if(pinBoxesOldIndex[pin] != index) {
        int8_t btnRequest = index - 1;

        // Scorched Earth approach, clear anything that matches to unmapped.
        inputsMap[btnRequest] = -1;
        // only reset if current pin was already mapped.
        if(currentPins.value(pin) > 0) {
            inputsMap[currentPins.value(pin) - 1] = -1;
        }
        QList<uint8_t> foundList = currentPins.keys(index);
        for(uint8_t i = 0; i < foundList.length(); i++) {
            currentPins[foundList[i]] = btnUnmapped;
            pinBoxes[foundList[i]]->setCurrentIndex(btnUnmapped);
            pinBoxesOldIndex[foundList[i]] = btnUnmapped;
        }
        // Then map the thing.
        currentPins[pin] = index;
        inputsMap[btnRequest] = pin;
    }
    // because "->currentIndex" is already updated, we just update it at the end of activations
    // to check that we aren't re-selecting the index for that box.
    pinBoxesOldIndex[pin] = index;
    DiffUpdate();
}

void guiWindow::irBoxes_activated(int index)
{
    // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
    uint8_t slot;
    QObject* obj = sender();
    for(uint8_t i = 0;;i++) {
        if(obj == irSens[i]) {
            slot = i;
            break;
        }
    }

    if(index != irSensOldIndex[slot]) {
        profilesTable[slot].irSensitivity = index;
    }
    irSensOldIndex[slot] = index;
    DiffUpdate();
}


void guiWindow::runModeBoxes_activated(int index)
{
    // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
    uint8_t slot;
    QObject* obj = sender();
    for(uint8_t i = 0;;i++) {
        if(obj == runMode[i]) {
            slot = i;
            break;
        }
    }

    if(index != runModeOldIndex[slot]) {
        profilesTable[slot].runMode = index;
    }
    runModeOldIndex[slot] = index;
    DiffUpdate();
}


void guiWindow::on_customPinsEnabled_stateChanged(int arg1)
{
    boolSettings[customPins] = arg1;
    BoxesUpdate();
    DiffUpdate();
}


void guiWindow::on_rumbleToggle_stateChanged(int arg1)
{
    boolSettings[rumble] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidToggle_stateChanged(int arg1)
{
    boolSettings[solenoid] = arg1;
    DiffUpdate();
}


void guiWindow::on_autofireToggle_stateChanged(int arg1)
{
    boolSettings[autofire] = arg1;
    DiffUpdate();
}


void guiWindow::on_simplePauseToggle_stateChanged(int arg1)
{
    boolSettings[simplePause] = arg1;
    DiffUpdate();
}


void guiWindow::on_holdToPauseToggle_stateChanged(int arg1)
{
    boolSettings[holdToPause] = arg1;
    DiffUpdate();
}


void guiWindow::on_commonAnodeToggle_stateChanged(int arg1)
{
    boolSettings[commonAnode] = arg1;
    DiffUpdate();
}


void guiWindow::on_lowButtonsToggle_stateChanged(int arg1)
{
    boolSettings[lowButtonsMode] = arg1;
    DiffUpdate();
}


void guiWindow::on_rumbleIntensityBox_valueChanged(int arg1)
{
    settingsTable[rumbleStrength] = arg1;
    DiffUpdate();
}


void guiWindow::on_rumbleLengthBox_valueChanged(int arg1)
{
    settingsTable[rumbleInterval] = arg1;
    DiffUpdate();
}


void guiWindow::on_holdToPauseLengthBox_valueChanged(int arg1)
{
    settingsTable[holdToPauseLength] = arg1;
    DiffUpdate();
}


void guiWindow::on_neopixelStrandLengthBox_valueChanged(int arg1)
{
    settingsTable[customLEDcount] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidNormalIntervalBox_valueChanged(int arg1)
{
    settingsTable[solenoidNormalInterval] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidFastIntervalBox_valueChanged(int arg1)
{
    settingsTable[solenoidFastInterval] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidHoldLengthBox_valueChanged(int arg1)
{
    settingsTable[solenoidHoldLength] = arg1;
    DiffUpdate();
}


void guiWindow::on_autofireWaitFactorBox_valueChanged(int arg1)
{
    settingsTable[autofireWaitFactor] = arg1;
    DiffUpdate();
}

// decimal-to-hex conversion
void guiWindow::on_productIdInput_textChanged(const QString &arg1)
{
    qint32  iTest = arg1.toInt(NULL, 10);
    QString hex;

    if(iTest >= INT8_MIN && iTest <= INT8_MAX){
            hex = QString("%1").arg(iTest & 0xFF, 2, 16);
    } else if(iTest >= INT16_MIN && iTest <= INT16_MAX){
            hex = QString("%1").arg(iTest & 0xFFFF, 4, 16);
    } else {
            hex = QString("%1").arg(iTest, 8, 16);
    }
    ui->productIdConverted->setText(hex);
}


void guiWindow::on_productIdInput_textEdited(const QString &arg1)
{
    tinyUSBtable.tinyUSBid = arg1;
    DiffUpdate();
}


void guiWindow::on_productNameInput_textEdited(const QString &arg1)
{
    // TODO: try this:
    // when this is called, iterate through arg1 by length of itself in a loop
    // and check if each character of i index is > 255.
    // if so, revert to last good name.
    // else, make this input the last good name and allow committing.
    tinyUSBtable.tinyUSBname = arg1;
    DiffUpdate();
}


void guiWindow::selectedProfile_isChecked(bool isChecked)
{
    // apparently we get two signals at once? So just filter for the on.
    if(isChecked && !serialActive) {
        // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
        uint8_t slot;
        QObject* obj = sender();
        for(uint8_t i = 0;;i++) {
            if(obj == selectedProfile[i]) {
                slot = i;
                break;
            }
        }
        if(slot != board.selectedProfile) {
            serialPort.write(QString("XC%1").arg(slot+1).toLocal8Bit());
            board.selectedProfile = slot;
            DiffUpdate();
        }
    }
}


void guiWindow::on_calib1Btn_clicked()
{
    serialPort.write("XC1C");
    if(serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Calibrating Profile 1.", "Aim the gun at the cursor and pull the trigger to set center.\nAdjust the X & Y scales with Buttons A & B, and pull the trigger to confirm.\n\nOnce the scales are set, you'll be able to test the new settings. Press the trigger button once more to confirm.", "Calibration", 2);
    }
}


void guiWindow::on_calib2Btn_clicked()
{
    serialPort.write("XC2C");
    if(serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Calibrating Profile 2.", "Aim the gun at the cursor and pull the trigger to set center.\nAdjust the X & Y scales with Buttons A & B, and pull the trigger to confirm.\n\nOnce the scales are set, you'll be able to test the new settings. Press the trigger button once more to confirm.", "Calibration", 2);
    }
}


void guiWindow::on_calib3Btn_clicked()
{
    serialPort.write("XC3C");
    if(serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Calibrating Profile 3.", "Aim the gun at the cursor and pull the trigger to set center.\nAdjust the X & Y scales with Buttons A & B, and pull the trigger to confirm.\n\nOnce the scales are set, you'll be able to test the new settings. Press the trigger button once more to confirm.", "Calibration", 2);
    }
}


void guiWindow::on_calib4Btn_clicked()
{
    serialPort.write("XC4C");
    if(serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Calibrating Profile 4.", "Aim the gun at the cursor and pull the trigger to set center.\nAdjust the X & Y scales with Buttons A & B, and pull the trigger to confirm.\n\nOnce the scales are set, you'll be able to test the new settings. Press the trigger button once more to confirm.", "Calibration", 2);
    }
}

// WARNING: make sure "serialActive" is set ON for important operations, or this will eat the fucker
void guiWindow::serialPort_readyRead()
{
    if(!serialActive) {
        while(!serialPort.atEnd()) {
            QString idleBuffer = serialPort.readLine();
            if(idleBuffer.contains("Pressed:")) {
                idleBuffer = idleBuffer.right(4);
                idleBuffer = idleBuffer.trimmed();
                uint8_t button = idleBuffer.toInt();
                switch(button) {
                case btnTrigger:
                    ui->btnTriggerLabel->setText("<font color=#FF0000>Trigger</font>");
                    break;
                case btnGunA:
                    ui->btnALabel->setText("<font color=#FF0000>Button A</font>");
                    break;
                case btnGunB:
                    ui->btnBLabel->setText("<font color=#FF0000>Button B</font>");
                    break;
                case btnGunC:
                    ui->btnCLabel->setText("<font color=#FF0000>Button C</font>");
                    break;
                case btnStart:
                    ui->btnStartLabel->setText("<font color=#FF0000>Start</font>");
                    break;
                case btnSelect:
                    ui->btnSelectLabel->setText("<font color=#FF0000>Select</font>");
                    break;
                case btnGunUp:
                    ui->btnGunUpLabel->setText("<font color=#FF0000>Up</font>");
                    break;
                case btnGunDown:
                    ui->btnGunDownLabel->setText("<font color=#FF0000>Down</font>");
                    break;
                case btnGunLeft:
                    ui->btnGunLeftLabel->setText("<font color=#FF0000>Left</font>");
                    break;
                case btnGunRight:
                    ui->btnGunRightLabel->setText("<font color=#FF0000>Right</font>");
                    break;
                case btnPedal:
                    ui->btnPedalLabel->setText("<font color=#FF0000>Pedal</font>");
                    break;
                case btnHome:
                    ui->btnHomeLabel->setText("<font color=#FF0000>Home</font>");
                    break;
                case btnPump:
                    ui->btnPumpLabel->setText("<font color=#FF0000>Pump Action</font>");
                    break;
                }
            } else if(idleBuffer.contains("Released:")) {
                idleBuffer = idleBuffer.right(4);
                idleBuffer = idleBuffer.trimmed();
                uint8_t button = idleBuffer.toInt();
                switch(button) {
                case btnTrigger:
                    ui->btnTriggerLabel->setText("Trigger");
                    break;
                case btnGunA:
                    ui->btnALabel->setText("Button A");
                    break;
                case btnGunB:
                    ui->btnBLabel->setText("Button B");
                    break;
                case btnGunC:
                    ui->btnCLabel->setText("Button C");
                    break;
                case btnStart:
                    ui->btnStartLabel->setText("Start");
                    break;
                case btnSelect:
                    ui->btnSelectLabel->setText("Select");
                    break;
                case btnGunUp:
                    ui->btnGunUpLabel->setText("Up");
                    break;
                case btnGunDown:
                    ui->btnGunDownLabel->setText("Down");
                    break;
                case btnGunLeft:
                    ui->btnGunLeftLabel->setText("Left");
                    break;
                case btnGunRight:
                    ui->btnGunRightLabel->setText("Right");
                    break;
                case btnPedal:
                    ui->btnPedalLabel->setText("Pedal");
                    break;
                case btnHome:
                    ui->btnHomeLabel->setText("Home");
                    break;
                case btnPump:
                    ui->btnPumpLabel->setText("Pump Action");
                    break;
                }
            } else if(idleBuffer.contains("Profile: ")) {
                idleBuffer = idleBuffer.right(3);
                idleBuffer = idleBuffer.trimmed();
                uint8_t selection = idleBuffer.toInt();
                if(selection != board.selectedProfile) {
                    board.selectedProfile = selection;
                    selectedProfile[selection]->setChecked(true);
                }
                DiffUpdate();
            } else if(idleBuffer.contains("UpdatedProf: ")) {
                idleBuffer = idleBuffer.right(3);
                idleBuffer = idleBuffer.trimmed();
                uint8_t selection = idleBuffer.toInt();
                if(selection != board.selectedProfile) {
                    selectedProfile[selection]->setChecked(true);
                }
                board.selectedProfile = selection;
                idleBuffer = serialPort.readLine();
                xScale[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].xScale = xScale[selection]->text().toInt();
                idleBuffer = serialPort.readLine();
                yScale[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].yScale = yScale[selection]->text().toInt();
                idleBuffer = serialPort.readLine();
                xCenter[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].xCenter = xCenter[selection]->text().toInt();
                idleBuffer = serialPort.readLine();
                yCenter[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].yCenter = yCenter[selection]->text().toInt();
                DiffUpdate();
            }
        }
    } else if(testMode) {
        QString testBuffer = serialPort.readLine();
        if(testBuffer.contains(',')) {
            QStringList coordsList = testBuffer.remove("\r\n").split(',', Qt::SkipEmptyParts);

            testPointTL.setRect(coordsList[0].toInt()-25, coordsList[1].toInt()-25, 50, 50);
            testPointTR.setRect(coordsList[2].toInt()-25, coordsList[3].toInt()-25, 50, 50);
            testPointBL.setRect(coordsList[4].toInt()-25, coordsList[5].toInt()-25, 50, 50);
            testPointBR.setRect(coordsList[6].toInt()-25, coordsList[7].toInt()-25, 50, 50);
            testPointMed.setRect(coordsList[8].toInt()-25,coordsList[9].toInt()-25, 50, 50);
            testPointD.setRect(coordsList[10].toInt()-25, coordsList[11].toInt()-25, 50, 50);

            QPolygonF poly;
            poly << QPointF(coordsList[0].toInt(), coordsList[1].toInt()) << QPointF(coordsList[2].toInt(), coordsList[3].toInt()) << QPointF(coordsList[6].toInt(), coordsList[7].toInt()) << QPointF(coordsList[4].toInt(), coordsList[5].toInt()) << QPointF(coordsList[0].toInt(), coordsList[1].toInt());
            testBox.setPolygon(poly);
        }
    }
}


void guiWindow::on_rumbleTestBtn_clicked()
{
    serialPort.write("Xtr");
    if(!serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Lost connection!", "Somehow this happened I guess???", "Oops!", 4);
    } else {
        ui->statusBar->showMessage("Sent a rumble test pulse.", 2500);
    }
}


void guiWindow::on_solenoidTestBtn_clicked()
{
    serialPort.write("Xts");
    if(!serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Lost connection!", "Somehow this happened I guess???", "Oops!", 4);
    } else {
        ui->statusBar->showMessage("Sent a solenoid test pulse.", 2500);
    }
}


void guiWindow::on_testBtn_clicked()
{
    if(serialPort.isOpen()) {
        // Pre-emptively put a sock in the readyRead signal
        serialActive = true;
        serialPort.write("XT");
        serialPort.waitForBytesWritten(1000);
        serialPort.waitForReadyRead(1000);
        if(serialPort.readLine().trimmed() == "Entering Test Mode...") {
            testMode = true;
            ui->testView->setEnabled(true);
            ui->buttonsTestArea->setEnabled(false);
            ui->testBtn->setText("Disable IR Test Mode");
            ui->confirmButton->setEnabled(false);
            ui->confirmButton->setText("[Disabled while in Test Mode]");
            ui->pinsTab->setEnabled(false);
            ui->settingsTab->setEnabled(false);
            ui->profilesTab->setEnabled(false);
            ui->feedbackTestsBox->setEnabled(false);
            ui->dangerZoneBox->setEnabled(false);
        } else {
            testMode = false;
            ui->testView->setEnabled(false);
            ui->buttonsTestArea->setEnabled(true);
            ui->testBtn->setText("Enable IR Test Mode");
            ui->pinsTab->setEnabled(true);
            ui->settingsTab->setEnabled(true);
            ui->profilesTab->setEnabled(true);
            ui->feedbackTestsBox->setEnabled(true);
            ui->dangerZoneBox->setEnabled(true);
            DiffUpdate();
            serialActive = false;
        }
    }
}


void guiWindow::on_clearEepromBtn_clicked()
{
    QMessageBox messageBox;
    messageBox.setText("Really delete saved data?");
    messageBox.setInformativeText("This operation will delete all saved data, including:\n\n - Calibration Profiles\n - Toggles\n - Settings\n - Custom Identifiers\n\nAre you sure about this?");
    messageBox.setWindowTitle("Delete Confirmation");
    messageBox.setIcon(QMessageBox::Warning);
    messageBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    messageBox.setDefaultButton(QMessageBox::Yes);
    int value = messageBox.exec();
    if(value == QMessageBox::Yes) {
        if(serialPort.isOpen()) {
            serialActive = true;
            // clear the buffer if anything's been sent.
            while(!serialPort.atEnd()) {
                serialPort.readLine();
            }
            serialPort.write("Xc");
            serialPort.waitForBytesWritten(2000);
            if(serialPort.waitForReadyRead(5000)) {
                QString buffer = serialPort.readLine();
                if(buffer.trimmed() == "Cleared! Please reset the board.") {
                    serialPort.write("XE");
                    serialPort.waitForBytesWritten(2000);
                    serialPort.close();
                    serialActive = false;
                    ui->comPortSelector->setCurrentIndex(0);
                    PopupWindow("Cleared storage.", "Please unplug the board and reinsert it into the PC.", "Clear Finished", 1);
                }
            }
        }
    } else {
        //qDebug() << "Clear operation canceled.";
        ui->statusBar->showMessage("Clear operation canceled.", 3000);
    }
}


void guiWindow::on_baudResetBtn_clicked()
{
    // TODO: Does not work for now, for some reason.
    // Seems to be a QT bug? This is nearly identical to Earle's code.
    qDebug() << "Sending reset command.";
    serialActive = true;
    serialPort.close();
    // DIRTY HACK: just directly call OS-level apps to do this for us.
    #ifdef Q_OS_UNIX
    // stty does this in a neat one-liner and is standard on *nixes
    QProcess *externalProg = new QProcess;
    QStringList args;
    args << "-F" << QString("%1").arg(serialFoundList[ui->comPortSelector->currentIndex()-1].systemLocation()) << "1200";
    externalProg->start("/usr/bin/stty", args);
    // At least on my system, the Bootloader device takes ~7s to appear
    QThread::msleep(7000);
    // Class-ify this function, maybe.
    QString picoPath;
    foreach(const QStorageInfo &storageDevices, QStorageInfo::mountedVolumes()) {
        if(storageDevices.isValid() && storageDevices.isReady() && storageDevices.displayName() == "RPI-RP2") {
            picoPath = storageDevices.device();
            qDebug() << "Found a Pico bootloader!";
            break;
        } else {
            qDebug() << "nope";
        }
    }
    qDebug() << picoPath;
    // QFile::copy("file", picoPath+"file");
    #elifdef Q_OS_WIN
    // Ooooh, Windows as a mode option that does basically the same!
    QProcess *externalProg = new QProcess;
    QStringList args;
    args << QString("%1").arg(serialFoundList[ui->comPortSelector->currentIndex()-1].portName()) << "baud=12" << "parity=n" << "data=8" << "stop=1" << "dtr=off";
    externalProg->start("mode", args);
    #else
    // The builtin method that currently does not work at all atm. Let's hope to use this soon.
    serialPort.setDataTerminalReady(false);
    qDebug() << serialPort.isDataTerminalReady();
    QThread::msleep(100);
    serialPort.setBaudRate(QSerialPort::Baud1200);
    qDebug() << serialPort.baudRate();
    QThread::msleep(100);
    serialPort.close();
    QThread::msleep(100);
    qDebug() << serialPort.baudRate();
    qDebug() << serialPort.isDataTerminalReady();
    serialPort.open(QIODevice::ReadOnly);
    #endif
    ui->statusBar->showMessage("Board reset to bootloader.", 5000);
    ui->comPortSelector->setCurrentIndex(0);
    serialActive = false;
}

void guiWindow::on_actionAbout_UI_triggered()
{
    QDialog *about = new QDialog;
    Ui::aboutDialog aboutDialog;
    aboutDialog.setupUi(about);
    about->show();
}

