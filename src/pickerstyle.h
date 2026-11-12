#pragma once

// 2.2 emoji / 2.5 GIFs: the emoji picker's palette, shared by the picker (emojipicker.cpp) and its GIFs tab
// (gifpanel.cpp), light and dark. 2.5 adds danger (a block reason's text, at least 4.5:1 on the popup) and dangerFill
// (the Remove all button: white text 5.7:1).

#include <QColor>

#include "uiutil.h"

namespace pickerstyle {

struct Colors {
    QColor background, border, field, fieldText, placeholder, text, muted, heading, hover, pressed, accent, ring, marked, dot, divider, thumb;
    QColor danger, dangerFill; // 2.5 GIFs
};

inline Colors colorsFor(bool dark, const QColor& base)
{
    Colors c;
    if (dark) {
        c.background = QColor(0x2b, 0x2d, 0x31);
        if (base.isValid() && base.lightness() < 128)
            c.background = ui::flatten(QColor(255, 255, 255, 10), base); // a step above the chat
        c.border      = QColor(0x1e, 0x1f, 0x22);
        c.field       = ui::flatten(QColor(0, 0, 0, 90), c.background);
        c.fieldText   = QColor(0xdb, 0xde, 0xe1);
        c.placeholder = QColor(0x94, 0x9b, 0xa4);
        c.text        = QColor(0xf2, 0xf3, 0xf5);
        c.muted       = QColor(0xb5, 0xba, 0xc1);
        c.heading     = QColor(0xb5, 0xba, 0xc1);
        c.hover       = ui::flatten(QColor(255, 255, 255, 20), c.background);
        c.pressed     = ui::flatten(QColor(255, 255, 255, 36), c.background);
        c.accent      = QColor(0x58, 0x65, 0xf2);
        c.ring        = QColor(0x94, 0x9c, 0xf7); // 3:1 on the dark popup
        c.marked      = ui::flatten(QColor(88, 101, 242, 70), c.background);
        c.dot         = ui::flatten(QColor(255, 255, 255, 18), c.background);
        c.divider     = ui::flatten(QColor(255, 255, 255, 14), c.background);
        c.thumb       = ui::flatten(QColor(255, 255, 255, 46), c.background);
        c.danger      = QColor(0xfa, 0x77, 0x7c);
        c.dangerFill  = QColor(0xc4, 0x28, 0x2d);
    } else {
        c.background  = QColor(0xff, 0xff, 0xff);
        c.border      = QColor(0xe3, 0xe5, 0xe8);
        c.field       = QColor(0xeb, 0xed, 0xef);
        c.fieldText   = QColor(0x31, 0x33, 0x38);
        c.placeholder = QColor(0x5c, 0x5e, 0x66);
        c.text        = QColor(0x06, 0x06, 0x07);
        c.muted       = QColor(0x4e, 0x50, 0x58);
        c.heading     = QColor(0x4e, 0x50, 0x58);
        c.hover       = QColor(0xf2, 0xf3, 0xf5);
        c.pressed     = QColor(0xe3, 0xe5, 0xe8);
        c.accent      = QColor(0x58, 0x65, 0xf2);
        c.ring        = QColor(0x58, 0x65, 0xf2);
        c.marked      = QColor(0xe6, 0xe8, 0xfd);
        c.dot         = QColor(0xf2, 0xf3, 0xf5);
        c.divider     = QColor(0xe3, 0xe5, 0xe8);
        c.thumb       = QColor(0xc4, 0xc9, 0xce);
        c.danger      = QColor(0xc4, 0x28, 0x2d);
        c.dangerFill  = QColor(0xc4, 0x28, 0x2d);
    }
    return c;
}

} // namespace pickerstyle
