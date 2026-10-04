// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalSettings.h"
#include "winrt/Windows.UI.ViewManagement.h"
#include "../../types/inc/colorTable.hpp"

using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings;
using namespace Microsoft::Console::Utils;

namespace winrt::Microsoft::Terminal::Settings
{
    static std::tuple<Windows::UI::Xaml::HorizontalAlignment, Windows::UI::Xaml::VerticalAlignment> ConvertConvergedAlignment(Model::ConvergedAlignment alignment)
    {
        using Model::ConvergedAlignment;
        // extract horizontal alignment
        Windows::UI::Xaml::HorizontalAlignment horizAlign;
        switch (alignment & static_cast<ConvergedAlignment>(0x0F))
        {
        case ConvergedAlignment::Horizontal_Left:
            horizAlign = Windows::UI::Xaml::HorizontalAlignment::Left;
            break;
        case ConvergedAlignment::Horizontal_Right:
            horizAlign = Windows::UI::Xaml::HorizontalAlignment::Right;
            break;
        case ConvergedAlignment::Horizontal_Center:
        default:
            horizAlign = Windows::UI::Xaml::HorizontalAlignment::Center;
            break;
        }

        // extract vertical alignment
        Windows::UI::Xaml::VerticalAlignment vertAlign;
        switch (alignment & static_cast<ConvergedAlignment>(0xF0))
        {
        case ConvergedAlignment::Vertical_Top:
            vertAlign = Windows::UI::Xaml::VerticalAlignment::Top;
            break;
        case ConvergedAlignment::Vertical_Bottom:
            vertAlign = Windows::UI::Xaml::VerticalAlignment::Bottom;
            break;
        case ConvergedAlignment::Vertical_Center:
        default:
            vertAlign = Windows::UI::Xaml::VerticalAlignment::Center;
            break;
        }

        return { horizAlign, vertAlign };
    }

    // The patterns every enabled integration asked the terminal to scan output
    // for. They join the text-kind tooltip rules in ICoreSettings::TextPatterns,
    // which is what makes a bare "stith://..." printed as plain text hoverable
    // even though nothing marked it as a link.
    //
    // Read here rather than in _ApplyWindowSettings because the integrations
    // live on the app's global settings, not on the window's.
    static std::vector<winrt::hstring> IntegrationDetectPatterns(const Model::CascadiaSettings& appSettings)
    {
        std::vector<winrt::hstring> patterns;
        if (!appSettings)
        {
            return patterns;
        }

        const auto globals = appSettings.GlobalSettings();
        if (!globals)
        {
            return patterns;
        }

        // Integrations() never hands back null -- it materialises an empty map
        // when nothing is configured -- and an empty one means there is no
        // reason to walk the registry at all.
        const auto configured = globals.Integrations();
        if (!configured || configured.Size() == 0)
        {
            return patterns;
        }

        for (const auto& manifest : Model::IntegrationRegistry::All())
        {
            if (!manifest)
            {
                continue;
            }
            const auto id = manifest.Id();
            if (id.empty() || !configured.HasKey(id))
            {
                continue;
            }
            const auto entry = configured.Lookup(id);
            if (!entry || !entry.Enabled())
            {
                continue;
            }
            if (const auto declared = manifest.DetectPatterns())
            {
                for (const auto& pattern : declared)
                {
                    if (!pattern.empty())
                    {
                        patterns.push_back(pattern);
                    }
                }
            }
        }
        return patterns;
    }

    winrt::com_ptr<TerminalSettings> TerminalSettings::_CreateWithProfileCommon(const Model::CascadiaSettings& appSettings, const Model::WindowSettings& windowSettings, const Model::Profile& profile)
    {
        auto settings{ winrt::make_self<TerminalSettings>() };

        const auto globals = appSettings.GlobalSettings();
        settings->_ApplyProfileSettings(profile);
        settings->_ApplyWindowSettings(windowSettings);
        settings->_ApplyAppearanceSettings(profile.DefaultAppearance(), globals.ColorSchemes(), globals.CurrentTheme(windowSettings));

        // Appended after the rules, so the pattern ids the rules already own
        // (2 + index) keep meaning what they meant.
        if (auto extra = IntegrationDetectPatterns(appSettings); !extra.empty())
        {
            auto textPatterns = settings->_TextPatterns.value_or(nullptr);
            if (!textPatterns)
            {
                textPatterns = winrt::single_threaded_vector<winrt::hstring>();
            }
            for (const auto& pattern : extra)
            {
                textPatterns.Append(pattern);
            }
            settings->_TextPatterns = textPatterns;
        }

        return settings;
    }

