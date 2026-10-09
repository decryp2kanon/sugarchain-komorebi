// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <chainparams.h>
#include <key.h>
#include <logging.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/networkstyle.h>
#include <qt/rpcconsole.h>
#include <qt/splashscreen.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <QAction>
#include <QFontMetrics>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTextEdit>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

namespace {
//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 4);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("regtest"));
}
} // namespace

void AppTests::splashProgressFormat()
{
    const QString counting{QString::fromStdString(FormatBlockIndexSplashMessage("Counting block index entries..."))};
    const QString progress{QString::fromStdString(FormatBlockIndexSplashMessage(
        "Loading block index: 17,228,000 / 44,782,474 (38%) | 687,943/s | elapsed 25s | ETA 40s"))};
    QCOMPARE(counting, QString("Counting block index entries...\n "));
    QCOMPARE(progress, QString("Loading block index...\n17,228,000 / 44,782,474 (38%)"));
    const QString legacy{QString::fromStdString(FormatBlockIndexSplashMessage(
        "Loading block index: 2,000 | 150/s | 13s"))};
    QCOMPARE(legacy, QString("Loading block index...\n2,000"));
    const QFontMetrics metrics{QApplication::font()};
    const QRect area{0, 0, 480, 320};
    const int flags{Qt::AlignBottom | Qt::AlignHCenter};
    QCOMPARE(metrics.boundingRect(area, flags, counting).height(), metrics.boundingRect(area, flags, progress).height());
    QScopedPointer<const NetworkStyle> style{NetworkStyle::instantiate(ChainType::REGTEST)};
    QVERIFY(style);
    SplashScreen splash{style.data()};
    splash.showMessage(counting, flags, QColor(55, 55, 55));
    const QPixmap counting_render{splash.grab()};
    QVERIFY(!counting_render.isNull());
    splash.showMessage(legacy, flags, QColor(55, 55, 55));
    QVERIFY(!splash.grab().isNull());
    splash.showMessage(progress, flags, QColor(55, 55, 55));
    const QPixmap progress_render{splash.grab()};
    QVERIFY(!progress_render.isNull());
    QCOMPARE(counting_render.size(), progress_render.size());
    QCOMPARE(QString::fromStdString(FormatBlockIndexSplashMessage(
        "Linking block index: 22,391,237 / 44,782,474 (50%) | elapsed 12s | ETA 12s")),
        QString("Linking block index...\n22,391,237 / 44,782,474 (50%)"));
    const QString sorting{QString::fromStdString(FormatBlockIndexSplashMessage(
        "Sorting block headers... | 44,782,474 entries | elapsed 0s"))};
    QCOMPARE(sorting, QString("Sorting block headers...\n "));
    splash.showMessage(sorting, flags, QColor(55, 55, 55));
    const QString completed{QString::fromStdString(FormatBlockIndexSplashMessage(
        "Sorting block headers completed: 44,782,474 entries | elapsed 4s"))};
    QCOMPARE(completed, QString("Sorting block headers completed.\n "));
    splash.showMessage(completed, flags, QColor(55, 55, 55));
    const QString chainstate{QString::fromStdString(FormatBlockIndexSplashMessage("Initializing chainstate..."))};
    QCOMPARE(chainstate, QString("Initializing chainstate...\n "));
    QCOMPARE(metrics.boundingRect(area, flags, chainstate).height(), metrics.boundingRect(area, flags, progress).height());
    splash.showMessage(chainstate, flags, QColor(55, 55, 55));
    const QImage blank_row{splash.grab().toImage()};
    splash.showMessage("Initializing chainstate...\n50%", flags, QColor(55, 55, 55));
    const QImage filled_row{splash.grab().toImage()};
    const int title_bottom{static_cast<int>((splash.height() - 5 - metrics.lineSpacing()) * blank_row.devicePixelRatio())};
    // Filling the second row must not move the title or any of the artwork.
    QCOMPARE(blank_row.copy(0, 0, blank_row.width(), title_bottom), filled_row.copy(0, 0, filled_row.width(), title_bottom));

    for (const std::string stage : {"Preparing block index", "Linking block index", "Collecting block file references",
             "Checking block files", "Preparing block headers", "Selecting best block header"}) {
        QCOMPARE(FormatBlockIndexSplashMessage(stage + "..."), stage + "...\n ");
        QCOMPARE(FormatBlockIndexSplashMessage(stage + ": 50 / 100 (50%)"), stage + "...\n50 / 100 (50%)");
        QCOMPARE(FormatBlockIndexSplashMessage(stage + "... | 100 entries | elapsed 0s"), stage + "...\n ");
        QCOMPARE(FormatBlockIndexSplashMessage(stage + ": 50 / 100 (50%) | elapsed 1s | ETA 1s"), stage + "...\n50 / 100 (50%)");
        QCOMPARE(FormatBlockIndexSplashMessage(stage + ": 100 / 100 (100%) | elapsed 2s | ETA 0s"), stage + "...\n100 / 100 (100%)");
    }
    QCOMPARE(FormatBlockIndexSplashMessage("Sorting block index completed."), std::string("Sorting block index completed.\n "));

}

//! Entry point for BitcoinApplication tests.
void AppTests::appTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        qWarning() << "Skipping AppTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
                      "with 'QT_QPA_PLATFORM=cocoa test_bitcoin-qt' on mac, or else use a linux or windows build.";
        return;
    }
#endif

    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    QVERIFY(m_app.createOptionsModel(/*resetSettings=*/true));
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().GetChainType()));
    m_app.setupPlatformStyle();
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};
    connect(window, &BitcoinGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.exit(0);
    }
}
