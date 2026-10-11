/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- HyperlinkPreview.h

Abstract:
- The data an integration produces for a hovered link, plus the x:Bind helpers
  the hyperlink card's preview template uses. See HyperlinkPreview.idl.

--*/
#pragma once

#include "HyperlinkPreviewField.g.h"
#include "HyperlinkPreviewComment.g.h"
#include "HyperlinkPreviewTab.g.h"
#include "HyperlinkPreviewActionField.g.h"
#include "HyperlinkPreviewActionOption.g.h"
#include "HyperlinkPreviewAction.g.h"
#include "HyperlinkActionResult.g.h"
#include "HyperlinkPreview.g.h"
#include "HyperlinkPreviewHelpers.g.h"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct HyperlinkPreviewField : HyperlinkPreviewFieldT<HyperlinkPreviewField>
    {
        HyperlinkPreviewField() = default;

        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(hstring, Value);
        WINRT_PROPERTY(HyperlinkPreviewFieldKind, Kind, HyperlinkPreviewFieldKind::Text);
        WINRT_PROPERTY(hstring, IconUri);
        WINRT_PROPERTY(hstring, Placement);
        WINRT_PROPERTY(hstring, LinkUri);
        WINRT_PROPERTY(hstring, Color);
        WINRT_PROPERTY(hstring, Group);
        WINRT_PROPERTY(hstring, GroupLabel);

        // WINRT_PROPERTY leaves the class `protected:`, so anything written by
        // hand after one has to say `public:` again or it vanishes from the
        // projection -- which shows up as an unrelated-looking link error.
    public:
        bool IsTitle() const noexcept { return _Kind == HyperlinkPreviewFieldKind::Title; }
        bool IsRow() const noexcept { return !IsTitle(); }
        bool IsBadge() const noexcept { return _Kind == HyperlinkPreviewFieldKind::Badge; }
        bool IsPlain() const noexcept { return !IsTitle() && !IsBadge(); }
        bool HasIcon() const noexcept { return !_IconUri.empty(); }
    };

    struct HyperlinkPreviewComment : HyperlinkPreviewCommentT<HyperlinkPreviewComment>
    {
        HyperlinkPreviewComment() = default;

        WINRT_PROPERTY(hstring, Author);
        WINRT_PROPERTY(hstring, AvatarUri);
        WINRT_PROPERTY(hstring, Body);
        WINRT_PROPERTY(hstring, Time);
    };

    struct HyperlinkPreviewTab : HyperlinkPreviewTabT<HyperlinkPreviewTab>
    {
        HyperlinkPreviewTab() = default;

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(HyperlinkPreviewTabKind, Kind, HyperlinkPreviewTabKind::Fields);
        WINRT_PROPERTY(hstring, Body);
        WINRT_PROPERTY(hstring, Format);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Control::HyperlinkPreviewComment>, Comments);
        WINRT_PROPERTY(bool, Pending, false);
    };

    struct HyperlinkPreviewActionField : HyperlinkPreviewActionFieldT<HyperlinkPreviewActionField>
    {
        HyperlinkPreviewActionField() = default;

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(bool, Required, false);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<hstring>, Options);
    };

    struct HyperlinkPreviewActionOption : HyperlinkPreviewActionOptionT<HyperlinkPreviewActionOption>
    {
        HyperlinkPreviewActionOption() = default;

        WINRT_PROPERTY(hstring, Id);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(hstring, Badge);
        WINRT_PROPERTY(hstring, Color);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Control::HyperlinkPreviewActionField>, Fields);

    public:
        // The card only puts a form in front of the user when the far end says
        // it will refuse the change without one.
        //
        // noexcept because x:Bind reads it during layout: walking a WinRT
        // collection is a COM call and can fail, and "no form" is a far better
        // answer there than an exception escaping into the XAML generated code.
        bool NeedsFields() const noexcept
        try
        {
            if (!_Fields)
            {
                return false;
            }
            for (const auto& field : _Fields)
            {
                if (field && field.Required())
                {
                    return true;
                }
            }
            return false;
        }
        catch (...)
        {
            return false;
        }
    };

    struct HyperlinkPreviewAction : HyperlinkPreviewActionT<HyperlinkPreviewAction>
    {
        HyperlinkPreviewAction() = default;

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Control::HyperlinkPreviewActionOption>, Options);
    };

    struct HyperlinkActionResult : HyperlinkActionResultT<HyperlinkActionResult>
    {
        HyperlinkActionResult() = default;

        WINRT_PROPERTY(bool, Ok, false);
        WINRT_PROPERTY(hstring, Error);
        WINRT_PROPERTY(hstring, UndoChoiceId);
        WINRT_PROPERTY(hstring, UndoLabel);
    };

    struct HyperlinkPreview : HyperlinkPreviewT<HyperlinkPreview>
    {
        HyperlinkPreview() = default;

        WINRT_PROPERTY(hstring, IntegrationId);
        WINRT_PROPERTY(hstring, IntegrationName);
        WINRT_PROPERTY(hstring, IntegrationIcon);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Control::HyperlinkPreviewField>, Fields);
        WINRT_PROPERTY(hstring, Html);
        WINRT_PROPERTY(hstring, DataJson);
        WINRT_PROPERTY(hstring, EmbedUrl);
        WINRT_PROPERTY(hstring, Error);
        WINRT_PROPERTY(hstring, ResolvedUri);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Control::HyperlinkPreviewTab>, Tabs);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Control::HyperlinkPreviewAction>, Actions);
        WINRT_PROPERTY(hstring, SourceText);
        WINRT_PROPERTY(hstring, FilePath);
        WINRT_PROPERTY(hstring, FileKind);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Control::HyperlinkPreviewTab>, FileSections);
    };

    struct HyperlinkPreviewHelpers
    {
        HyperlinkPreviewHelpers() = default;

        static bool HasNestedPreview();
        static Windows::UI::Xaml::Controls::RichTextBlock CreateTextView(const hstring& text, const Control::IHyperlinkPreviewProvider& provider, const Control::IControlSettings& settings, bool compact, int32_t depth);
        static void AttachLinkTooltips(const Windows::UI::Xaml::FrameworkElement& root, const Control::IHyperlinkPreviewProvider& provider, const Control::IControlSettings& settings, bool compact, int32_t depth);

        static Windows::UI::Xaml::FrameworkElement CreateFileView(const Control::HyperlinkPreview& preview, bool compact);
        static Windows::UI::Xaml::Media::Brush BadgeBrush(const hstring& color);
        static Windows::UI::Xaml::Media::ImageSource ImageFromUri(const hstring& uri);
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(HyperlinkPreviewField);
    BASIC_FACTORY(HyperlinkPreviewComment);
    BASIC_FACTORY(HyperlinkPreviewTab);
    BASIC_FACTORY(HyperlinkPreviewActionField);
    BASIC_FACTORY(HyperlinkPreviewActionOption);
    BASIC_FACTORY(HyperlinkPreviewAction);
    BASIC_FACTORY(HyperlinkActionResult);
    BASIC_FACTORY(HyperlinkPreview);

    struct HyperlinkPreviewHelpers : HyperlinkPreviewHelpersT<HyperlinkPreviewHelpers, implementation::HyperlinkPreviewHelpers>
    {
    };
}
