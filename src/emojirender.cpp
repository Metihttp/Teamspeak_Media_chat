#include "emojirender.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QSet>
#include <QThread>
#include <QVector>

#include <windows.h>

#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <condition_variable>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "emojicache.h"
#include "emojidata.h"

using Microsoft::WRL::ComPtr;

namespace emoji {

namespace {

constexpr wchar_t kFamily[]     = L"Segoe UI Emoji";
constexpr int     kMaxTextUnits = 64;
constexpr int     kMaxSide      = 512; // device pixels
constexpr qreal   kFill         = 0.90; // how much of the square the reference emoji (U+1F600) fills
constexpr int     kCheckSide    = 64;   // the size sequences are shaped at for the colour check
constexpr int     kMaxQueued    = 4096; // worker jobs; the oldest prefetches go first beyond this
constexpr int     kDeliverEvent = QEvent::User + 0x7452;

using D2D1CreateFactoryFn   = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

bool onGuiThread()
{
    const QCoreApplication* app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

// Collects the glyph runs a text layout would draw (to see how Segoe UI Emoji shapes a sequence).
// A stack object: the reference count only satisfies the interface.
class GlyphCollector final : public IDWriteTextRenderer
{
  public:
    struct Run {
        ComPtr<IDWriteFontFace> face;
        std::vector<UINT16>     glyphs;
        std::vector<FLOAT>      advances;
    };
    std::vector<Run> runs; // not a Qt container: ComPtr overloads operator&

    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    IFACEMETHODIMP_(ULONG) Release() override { return --m_refs; }

    IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override
    {
        *disabled = TRUE;
        return S_OK;
    }
    IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    {
        *transform = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override
    {
        *pixelsPerDip = 1.0f;
        return S_OK;
    }
    IFACEMETHODIMP DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* glyphRun, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                IUnknown*) override
    {
        if (!glyphRun || runs.size() >= 16)
            return S_OK;
        Run run;
        run.face = glyphRun->fontFace;
        for (UINT32 i = 0; i < glyphRun->glyphCount && i < 64; ++i) {
            run.glyphs.push_back(glyphRun->glyphIndices ? glyphRun->glyphIndices[i] : 0);
            run.advances.push_back(glyphRun->glyphAdvances ? glyphRun->glyphAdvances[i] : 1.0f);
        }
        runs.push_back(std::move(run));
        return S_OK;
    }
    IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

  private:
    ULONG m_refs = 1;
};

QVector<uint> codePoints(const QString& text)
{
    QVector<uint> out;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            out.append(QChar::surrogateToUcs4(c, text.at(i + 1)));
            ++i;
        } else {
            out.append(c.unicode());
        }
    }
    return out;
}

// The text engine: QPainter with the emoji font (what Qt can do; usually monochrome). GUI thread.
QImage paintText(const QString& text, int side)
{
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::TextAntialiasing);
    QFont font(QString::fromLatin1("Segoe UI Emoji"));
    font.setPixelSize(qMax(1, qRound(side * 0.72)));
    p.setFont(font);
    p.setPen(QColor(0x5c, 0x5e, 0x66));
    p.drawText(QRect(0, 0, side, side), Qt::AlignCenter, text);
    p.end();
    return image;
}

// DirectWrite + Direct2D for one thread: everything it holds is created, used and released there.
class ColorEngine
{
  public:
    ColorEngine() = default;
    ~ColorEngine() { release(); }
    ColorEngine(const ColorEngine&)            = delete;
    ColorEngine& operator=(const ColorEngine&) = delete;

