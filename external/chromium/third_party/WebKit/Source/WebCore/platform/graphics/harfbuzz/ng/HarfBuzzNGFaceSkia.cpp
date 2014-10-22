/*
 * Copyright (c) 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "HarfBuzzNGFace.h"

#include "FontPlatformData.h"
#include "GlyphBuffer.h"
#include "HarfBuzzShaper.h"
#include "SimpleFontData.h"
#include "SkFontHost.h"
#include "SkPaint.h"
#include "SkPath.h"
#include "SkPoint.h"
#include "SkRect.h"
#include "SkUtils.h"

#include "hb.h"
#include <wtf/HashMap.h>

namespace WebCore {

// Our implementation of the callbacks which Harfbuzz requires by using Skia
// calls. See the Harfbuzz source for references about what these callbacks do.

struct HarfBuzzFontData {
    HarfBuzzFontData(WTF::HashMap<uint32_t, uint16_t>* glyphCacheForFaceCacheEntry)
        : m_glyphCacheForFaceCacheEntry(glyphCacheForFaceCacheEntry)
    { }
    SkPaint m_paint;
    WTF::HashMap<uint32_t, uint16_t>* m_glyphCacheForFaceCacheEntry;
};

static hb_position_t SkiaScalarToHarfbuzzPosition(SkScalar value)
{
    return SkScalarToFixed(value);
}

static void SkiaGetGlyphWidthAndExtents(SkPaint* paint, hb_codepoint_t codepoint, hb_position_t* width, hb_glyph_extents_t* extents)
{
    ASSERT(codepoint <= 0xFFFF);
    paint->setTextEncoding(SkPaint::kGlyphID_TextEncoding);

    SkScalar skWidth;
    SkRect skBounds;
    uint16_t glyph = codepoint;

    paint->getTextWidths(&glyph, sizeof(glyph), &skWidth, &skBounds);
    if (width)
        *width = SkiaScalarToHarfbuzzPosition(skWidth);
    if (extents) {
        // Invert y-axis because Skia is y-grows-down but we set up harfbuzz to be y-grows-up.
        extents->x_bearing = SkiaScalarToHarfbuzzPosition(skBounds.fLeft);
        extents->y_bearing = SkiaScalarToHarfbuzzPosition(-skBounds.fTop);
        extents->width = SkiaScalarToHarfbuzzPosition(skBounds.width());
        extents->height = SkiaScalarToHarfbuzzPosition(-skBounds.height());
    }
}

static hb_bool_t harfbuzzGetGlyph(hb_font_t* hbFont, void* fontData, hb_codepoint_t unicode, hb_codepoint_t variationSelector, hb_codepoint_t* glyph, void* userData)
{
    HarfBuzzFontData* hbFontData = reinterpret_cast<HarfBuzzFontData*>(fontData);

    WTF::HashMap<uint32_t, uint16_t>::AddResult result = hbFontData->m_glyphCacheForFaceCacheEntry->add(unicode, 0);
    if (result.isNewEntry) {
        SkPaint* paint = &hbFontData->m_paint;
        paint->setTextEncoding(SkPaint::kUTF32_TextEncoding);
        uint16_t glyph16;
        paint->textToGlyphs(&unicode, sizeof(hb_codepoint_t), &glyph16);
        result.iterator->value = glyph16;
        *glyph = glyph16;
    }
    *glyph = result.iterator->value;
    return !!*glyph;
}

static hb_position_t harfbuzzGetGlyphHorizontalAdvance(hb_font_t* hbFont, void* fontData, hb_codepoint_t glyph, void* userData)
{
    HarfBuzzFontData* hbFontData = reinterpret_cast<HarfBuzzFontData*>(fontData);
    hb_position_t advance = 0;

    SkiaGetGlyphWidthAndExtents(&hbFontData->m_paint, glyph, &advance, 0);
    return advance;
}

static hb_bool_t harfbuzzGetGlyphHorizontalOrigin(hb_font_t* hbFont, void* fontData, hb_codepoint_t glyph, hb_position_t* x, hb_position_t* y, void* userData)
{
    // Just return true, following the way that Harfbuzz-FreeType
    // implementation does.
    return true;
}

static hb_bool_t harfbuzzGetGlyphExtents(hb_font_t* hbFont, void* fontData, hb_codepoint_t glyph, hb_glyph_extents_t* extents, void* userData)
{
    HarfBuzzFontData* hbFontData = reinterpret_cast<HarfBuzzFontData*>(fontData);

    SkiaGetGlyphWidthAndExtents(&hbFontData->m_paint, glyph, 0, extents);
    return true;
}

static hb_font_funcs_t* harfbuzzSkiaGetFontFuncs()
{
    static hb_font_funcs_t* harfbuzzSkiaFontFuncs = 0;

    // We don't set callback functions which we can't support.
    // Harfbuzz will use the fallback implementation if they aren't set.
    if (!harfbuzzSkiaFontFuncs) {
        harfbuzzSkiaFontFuncs = hb_font_funcs_create();
        hb_font_funcs_set_glyph_func(harfbuzzSkiaFontFuncs, harfbuzzGetGlyph, 0, 0);
        hb_font_funcs_set_glyph_h_advance_func(harfbuzzSkiaFontFuncs, harfbuzzGetGlyphHorizontalAdvance, 0, 0);
        hb_font_funcs_set_glyph_h_origin_func(harfbuzzSkiaFontFuncs, harfbuzzGetGlyphHorizontalOrigin, 0, 0);
        hb_font_funcs_set_glyph_extents_func(harfbuzzSkiaFontFuncs, harfbuzzGetGlyphExtents, 0, 0);
        hb_font_funcs_make_immutable(harfbuzzSkiaFontFuncs);
    }
    return harfbuzzSkiaFontFuncs;
}

static hb_blob_t* harfbuzzSkiaGetTable(hb_face_t* face, hb_tag_t tag, void* userData)
{
    SkFontID uniqueID = static_cast<SkFontID>(reinterpret_cast<uintptr_t>(userData));

    const size_t tableSize = SkFontHost::GetTableSize(uniqueID, tag);
    if (!tableSize)
        return 0;

    char* buffer = reinterpret_cast<char*>(fastMalloc(tableSize));
    if (!buffer)
        return 0;
    size_t actualSize = SkFontHost::GetTableData(uniqueID, tag, 0, tableSize, buffer);
    if (tableSize != actualSize) {
        fastFree(buffer);
        return 0;
    }

    return hb_blob_create(const_cast<char*>(buffer), tableSize,
                          HB_MEMORY_MODE_WRITABLE, buffer, fastFree);
}

static void destroyHarfBuzzFontData(void* userData)
{
    HarfBuzzFontData* hbFontData = reinterpret_cast<HarfBuzzFontData*>(userData);
    delete hbFontData;
}

hb_face_t* HarfBuzzNGFace::createFace()
{
    hb_face_t* face = hb_face_create_for_tables(harfbuzzSkiaGetTable, reinterpret_cast<void*>(m_platformData->uniqueID()), 0);
    ASSERT(face);
    return face;
}

hb_font_t* HarfBuzzNGFace::createFont()
{
    HarfBuzzFontData* hbFontData = new HarfBuzzFontData(m_glyphCacheForFaceCacheEntry);
    m_platformData->setupPaint(&hbFontData->m_paint);
    hb_font_t* font = hb_font_create(m_face);
    hb_font_set_funcs(font, harfbuzzSkiaGetFontFuncs(), hbFontData, destroyHarfBuzzFontData);
    float size = m_platformData->size();
    int scale = SkiaScalarToHarfbuzzPosition(size);
    hb_font_set_scale(font, scale, scale);
    hb_font_make_immutable(font);
    return font;
}

GlyphBufferAdvance HarfBuzzShaper::createGlyphBufferAdvance(float width, float height)
{
    return GlyphBufferAdvance(width, height);
}

} // namespace WebCore