    winrt::com_ptr<TerminalSettings> TerminalSettings::CreateForPreview(const Model::CascadiaSettings& appSettings, const Model::WindowSettings& windowSettings, const Model::Profile& profile)
    {
        const auto settings = _CreateWithProfileCommon(appSettings, windowSettings, profile);
        settings->_UseBackgroundImageForWindow = false;
        return settings;
    }

    // Method Description:
    // - Like CreateForPreview, but overlays the profile's unfocused appearance on top of the
    //   resolved focused settings so the preview reflects the unfocused appearance. If the profile
    //   has no unfocused appearance, this behaves like CreateForPreview (the focused appearance).
    winrt::com_ptr<TerminalSettings> TerminalSettings::CreateForPreviewUnfocused(const Model::CascadiaSettings& appSettings, const Model::WindowSettings& windowSettings, const Model::Profile& profile)
    {
        const auto settings = _CreateWithProfileCommon(appSettings, windowSettings, profile);
        settings->_UseBackgroundImageForWindow = false;
        if (const auto& unfocusedAppearance{ profile.UnfocusedAppearance() })
        {
            const auto globals = appSettings.GlobalSettings();
            settings->_ApplyAppearanceSettings(unfocusedAppearance, globals.ColorSchemes(), globals.CurrentTheme(windowSettings));
        }
        return settings;
    }

    // Method Description:
    // - Create a TerminalSettingsCreateResult for the provided profile guid. We'll
    //   use the guid to look up the profile that should be used to
    //   create these TerminalSettings. Then, we'll apply settings contained in the
    //   global and profile settings to the instance.
    // Arguments:
    // - appSettings: the set of settings being used to construct the new terminal
    // - profileGuid: the unique identifier (guid) of the profile
    // Return Value:
    // - A TerminalSettingsCreateResult, which contains a pair of TerminalSettings objects,
    //   one for when the terminal is focused and the other for when the terminal is unfocused
    TerminalSettingsCreateResult TerminalSettings::CreateWithProfile(const Model::CascadiaSettings& appSettings, const Model::WindowSettings& windowSettings, const Model::Profile& profile)
    {
        const auto settings = _CreateWithProfileCommon(appSettings, windowSettings, profile);

        winrt::com_ptr<TerminalSettings> child{ nullptr };
        if (const auto& unfocusedAppearance{ profile.UnfocusedAppearance() })
        {
            const auto globals = appSettings.GlobalSettings();
            child = winrt::make_self<TerminalSettings>();
            child->_parent = settings->get_strong();
            child->_ApplyAppearanceSettings(unfocusedAppearance, globals.ColorSchemes(), globals.CurrentTheme(windowSettings));
        }

        return TerminalSettingsCreateResult{ settings.get(), child.get() };
    }

