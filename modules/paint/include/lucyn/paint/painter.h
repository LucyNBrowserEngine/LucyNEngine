// lucyn/paint/painter.h - the Painter interface from README's Rendering Pipeline.
// "layout never draws": it builds a DisplayList, and exactly one backend file
// turns that into pixels. SoftwarePainter (software_painter.h) ships first;
// a SkiaPainter could implement this same interface later and nothing else
// in the engine would notice.
#pragma once
#include <lucyn/paint/displaylist.h>

namespace lucyn::paint {

struct Painter {
    virtual void draw(const DisplayList& list) = 0;
    virtual ~Painter() = default;
};

}  // namespace lucyn::paint