    bool start()
    {
        m_d2dModule    = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        m_dwriteModule = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m_d2dModule || !m_dwriteModule)
            return fail("load d2d1.dll / dwrite.dll", HRESULT_FROM_WIN32(GetLastError()));
        const auto createD2d    = reinterpret_cast<D2D1CreateFactoryFn>(reinterpret_cast<void*>(GetProcAddress(m_d2dModule, "D2D1CreateFactory")));
        const auto createDwrite = reinterpret_cast<DWriteCreateFactoryFn>(reinterpret_cast<void*>(GetProcAddress(m_dwriteModule, "DWriteCreateFactory")));
        if (!createD2d || !createDwrite)
            return fail("find the factory functions", E_NOINTERFACE);
        HRESULT hr = createD2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(m_d2d.GetAddressOf()));
        if (FAILED(hr))
            return fail("D2D1CreateFactory", hr);
        // Isolated: its font caches are ours and go with it.
        ComPtr<IUnknown> unknown;
        hr = createDwrite(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), unknown.GetAddressOf());
        if (FAILED(hr) || FAILED(hr = unknown.As(&m_dwrite)))
            return fail("DWriteCreateFactory", hr);
        unknown.As(&m_dwrite2); // Windows 8.1+: colour glyph runs
        unknown.As(&m_dwrite4); // Windows 10 1607+: with SVG, bitmaps and COLR v1

        if (FAILED(hr = m_dwrite->GetSystemFontCollection(m_fonts.GetAddressOf(), FALSE)))
            return fail("GetSystemFontCollection", hr);
        UINT32 index  = 0;
        BOOL   exists = FALSE;
        if (FAILED(hr = m_fonts->FindFamilyName(kFamily, &index, &exists)) || !exists)
            return fail("find Segoe UI Emoji", hr);
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteFont>       font;
        if (FAILED(m_fonts->GetFontFamily(index, family.GetAddressOf()))
            || FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf()))
            || FAILED(font->CreateFontFace(m_face.GetAddressOf())))
            return fail("open Segoe UI Emoji", E_FAIL);
        m_faceKey = faceKey(m_face.Get());
        m_version = versionOf(font.Get());
        if (FAILED(m_dwrite->CreateCustomRenderingParams(1.8f, 0.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                         m_params.GetAddressOf())))
            m_params.Reset();

        calibrate();
        m_dc = CreateCompatibleDC(nullptr);
        if (!m_dc)
            return fail("CreateCompatibleDC", HRESULT_FROM_WIN32(GetLastError()));
        // Without a single colour glyph (a font without colour tables) nothing would be gained.
        UINT16       glyph = 0;
        const UINT32 grin  = 0x1F600;
        if (FAILED(m_face->GetGlyphIndices(&grin, 1, &glyph)) || glyph == 0 || !glyphHasColor(glyph))
            return fail("no colour glyphs in Segoe UI Emoji", E_FAIL);
        calibrateByInk();
        m_checkAdvance = advanceOf(QString::fromUcs4(&grin, 1), kCheckSide);
        return true;
    }

    QString error() const { return m_error; }
    QString version() const { return m_version; }

    void release()
    {
        m_brush.Reset();
        m_target.Reset();
        m_formats.clear();
        m_params.Reset();
        m_face.Reset();
        m_fonts.Reset();
        m_dwrite4.Reset();
        m_dwrite2.Reset();
        m_dwrite.Reset();
        m_d2d.Reset();
        if (m_dc) {
            if (m_oldBitmap)
                SelectObject(m_dc, m_oldBitmap);
            DeleteDC(m_dc);
            m_dc = nullptr;
        }
        m_oldBitmap = nullptr;
        if (m_bitmap)
            DeleteObject(m_bitmap);
        m_bitmap  = nullptr;
        m_bits    = nullptr;
        m_dibSide = 0;
        // Last: no object of theirs is left.
        if (m_dwriteModule)
            FreeLibrary(m_dwriteModule);
        if (m_d2dModule)
            FreeLibrary(m_d2dModule);
        m_dwriteModule = nullptr;
        m_d2dModule    = nullptr;
    }

    QImage render(const QString& text, int side)
    {
        if (!ensureTarget(side))
            return {};
        IDWriteTextFormat* format = formatFor(side);
        if (!format)
            return {};
        ComPtr<IDWriteTextLayout> layout;
        const auto*               utf16 = reinterpret_cast<const WCHAR*>(text.utf16());
        if (FAILED(m_dwrite->CreateTextLayout(utf16, static_cast<UINT32>(text.size()), format, 4.0f * side, 4.0f * side, layout.GetAddressOf())))
            return {};
        DWRITE_TEXT_METRICS metrics{};
        DWRITE_LINE_METRICS line{};
        UINT32              lines = 0;
        if (FAILED(layout->GetMetrics(&metrics)) || FAILED(layout->GetLineMetrics(&line, 1, &lines)) || lines < 1)
            return {};
        // Centred by its advance; the baseline where the reference emoji sits centred in the square.
        const FLOAT x = static_cast<FLOAT>((side - metrics.widthIncludingTrailingWhitespace) / 2.0);
        const FLOAT y = static_cast<FLOAT>(side * m_baseline - line.baseline);

        const RECT bounds{0, 0, side, side};
        if (FAILED(m_target->BindDC(m_dc, &bounds)))
            return {};
        m_target->BeginDraw();
        m_target->SetTransform(D2D1::Matrix3x2F::Identity());
        m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_target->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        const HRESULT hr = m_target->EndDraw();
        if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
            m_brush.Reset();
            m_target.Reset();
            return {};
        }
        if (FAILED(hr))
            return {};
        GdiFlush();
        QImage      image(side, side, QImage::Format_ARGB32_Premultiplied);
        const auto* source = static_cast<const uchar*>(m_bits);
        for (int row = 0; row < side; ++row)
            memcpy(image.scanLine(row), source + static_cast<size_t>(row) * m_dibSide * 4, static_cast<size_t>(side) * 4);
        return image;
    }

    // One colour picture of Segoe UI Emoji for the whole text (see emojirender.h).
    bool drawsInColor(const QString& text)
    {
        if (!m_face)
            return false;
        const QVector<uint> points = codePoints(text);
        QVector<uint>       significant;
        for (uint cp : points) {
            if (cp != 0xFE0F && cp != 0xFE0E)
                significant.append(cp);
        }
        if (significant.isEmpty())
            return false;
        UINT16       single = 0;
        const UINT32 first  = significant.first();
        if (FAILED(m_face->GetGlyphIndices(&first, 1, &single)))
            return false;
        if (significant.size() == 1 && single != 0 && glyphHasColor(single))
            return true;
        if (significant.size() == 1 && significant.size() == points.size())
            return false; // no selector that could pick another glyph

        // A sequence, as DirectWrite shapes it: every visible glyph from the emoji font and in colour, all
        // of them together no wider than one emoji (Windows draws families as a few narrow pieces; an
        // unknown ZWJ sequence or skin tone falls apart into full-width emoji), and not just the first code
        // point's own glyph (an unknown tag sequence shows its black flag and hides the rest).
        IDWriteTextFormat* format = formatFor(kCheckSide);
        if (!format)
            return false;
        ComPtr<IDWriteTextLayout> layout;
        const auto*               utf16 = reinterpret_cast<const WCHAR*>(text.utf16());
        if (FAILED(m_dwrite->CreateTextLayout(utf16, static_cast<UINT32>(text.size()), format, 1000.0f, 1000.0f, layout.GetAddressOf())))
            return false;
        GlyphCollector collector;
        if (FAILED(layout->Draw(nullptr, &collector, 0.0f, 0.0f)))
            return false;
        int    visible = 0;
        UINT16 glyph   = 0;
        qreal  width   = 0;
        for (const GlyphCollector::Run& run : collector.runs) {
            for (size_t i = 0; i < run.glyphs.size(); ++i) {
                const FLOAT advance = i < run.advances.size() ? run.advances[i] : 0.0f;
                if (advance <= 0.01f)
                    continue; // joiners, selectors, tags
                if (faceKey(run.face.Get()) != m_faceKey || run.glyphs[i] == 0 || !glyphHasColor(run.glyphs[i]))
                    return false; // a fallback font, a missing glyph, or a monochrome piece
                ++visible;
                glyph = run.glyphs[i];
                width += advance;
            }
        }
        if (visible == 0 || (visible == 1 && glyph == single))
            return false;
        return m_checkAdvance <= 0 || width <= m_checkAdvance * 1.2;
    }

  private:
    bool fail(const char* step, HRESULT hr)
    {
        m_error = QString::fromLatin1("%1 failed (0x%2)").arg(QString::fromLatin1(step)).arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        return false;
    }

    static QByteArray faceKey(IDWriteFontFace* face)
    {
        if (!face)
            return {};
        UINT32 files = 0;
        if (FAILED(face->GetFiles(&files, nullptr)) || files == 0 || files > 16)
            return {};
        std::vector<IDWriteFontFile*> list(files, nullptr);
        if (FAILED(face->GetFiles(&files, list.data())))
            return {};
        QByteArray  key  = QByteArray::number(face->GetIndex()) + ':';
        const void* data = nullptr;
        UINT32      size = 0;
        if (list.front() && SUCCEEDED(list.front()->GetReferenceKey(&data, &size)) && data)
            key += QByteArray(static_cast<const char*>(data), static_cast<int>(qMin<UINT32>(size, 4096)));
        for (IDWriteFontFile* file : list) {
            if (file)
                file->Release();
        }
        return key;
    }

    static QString versionOf(IDWriteFont* font)
    {
        ComPtr<IDWriteLocalizedStrings> strings;
        BOOL                            exists = FALSE;
        if (FAILED(font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_VERSION_STRINGS, strings.GetAddressOf(), &exists)) || !exists || !strings)
            return {};
        UINT32 length = 0;
        if (FAILED(strings->GetStringLength(0, &length)) || length == 0 || length > 200)
            return {};
        std::vector<WCHAR> buffer(length + 1, 0);
        if (FAILED(strings->GetString(0, buffer.data(), length + 1)))
            return {};
        return QString::fromWCharArray(buffer.data(), static_cast<int>(length)).trimmed();
    }

    // The glyph has colour layers (COLR v0 or v1, SVG or bitmaps). The per-glyph image format query
    // reports nothing for Segoe UI Emoji on Windows 11, so the colour translation is asked instead.
    bool glyphHasColor(UINT16 glyph)
    {
        FLOAT            advance = 1.0f;
        DWRITE_GLYPH_RUN run{};
        run.fontFace      = m_face.Get();
        run.fontEmSize    = 32.0f;
        run.glyphCount    = 1;
        run.glyphIndices  = &glyph;
        run.glyphAdvances = &advance;
        if (m_dwrite4) {
            const DWRITE_GLYPH_IMAGE_FORMATS base = DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_SVG | DWRITE_GLYPH_IMAGE_FORMATS_PNG
                                                    | DWRITE_GLYPH_IMAGE_FORMATS_JPEG | DWRITE_GLYPH_IMAGE_FORMATS_TIFF
                                                    | DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;
            for (const DWRITE_GLYPH_IMAGE_FORMATS formats : {base | DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE, base}) {
                ComPtr<IDWriteColorGlyphRunEnumerator1> layers;
                const HRESULT hr = m_dwrite4->TranslateColorGlyphRun(D2D1::Point2F(0, 0), &run, nullptr, formats, DWRITE_MEASURING_MODE_NATURAL, nullptr, 0,
                                                                     layers.GetAddressOf());
                if (SUCCEEDED(hr))
                    return true;
                if (hr == DWRITE_E_NOCOLOR)
                    return false;
                // E_INVALIDARG: a Windows that doesn't know paint trees yet; tried again without them.
            }
        }
        if (!m_dwrite2)
            return false;
        ComPtr<IDWriteColorGlyphRunEnumerator> layers;
        return SUCCEEDED(m_dwrite2->TranslateColorGlyphRun(0, 0, &run, nullptr, DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, layers.GetAddressOf()));
    }

    // Font size and baseline from the reference emoji's design metrics (U+1F600).
    void calibrate()
    {
        m_fontScale = 0.75;
        m_baseline  = 0.82;
        DWRITE_FONT_METRICS font{};
        m_face->GetMetrics(&font);
        UINT16       glyph = 0;
        const UINT32 grin  = 0x1F600;
        if (font.designUnitsPerEm == 0 || FAILED(m_face->GetGlyphIndices(&grin, 1, &glyph)) || glyph == 0)
            return;
        DWRITE_GLYPH_METRICS g{};
        if (FAILED(m_face->GetDesignGlyphMetrics(&glyph, 1, &g, FALSE)))
            return;
        const qreal em     = font.designUnitsPerEm;
        const qreal height = (static_cast<qreal>(g.advanceHeight) - g.topSideBearing - g.bottomSideBearing) / em;
        const qreal top    = (static_cast<qreal>(g.verticalOriginY) - g.topSideBearing) / em; // above the baseline
        if (height < 0.2 || height > 3.0)
            return;
        m_fontScale = kFill / height;                          // the ink is kFill of the square high
        m_baseline  = (1.0 - kFill) / 2.0 + top * m_fontScale; // ... and centred
    }

    // The colour layers can sit inside the outline box the design metrics describe: measured once on
    // U+1F600 and corrected, so the reference face fills kFill of the square, centred.
    void calibrateByInk()
    {
        constexpr int side   = 128;
        const UINT32  grin   = 0x1F600;
        const QImage  sample = render(QString::fromUcs4(&grin, 1), side);
        int           top    = side;
        int           bottom = -1;
        for (int y = 0; y < sample.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
            for (int x = 0; x < sample.width(); ++x) {
                if (qAlpha(line[x]) > 24) {
                    top    = qMin(top, y);
                    bottom = qMax(bottom, y);
                    break;
                }
            }
        }
        if (bottom <= top)
            return;
        const qreal scale = kFill * side / (bottom - top + 1);
        if (scale < 0.7 || scale > 1.5)
            return;
        const qreal ascent = (m_baseline * side - top) / (m_fontScale * side); // ink above the baseline, in ems
        m_fontScale *= scale;
        m_baseline = (1.0 - kFill) / 2.0 + ascent * m_fontScale;
        m_formats.clear();
    }

    qreal advanceOf(const QString& text, int side)
    {
        IDWriteTextFormat*        format = formatFor(side);
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS       metrics{};
        if (!format
            || FAILED(m_dwrite->CreateTextLayout(reinterpret_cast<const WCHAR*>(text.utf16()), static_cast<UINT32>(text.size()), format, 1000.0f, 1000.0f,
                                                 layout.GetAddressOf()))
            || FAILED(layout->GetMetrics(&metrics)))
            return 0;
        return metrics.widthIncludingTrailingWhitespace;
    }

    IDWriteTextFormat* formatFor(int side)
    {
        const auto known = m_formats.find(side);
        if (known != m_formats.end())
            return known->second.Get();
        if (m_formats.size() > 32)
            m_formats.clear();
        ComPtr<IDWriteTextFormat> format;
        const FLOAT               size = static_cast<FLOAT>(qMax(1.0, side * m_fontScale));
        if (FAILED(m_dwrite->CreateTextFormat(kFamily, m_fonts.Get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                              L"en-us", format.GetAddressOf())))
            return nullptr;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        m_formats.emplace(side, format);
        return format.Get();
    }

    bool ensureTarget(int side)
    {
        if (!m_dc || !m_d2d)
            return false;
        if (side > m_dibSide) {
            const int  dib = qMin(kMaxSide, ((side + 63) / 64) * 64);
            BITMAPINFO info{};
            info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth       = dib;
            info.bmiHeader.biHeight      = -dib; // top-down
            info.bmiHeader.biPlanes      = 1;
            info.bmiHeader.biBitCount    = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void*         bits           = nullptr;
            const HBITMAP bitmap         = CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bitmap || !bits) {
                if (bitmap)
                    DeleteObject(bitmap);
                return false;
            }
            const HGDIOBJ previous = SelectObject(m_dc, bitmap);
            if (!m_oldBitmap)
                m_oldBitmap = previous;
            if (m_bitmap)
                DeleteObject(m_bitmap);
            m_bitmap  = bitmap;
            m_bits    = bits;
            m_dibSide = dib;
        }
        if (!m_target) {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            if (FAILED(m_d2d->CreateDCRenderTarget(&props, m_target.GetAddressOf())))
                return false;
            m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            m_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (m_params)
                m_target->SetTextRenderingParams(m_params.Get());
            if (FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.31f, 0.34f, 1.0f), m_brush.GetAddressOf()))) {
                m_target.Reset();
                return false;
            }
        }
        return side <= m_dibSide;
    }

    HMODULE                                  m_d2dModule    = nullptr;
    HMODULE                                  m_dwriteModule = nullptr;
    ComPtr<ID2D1Factory>                     m_d2d;
    ComPtr<IDWriteFactory>                   m_dwrite;
    ComPtr<IDWriteFactory2>                  m_dwrite2;
    ComPtr<IDWriteFactory4>                  m_dwrite4;
    ComPtr<IDWriteFontCollection>            m_fonts;
    ComPtr<IDWriteFontFace>                  m_face;
    ComPtr<IDWriteRenderingParams>           m_params;
    ComPtr<ID2D1DCRenderTarget>              m_target;
    ComPtr<ID2D1SolidColorBrush>             m_brush;
    std::map<int, ComPtr<IDWriteTextFormat>> m_formats; // not a Qt container: ComPtr overloads operator&
    QByteArray                               m_faceKey;
    QString                                  m_version;
    QString                                  m_error;
    HDC                                      m_dc           = nullptr;
    HBITMAP                                  m_bitmap       = nullptr;
    HGDIOBJ                                  m_oldBitmap    = nullptr;
    void*                                    m_bits         = nullptr;
    int                                      m_dibSide      = 0;
    qreal                                    m_fontScale    = 0.75;
    qreal                                    m_baseline     = 0.82;
    qreal                                    m_checkAdvance = 0; // U+1F600's advance at kCheckSide
};

