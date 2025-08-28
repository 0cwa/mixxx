#pragma once

#include <QString>

#include "util/parented_ptr.h"
#include "widget/wcuemenupopup.h"
#include "widget/wpushbutton.h"

class WMemoryCueButton : public WPushButton {
    Q_OBJECT
  public:
    WMemoryCueButton(QWidget* pParent, const QString& group);

    // Standard skin setup (creates the popup, sets palette, etc.)
    void setup(const QDomNode& node, const SkinContext& context) override;

  protected:
    void mousePressEvent(QMouseEvent* pEvent) override;

  private:
    const QString m_group;
    parented_ptr<WCueMenuPopup> m_pCueMenuPopup;
};

