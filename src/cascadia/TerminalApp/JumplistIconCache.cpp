// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "JumplistIconCache.h"

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <til/hash.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace
{
    // Jump list icons are drawn small. 32px covers the sizes the shell asks for
    // on a 100-200% display without carrying four bitmaps per file.
    constexpr UINT IconSize = 32;
    // Leaves a little air around the glyph; drawing at the full box makes MDL2
    // shapes touch the edge and read as clipped.
    constexpr float FontSize = 24.0f;

    // MDL2/Fluent icon glyphs live in the Private Use Area. Anything else is a
    // real character -- an emoji or a letter -- and wants a text font.
    bool IsPrivateUseArea(std::wstring_view glyph) noexcept
    {
        return !glyph.empty() && glyph.front() >= 0xE000 && glyph.front() <= 0xF8FF;
    }

    // Picking a family that is not installed is not an error in DirectWrite --
    // it silently substitutes and you get tofu. So ask first.
    bool FamilyExists(IDWriteFontCollection* collection, const wchar_t* name) noexcept
    {
        UINT32 index{};
        BOOL exists{ FALSE };
        return collection && SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
    }

    const wchar_t* PickFontFamily(IDWriteFontCollection* collection, std::wstring_view glyph) noexcept
    {
        if (IsPrivateUseArea(glyph))
        {
            // Segoe Fluent Icons is the Windows 11 font; Segoe MDL2 Assets is
            // its Windows 10 predecessor and still present on 11. The glyph
            // values overlap for everything Terminal ships.
            if (FamilyExists(collection, L"Segoe Fluent Icons"))
            {
                return L"Segoe Fluent Icons";
            }
            return L"Segoe MDL2 Assets";
        }
        if (FamilyExists(collection, L"Segoe UI Emoji"))
        {
            return L"Segoe UI Emoji";
        }
        return L"Segoe UI";
    }

    // The .ico container, written by hand because WIC ships an ICO *decoder*
    // but no encoder. A single PNG-compressed 32x32 entry is all we need; PNG
    // inside ICO has been supported since Vista.
    std::vector<uint8_t> WrapPngInIco(const std::vector<uint8_t>& png)
    {
        std::vector<uint8_t> ico;
        ico.reserve(png.size() + 22);

        const auto push16 = [&](uint16_t v) {
            ico.push_back(static_cast<uint8_t>(v & 0xFF));
            ico.push_back(static_cast<uint8_t>(v >> 8));
        };
        const auto push32 = [&](uint32_t v) {
            ico.push_back(static_cast<uint8_t>(v & 0xFF));
            ico.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            ico.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            ico.push_back(static_cast<uint8_t>(v >> 24));
        };

        // ICONDIR
        push16(0); // reserved
        push16(1); // type: icon
        push16(1); // one image

        // ICONDIRENTRY
        ico.push_back(static_cast<uint8_t>(IconSize)); // width
        ico.push_back(static_cast<uint8_t>(IconSize)); // height
        ico.push_back(0); // palette size (0 = not paletted)
        ico.push_back(0); // reserved
        push16(1); // colour planes
        push16(32); // bits per pixel
        push32(gsl::narrow_cast<uint32_t>(png.size()));
        push32(6 + 16); // offset: past ICONDIR + one ICONDIRENTRY

        ico.insert(ico.end(), png.begin(), png.end());
        return ico;
    }

    // Renders `glyph` centred on a transparent 32x32 bitmap and returns it as PNG.
    std::vector<uint8_t> RasterizeToPng(std::wstring_view glyph, uint32_t foreground)
    {
        auto wic{ winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER) };

        wil::com_ptr<IWICBitmap> bitmap;
        THROW_IF_FAILED(wic->CreateBitmap(IconSize, IconSize, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put()));

        wil::com_ptr<ID2D1Factory> d2dFactory;
        THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.put()));

        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);

        wil::com_ptr<ID2D1RenderTarget> target;
        THROW_IF_FAILED(d2dFactory->CreateWicBitmapRenderTarget(bitmap.get(), props, target.put()));

        wil::com_ptr<IDWriteFactory> dwrite;
        THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown**>(dwrite.put())));

        wil::com_ptr<IDWriteFontCollection> systemFonts;
        LOG_IF_FAILED(dwrite->GetSystemFontCollection(systemFonts.put(), FALSE));

        wil::com_ptr<IDWriteTextFormat> format;
        THROW_IF_FAILED(dwrite->CreateTextFormat(
            PickFontFamily(systemFonts.get(), glyph),
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            FontSize,
            L"",
            format.put()));
        THROW_IF_FAILED(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        THROW_IF_FAILED(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));

        wil::com_ptr<ID2D1SolidColorBrush> brush;
        const auto colour = D2D1::ColorF(foreground & 0x00FFFFFF, 1.0f);

        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 0.0f));
        THROW_IF_FAILED(target->CreateSolidColorBrush(colour, brush.put()));
        // ENABLE_COLOR_FONT is what makes a colour emoji come out in colour
        // rather than as a monochrome outline. On a render target that does not
        // support it the flag is ignored, which degrades rather than fails.
        target->DrawTextW(
            glyph.data(),
            gsl::narrow_cast<UINT32>(glyph.size()),
            format.get(),
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(IconSize), static_cast<float>(IconSize)),
            brush.get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        THROW_IF_FAILED(target->EndDraw());

        // CreateStreamOnHGlobal rather than SHCreateMemStream: it is in ole32,
        // which this project already links, and needs no shlwapi dependency.
        wil::com_ptr<IStream> stream;
        THROW_IF_FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));

        wil::com_ptr<IWICBitmapEncoder> encoder;
        THROW_IF_FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        THROW_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

        wil::com_ptr<IWICBitmapFrameEncode> frame;
        THROW_IF_FAILED(encoder->CreateNewFrame(frame.put(), nullptr));
        THROW_IF_FAILED(frame->Initialize(nullptr));
        THROW_IF_FAILED(frame->WriteSource(bitmap.get(), nullptr));
        THROW_IF_FAILED(frame->Commit());
        THROW_IF_FAILED(encoder->Commit());

        STATSTG stat{};
        THROW_IF_FAILED(stream->Stat(&stat, STATFLAG_NONAME));
        const auto size = gsl::narrow<uint32_t>(stat.cbSize.QuadPart);

        std::vector<uint8_t> png(size);
        LARGE_INTEGER zero{};
        THROW_IF_FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
        ULONG read{};
        THROW_IF_FAILED(stream->Read(png.data(), size, &read));
        png.resize(read);
        return png;
    }
}

