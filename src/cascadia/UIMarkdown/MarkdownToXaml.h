// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include "pch.h"
#include <cmark.h>

struct MarkdownToXaml
{
public:
    static winrt::Windows::UI::Xaml::Controls::RichTextBlock Convert(std::string_view markdownText, const winrt::hstring& baseUrl, size_t depth = 0);

private:
    MarkdownToXaml(const winrt::hstring& baseUrl);

    winrt::hstring _baseUri{ L"" };

    winrt::Windows::UI::Xaml::Controls::RichTextBlock _root{};
    winrt::Windows::UI::Xaml::Documents::Run _lastRun{ nullptr };
    winrt::Windows::UI::Xaml::Documents::Span _lastSpan{ nullptr };
    winrt::Windows::UI::Xaml::Documents::Paragraph _lastParagraph{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Image _lastImage{ nullptr };

    int _indent = 0;
    int _blockQuoteDepth = 0;

    // One entry per list we are inside, innermost last. Tightness decides the
    // gap between items; the counter numbers an ordered list.
    struct ListState
    {
        bool ordered{ false };
        int next{ 1 };
        bool tight{ true };
    };
    std::vector<ListState> _lists;

    // Set by a list item just before it writes its marker, so only the
    // paragraph that carries a marker gets the hanging indent that makes room
    // for it. A second paragraph in the same item lines up with the item's
    // text instead.
    double _pendingMarkerWidth{ 0 };

    // The size body text is laid out at. Every gap and heading size below is a
    // multiple of it, the way GitHub's stylesheet is written in em.
    double _baseFontSize{ 14 };

    winrt::Windows::UI::Xaml::Documents::Paragraph _CurrentParagraph();
    winrt::Windows::UI::Xaml::Documents::Run _CurrentRun();
    winrt::Windows::UI::Xaml::Documents::Span _CurrentSpan();
    winrt::Windows::UI::Xaml::Documents::Run _NewRun();
    void _EndRun() noexcept;
    void _EndSpan() noexcept;
    void _EndParagraph() noexcept;

    winrt::Windows::UI::Xaml::Controls::TextBlock _makeDefaultTextBlock();

    void _RenderNode(cmark_node* node, cmark_event_type ev_type);
    void _AppendBlock(const winrt::Windows::UI::Xaml::FrameworkElement& element);
    void _AppendRule(double thickness, double topGap, double bottomGap);
    void _SetLastBlockBottom(double bottom);
    void _TrimOuterMargins();
    double _paragraphBottomGap() const noexcept;
};
