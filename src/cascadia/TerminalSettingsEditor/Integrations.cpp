// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Integrations.h"
#include "Integrations.g.cpp"
#include "IntegrationAccount.h"
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Automation.h>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // Returns the first PasswordBox in a subtree, or null.
    static Controls::PasswordBox _findPasswordBox(const DependencyObject& root)
    {
        if (!root)
        {
            return nullptr;
        }
        if (const auto box = root.try_as<Controls::PasswordBox>())
        {
            return box;
        }

        const auto count = Media::VisualTreeHelper::GetChildrenCount(root);
        for (auto i = 0; i < count; ++i)
        {
            if (const auto box = _findPasswordBox(Media::VisualTreeHelper::GetChild(root, i)))
            {
                return box;
            }
        }
        return nullptr;
    }

    // Walks up from a credential row's button to the panel that also holds its
    // PasswordBox. The rows come from a DataTemplate, so the box has no name in
    // the page's namescope and has to be found relative to the clicked button.
    // The walk stops well short of the ItemsControl, so it can never reach into
    // a different credential's row.
    static Controls::PasswordBox _credentialBoxFor(const IInspectable& sender)
    {
        auto current = sender.try_as<DependencyObject>();
        for (auto depth = 0; current && depth < 3; ++depth)
        {
            current = Media::VisualTreeHelper::GetParent(current);
            if (const auto box = _findPasswordBox(current))
            {
                return box;
            }
        }
        return nullptr;
    }

    // The box is deliberately not bound to PendingValue -- Password plus
    // PasswordChanged on one control is re-entrant -- so emptying it after a save
    // or a clear happens here. Nothing is lost: a secret in the vault can never be
    // read back to re-fill the box anyway.
    static void _clearCredentialBox(const IInspectable& sender)
    {
        if (const auto box = _credentialBoxFor(sender))
        {
            box.Password(hstring{});
        }
    }

    Integrations::Integrations()
    {
        InitializeComponent();
    }

    void Integrations::OnNavigatedTo(const NavigationEventArgs& e)
    {
        const auto args = e.Parameter().as<Editor::NavigateToPageArgs>();
        _ViewModel = args.ViewModel().as<Editor::IntegrationsViewModel>();
        BringIntoViewWhenLoaded(args.ElementToFocus());
        if (_ViewModel.CurrentIntegration()) _RefreshAccount();

        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "NavigatedToPage",
            TraceLoggingDescription("Event emitted when the user navigates to a page in the settings UI"),
            TraceLoggingValue("integrations", "PageId", "The identifier of the page that was navigated to"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    void Integrations::IntegrationNavigator_Click(const IInspectable& sender, const RoutedEventArgs& /*e*/)
    {
        const auto integration = sender.as<FrameworkElement>().Tag().as<Editor::IntegrationViewModel>();
        _ViewModel.CurrentIntegration(integration);
        RuleAddedInfoBar().IsOpen(false);
        _RefreshAccount();
    }

    void Integrations::CloseIntegrationEditor_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        ++_accountGeneration;
        _ViewModel.CurrentIntegration(nullptr);
        RuleAddedInfoBar().IsOpen(false);
    }

    // A PasswordBox's contents can't be read from the Save button's own data
    // context, so each box pushes what it holds into its view model as it's typed.
    // The value never leaves memory until Save writes it to the credential vault.
    void Integrations::CredentialPassword_Changed(const IInspectable& sender, const RoutedEventArgs& /*e*/)
    {
        const auto box = sender.as<Controls::PasswordBox>();
        if (const auto credential = box.Tag().try_as<Editor::IntegrationCredentialViewModel>())
        {
            credential.PendingValue(box.Password());
        }
    }

    void Integrations::SaveCredential_Click(const IInspectable& sender, const RoutedEventArgs& /*e*/)
    {
        const auto credential = sender.as<FrameworkElement>().Tag().as<Editor::IntegrationCredentialViewModel>();
        // PendingValue is what the box last pushed in; read it before the box is emptied.
        credential.Save(credential.PendingValue());
        _clearCredentialBox(sender);
        _RefreshAccount();
    }

    void Integrations::ClearCredential_Click(const IInspectable& sender, const RoutedEventArgs& /*e*/)
    {
        const auto credential = sender.as<FrameworkElement>().Tag().as<Editor::IntegrationCredentialViewModel>();
        credential.Clear();
        _clearCredentialBox(sender);
        _RefreshAccount();
    }

    void Integrations::AddMatcherAsRule_Click(const IInspectable& sender, const RoutedEventArgs& /*e*/)
    {
        const auto matcher = sender.as<FrameworkElement>().Tag().as<Editor::IntegrationMatcherViewModel>();
        matcher.AddAsRule();

        // The rule lands on the Link Tooltip page; say so rather than yanking the
        // user over there mid-configuration. The bar's action button navigates.
        RuleAddedInfoBar().IsOpen(true);
    }

    void Integrations::GoToLinkTooltip_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        RuleAddedInfoBar().IsOpen(false);
        _ViewModel.RequestNavigateToLinkTooltip();
    }
}

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    void Integrations::PreferredOwners_Loaded(const IInspectable& sender, const RoutedEventArgs&)
    {
        const auto host = sender.as<Controls::StackPanel>();
        const auto setting = host.Tag().as<Editor::IntegrationSettingViewModel>();
        if (!setting.IsOwnerList()) return;
        _ownerHost = make_weak(host); _ownerSetting = setting;
        _ownerValues = IntegrationAccounts::Owners(std::wstring_view{ setting.Value() });
        _RenderOwners();
    }

    bool Integrations::_SaveOwners()
    {
        std::wstring combined;
        std::set<std::wstring> seen;
        for (const auto& owner : _ownerValues)
        {
            if (owner.empty()) continue;
            const auto parsed = IntegrationAccounts::Owners(owner);
            if (parsed.size() != 1 || parsed.front() != owner) return false;
            auto lower = owner; std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
            if (!seen.insert(lower).second) return false;
            if (!combined.empty()) combined += L",";
            combined += owner;
        }
        _ownerSetting.Value(combined);
        return true;
    }

    // Pinned organizations: rows reordered by dragging (the ↑/↓ buttons stay
    // for the keyboard), followed by the organizations resolution reaches on its
    // own, which is everything account discovery found and nobody pinned.
    void Integrations::_RenderOwners()
    {
        const auto host = _ownerHost.get(); if (!host) return;
        host.Children().Clear(); host.Spacing(6);
        const auto weak = get_weak();
        Controls::TextBlock error; error.TextWrapping(TextWrapping::Wrap);
        const auto weakError = make_weak(error);

        auto rows = winrt::single_threaded_observable_vector<IInspectable>();
        for (size_t index = 0; index < _ownerValues.size(); ++index)
        {
            Controls::StackPanel row; row.Orientation(Controls::Orientation::Horizontal); row.Spacing(6);
            row.Tag(box_value(winrt::hstring{ _ownerValues[index] }));
            Controls::FontIcon grip; grip.Glyph(L"\xE76F"); grip.FontSize(14); grip.Opacity(0.6); grip.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(grip);
            Controls::TextBox input; input.Width(240); input.Text(_ownerValues[index]); input.PlaceholderText(L"Organization or account");
            Automation::AutomationProperties::SetName(input, L"Pinned organization " + std::to_wstring(index + 1));
            input.TextChanged([weak, index](const IInspectable& sender, auto&&) { if (const auto self = weak.get(); self && index < self->_ownerValues.size()) self->_ownerValues[index] = sender.as<Controls::TextBox>().Text(); });
            input.LostFocus([weak, weakError](auto&&, auto&&) { if (const auto self = weak.get()) if (const auto message = weakError.get()) message.Text(self->_SaveOwners() ? L"" : L"Enter one unique GitHub organization or account per row."); });
            row.Children().Append(input);
            for (const auto delta : { -1, 1, 0 })
            {
                Controls::Button button; button.Content(box_value(delta < 0 ? L"↑" : delta > 0 ? L"↓" : L"×"));
                Automation::AutomationProperties::SetName(button, delta < 0 ? L"Move organization up" : delta > 0 ? L"Move organization down" : L"Remove organization");
                button.IsEnabled(delta == 0 || (delta < 0 ? index > 0 : index + 1 < _ownerValues.size()));
                button.Click([weak, index, delta](auto&&, auto&&) {
                    if (const auto self = weak.get(); self && index < self->_ownerValues.size())
                    {
                        if (delta == 0) self->_ownerValues.erase(self->_ownerValues.begin() + index);
                        else { const auto next = static_cast<int64_t>(index) + delta; if (next >= 0 && next < static_cast<int64_t>(self->_ownerValues.size())) std::swap(self->_ownerValues[index], self->_ownerValues[static_cast<size_t>(next)]); }
                        self->_SaveOwners(); self->_RenderOwners();
                    }
                });
                row.Children().Append(button);
            }
            rows.Append(row);
        }

        if (rows.Size() > 0)
        {
            // An observable source is what makes CanReorderItems move rows; the
            // new order is read back from each row's Tag once the drop lands.
            Controls::ListView list;
            list.SelectionMode(Controls::ListViewSelectionMode::None);
            list.CanDragItems(true); list.CanReorderItems(true); list.AllowDrop(true);
            list.ItemsSource(rows);
            Automation::AutomationProperties::SetName(list, L"Pinned organizations, drag to reorder");
            list.DragItemsCompleted([weak, rows](auto&&, auto&&) {
                if (const auto self = weak.get())
                {
                    std::vector<std::wstring> reordered;
                    for (const auto& row : rows)
                    {
                        if (const auto panel = row.try_as<Controls::StackPanel>()) reordered.emplace_back(unbox_value_or<winrt::hstring>(panel.Tag(), L""));
                    }
                    // Typed but unsaved text is in _ownerValues, not in the Tags,
                    // so a drop applies the new order to the current values.
                    std::vector<std::wstring> values;
                    for (const auto& owner : reordered)
                    {
                        const auto it = std::find(self->_ownerValues.begin(), self->_ownerValues.end(), owner);
                        if (it != self->_ownerValues.end()) values.push_back(owner);
                    }
                    if (values.size() == self->_ownerValues.size()) self->_ownerValues = std::move(values);
                    self->_SaveOwners(); self->_RenderOwners();
                }
            });
            host.Children().Append(list);
        }
        host.Children().Append(error);

        // What resolution tries after the pinned rows, without being asked to.
        std::wstring implicit;
        for (const auto& org : _discoveredOwners)
        {
            if (std::any_of(_ownerValues.begin(), _ownerValues.end(), [&](const auto& item) { return _wcsicmp(item.c_str(), org.c_str()) == 0; })) continue;
            if (!implicit.empty()) implicit += L", ";
            implicit += org;
        }
        Controls::TextBlock reach; reach.TextWrapping(TextWrapping::Wrap); reach.Opacity(0.75);
        reach.Text(implicit.empty() ? L"Organizations your account can reach are tried after these automatically." :
                                      L"Then tried automatically: " + implicit + L".");
        host.Children().Append(reach);

        Controls::StackPanel buttons; buttons.Orientation(Controls::Orientation::Horizontal); buttons.Spacing(8);
        Controls::Button add; add.Content(box_value(L"Add organization"));
        add.Click([weak](auto&&, auto&&) { if (const auto self = weak.get()) { self->_ownerValues.emplace_back(); self->_RenderOwners(); } });
        buttons.Children().Append(add);
        Controls::Button discover; discover.Content(box_value(L"Pin a reachable organization"));
        Controls::MenuFlyout menu; discover.Flyout(menu);
        discover.Click([weak, menu](auto&&, auto&&) { if (const auto self = weak.get()) self->_RefreshAccount(true, menu); });
        buttons.Children().Append(discover); host.Children().Append(buttons);
    }

    void Integrations::RepoOwners_Loaded(const IInspectable& sender, const RoutedEventArgs&)
    {
        const auto host = sender.as<Controls::StackPanel>();
        const auto setting = host.Tag().as<Editor::IntegrationSettingViewModel>();
        if (!setting.IsRepoOwnerList()) return;
        _repoOwnerHost = make_weak(host); _repoOwnerSetting = setting;
        _RenderRepoOwners();
    }

    // Picks made on a link card. Read-only rows: a pick is made where the
    // ambiguity shows up, and only ever undone here.
    void Integrations::_RenderRepoOwners()
    {
        const auto host = _repoOwnerHost.get(); if (!host || !_repoOwnerSetting) return;
        host.Children().Clear(); host.Spacing(6);
        const auto pairs = ::Microsoft::Terminal::GitHubRepoOwners(std::wstring_view{ _repoOwnerSetting.Value() });
        if (pairs.empty())
        {
            Controls::TextBlock none; none.Opacity(0.75); none.TextWrapping(TextWrapping::Wrap);
            none.Text(L"None yet. Choosing an owner on a repo#number card adds it here.");
            host.Children().Append(none);
            return;
        }
        const auto weak = get_weak();
        for (const auto& [repo, owner] : pairs)
        {
            Controls::StackPanel row; row.Orientation(Controls::Orientation::Horizontal); row.Spacing(8);
            Controls::TextBlock text; text.VerticalAlignment(VerticalAlignment::Center); text.Width(260);
            text.Text(repo + L"  →  " + owner + L"/" + repo);
            row.Children().Append(text);
            Controls::Button remove; remove.Content(box_value(L"×"));
            Automation::AutomationProperties::SetName(remove, L"Forget the owner of " + repo);
            remove.Click([weak, repo](auto&&, auto&&) {
                if (const auto self = weak.get(); self && self->_repoOwnerSetting)
                {
                    auto remaining = ::Microsoft::Terminal::GitHubRepoOwners(std::wstring_view{ self->_repoOwnerSetting.Value() });
                    std::erase_if(remaining, [&](const auto& pair) { return ::Microsoft::Terminal::SameGitHubName(pair.first, repo); });
                    self->_repoOwnerSetting.Value(winrt::hstring{ ::Microsoft::Terminal::FormatGitHubRepoOwners(remaining) });
                    self->_RenderRepoOwners();
                }
            });
            row.Children().Append(remove);
            host.Children().Append(row);
        }
    }

    winrt::fire_and_forget Integrations::_RefreshAccount(bool discover, Controls::MenuFlyout menu)
    {
        try
        {
        const auto lifetime = get_strong();
        const auto generation = ++_accountGeneration;
        const auto integration = _ViewModel.CurrentIntegration();
        if (!integration) co_return;
        const auto weak = get_weak();
        const auto addText = [](const Controls::StackPanel& host, const std::wstring& value) {
            Controls::TextBlock text; text.Text(value); text.TextWrapping(TextWrapping::Wrap); host.Children().Append(text);
        };
        AccountHost().Children().Clear();
        if (!integration.Enabled()) { addText(AccountHost(), L"Integration disabled"); co_return; }
        if (!integration.IsConfigured()) { addText(AccountHost(), L"Complete the required settings and credentials"); co_return; }
        addText(AccountHost(), L"Checking connection…");
        if (menu) { menu.Items().Clear(); Controls::MenuFlyoutItem loading; loading.Text(L"Finding organizations…"); loading.IsEnabled(false); menu.Items().Append(loading); }
        IntegrationAccounts::Values settings, credentials;
        const std::wstring provider{ integration.Manifest().AccountProvider() };
        // GitHub always discovers: the organizations it finds are what repo#number
        // resolution tries after the pinned ones, and the owner list says which.
        discover = discover || provider == L"github";
        for (const auto& setting : integration.Settings()) settings[std::wstring{ setting.Key() }] = setting.Value();
        for (const auto& credential : integration.Credentials()) credentials[std::wstring{ credential.Key() }] = Model::IntegrationCredentialStore::Get(integration.Id(), credential.Key());
        const auto ui = winrt::apartment_context{};
        IntegrationAccounts::Result result;
        try
        {
            co_await winrt::resume_background();
            result = IntegrationAccounts::Check(provider, std::move(settings), std::move(credentials), discover);
        }
        catch (...) { result.message = L"Could not verify the connection. Try again."; }
        co_await ui;
        if (generation != _accountGeneration || _ViewModel.CurrentIntegration() != integration) co_return;
        try
        {
            AccountHost().Children().Clear();
            Controls::StackPanel row; row.Orientation(Controls::Orientation::Horizontal); row.Spacing(12);
            if (!result.avatar.empty())
            {
                Controls::PersonPicture picture; picture.Width(48); picture.Height(48);
                picture.ProfilePicture(Media::Imaging::BitmapImage{ Windows::Foundation::Uri{ result.avatar } }); row.Children().Append(picture);
            }
            Controls::StackPanel identity; identity.Spacing(2);
            if (!result.name.empty()) { Controls::TextBlock name; name.Text(result.name); name.FontSize(18); identity.Children().Append(name); }
            if (!result.login.empty()) addText(identity, result.login);
            addText(identity, result.message);
            if (!result.source.empty()) addText(identity, result.source);
            row.Children().Append(identity);
            Controls::Button refresh; refresh.Content(box_value(L"Refresh")); refresh.Click([weak](auto&&, auto&&) { if (const auto self = weak.get()) self->_RefreshAccount(); }); row.Children().Append(refresh);
            AccountHost().Children().Append(row);
            if (!result.discoveryMessage.empty()) addText(AccountHost(), result.discoveryMessage);
            if (discover && result.connected)
            {
                _discoveredOwners = result.organizations;
                _RenderOwners();
            }
            if (menu)
            {
                menu.Items().Clear();
                const auto chosen = IntegrationAccounts::Owners(std::wstring_view{ _ownerSetting.Value() });
                for (const auto& org : result.organizations)
                {
                    if (std::any_of(chosen.begin(), chosen.end(), [&](const auto& item) { return _wcsicmp(item.c_str(), org.c_str()) == 0; })) continue;
                    Controls::MenuFlyoutItem item; item.Text(org);
                    item.Click([weak, org](auto&&, auto&&) { if (const auto self = weak.get()) { self->_ownerValues.push_back(org); self->_SaveOwners(); self->_RenderOwners(); } });
                    menu.Items().Append(item);
                }
                if (!menu.Items().Size()) { Controls::MenuFlyoutItem item; item.Text(result.connected ? L"No additional organizations found" : L"Could not load organizations"); item.IsEnabled(false); menu.Items().Append(item); }
            }
        } CATCH_LOG();
        } CATCH_LOG();
    }
}

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    void Integrations::IntegrationEnabled_Toggled(const IInspectable& sender, const RoutedEventArgs&)
    {
        if (_ViewModel && _ViewModel.CurrentIntegration())
        {
            _ViewModel.CurrentIntegration().Enabled(sender.as<Controls::ToggleSwitch>().IsOn());
            _RefreshAccount();
        }
    }
}
