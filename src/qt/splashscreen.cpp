// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <qt/splashscreen.h>

#include <clientversion.h>
#include <common/system.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/walletmodel.h>
#include <util/translation.h>

#include <functional>

#include <QApplication>
#include <QCloseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QScreen>


SplashScreen::SplashScreen(const NetworkStyle* networkStyle)
    : QWidget()
{
    m_elapsed_timer.setParent(this);
    m_elapsed_timer.setInterval(1000);
    m_elapsed_timer.setTimerType(Qt::PreciseTimer);
    m_elapsed_timer.setObjectName("startupElapsedTimer");
    connect(&m_elapsed_timer, &QTimer::timeout, this, [this] {
        curMessage = m_elapsed_prefix + QString("%1s elapsed").arg(m_stage_clock.elapsed() / 1000);
        update();
    });

    // set reference point, paddings
    int paddingRight            = 50;
    int paddingTop              = 50;
    int titleVersionVSpace      = 17;
    int titleCopyrightVSpace    = 40;

    float fontFactor            = 1.0;
    float devicePixelRatio      = 1.0;
    devicePixelRatio = static_cast<QGuiApplication*>(QCoreApplication::instance())->devicePixelRatio();

    // define text to place
    QString titleText       = CLIENT_NAME;
    QString versionText     = QString("Version %1").arg(QString::fromStdString(FormatFullVersion()));
    QString copyrightText   = QString::fromUtf8(CopyrightHolders(strprintf("\xc2\xA9 %u-%u ", 2009, COPYRIGHT_YEAR)).c_str());
    const QString& titleAddText    = networkStyle->getTitleAddText();

    QString font            = QApplication::font().toString();

    // create a bitmap according to device pixelratio
    QSize splashSize(480*devicePixelRatio,320*devicePixelRatio);
    pixmap = QPixmap(splashSize);

    // change to HiDPI if it makes sense
    pixmap.setDevicePixelRatio(devicePixelRatio);

    QPainter pixPaint(&pixmap);
    pixPaint.setPen(QColor(100,100,100));

    // draw a slightly radial gradient
    QRadialGradient gradient(QPoint(0,0), splashSize.width()/devicePixelRatio);
    gradient.setColorAt(0, Qt::white);
    gradient.setColorAt(1, QColor(247,247,247));
    QRect rGradient(QPoint(0,0), splashSize);
    pixPaint.fillRect(rGradient, gradient);

    // Show the complete official logo, using the 1024x1024 PNG for HiDPI.
    QRect rectIcon(QPoint(24,48), QSize(192,192));

    const QSize requiredSize(1024,1024);
    QPixmap icon(networkStyle->getAppIcon().pixmap(requiredSize));

    pixPaint.setRenderHint(QPainter::SmoothPixmapTransform);
    pixPaint.drawPixmap(rectIcon, icon);

    // check font size and drawing with
    pixPaint.setFont(QFont(font, 33*fontFactor));
    QFontMetrics fm = pixPaint.fontMetrics();
    int titleTextWidth = GUIUtil::TextWidth(fm, titleText);
    if (titleTextWidth > 176) {
        fontFactor = fontFactor * 176 / titleTextWidth;
    }

    pixPaint.setFont(QFont(font, 33*fontFactor));
    fm = pixPaint.fontMetrics();
    titleTextWidth  = GUIUtil::TextWidth(fm, titleText);
    pixPaint.drawText(pixmap.width()/devicePixelRatio-titleTextWidth-paddingRight,paddingTop,titleText);

    pixPaint.setFont(QFont(font, 15*fontFactor));

    // if the version string is too long, reduce size
    fm = pixPaint.fontMetrics();
    int versionTextWidth  = GUIUtil::TextWidth(fm, versionText);
    if(versionTextWidth > titleTextWidth+paddingRight-10) {
        pixPaint.setFont(QFont(font, 10*fontFactor));
        titleVersionVSpace -= 5;
    }
    pixPaint.drawText(pixmap.width()/devicePixelRatio-titleTextWidth-paddingRight+2,paddingTop+titleVersionVSpace,versionText);

    // draw copyright stuff
    {
        pixPaint.setFont(QFont(font, 10*fontFactor));
        const int x = pixmap.width()/devicePixelRatio-titleTextWidth-paddingRight;
        const int y = paddingTop+titleCopyrightVSpace;
        QRect copyrightRect(x, y, pixmap.width() - x - paddingRight, pixmap.height() - y);
        pixPaint.drawText(copyrightRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, copyrightText);
    }

    // draw additional text if special network
    if(!titleAddText.isEmpty()) {
        QFont boldFont = QFont(font, 10*fontFactor);
        boldFont.setWeight(QFont::Bold);
        pixPaint.setFont(boldFont);
        fm = pixPaint.fontMetrics();
        int titleAddTextWidth  = GUIUtil::TextWidth(fm, titleAddText);
        pixPaint.drawText(pixmap.width()/devicePixelRatio-titleAddTextWidth-10,15,titleAddText);
    }

    pixPaint.end();

    // Set window title
    setWindowTitle(titleText + " " + titleAddText);

    // Resize window and move to center of desktop, disallow resizing
    QRect r(QPoint(), QSize(pixmap.size().width()/devicePixelRatio,pixmap.size().height()/devicePixelRatio));
    resize(r.size());
    setFixedSize(r.size());
    move(QGuiApplication::primaryScreen()->geometry().center() - r.center());

    installEventFilter(this);

    GUIUtil::handleCloseWindowShortcut(this);
}