// ---- the worker thread ------------------------------------------------------------------------------

struct Job {
    QString text;
    int     side = 0;
};

struct Result {
    QString text;
    int     side = 0;
    QImage  image;
    bool    dropped = false; // left out of a full queue: asked for again when needed
};

// Draws queued pictures with its own colour engine (created and released on this thread). Newest
// requests first: what was asked for last is what is on screen now. Each picture is queued once: asking
// again (every paint of the picker asks for what it still lacks) only moves an urgent one to the front.
class RenderWorker final : public QThread
{
  public:
    explicit RenderWorker(QObject* notifier)
        : m_notifier(notifier)
    {
        setObjectName(QString::fromLatin1("tsmedia emoji"));
    }

    static QString keyOf(const Job& job) { return job.text + QChar(0x1f) + QString::number(job.side); }

    void enqueue(const Job& job, bool urgent)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping || m_failed)
                return;
            const QString key = keyOf(job);
            if (key == m_current || m_done.contains(key))
                return; // being drawn now, or drawn and on its way to the GUI thread
            const auto queued = m_index.find(key);
            if (queued != m_index.end()) {
                if (urgent)
                    m_jobs.splice(m_jobs.end(), m_jobs, queued.value()); // drawn next (iterators stay valid)
                return;
            }
            if (urgent) {
                m_jobs.push_back(job);
                m_index.insert(key, std::prev(m_jobs.end()));
            } else {
                m_jobs.push_front(job);
                m_index.insert(key, m_jobs.begin());
            }
            bool dropped = false;
            while (static_cast<int>(m_jobs.size()) > kMaxQueued) {
                Result gone;
                gone.text    = m_jobs.front().text;
                gone.side    = m_jobs.front().side;
                gone.dropped = true;
                m_index.remove(keyOf(m_jobs.front()));
                m_results.push_back(std::move(gone));
                m_jobs.pop_front();
                dropped = true;
            }
            if (dropped && !m_posted) {
                m_posted = true;
                QCoreApplication::postEvent(m_notifier, new QEvent(static_cast<QEvent::Type>(kDeliverEvent)));
            }
        }
        m_wake.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            m_jobs.clear();
            m_index.clear();
        }
        m_wake.notify_one();
    }

    std::vector<Result> takeResults()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Result>         out;
        out.swap(m_results);
        m_done.clear(); // in the cache now (or dropped): asked for again only when it has gone
        m_posted = false;
        return out;
    }

    bool failed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_failed;
    }

    qint64 drawn() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_drawn;
    }

  protected:
    void run() override
    {
        ColorEngine engine;
        if (!engine.start()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_failed = true;
            // Everything asked for so far is given back undrawn: the GUI thread draws it itself now (a
            // chat waiting for these pictures is told, instead of waiting for ever).
            for (const Job& job : m_jobs) {
                Result gone;
                gone.text    = job.text;
                gone.side    = job.side;
                gone.dropped = true;
                m_results.push_back(std::move(gone));
            }
            m_jobs.clear();
            m_index.clear();
            if (!m_stopping && !m_posted) {
                m_posted = true;
                QCoreApplication::postEvent(m_notifier, new QEvent(static_cast<QEvent::Type>(kDeliverEvent)));
            }
            return;
        }
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
                if (m_stopping)
                    break;
                job = m_jobs.back();
                m_jobs.pop_back();
                m_current = keyOf(job);
                m_index.remove(m_current);
            }
            Result result{job.text, job.side, engine.render(job.text, job.side)};
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done.insert(m_current);
            m_current.clear();
            ++m_drawn;
            if (m_stopping)
                break;
            m_results.push_back(std::move(result));
            if (!m_posted) {
                m_posted = true;
                // Posted to the notifier, which lives on the GUI thread; deleting it (after this thread has
                // been joined) drops a delivery still pending.
                QCoreApplication::postEvent(m_notifier, new QEvent(static_cast<QEvent::Type>(kDeliverEvent)));
            }
        }
        // engine releases everything here, on this thread
    }

  private:
    QObject*                                 m_notifier;
    mutable std::mutex                       m_mutex;
    std::condition_variable                  m_wake;
    std::list<Job>                           m_jobs;    // front: drawn last; back: drawn next
    QHash<QString, std::list<Job>::iterator> m_index;   // each queued picture once
    QString                                  m_current; // being drawn
    QSet<QString>                            m_done;    // drawn, not taken by the GUI thread yet
    std::vector<Result>                      m_results;
    bool                                     m_stopping = false;
    bool                                     m_posted   = false;
    bool                                     m_failed   = false;
    qint64                                   m_drawn    = 0;
};

