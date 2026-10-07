#include "stems/dlgstemconversion.h"

#include <QBrush>
#include <QFileDialog>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPalette>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "track/track.h"
#include "util/color/color.h"
#include "widget/dlgstemconversionoptions.h"

namespace {

constexpr double kMinimumStatusContrast = 4.5;

double relativeLuminance(const QColor& color) {
    const auto linearize = [](double channel) {
        return channel <= 0.03928
                ? channel / 12.92
                : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linearize(color.redF()) +
            0.7152 * linearize(color.greenF()) +
            0.0722 * linearize(color.blueF());
}

double contrastRatio(const QColor& foreground, const QColor& background) {
    const double foregroundLuminance = relativeLuminance(foreground);
    const double backgroundLuminance = relativeLuminance(background);
    return (std::max(foregroundLuminance, backgroundLuminance) + 0.05) /
            (std::min(foregroundLuminance, backgroundLuminance) + 0.05);
}

QColor contrastSafeColor(QColor foreground, const QColor& background) {
    if (contrastRatio(foreground, background) >= kMinimumStatusContrast) {
        return foreground;
    }

    const QColor black = QColor(Qt::black);
    const QColor white = QColor(Qt::white);
    return contrastRatio(black, background) >= contrastRatio(white, background)
            ? black
            : white;
}

QColor paletteStatusColor(const QPalette& palette,
        QPalette::ColorGroup colorGroup,
        QPalette::ColorRole accentRole) {
    const QColor foreground = Color::blendColors(
            palette.color(colorGroup, accentRole),
            palette.color(colorGroup, QPalette::Text));
    return contrastSafeColor(foreground,
            palette.color(colorGroup, QPalette::Window));
}

void setStatusColor(QLabel* pStatusLabel, QPalette::ColorRole accentRole) {
    QPalette palette = pStatusLabel->palette();

    const QPalette::ColorGroup currentGroup = palette.currentColorGroup();
    const QPalette::ColorGroup otherGroup =
            currentGroup == QPalette::Inactive ? QPalette::Active : QPalette::Inactive;
    for (const QPalette::ColorGroup colorGroup : {currentGroup, otherGroup}) {
        palette.setColor(colorGroup,
                QPalette::WindowText,
                paletteStatusColor(palette, colorGroup, accentRole));
    }

    pStatusLabel->setPalette(palette);
}

} // namespace

DlgStemConversion::DlgStemConversion(
        StemConversionManager* pConversionManager,
        QWidget* parent)
        : QDialog(parent),
          m_pConversionManager(pConversionManager) {
    setWindowTitle(tr("Stem Conversion Status"));
    setMinimumWidth(600);
    setMinimumHeight(400);

    // Window modeless (non-blocking) - IMPORTANT: This allows the window to be closed
    setWindowModality(Qt::NonModal);

    // Allow the window to be closed with the X button
    setWindowFlags(windowFlags() | Qt::WindowCloseButtonHint);

    createUI();
    connectSignals();
    updateConversionList();
}

void DlgStemConversion::createUI() {
    QVBoxLayout* pMainLayout = new QVBoxLayout(this);

    // Current conversion section
    QGroupBox* pCurrentGroup = new QGroupBox(tr("Current Conversion"), this);
    QVBoxLayout* pCurrentLayout = new QVBoxLayout(pCurrentGroup);

    m_pCurrentTrackLabel = new QLabel(tr("No conversion in progress"), this);
    pCurrentLayout->addWidget(m_pCurrentTrackLabel);

    m_pProgressBar = new QProgressBar(this);
    m_pProgressBar->setRange(0, 100);
    m_pProgressBar->setValue(0);
    pCurrentLayout->addWidget(m_pProgressBar);

    // Phase/Status message label (NEW)
    m_pStatusLabel = new QLabel(tr("Waiting for conversion..."), this);
    QFont statusFont = m_pStatusLabel->font();
    statusFont.setBold(true);
    m_pStatusLabel->setFont(statusFont);
    setStatusColor(m_pStatusLabel, QPalette::Link);
    pCurrentLayout->addWidget(m_pStatusLabel);

    pMainLayout->addWidget(pCurrentGroup);

    // Conversion history section
    QGroupBox* pHistoryGroup = new QGroupBox(tr("Conversion History"), this);
    QVBoxLayout* pHistoryLayout = new QVBoxLayout(pHistoryGroup);

    m_pConversionListWidget = new QListWidget(this);
    pHistoryLayout->addWidget(m_pConversionListWidget);

    pMainLayout->addWidget(pHistoryGroup);

    // Buttons
    QHBoxLayout* pButtonLayout = new QHBoxLayout();
    m_pClearHistoryButton = new QPushButton(tr("Clear History"), this);
    connect(m_pClearHistoryButton, &QPushButton::clicked, this, &DlgStemConversion::onClearHistory);
    pButtonLayout->addWidget(m_pClearHistoryButton);

    pButtonLayout->addStretch();

    m_pConvertNewButton = new QPushButton(tr("Convert New Track"), this);
    connect(m_pConvertNewButton,
            &QPushButton::clicked,
            this,
            &DlgStemConversion::onConvertNewTrack);
    pButtonLayout->addWidget(m_pConvertNewButton);

    m_pCloseButton = new QPushButton(tr("Close"), this);
    // Use reject() instead of accept() to close the dialog without blocking
    connect(m_pCloseButton, &QPushButton::clicked, this, &QDialog::reject);
    pButtonLayout->addWidget(m_pCloseButton);

    pMainLayout->addLayout(pButtonLayout);
}

void DlgStemConversion::connectSignals() {
    if (!m_pConversionManager) {
        return;
    }

    connect(m_pConversionManager,
            &StemConversionManager::conversionStarted,
            this,
            &DlgStemConversion::onConversionStarted);
    connect(m_pConversionManager,
            &StemConversionManager::conversionProgress,
            this,
            &DlgStemConversion::onConversionProgress);
    connect(m_pConversionManager,
            &StemConversionManager::conversionCompleted,
            this,
            &DlgStemConversion::onConversionCompleted);
    connect(m_pConversionManager,
            &StemConversionManager::conversionFailed,
            this,
            &DlgStemConversion::onConversionFailed);
    connect(m_pConversionManager,
            &StemConversionManager::queueChanged,
            this,
            &DlgStemConversion::onQueueChanged);
}

void DlgStemConversion::onConversionStarted(TrackId, const QString& trackTitle) {
    m_pCurrentTrackLabel->setText(tr("Converting: %1").arg(trackTitle));
    m_pProgressBar->setValue(0);
    m_pStatusLabel->setText(tr("⏳ Initializing conversion..."));
}

void DlgStemConversion::onConversionProgress(
        TrackId, float progress, const QString& message) {
    m_pProgressBar->setValue(static_cast<int>(progress * 100));

    // Format the message with progress indicator and emoji
    QString displayMessage = message;

    // Add progress percentage
    displayMessage = tr("%1 (%2%)")
                             .arg(displayMessage)
                             .arg(static_cast<int>(progress * 100));

    // Add phase emoji based on progress
    if (progress < 0.2f) {
        displayMessage = tr("🔍 %1").arg(displayMessage); // Searching/Initializing
    } else if (progress < 0.5f) {
        displayMessage = tr("🎵 %1").arg(displayMessage); // Demucs separation
    } else if (progress < 0.7f) {
        displayMessage = tr("🔄 %1").arg(displayMessage); // Converting to M4A
    } else if (progress < 0.9f) {
        displayMessage = tr("📦 %1").arg(displayMessage); // Creating container
    } else {
        displayMessage = tr("✅ %1").arg(displayMessage); // Finalizing
    }

    m_pStatusLabel->setText(displayMessage);
}

void DlgStemConversion::onConversionCompleted(TrackId, const QString& trackTitle) {
    m_pProgressBar->setValue(100);
    m_pStatusLabel->setText(
            tr("✅ %1 - Conversion completed successfully! (100%)").arg(trackTitle));
    updateConversionList();
}

void DlgStemConversion::onConversionFailed(TrackId,
        const QString& trackTitle,
        const QString& errorMessage) {
    m_pStatusLabel->setText(tr("❌ Error converting %1: %2").arg(trackTitle, errorMessage));
    setStatusColor(m_pStatusLabel, QPalette::LinkVisited);
    updateConversionList();
}

void DlgStemConversion::onQueueChanged(int pendingCount) {
    if (pendingCount == 0) {
        m_pCurrentTrackLabel->setText(tr("No conversion in progress"));
        setStatusColor(m_pStatusLabel, QPalette::Link);
    }
    updateConversionList();
}

void DlgStemConversion::onClearHistory() {
    if (m_pConversionManager) {
        m_pConversionManager->clearConversionHistory();
        updateConversionList();
    }
}

void DlgStemConversion::updateConversionList() {
    m_pConversionListWidget->clear();

    if (!m_pConversionManager) {
        return;
    }

    QList<StemConversionManager::ConversionStatus> history =
            m_pConversionManager->getConversionHistory();

    for (const auto& status : history) {
        QString stateText = getStateDisplayText(status.state);
        QString itemText = QString("%1 - %2").arg(status.trackTitle, stateText);

        QListWidgetItem* pItem = new QListWidgetItem(itemText, m_pConversionListWidget);

        const QPalette palette = m_pConversionListWidget->palette();
        if (status.state == StemConverter::ConversionState::Completed) {
            pItem->setBackground(palette.brush(QPalette::Active, QPalette::AlternateBase));
        } else if (status.state == StemConverter::ConversionState::Failed) {
            pItem->setBackground(palette.brush(QPalette::Active, QPalette::Midlight));
        } else if (status.state == StemConverter::ConversionState::Processing) {
            pItem->setBackground(palette.brush(QPalette::Active, QPalette::Highlight));
            pItem->setForeground(palette.brush(QPalette::Active, QPalette::HighlightedText));
        }
    }
}

QString DlgStemConversion::getStateDisplayText(StemConverter::ConversionState state) const {
    switch (state) {
    case StemConverter::ConversionState::Idle:
        return tr("Idle");
    case StemConverter::ConversionState::Processing:
        return tr("Processing");
    case StemConverter::ConversionState::Completed:
        return tr("Completed");
    case StemConverter::ConversionState::Failed:
        return tr("Failed");
    default:
        return tr("Unknown");
    }
}

void DlgStemConversion::onConvertNewTrack() {
    QString filePath = QFileDialog::getOpenFileName(
            this,
            tr("Select Audio File to Convert"),
            QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
            tr("Audio Files (*.mp3 *.wav *.flac *.m4a *.ogg)"));

    if (filePath.isEmpty()) {
        return; // User canceled
    }

    // Create a temporary track object
    TrackPointer pTrack = Track::newTemporary(filePath);
    if (!pTrack) {
        QMessageBox::warning(
                this, tr("Error"), tr("Could not load the selected track."));
        return;
    }

    // Open the options dialog
    DlgStemConversionOptions optionsDialog(pTrack->getLocation(), this);
    if (optionsDialog.exec() == QDialog::Accepted) {
        if (!m_pConversionManager) {
            return;
        }
        StemConverter::Resolution resolution;
        if (optionsDialog.getSelectedResolution() == DlgStemConversionOptions::Resolution::High) {
            resolution = StemConverter::Resolution::High;
        } else {
            resolution = StemConverter::Resolution::Low;
        }
        m_pConversionManager->convertTrack(pTrack, resolution);
    }
}

#include "moc_dlgstemconversion.cpp"