namespace TerminalApp
{
    JumplistIconCache::JumplistIconCache(std::wstring_view folder)
    try
    {
        // Beside settings.json rather than somewhere invented, so the cache
        // follows the slot: each package identity has its own settings folder,
        // and Dev and Test must not share rasterized icons.
        _dir = std::filesystem::path{ std::wstring_view{ CascadiaSettings::SettingsDirectory() } } / std::filesystem::path{ folder };

        // The jump list follows the system theme, and UISettings reports the
        // foreground the system expects text to be drawn in -- which is exactly
        // what a monochrome MDL2 glyph needs to be tinted with.
        const winrt::Windows::UI::ViewManagement::UISettings settings;
        const auto fg = settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
        _foreground = (static_cast<uint32_t>(fg.R) << 16) | (static_cast<uint32_t>(fg.G) << 8) | fg.B;

        std::error_code ec;
        std::filesystem::create_directories(_dir, ec);
        _ok = !ec;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        _ok = false;
    }

    std::optional<std::wstring> JumplistIconCache::Ensure(std::wstring_view glyph) noexcept
    try
    {
        if (!_ok || glyph.empty())
        {
            return std::nullopt;
        }

        // Keyed on glyph AND colour: the same glyph rendered for a light theme
        // is the wrong file for a dark one.
        const auto key = til::hash(glyph) ^ til::hash(_foreground);
        const auto name = fmt::format(FMT_COMPILE(L"{:016x}.ico"), key);
        const auto path = _dir / name;

        _used.emplace(name);

        if (std::filesystem::exists(path))
        {
            return path.wstring();
        }

        const auto ico = WrapPngInIco(RasterizeToPng(glyph, _foreground));

        // Write to a sibling temp file and move it into place, so two Terminals
        // rasterizing the same glyph at once cannot leave a torn .ico behind for
        // the shell to read.
        const auto temp = _dir / fmt::format(FMT_COMPILE(L"{:016x}.{}.tmp"), key, GetCurrentProcessId());
        {
            wil::unique_hfile file{ CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
            THROW_LAST_ERROR_IF(!file);
            DWORD written{};
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), ico.data(), gsl::narrow_cast<DWORD>(ico.size()), &written, nullptr));
        }
        THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING));

        return path.wstring();
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return std::nullopt;
    }

    void JumplistIconCache::PruneUnused() const noexcept
    try
    {
        if (!_ok)
        {
            return;
        }

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{ _dir, ec })
        {
            if (ec)
            {
                break;
            }
            const auto name = entry.path().filename().wstring();
            if (!_used.contains(name))
            {
                std::error_code ignored;
                std::filesystem::remove(entry.path(), ignored);
            }
        }
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
    }
}