    // Method Description:
    // - Create a TerminalSettings object for the provided newTerminalArgs. We'll
    //   use the newTerminalArgs to look up the profile that should be used to
    //   create these TerminalSettings. Then, we'll apply settings contained in the
    //   newTerminalArgs to the profile's settings, to enable customization on top
    //   of the profile's default values.
    // Arguments:
    // - appSettings: the set of settings being used to construct the new terminal
    // - newTerminalArgs: An object that may contain a profile name or GUID to
    //   actually use. If the Profile value is not a guid, we'll treat it as a name,
    //   and attempt to look the profile up by name instead.
    //   * Additionally, we'll use other values (such as Commandline,
    //     StartingDirectory) in this object to override the settings directly from
    //     the profile.
    // Return Value:
    // - A TerminalSettingsCreateResult object, which contains a pair of TerminalSettings
    //   objects. One for when the terminal is focused and one for when the terminal is unfocused.
    TerminalSettingsCreateResult TerminalSettings::CreateWithNewTerminalArgs(const Model::CascadiaSettings& appSettings,
                                                                             const Model::WindowSettings& windowSettings,
                                                                             const Model::NewTerminalArgs& newTerminalArgs)
    {
        const auto profile = appSettings.GetProfileForArgs(newTerminalArgs);
        auto settingsPair{ CreateWithProfile(appSettings, windowSettings, profile) };
        auto defaultSettings = settingsPair.DefaultSettings();

        if (newTerminalArgs)
        {
            if (const auto id = newTerminalArgs.SessionId(); id != winrt::guid{})
            {
                defaultSettings->_SessionId = id;
            }

            // Override commandline, starting directory if they exist in newTerminalArgs
            if (!newTerminalArgs.Commandline().empty())
            {
                if (!newTerminalArgs.AppendCommandLine())
                {
                    defaultSettings->_Commandline = newTerminalArgs.Commandline();
                }
                else
                {
                    defaultSettings->_Commandline = defaultSettings->Commandline() + L" " + newTerminalArgs.Commandline();
                }
            }
            if (!newTerminalArgs.StartingDirectory().empty())
            {
                defaultSettings->_StartingDirectory = newTerminalArgs.StartingDirectory();
            }
            if (!newTerminalArgs.TabTitle().empty())
            {
                defaultSettings->_StartingTitle = newTerminalArgs.TabTitle();
            }
            else
            {
                // There was no title, and no profile from which to infer the title.
                // Per GH#6776, promote the first component of the command line to the title.
                // This will ensure that the tab we spawn has a name (since it didn't get one from its profile!)
                if (newTerminalArgs.Profile().empty() && !newTerminalArgs.Commandline().empty())
                {
                    const std::wstring_view commandLine{ newTerminalArgs.Commandline() };
                    const auto start{ til::at(commandLine, 0) == L'"' ? 1 : 0 };
                    const auto terminator{ commandLine.find_first_of(start ? L'"' : L' ', start) }; // look past the first character if it starts with "
                    // We have to take a copy here; winrt::param::hstring requires a null-terminated string
                    const std::wstring firstComponent{ commandLine.substr(start, terminator - start) };
                    defaultSettings->_StartingTitle = winrt::hstring{ firstComponent };
                }
            }
            if (newTerminalArgs.TabColor())
            {
                defaultSettings->_StartingTabColor = winrt::Windows::Foundation::IReference<winrt::Microsoft::Terminal::Core::Color>{ static_cast<winrt::Microsoft::Terminal::Core::Color>(til::color{ newTerminalArgs.TabColor().Value() }) };
            }
            if (newTerminalArgs.SuppressApplicationTitle())
            {
                defaultSettings->_SuppressApplicationTitle = newTerminalArgs.SuppressApplicationTitle().Value();
            }
            if (!newTerminalArgs.ColorScheme().empty())
            {
                const auto schemes = appSettings.GlobalSettings().ColorSchemes();
                if (const auto& scheme = schemes.TryLookup(newTerminalArgs.ColorScheme()))
                {
                    defaultSettings->ApplyColorScheme(scheme);
                }
            }
            // Elevate on NewTerminalArgs is an optional value, so the default
            // value (null) doesn't override a profile's value. Note that
            // elevate:false in an already elevated terminal does nothing - the
            // profile will still be launched elevated.
            if (newTerminalArgs.Elevate())
            {
                defaultSettings->_Elevate = newTerminalArgs.Elevate().Value();
            }

            if (newTerminalArgs.ReloadEnvironmentVariables())
            {
                defaultSettings->_ReloadEnvironmentVariables = newTerminalArgs.ReloadEnvironmentVariables().Value();
            }
        }

        return settingsPair;
    }