struct State {
    Engine                       engine = Engine::None;
    std::unique_ptr<ColorEngine> color; // GUI thread
    ImageCache                   cache;
    QVector<qint8>               support; // by id: -1 not checked yet, 0 / 1
    QString                      error;   // why the colour engine didn't start
    ImageNotifier*               notifier = nullptr;
    RenderWorker*                worker   = nullptr;
    QSet<QString>                pending; // "text\x1fside" on its way
};

State* g_state        = nullptr;
bool   g_colorAllowed = true;
int    g_cacheEntries = 2048;
qint64 g_cacheBytes   = 24LL * 1024 * 1024;

State* state()
{
    if (!onGuiThread())
        return nullptr;
    if (!g_state) {
        g_state = new State;
        g_state->cache.setLimits(g_cacheEntries, g_cacheBytes);
        if (g_colorAllowed) {
            auto color = std::make_unique<ColorEngine>();
            if (color->start()) {
                g_state->color  = std::move(color);
                g_state->engine = Engine::Color;
            } else {
                g_state->error = color->error();
            }
        }
        if (g_state->engine == Engine::None)
            g_state->engine = Engine::Text; // the failed engine released everything it had
        g_state->notifier = new ImageNotifier;
    }
    return g_state;
}

int sideFor(int logicalPx, qreal dpr)
{
    const qreal ratio = dpr > 0 ? qBound(0.5, dpr, 8.0) : 1.0;
    return qBound(1, qRound(logicalPx * ratio), kMaxSide);
}

