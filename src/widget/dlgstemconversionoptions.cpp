#include "widget/dlgstemconversionoptions.h"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

DlgStemConversionOptions::DlgStemConversionOptions(const QString& trackPath, QWidget* parent)
        : QDialog(parent),
          m_trackPath(trackPath),
          m_selectedResolution(Resolution::Low) {
    setWindowTitle(tr("Stem Conversion Options"));
    setMinimumWidth(400);
    setMinimumHeight(200);
    setWindowModality(Qt::ApplicationModal);

    createUI();
    connectSignals();
}

void DlgStemConversionOptions::createUI() {
    QVBoxLayout* pMainLayout = new QVBoxLayout(this);

    // Title
    QLabel* pTitleLabel = new QLabel(tr("Select Stem Conversion Parameters"), this);
    QFont titleFont = pTitleLabel->font();
    titleFont.setPointSize(12);
    titleFont.setBold(true);
    pTitleLabel->setFont(titleFont);
    pMainLayout->addWidget(pTitleLabel);

    pMainLayout->addSpacing(10);

    // Track path display
    QGroupBox* pTrackInfoGroup = new QGroupBox(tr("Track to Convert"), this);
    QVBoxLayout* pTrackInfoLayout = new QVBoxLayout(pTrackInfoGroup);
    m_pTrackPathLabel = new QLabel(m_trackPath, this);
    m_pTrackPathLabel->setWordWrap(true);
    pTrackInfoLayout->addWidget(m_pTrackPathLabel);
    pMainLayout->addWidget(pTrackInfoGroup);

    // Resolution selection group
    QGroupBox* pResolutionGroup = new QGroupBox(tr("Audio Resolution"), this);
    QVBoxLayout* pResolutionLayout = new QVBoxLayout(pResolutionGroup);

    QLabel* pResolutionLabel = new QLabel(
            tr("Select the output resolution for stem separation:"), this);
    pResolutionLayout->addWidget(pResolutionLabel);

    m_pResolutionComboBox = new QComboBox(this);
    m_pResolutionComboBox->addItem(tr("Standard Resolution (44.1 kHz, verified model)"),
            static_cast<int>(Resolution::Low));
    m_pResolutionComboBox->setCurrentIndex(0);
    pResolutionLayout->addWidget(m_pResolutionComboBox);

    QLabel* pInfoLabel = new QLabel(
            tr("The fine-tuned htdemucs_ft.onnx model is unavailable because no verified "
               "artifact is published. It will remain disabled until a maintainer provides "
               "one."),
            this);
    pInfoLabel->setStyleSheet("color: gray; font-size: 10px;");
    pInfoLabel->setWordWrap(true);
    pResolutionLayout->addWidget(pInfoLabel);

    pMainLayout->addWidget(pResolutionGroup);

    pMainLayout->addStretch();

    m_pButtonBox = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_pButtonBox->button(QDialogButtonBox::Ok)->setText(tr("Start Conversion"));
    m_pButtonBox->button(QDialogButtonBox::Ok)->setMinimumWidth(120);
    m_pButtonBox->button(QDialogButtonBox::Cancel)->setMinimumWidth(100);
    pMainLayout->addWidget(m_pButtonBox);
}

void DlgStemConversionOptions::connectSignals() {
    connect(m_pResolutionComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                m_selectedResolution = static_cast<Resolution>(
                        m_pResolutionComboBox->itemData(index).toInt());
            });
    connect(m_pButtonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_pButtonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

DlgStemConversionOptions::Resolution DlgStemConversionOptions::getSelectedResolution() const {
    return m_selectedResolution;
}

#include "moc_dlgstemconversionoptions.cpp"