    void TerminalSettings::_ApplyAppearanceSettings(const Model::IAppearanceConfig& appearance,
                                                    const Windows::Foundation::Collections::IMapView<winrt::hstring, Model::ColorScheme>& schemes,
                                                    const winrt::Microsoft::Terminal::Settings::Model::Theme currentTheme)
    {
        _CursorShape = appearance.CursorShape();
        _CursorHeight = appearance.CursorHeight();

        auto requestedTheme = currentTheme.RequestedTheme();
        if (requestedTheme == winrt::Windows::UI::Xaml::ElementTheme::Default)
        {
            requestedTheme = Model::Theme::IsSystemInDarkTheme() ?
                                 winrt::Windows::UI::Xaml::ElementTheme::Dark :
                                 winrt::Windows::UI::Xaml::ElementTheme::Light;
        }

        Model::ColorScheme activeScheme{ nullptr };
        switch (requestedTheme)
        {
        case winrt::Windows::UI::Xaml::ElementTheme::Light:
            if (const auto scheme = schemes.TryLookup(appearance.LightColorSchemeName()))
            {
                ApplyColorScheme(scheme);
                activeScheme = scheme;
            }
            break;
        case winrt::Windows::UI::Xaml::ElementTheme::Dark:
            if (const auto scheme = schemes.TryLookup(appearance.DarkColorSchemeName()))
            {
                ApplyColorScheme(scheme);
                activeScheme = scheme;
            }
            break;
        case winrt::Windows::UI::Xaml::ElementTheme::Default:
            // This shouldn't happen!
            break;
        }

        if (appearance.Foreground())
        {
            _DefaultForeground = til::color{ appearance.Foreground().Value() };
        }
        if (appearance.Background())
        {
            _DefaultBackground = til::color{ appearance.Background().Value() };
        }
        if (appearance.SelectionBackground())
        {
            _SelectionBackground = til::color{ appearance.SelectionBackground().Value() };
        }
        if (appearance.CursorColor())
        {
            _CursorColor = til::color{ appearance.CursorColor().Value() };
        }

        if (const auto backgroundImage{ appearance.BackgroundImagePath() })
        {
            _BackgroundImage = backgroundImage.Resolved();
        }

        if (const auto pixelShader{ appearance.PixelShaderPath() })
        {
            _PixelShaderPath = pixelShader.Resolved();
        }

        if (const auto pixelShaderImage{ appearance.PixelShaderImagePath() })
        {
            _PixelShaderImagePath = pixelShaderImage.Resolved();
        }

        _BackgroundImageOpacity = appearance.BackgroundImageOpacity();
        _BackgroundImageStretchMode = appearance.BackgroundImageStretchMode();
        std::tie(_BackgroundImageHorizontalAlignment, _BackgroundImageVerticalAlignment) = ConvertConvergedAlignment(appearance.BackgroundImageAlignment());

        _RetroTerminalEffect = appearance.RetroTerminalEffect();

        _IntenseIsBold = WI_IsFlagSet(appearance.IntenseTextStyle(), Microsoft::Terminal::Settings::Model::IntenseStyle::Bold);
        _IntenseIsBright = WI_IsFlagSet(appearance.IntenseTextStyle(), Microsoft::Terminal::Settings::Model::IntenseStyle::Bright);

        // Mullion: whether text too close to its background gets adjusted is
        // mostly a property of the palette -- a low-contrast scheme wants it and
        // a carefully tuned one does not. So a scheme that states a value wins
        // over profiles.defaults and the built-in default, and only a value set
        // on this profile's own appearance beats the scheme. HasX() reports the
        // appearance's own layer, not what it inherits.
        _AdjustIndistinguishableColors = appearance.AdjustIndistinguishableColors();
        if (activeScheme && !appearance.HasAdjustIndistinguishableColors())
        {
            if (const auto schemeMode = activeScheme.AdjustIndistinguishableColors())
            {
                _AdjustIndistinguishableColors = schemeMode.Value();
            }
        }
        _Opacity = appearance.Opacity();
        _UseAcrylic = appearance.UseAcrylic();
    }