QString pendingKey(const QString& text, int side)
{
    return text + QChar(0x1f) + QString::number(side);
}

QImage withRatio(QImage image, int side, int logicalPx)
{
    if (!image.isNull())
        image.setDevicePixelRatio(static_cast<qreal>(side) / logicalPx);
    return image;
}

bool validRequest(const QString& text, int logicalPx)
{
    return !text.isEmpty() && text.size() <= kMaxTextUnits && logicalPx > 0;
}

} // namespace

// ---- ImageNotifier --------------------------------------------------------------------------------

ImageNotifier::ImageNotifier()
{
    setObjectName(QString::fromLatin1("tsmediaEmojiNotifier"));
}

bool ImageNotifier::event(QEvent* event)
{
    if (event->type() != static_cast<QEvent::Type>(kDeliverEvent))
        return QObject::event(event);
    State* s = g_state;
    if (!s || !s->worker)
        return true;
    for (Result& result : s->worker->takeResults()) {
        s->pending.remove(pendingKey(result.text, result.side));
        if (result.dropped)
            continue;
        // A picture the worker couldn't draw gets the text engine's, so nobody keeps asking for it.
        s->cache.insert(result.text, result.side, result.image.isNull() ? paintText(result.text, result.side) : result.image);
    }
    emit imagesReady();
    return true;
}

