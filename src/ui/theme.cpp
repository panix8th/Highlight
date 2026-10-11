#include "theme.h"

#include "../common/paths.h"
#include "../common/strings.h"

#include <filesystem>
#include <string>

namespace ui {

Fonts fonts;
float dpi_scale = 1.0f;

static ImVector<ImWchar> g_ranges;

static ImFont *load(const char *file, float size)
{
	ImGuiIO &io = ImGui::GetIO();
	std::filesystem::path p = hl::paths::fonts_dir() / file;
	std::string utf8 = hl::narrow(p.wstring());
	ImFontConfig cfg;
	cfg.OversampleH = 2;
	cfg.OversampleV = 1;
	cfg.PixelSnapH = true;
	ImFont *f = nullptr;
	if (std::filesystem::exists(p))
		f = io.Fonts->AddFontFromFileTTF(utf8.c_str(), size * dpi_scale, &cfg, g_ranges.Data);
	if (!f) {
		/* Fall back to Segoe UI so the app still works from a broken install. */
		const char *fallback = "C:\\Windows\\Fonts\\segoeui.ttf";
		if (std::filesystem::exists(fallback))
			f = io.Fonts->AddFontFromFileTTF(fallback, size * dpi_scale, &cfg, g_ranges.Data);
		else
			f = io.Fonts->AddFontDefault();
	}
	return f;
}

void setup_theme(float scale)
{
	dpi_scale = scale;
	ImGuiIO &io = ImGui::GetIO();
	io.Fonts->Clear();

	if (g_ranges.empty()) {
		ImFontGlyphRangesBuilder b;
		b.AddRanges(io.Fonts->GetGlyphRangesDefault());
		b.AddText("·—–×≈→←…“”‘’•✓▶■");
		b.BuildRanges(&g_ranges);
	}

	fonts.body = load("Archivo-Regular.ttf", 14.0f);
	fonts.body_medium = load("Archivo-Medium.ttf", 14.0f);
	fonts.body_semibold = load("Archivo-SemiBold.ttf", 14.0f);
	fonts.body_bold = load("Archivo-Bold.ttf", 15.0f);
	fonts.small = load("Archivo-Regular.ttf", 12.5f);
	fonts.small_semibold = load("Archivo-SemiBold.ttf", 12.5f);
	fonts.h1 = load("ArchivoExpanded-ExtraBold.ttf", 30.0f);
	fonts.h2 = load("Archivo-Bold.ttf", 16.0f);
	fonts.title = load("ArchivoExpanded-ExtraBold.ttf", 18.0f);
	fonts.wordmark = load("ArchivoExpanded-ExtraBold.ttf", 13.0f);
	fonts.big = load("ArchivoExpanded-ExtraBold.ttf", 38.0f);
	fonts.mono = load("JetBrainsMono-Medium.ttf", 13.0f);
	fonts.mono_small = load("JetBrainsMono-Medium.ttf", 11.0f);
	fonts.mono_bold = load("JetBrainsMono-Bold.ttf", 13.0f);
	io.FontDefault = fonts.body;

	ImGuiStyle &s = ImGui::GetStyle();
	s = ImGuiStyle();
	s.WindowPadding = ImVec2(0, 0);
	s.WindowRounding = 0;
	s.WindowBorderSize = 0;
	s.ChildRounding = 14;
	s.ChildBorderSize = 1;
	s.PopupRounding = 12;
	s.PopupBorderSize = 1;
	s.FrameRounding = 10;
	s.FramePadding = ImVec2(12, 10);
	s.ItemSpacing = ImVec2(10, 10);
	s.ItemInnerSpacing = ImVec2(8, 6);
	s.ScrollbarSize = 10;
	s.ScrollbarRounding = 6;
	s.GrabMinSize = 14;
	s.GrabRounding = 7;
	s.TabRounding = 8;
	s.SelectableTextAlign = ImVec2(0, 0.5f);
	s.ScaleAllSizes(scale);

	ImVec4 *c = s.Colors;
	auto v = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
	c[ImGuiCol_Text] = v(col::text);
	c[ImGuiCol_TextDisabled] = v(col::dim);
	c[ImGuiCol_WindowBg] = v(col::bg0);
	c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_PopupBg] = v(col::bg1);
	c[ImGuiCol_Border] = v(col::line);
	c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_FrameBg] = v(col::bg0);
	c[ImGuiCol_FrameBgHovered] = v(col::bg3);
	c[ImGuiCol_FrameBgActive] = v(col::bg3);
	c[ImGuiCol_TitleBg] = v(col::bg1);
	c[ImGuiCol_TitleBgActive] = v(col::bg1);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = v(col::line);
	c[ImGuiCol_ScrollbarGrabHovered] = v(col::key_border);
	c[ImGuiCol_ScrollbarGrabActive] = v(col::ring);
	c[ImGuiCol_CheckMark] = v(col::accent);
	c[ImGuiCol_SliderGrab] = v(col::accent);
	c[ImGuiCol_SliderGrabActive] = v(col::accent_text);
	c[ImGuiCol_Button] = v(col::bg3);
	c[ImGuiCol_ButtonHovered] = v(col::line);
	c[ImGuiCol_ButtonActive] = v(col::key_border);
	c[ImGuiCol_Header] = v(col::bg3);
	c[ImGuiCol_HeaderHovered] = v(col::bg3);
	c[ImGuiCol_HeaderActive] = v(col::line);
	c[ImGuiCol_Separator] = v(col::line2);
	c[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 0.35f, 0.12f, 0.35f);
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.024f, 0.03f, 0.72f);
	c[ImGuiCol_NavCursor] = v(col::accent);
}

} // namespace ui
