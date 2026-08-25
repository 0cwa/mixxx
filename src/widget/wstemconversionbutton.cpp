#include "widget/wstemconversionbutton.h"

#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

#include "stems/dlgstemconversion.h"

WStemConversionButton::WStemConversionButton(
        StemConversionManager* pConversionManager,
        QWidget* parent)
        : QPushButton(parent),
          m_pConversionManager(pConversionManager),
          m_rotationAngle(0),
          m_pendingCount(0),
          m_isProcessing(false),
          m_pDialog(nullptr),
          m_pAnimationTimer(nullptr) {
    createUI();
    connectSignals();
    updateButtonState();
}

void WStemConversionButton::createUI() {
    setObjectName(QStringLiteral("StemConversionButton"));
    setToolTip(tr("Click to view stem conversion status"));
    setMinimumWidth(50);
    setMinimumHeight(50);
    setMaximumWidth(60);
    setMaximumHeight(60);

    setText("STEM");

    connect(this, &QPushButton::clicked, this, &WStemConversionButton::onButtonClicked);
}

void WStemConversionButton::connectSignals() {
    if (!m_pConversionManager) {
        return;
    }

    connect(m_pConversionManager,
            &StemConversionManager::conversionStarted,
            this,
            &WStemConversionButton::onConversionStarted);
    connect(m_pConversionManager,
            &StemConversionManager::conversionCompleted,
            this,
            &WStemConversionButton::onConversionCompleted);
    connect(m_pConversionManager,
            &StemConversionManager::conversionFailed,
            this,
            &WStemConversionButton::onConversionFailed);
    connect(m_pConversionManager,
            &StemConversionManager::queueChanged,
            this,
            &WStemConversionButton::onQueueChanged);
}

void WStemConversionButton::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Draw status indicator circle
    int size = 12;
    int x = width() - size - 4;
    int y = height() - size - 4;

    // Determine color based on state
    QColor indicatorColor = QColor(100, 100, 100); // Gray (idle)
    if (m_isProcessing) {
        indicatorColor = QColor(255, 165, 0); // Orange (processing)
    } else if (m_pendingCount > 0) {
        indicatorColor = QColor(255, 165, 0); // Orange (pending)
    }

    // Draw indicator circle
    painter.setBrush(indicatorColor);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(x, y, size, size);

    // If processing, draw rotation animation
    if (m_isProcessing) {
        painter.save();
        painter.translate(x + size / 2, y + size / 2);
        painter.rotate(m_rotationAngle);

        painter.setPen(QPen(Qt::white, 2));
        painter.drawLine(0, -size / 2, 0, -size / 2 + 3);

        painter.restore();
    }

    // Draw pending count if any
    if (m_pendingCount > 0) {
        painter.setPen(Qt::white);
        painter.setFont(QFont("Arial", 7, QFont::Bold));
        painter.drawText(x, y, size, size, Qt::AlignCenter, QString::number(m_pendingCount));
    }
}

void WStemConversionButton::onConversionStarted(TrackId trackId, const QString& trackTitle) {
    static_cast<void>(trackId);
    static_cast<void>(trackTitle);
    m_isProcessing = true;
    startAnimation();
    updateButtonState();
}

void WStemConversionButton::onConversionCompleted(TrackId trackId) {
    static_cast<void>(trackId);
    updateButtonState();
}

void WStemConversionButton::onConversionFailed(TrackId trackId, const QString& errorMessage) {
    static_cast<void>(trackId);
    static_cast<void>(errorMessage);
    updateButtonState();
}

void WStemConversionButton::onQueueChanged(int pendingCount) {
    m_pendingCount = pendingCount;
    updateButtonState();
    update();
}

void WStemConversionButton::onAnimationTick() {
    m_rotationAngle = (m_rotationAngle + 6) % 360;
    update();
}

void WStemConversionButton::onButtonClicked() {
    if (!m_pConversionManager) {
        return;
    }

    // Show the existing window if it is already open.
    if (m_pDialog) {
        m_pDialog->show();
        m_pDialog->raise();
        m_pDialog->activateWindow();
        return;
    }

    // Create the dialog as a modeless window.
    m_pDialog = make_parented<DlgStemConversion>(m_pConversionManager, this);

    // Show the non-blocking window.
    m_pDialog->show();
}

void WStemConversionButton::updateButtonState() {
    if (!m_pConversionManager) {
        return;
    }

    auto currentConversion = m_pConversionManager->getCurrentConversion();
    m_isProcessing = currentConversion.has_value();

    if (m_isProcessing) {
        setToolTip(tr("Converting: %1\nClick for details")
                        .arg(currentConversion->trackTitle));
        startAnimation();
    } else if (m_pendingCount > 0) {
        setToolTip(tr("Pending conversions: %1\nClick for details")
                        .arg(m_pendingCount));
        stopAnimation();
    } else {
        setToolTip(tr("No conversions in progress\nClick to view history"));
        stopAnimation();
    }

    update();
}

void WStemConversionButton::startAnimation() {
    if (!m_pAnimationTimer) {
        m_pAnimationTimer = make_parented<QTimer>(this);
        connect(m_pAnimationTimer,
                &QTimer::timeout,
                this,
                &WStemConversionButton::onAnimationTick);
        m_pAnimationTimer->setInterval(50);
    }
    if (!m_pAnimationTimer->isActive()) {
        m_pAnimationTimer->start();
    }
}

void WStemConversionButton::stopAnimation() {
    if (m_pAnimationTimer) {
        m_pAnimationTimer->stop();
    }
    m_rotationAngle = 0;
    update();
}

#include "moc_wstemconversionbutton.cpp"
