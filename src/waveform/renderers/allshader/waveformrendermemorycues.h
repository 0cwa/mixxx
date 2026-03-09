#pragma once

#include <QColor>

#include "rendergraph/geometrynode.h"
#include "util/class.h"
#include "waveform/renderers/waveformrendererabstract.h"

class QDomNode;
class SkinContext;

namespace allshader {
class WaveformRenderMemoryCues;
} // namespace allshader

class allshader::WaveformRenderMemoryCues final
        : public QObject,
          public ::WaveformRendererAbstract,
          public rendergraph::GeometryNode {
    Q_OBJECT
  public:
    explicit WaveformRenderMemoryCues(
            WaveformWidgetRenderer* waveformWidget,
            ::WaveformRendererAbstract::PositionSource type =
                    ::WaveformRendererAbstract::Play);

    void setup(const QDomNode& node, const SkinContext& skinContext) override;
    void draw(QPainter* painter, QPaintEvent* event) override;

    void preprocess() override;

  public slots:
    void setColor(const QColor& color) { m_color = color; }

  private:
    QColor m_color;
    bool m_isSlipRenderer;
    bool preprocessInner();

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderMemoryCues);
};
