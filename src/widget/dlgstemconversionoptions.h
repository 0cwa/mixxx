#pragma once

#include <QComboBox>
#include <QDialog>
#include <QLabel>

class QDialogButtonBox;

/// Dialog that allows the user to select stem conversion parameters
class DlgStemConversionOptions : public QDialog {
    Q_OBJECT

  public:
    enum class Resolution {
        Low, // 44.1 kHz, verified base model
        High // Unsupported until a verified artifact is published
    };

    explicit DlgStemConversionOptions(const QString& trackPath, QWidget* parent = nullptr);
    ~DlgStemConversionOptions() override = default;

    /// Get the selected resolution
    Resolution getSelectedResolution() const;

  private:
    void createUI();
    void connectSignals();

    QString m_trackPath;
    QLabel* m_pTrackPathLabel;
    QComboBox* m_pResolutionComboBox;
    QDialogButtonBox* m_pButtonBox;
    Resolution m_selectedResolution;
};