SplashScreen::~SplashScreen()
{
    if (m_node) unsubscribeFromCoreSignals();
}

void SplashScreen::setNode(interfaces::Node& node)
{
    assert(!m_node);
    m_node = &node;
    subscribeToCoreSignals();
    if (m_shutdown) m_node->startShutdown();
}

void SplashScreen::shutdown()
{
    m_shutdown = true;
    if (m_node) m_node->startShutdown();
}

bool SplashScreen::eventFilter(QObject * obj, QEvent * ev) {
    if (ev->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(ev);
        if (keyEvent->key() == Qt::Key_Q) {
            shutdown();
        }
    }
    return QObject::eventFilter(obj, ev);
}

static void InitMessage(SplashScreen *splash, const std::string &message)
{
    bool invoked = QMetaObject::invokeMethod(splash, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(message)),
        Q_ARG(int, Qt::AlignBottom|Qt::AlignHCenter),
        Q_ARG(QColor, QColor(55,55,55)));
    assert(invoked);
}

std::string FormatBlockIndexSplashMessage(const std::string& message)
{
    if (message == "Counting block index entries...") {
        // Reserve the same three-line height while the total is still unknown.
        return "Loading block index...\nCounting entries...\n0s elapsed";
    }
    static const std::string prefix{"Loading block index: "};
    if (message.compare(0, prefix.size(), prefix) != 0) {
        // New startup stages retain the same three-line layout as index loading.
        const std::string stages[]{"Preparing block index", "Sorting block index", "Linking block index",
            "Collecting block file references", "Checking block files", "Preparing block headers",
            "Sorting block headers", "Selecting best block header"};
        for (const auto& stage : stages) {
            if (message.compare(0, stage.size(), stage) != 0) continue;
            const size_t elapsed{message.find(" | elapsed ")};
            if (elapsed == std::string::npos) return message;
            const size_t eta{message.find(" | ETA ", elapsed)};
            const size_t body{message.find(": ")};
            const size_t separator{message.find(" | ")};
            if (separator == std::string::npos) return message;
            const bool counted{body != std::string::npos && body < separator};
            const size_t begin{counted ? body + 2 : separator + 3};
            const size_t end{counted ? separator : elapsed};
            if (end < begin) return message;
            const std::string duration{message.substr(elapsed + 11, (eta == std::string::npos ? message.size() : eta) - elapsed - 11)};
            return stage + (message.compare(stage.size(), 11, " completed:") == 0 ? " completed.\n" : "...\n") + message.substr(begin, end - begin) + "\n" + duration + " elapsed" +
                (eta == std::string::npos ? "" : message.substr(eta));
        }
        return message;
    }
    const std::string body{message.substr(prefix.size())};
    const size_t rate_pos{body.find(" | ")};
    const size_t elapsed_pos{body.find(" | elapsed ")};
    const size_t eta_pos{body.find(" | ETA ")};
    if (rate_pos == std::string::npos || elapsed_pos == std::string::npos || eta_pos == std::string::npos ||
        rate_pos >= elapsed_pos || elapsed_pos + 11 > eta_pos) {
        return "Loading block index...\n" + body;
    }
    const std::string rate{body.substr(rate_pos + 3, elapsed_pos - rate_pos - 3)};
    const std::string elapsed{body.substr(elapsed_pos + 11, eta_pos - elapsed_pos - 11)};
    return "Loading block index...\n" + body.substr(0, rate_pos) + "\n" +
           rate + " | " + elapsed + " elapsed" + body.substr(eta_pos);
}

