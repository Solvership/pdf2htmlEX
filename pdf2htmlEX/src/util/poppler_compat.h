/*
 * poppler_compat.h
 *
 * Small helpers that let the code compile against Poppler 24.x as well as
 * Poppler >= 25.x (std::array matrices, enum class writing mode, ...).
 * P2H_POPPLER_VERSION is major * 100 + minor, defined by CMakeLists.txt.
 */

#ifndef POPPLER_COMPAT_H__
#define POPPLER_COMPAT_H__

#include <array>
#include <cstddef>
#include <tuple>
#include <string>

#include <goo/GooString.h>
#include <GfxFont.h>
#include <OutputDev.h>

#ifndef P2H_POPPLER_VERSION
#define P2H_POPPLER_VERSION 2406
#endif

namespace pdf2htmlEX {

/*
 * Argument types of OutputDev virtual methods, taken from Poppler itself so
 * that the overrides always match (they changed from GooString* / double* to
 * std::string / std::array).
 */
namespace compat_detail {
template <class T> struct mem_fn_args;
template <class C, class R, class... A> struct mem_fn_args<R (C::*)(A...)>
{
    template <std::size_t I> using arg = std::tuple_element_t<I, std::tuple<A...>>;
};
} // namespace compat_detail

using DrawStringArg = compat_detail::mem_fn_args<decltype(&OutputDev::drawString)>::arg<1>;
using BeginStringArg = compat_detail::mem_fn_args<decltype(&OutputDev::beginString)>::arg<1>;
using DefaultCTMArg = compat_detail::mem_fn_args<decltype(&OutputDev::setDefaultCTM)>::arg<0>;
using MarkedContentNameArg = compat_detail::mem_fn_args<decltype(&OutputDev::beginMarkedContent)>::arg<0>;
using TransparencyBBoxArg = compat_detail::mem_fn_args<decltype(&OutputDev::beginTransparencyGroup)>::arg<1>;

// the std::string behind a string argument
inline const std::string & str_of(const GooString * s) { return s->toStr(); }
inline const std::string & str_of(const std::string & s) { return s; }

// raw pointer to the elements of a matrix / bbox, whether Poppler returns a pointer or a std::array
inline const double * dptr(const double * p) { return p; }
inline double * dptr(double * p) { return p; }
template <std::size_t N> inline const double * dptr(const std::array<double, N> & a) { return a.data(); }
template <std::size_t N> inline double * dptr(std::array<double, N> & a) { return a.data(); }

// whether a font uses vertical writing mode
inline bool font_is_vertical(const GfxFont * font)
{
    // int in Poppler 24, enum class { Horizontal, Vertical } later
    return static_cast<int>(font->getWMode()) != 0;
}

// current text position (user space): Poppler >= 25 keeps it separately from
// the current point of paths (getCurTextX/Y), before it was getCurX/Y
template <class S> inline double cur_text_x(const S * state)
{
    if constexpr (requires { state->getCurTextX(); })
        return state->getCurTextX();
    else
        return state->getCurX();
}
template <class S> inline double cur_text_y(const S * state)
{
    if constexpr (requires { state->getCurTextY(); })
        return state->getCurTextY();
    else
        return state->getCurY();
}

} // namespace pdf2htmlEX

#endif // POPPLER_COMPAT_H__