    // Method Description:
    // - Apply Profile settings, as well as any colors from our color scheme, if we have one.
    // Arguments:
    // - profile: the profile settings we're applying
    // - schemes: a map of schemes to look for our color scheme in, if we have one.
    // Return Value:
    // - <none>
    void TerminalSettings::_ApplyProfileSettings(const Model::Profile& profile)
    {
        // Fill in the Terminal Setting's CoreSettings from the profile
        _HistorySize = profile.HistorySize();
        _SnapOnInput = profile.SnapOnInput();
        _AltGrAliasing = profile.AltGrAliasing();
        _AnswerbackMessage = profile.AnswerbackMessage();

        const auto fontInfo = profile.FontInfo();
        _FontFace = fontInfo.FontFace();
        _FontSize = fontInfo.FontSize();
        _FontWeight = fontInfo.FontWeight();
        _FontFeatures = fontInfo.FontFeatures();
        _FontAxes = fontInfo.FontAxes();
        _EnableBuiltinGlyphs = fontInfo.EnableBuiltinGlyphs();
        _EnableColorGlyphs = fontInfo.EnableColorGlyphs();
        _CellWidth = fontInfo.CellWidth();
        _CellHeight = fontInfo.CellHeight();
        _Padding = profile.Padding();

        _Commandline = profile.Commandline();

        _StartingDirectory = profile.EvaluatedStartingDirectory();

        // GH#2373: Use the tabTitle as the starting title if it exists; otherwise,
        // use the profile name
        _StartingTitle = !profile.TabTitle().empty() ? profile.TabTitle() : profile.Name();

        if (profile.SuppressApplicationTitle())
        {
            _SuppressApplicationTitle = profile.SuppressApplicationTitle();
        }

        _ScrollState = profile.ScrollState();

        _AntialiasingMode = profile.AntialiasingMode();

        if (profile.TabColor())
        {
            const til::color colorRef{ profile.TabColor().Value() };
            _TabColor = static_cast<winrt::Microsoft::Terminal::Core::Color>(colorRef);
        }

        if (const auto profileEnvVars{ profile.EnvironmentVariables() })
        {
            std::unordered_map<winrt::hstring, winrt::hstring> environmentVariables;
            for (const auto& [key, value] : profileEnvVars)
            {
                environmentVariables.emplace(key, value);
            }
            _EnvironmentVariables = winrt::single_threaded_map(std::move(environmentVariables)).GetView();
        }
        else
        {
            _EnvironmentVariables = std::nullopt;
        }

        _Elevate = profile.Elevate();
        _AutoMarkPrompts = Feature_ScrollbarMarks::IsEnabled() && profile.AutoMarkPrompts();
        _ShowMarks = Feature_ScrollbarMarks::IsEnabled() && profile.ShowMarks();

        _RightClickContextMenu = profile.RightClickContextMenu();
        _RepositionCursorWithMouse = profile.RepositionCursorWithMouse();
        _ReloadEnvironmentVariables = profile.ReloadEnvironmentVariables();
        _RainbowSuggestions = profile.RainbowSuggestions();
        _ForceVTInput = profile.ForceVTInput();
        _AllowKittyKeyboardMode = profile.AllowKittyKeyboardMode();
        _AllowVtChecksumReport = profile.AllowVtChecksumReport();
        _AllowVtClipboardWrite = profile.AllowVtClipboardWrite();
        _AllowOscNotifications = profile.AllowOscNotifications();
        _PathTranslationStyle = profile.PathTranslationStyle();
        _DragDropDelimiter = profile.DragDropDelimiter();
    }