static void ShowProgress(SplashScreen *splash, const std::string &title, int nProgress, bool resume_possible)
{
    InitMessage(splash, title + std::string("\n") +
            (resume_possible ? SplashScreen::tr("(press q to shutdown and continue later)").toStdString()
                                : SplashScreen::tr("press q to shutdown").toStdString()) +
            strprintf("\n%d", nProgress) + "%");
}

void SplashScreen::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_init_message = m_node->handleInitMessage([this](const std::string& message) {
        InitMessage(this, FormatBlockIndexSplashMessage(message));
    });
    m_handler_show_progress = m_node->handleShowProgress([this](const std::string& title, int nProgress, bool resume_possible) {
        ShowProgress(this, title, nProgress, resume_possible);
    });
    m_handler_init_wallet = m_node->handleInitWallet([this]() { handleLoadWallet(); });
}

void SplashScreen::handleLoadWallet()
{
#ifdef ENABLE_WALLET
    if (!WalletModel::isWalletEnabled()) return;
    m_handler_load_wallet = m_node->walletLoader().handleLoadWallet([this](std::unique_ptr<interfaces::Wallet> wallet) {
        m_connected_wallet_handlers.emplace_back(wallet->handleShowProgress([this](const std::string& title, int nProgress) {
            ShowProgress(this, title, nProgress, /*resume_possible=*/false);
        }));
        m_connected_wallets.emplace_back(std::move(wallet));
    });
#endif
}

void SplashScreen::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_init_message->disconnect();
    m_handler_show_progress->disconnect();
    for (const auto& handler : m_connected_wallet_handlers) {
        handler->disconnect();
    }
    m_connected_wallet_handlers.clear();
    m_connected_wallets.clear();
}

void SplashScreen::showMessage(const QString &message, int alignment, const QColor &color)
{
    m_elapsed_timer.stop();
    curMessage = message;
    // Only opaque startup stages use the GUI-local clock. No background thread,
    // repeated core log messages, cs_main acquisition or invented ETA is needed.
    if ((message.startsWith("Sorting block index...\n") || message.startsWith("Sorting block headers...\n") ||
         message.startsWith("Loading block index...\nCounting entries...\n")) &&
        message.endsWith("\n0s elapsed")) {
        m_elapsed_prefix = message.left(message.lastIndexOf('\n') + 1);
        m_stage_clock.start();
        m_elapsed_timer.start();
    }
    curAlignment = alignment;
    curColor = color;
    update();
}

void SplashScreen::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.drawPixmap(0, 0, pixmap);
    QRect r = rect().adjusted(5, 5, -5, -5);
    painter.setPen(curColor);
    painter.drawText(r, curAlignment, curMessage);
}

void SplashScreen::closeEvent(QCloseEvent *event)
{
    shutdown(); // allows an "emergency" shutdown during startup
    event->ignore();
}