// ---- the API ------------------------------------------------------------------------------------------

Engine engine()
{
    State* s = state();
    return s ? s->engine : Engine::None;
}

bool hasColor()
{
    return engine() == Engine::Color;
}

QString engineDescription()
{
    State* s = state();
    if (!s)
        return QString::fromLatin1("not started");
    if (s->engine == Engine::Color) {
        const QString version = s->color->version();
        return version.isEmpty() ? QString::fromLatin1("DirectWrite + Direct2D, Segoe UI Emoji")
                                 : QString::fromLatin1("DirectWrite + Direct2D, Segoe UI Emoji %1").arg(version.left(40));
    }
    return s->error.isEmpty() ? QString::fromLatin1("QPainter text") : QString::fromLatin1("QPainter text (%1)").arg(s->error);
}

QImage render(const QString& text, int logicalPx, qreal dpr)
{
    State* s = state();
    if (!s || !validRequest(text, logicalPx))
        return {};
    const int side  = sideFor(logicalPx, dpr);
    QImage    image = s->cache.find(text, side);
    if (image.isNull()) {
        if (s->engine == Engine::Color)
            image = s->color->render(text, side);
        if (image.isNull())
            image = paintText(text, side);
        s->cache.insert(text, side, image);
    }
    return withRatio(image, side, logicalPx);
}

