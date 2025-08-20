#include "waveform/renderers/allshader/waveformrendermemorycues.h"

#include <QtDebug>
#include <QDomNode>

#include "moc_waveformrendermemorycues.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/unicolormaterial.h"
#include "rendergraph/vertexupdaters/vertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "track/track.h"
#include "track/cue.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace allshader {

WaveformRenderMemoryCues::WaveformRenderMemoryCues(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : ::WaveformRendererAbstract(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip) {
    initForRectangles<UniColorMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRenderMemoryCues::setup(const QDomNode& node, const SkinContext& skinContext) {
    m_color = QColor(skinContext.selectString(node, QStringLiteral("MemoryCueColor")));
    m_color = WSkinColor::getCorrectColor(m_color).toRgb();
}

void WaveformRenderMemoryCues::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderMemoryCues::preprocess() {
    if (!preprocessInner()) {
        geometry().allocate(0);
        markDirtyGeometry();
    }
}

bool WaveformRenderMemoryCues::preprocessInner() {
    const TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;


    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    m_color.setAlphaF(1.0f);

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0.0) {
        return false;
    }

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition(positionType);
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition(positionType);

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);

    if (!startPosition.isValid() || !endPosition.isValid()) {
        return false;
    }

    QList<CuePointer> memoryCuesOnScreen;
    const QList<CuePointer> cues = trackInfo->getCuePoints();
    for (const auto& pCue : cues) {
        if (!pCue) continue;
        if (pCue->getType() == mixxx::CueType::Memory) {
            double testPos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
            if(testPos > (firstDisplayedPosition * trackSamples)
                    && testPos < (lastDisplayedPosition * trackSamples)) {
                memoryCuesOnScreen.append(pCue);
            }
        }
    }

    const float rendererBreadth = m_waveformRenderer->getBreadth();

    const int numVerticesPerLine = 6; // 2 triangles

    const int reserved = (memoryCuesOnScreen.size() * 3) + (memoryCuesOnScreen.size() * numVerticesPerLine);
    geometry().allocate(reserved);

    VertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::Point2D>()};

    for (const auto& pCue : memoryCuesOnScreen) {
        double beatPosition = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const float x1 = static_cast<float>(xBeatPoint) + 0.f;
        const float x2 = x1 + 2.f;

        vertexUpdater.addRectangle({x1, 0.f},
                {x2, m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth});
        vertexUpdater.addTriangle({x1 - 8.f, 0.f}, {x2 + 8.f, 0.f}, {x1, 12.f});
    }
    markDirtyGeometry();

    DEBUG_ASSERT(reserved == vertexUpdater.index());

    material().setUniform(1, m_color);
    markDirtyMaterial();

    return true;
}

} // namespace allshader