    // Method Description:
    // - Applies appropriate settings from the globals into the TerminalSettings object.
    // Arguments:
    // - globalSettings: the global property values we're applying.
    // Return Value:
    // - <none>
    void TerminalSettings::_ApplyWindowSettings(const Model::WindowSettings& windowSettings) noexcept
    {
        _InitialRows = windowSettings.InitialRows();
        _InitialCols = windowSettings.InitialCols();

        _WordDelimiters = windowSettings.WordDelimiters();
        _CopyOnSelect = windowSettings.CopyOnSelect();
        _CopyFormatting = windowSettings.CopyFormatting();
        _FocusFollowMouse = windowSettings.FocusFollowMouse();
        _ScrollToZoom = windowSettings.ScrollToZoom();
        _HyperlinkClickable = windowSettings.HyperlinkClickable();
        _HyperlinkClickableKinds = windowSettings.HyperlinkClickableKinds();
        _HyperlinkPrimaryClickModifier = static_cast<Control::HyperlinkClickModifier>(windowSettings.HyperlinkPrimaryClickModifier());
        _HyperlinkPrimaryClickGesture = static_cast<Control::HyperlinkClickGesture>(windowSettings.HyperlinkPrimaryClickGesture());
        _HyperlinkPrimaryAction = windowSettings.HyperlinkPrimaryAction();
        _HyperlinkAlternativeClickModifier = static_cast<Control::HyperlinkClickModifier>(windowSettings.HyperlinkAlternativeClickModifier());
        _HyperlinkAlternativeClickGesture = static_cast<Control::HyperlinkClickGesture>(windowSettings.HyperlinkAlternativeClickGesture());
        _HyperlinkAlternativeAction = windowSettings.HyperlinkAlternativeAction();
        _HyperlinkTooltipMaxWidth = windowSettings.HyperlinkTooltipMaxWidth();
        _HyperlinkTooltipMaxHeight = windowSettings.HyperlinkTooltipMaxHeight();
        _HyperlinkTooltipNested = windowSettings.HyperlinkTooltipNested();
        _HyperlinkTooltipShowDelay = windowSettings.HyperlinkTooltipShowDelay();
        _HyperlinkTooltipHideDelay = windowSettings.HyperlinkTooltipHideDelay();
        _HyperlinkTooltipActions = windowSettings.HyperlinkTooltipActions();
        _HyperlinkTooltipButtons = windowSettings.HyperlinkTooltipButtons();
        _HyperlinkTooltipHint = windowSettings.HyperlinkTooltipHint();
        _HyperlinkTooltipShowRule = windowSettings.HyperlinkTooltipShowRule();
        _HyperlinkPreviewInPane = windowSettings.HyperlinkPreviewInPane();
        _HyperlinkIntegrationDisplayMode = static_cast<Control::HyperlinkIntegrationDisplayMode>(windowSettings.HyperlinkIntegrationDisplayMode());
        _HyperlinkActionPlacement = static_cast<Control::HyperlinkActionPlacement>(windowSettings.HyperlinkActionPlacement());

        if (const auto modelRules = windowSettings.HyperlinkTooltipRules())
        {
            auto controlRules = winrt::single_threaded_vector<HyperlinkTooltipRule>();
            for (const auto& modelRule : modelRules)
            {
                HyperlinkTooltipRule controlRule{};
                controlRule.Name(modelRule.Name());
                controlRule.Enabled(modelRule.Enabled());
                controlRule.Kind(static_cast<HyperlinkMatchKind>(modelRule.Kind()));
                controlRule.Integration(modelRule.Integration());
                controlRule.ShowPreview(modelRule.ShowPreview());
                controlRule.Schemes(modelRule.Schemes());
                controlRule.Pattern(modelRule.Pattern());
                controlRule.FileTypeGroup(static_cast<HyperlinkFileTypeGroup>(modelRule.FileTypeGroup()));
                controlRule.CustomExtensions(modelRule.CustomExtensions());
                controlRule.TooltipShowDelay(modelRule.TooltipShowDelay());
                controlRule.TooltipHideDelay(modelRule.TooltipHideDelay());
                controlRule.TooltipMaxWidth(modelRule.TooltipMaxWidth());
                // Which buttons this rule's card carries. An empty (or unset)
                // list inherits the window's hyperlink.tooltipButtons, and an
                // unset ShowInPane inherits hyperlink.previewInPane -- the
                // control resolves both, so both are mirrored as they are.
                controlRule.Buttons(modelRule.Buttons());
                controlRule.ShowInPane(modelRule.ShowInPane());
                // Empty inherits the window's hyperlink.primaryAction /
                // hyperlink.alternativeAction; "none" suppresses that click for
                // this rule. The control resolves both, so both pass through.
                controlRule.PrimaryAction(modelRule.PrimaryAction());
                controlRule.AlternativeAction(modelRule.AlternativeAction());

                auto controlActions = winrt::single_threaded_vector<HyperlinkTooltipAction>();
                if (const auto modelActions = modelRule.CustomActions())
                {
                    for (const auto& modelAction : modelActions)
                    {
                        HyperlinkTooltipAction controlAction{};
                        controlAction.Name(modelAction.Name());
                        controlAction.ActionId(modelAction.ActionId());
                        controlAction.Icon(modelAction.Icon() ? modelAction.Icon().Resolved() : hstring{});
                        controlActions.Append(controlAction);
                    }
                }
                controlRule.CustomActions(controlActions);

                controlRules.Append(controlRule);
            }
            _HyperlinkTooltipRules = controlRules;

            // Text-kind rules are scanned over the buffer by TerminalCore alongside
            // URL detection. Same list order as the rules, so a match's pattern id
            // (2 + index) can be mapped back if anything ever needs it.
            auto textPatterns = winrt::single_threaded_vector<hstring>();
            for (const auto& modelRule : modelRules)
            {
                if (modelRule.Enabled() && modelRule.Kind() == Model::HyperlinkMatchKind::Text && !modelRule.Pattern().empty())
                {
                    textPatterns.Append(modelRule.Pattern());
                }
            }
            _TextPatterns = textPatterns;
        }

        _ScrollToChangeOpacity = windowSettings.ScrollToChangeOpacity();
        _GraphicsAPI = windowSettings.GraphicsAPI();
        _DisablePartialInvalidation = windowSettings.DisablePartialInvalidation();
        _SoftwareRendering = windowSettings.SoftwareRendering();
        _TextMeasurement = windowSettings.TextMeasurement();
        _AmbiguousWidth = windowSettings.AmbiguousWidth();
        _DefaultInputScope = windowSettings.DefaultInputScope();
        _UseBackgroundImageForWindow = windowSettings.UseBackgroundImageForWindow();
        _TrimBlockSelection = windowSettings.TrimBlockSelection();
        _DetectURLs = windowSettings.DetectURLs();
        _EnableUnfocusedAcrylic = windowSettings.EnableUnfocusedAcrylic();
    }

