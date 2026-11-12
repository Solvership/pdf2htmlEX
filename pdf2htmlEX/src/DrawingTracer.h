/*
 * DrawingTracer.h
 *
 *  Created on: 2014-6-15
 *      Author: duanyao
 */

#ifndef DRAWINGTRACER_H__
#define DRAWINGTRACER_H__

#include <functional>

#include <GfxState.h>

#include <vector>
#include <array>

#include "pdf2htmlEX-config.h"

#if ENABLE_SVG
#include <cairo.h>
#endif

#include "Param.h"

namespace pdf2htmlEX
{

class DrawingTracer
{
public:
    /*
     * The callback to receive drawn event.
     * bbox in device space.
     */
    // a non-char graphics is drawn
    std::function<void(cairo_t *cairo, double * bbox, int what)> on_non_char_drawn;
    // a char is drawn in the clip area
    std::function<void(cairo_t *cairo, double * bbox)> on_char_drawn;
    // a char is drawn out of/partially in the clip area
    std::function<void(cairo_t *cairo, double * bbox, int pts_visible)> on_char_clipped;
    /*
     * An axis-aligned straight line was stroked (kind == RULE_STROKE, (x0,y0)-(x1,y1) is the
     * center line and `width` the line width) or an axis-aligned rectangle was filled
     * (kind == RULE_FILL, (x0,y0)-(x1,y1) are the corners, width == 0).
     * Coordinates are in the same (page) space as the char bboxes.
     * Used to detect table rulings.
     */
    enum { RULE_STROKE = 0, RULE_FILL = 1 };

    // baseline direction of the last char passed to draw_char (tracer space)
    double last_char_dir[2] = {1, 0};
    // white: drawn in (near) white, i.e. invisible on a white page
    std::function<void(double x0, double y0, double x1, double y1, double width, int kind, bool white)> on_rule_drawn;

    DrawingTracer(const Param & param);
    virtual ~DrawingTracer();
    void reset(GfxState * state);

    /*
     * A character is drawing
     * x, y: glyph-drawing position, in PDF text object space.
     * width, height: glyph width/height
     */
    void draw_char(GfxState * state, double x, double y, double width, double height, int inTransparencyGroup);
    /*
     * An image is drawing
     */
    void draw_image(GfxState * state);
    void update_ctm(GfxState * state, double m11, double m12, double m21, double m22, double m31, double m32);
    void clip(GfxState * state, bool even_odd = false);
    void clip_to_stroke_path(GfxState * state);
    void fill(GfxState * state, bool even_odd = false);
    void stroke(GfxState * state);
    void save();
    void restore();

    // whether drawing operations are traced at all
    bool tracing() const { return param.correct_text_visibility || param.semantic_tables; }

private:
    void finish();
    void emit_fill_rects(const GfxPath * path, bool white);
    // Following methods operate in user space (just before CTM is applied)
    void do_path(GfxState * state, const GfxPath * path);
    void draw_non_char_bbox(GfxState * state, double * bbox, int what);
    void draw_char_bbox(GfxState * state, double * bbox, int inTransparencyGroup);
    // If cairo is available, parameter state is ignored
    void xform_pt(double & x, double & y);

    const Param & param;

    std::vector<double*> ctm_stack;

#if ENABLE_SVG
    cairo_t * cairo;
#endif
};

} /* namespace pdf2htmlEX */
#endif /* DRAWINGTRACER_H__ */