QImage render(int id, int logicalPx, qreal dpr)
{
    return isValid(id) ? render(text(id), logicalPx, dpr) : QImage();
}

QImage cachedImage(const QString& text, int logicalPx, qreal dpr)
{
    State* s = state();
    if (!s || !validRequest(text, logicalPx))
        return {};
    const int side = sideFor(logicalPx, dpr);
    return withRatio(s->cache.find(text, side), side, logicalPx);
}

QImage requestImage(const QString& text, int logicalPx, qreal dpr, bool urgent)
{
    State* s = state();
    if (!s || !validRequest(text, logicalPx))
        return {};
    const int side   = sideFor(logicalPx, dpr);
    QImage    cached = s->cache.find(text, side);
    if (!cached.isNull())
        return withRatio(cached, side, logicalPx);
    if (s->engine != Engine::Color)
        return render(text, logicalPx, dpr); // QPainter text is quick (and GUI-thread only)
    if (!s->worker) {
        s->worker = new RenderWorker(s->notifier);
        s->worker->start(QThread::LowPriority);
    }
    if (s->worker->failed())
        return render(text, logicalPx, dpr);
    const QString key = pendingKey(text, side);
    if (!s->pending.contains(key) || urgent) {
        s->pending.insert(key);
        s->worker->enqueue(Job{text, side}, urgent);
    }
    return {};
}

