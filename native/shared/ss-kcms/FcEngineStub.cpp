/*
    The Fonts module's preview engine without Xft: kfontinst's own FcEngine draws the preview with Xft on an X server (a
    "FIXME: no Xft on Wayland" in it), and there is none here. This draws the text with Qt (QPainter, the font by family and
    style) into the image PreviewRenderEngine::drawAutoSize wants. The rest of CFcEngine (the font installer's viewer) is not
    needed by the module and not defined.
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "Fc.h"
#include "FcEngine.h"

#include <KLocalizedString>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>

namespace KFI
{
bool CFcEngine::theirFcDirty = true;

CFcEngine::CFcEngine(bool)
    : m_installed(false)
    , m_style(0)
    , m_index(0)
    , m_indexCount(0)
    , m_alphaSizeIndex(0)
    , m_scalable(FcTrue)
    , m_xft(nullptr)
{
}

CFcEngine::~CFcEngine()
{
}

QString CFcEngine::getDefaultPreviewString()
{
    return i18nc("A sentence that uses all letters of the alphabet", "The quick brown fox jumps over the lazy dog");
}

QFont CFcEngine::getQFont(const QString &family, quint32 style, int size)
{
    QFont font(family);
    font.setPixelSize(size);
    int weight = 80, width = 100, slant = 0;   // fontconfig's: regular, normal, roman
    FC::decomposeStyleVal(style, weight, width, slant);
    font.setItalic(slant > 0);
    font.setBold(weight >= FC_WEIGHT_BOLD);
    return font;
}

QImage CFcEngine::draw(const QString &name, quint32 style, int, const QColor &txt, const QColor &bgnd, int fSize, const QString &text)
{
    const QFont font = getQFont(name, style, fSize > 0 ? fSize : 12);
    const QFontMetrics fm(font);
    const QRect box = fm.boundingRect(QRect(0, 0, 4000, 4000), Qt::AlignLeft | Qt::TextSingleLine, text);
    QImage image(qMax(1, box.width() + 4), qMax(1, fm.height() + 4), QImage::Format_ARGB32_Premultiplied);
    image.fill(bgnd);
    QPainter p(&image);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(txt);
    p.drawText(2, 2 + fm.ascent(), text);
    return image;
}
}