    // Method Description:
    // - Apply a given ColorScheme's values to the TerminalSettings object.
    //      Sets the foreground, background, and color table of the settings object.
    // Arguments:
    // - scheme: the ColorScheme we are applying to the TerminalSettings object
    // Return Value:
    // - <none>
    void TerminalSettings::ApplyColorScheme(const Model::ColorScheme& scheme)
    {
        // If the scheme was nullptr, then just clear out the current color
        // settings.
        if (scheme == nullptr)
        {
            _DefaultForeground = std::nullopt;
            _DefaultBackground = std::nullopt;
            _SelectionBackground = std::nullopt;
            _CursorColor = std::nullopt;
            _ColorTable = std::nullopt;
        }
        else
        {
            _DefaultForeground = til::color{ scheme.Foreground() };
            _DefaultBackground = til::color{ scheme.Background() };
            _SelectionBackground = til::color{ scheme.SelectionBackground() };
            _CursorColor = til::color{ scheme.CursorColor() };

            const auto table = scheme.Table();
            std::array<winrt::Microsoft::Terminal::Core::Color, COLOR_TABLE_SIZE> colorTable{};
            std::transform(table.cbegin(), table.cend(), colorTable.begin(), [](auto&& color) {
                return static_cast<winrt::Microsoft::Terminal::Core::Color>(til::color{ color });
            });
            SetColorTable(colorTable);
        }
    }

    void TerminalSettings::SetColorTable(const std::array<winrt::Microsoft::Terminal::Core::Color, 16>& colors)
    {
        _ColorTable = colors;
    }

    void TerminalSettings::GetColorTable(winrt::com_array<Microsoft::Terminal::Core::Color>& table) noexcept
    {
        auto span = _getColorTableImpl();
        std::array<winrt::Microsoft::Terminal::Core::Color, COLOR_TABLE_SIZE> colorTable{};
        if (span.size() > 0)
        {
            std::copy(span.begin(), span.end(), colorTable.begin());
        }
        else
        {
            const auto campbellSpan = CampbellColorTable();
            std::transform(campbellSpan.begin(), campbellSpan.end(), colorTable.begin(), [](auto&& color) {
                return static_cast<winrt::Microsoft::Terminal::Core::Color>(til::color{ color });
            });
            span = colorTable;
        }

        table = winrt::com_array(span.begin(), span.end());
    }

    std::span<winrt::Microsoft::Terminal::Core::Color> TerminalSettings::_getColorTableImpl()
    {
        if (_ColorTable.has_value())
        {
            return std::span{ *_ColorTable };
        }
        if (_parent)
        {
            auto parentSpan = _parent->_getColorTableImpl();
            if (parentSpan.size() > 0)
            {
                return parentSpan;
            }
        }
        return {};
    }
}