QImage requestImage(int id, int logicalPx, qreal dpr, bool urgent)
{
    return isValid(id) ? requestImage(text(id), logicalPx, dpr, urgent) : QImage();
}

void prefetch(const QVector<int>& ids, int logicalPx, qreal dpr)
{
    State* s = state();
    if (!s || s->engine != Engine::Color)
        return;
    // Each goes to the far end of the queue (drawn last), so the last id ends up there and the first id
    // is drawn first of these.
    for (int id : ids) {
        if (isValid(id))
            requestImage(text(id), logicalPx, dpr, false);
    }
}

ImageNotifier* notifier()
{
    State* s = state();
    return s ? s->notifier : nullptr;
}

bool drawsInColor(const QString& text)
{
    State* s = state();
    return s && s->engine == Engine::Color && !text.isEmpty() && text.size() <= kMaxTextUnits && s->color->drawsInColor(text);
}

bool supported(int id)
{
    State* s = state();
    if (!s || !isValid(id))
        return false;
    if (s->engine != Engine::Color)
        return true;
    if (s->support.size() != count())
        s->support = QVector<qint8>(count(), -1);
    qint8& known = s->support[id];
    if (known < 0)
        known = s->color->drawsInColor(text(id)) ? 1 : 0;
    return known == 1;
}

void shutdown()
{
    if (!g_state)
        return;
    if (g_state->worker) {
        g_state->worker->stop();
        g_state->worker->wait(); // its engine is released on its own thread before it ends
        delete g_state->worker;
        g_state->worker = nullptr;
    }
    delete g_state->notifier; // a delivery still posted to it goes with it
    g_state->notifier = nullptr;
    delete g_state; // the GUI thread's engine releases its COM and GDI objects, then the DLLs
    g_state = nullptr;
}

CacheInfo cacheInfo()
{
    CacheInfo info;
    if (g_state && onGuiThread()) {
        info.entries = g_state->cache.entries();
        info.bytes   = g_state->cache.bytes();
        info.hits    = g_state->cache.hits();
        info.misses  = g_state->cache.misses();
        info.pending = g_state->pending.size();
        info.drawn   = g_state->worker ? g_state->worker->drawn() : 0;
    }
    return info;
}

void setCacheLimits(int maxEntries, qint64 maxBytes)
{
    g_cacheEntries = qMax(1, maxEntries);
    g_cacheBytes   = qMax<qint64>(1, maxBytes);
    if (g_state && onGuiThread())
        g_state->cache.setLimits(g_cacheEntries, g_cacheBytes);
}

void setColorEngineAllowed(bool allowed)
{
    g_colorAllowed = allowed;
}

} // namespace emoji
