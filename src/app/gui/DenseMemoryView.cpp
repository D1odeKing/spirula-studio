#include "app/gui/DenseMemoryView.h"

#include "app/gui/Ui.h"
#include "i18n/catalog/Dense.h"
#include "i18n/catalog/Gui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace gui {
namespace {

namespace D = spirula::i18n::msg::dense;
namespace G = spirula::i18n::msg::gui;
using spirula::dense::MemoryRisk;

// The status strip's pressure colours (GuiApp.cpp, VramForecastView.cpp).
const ImVec4 kOk(0.35f, 0.85f, 0.45f, 1.0f);
const ImVec4 kWarn(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kErr(1.0f, 0.42f, 0.42f, 1.0f);
const ImVec4 kOthers(0.55f, 0.55f, 0.60f, 1.0f);
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr size_t kMaxSamples = 2048;

std::string gib(double bytes) {
    char s[32];
    std::snprintf(s, sizeof s, "%.2f", bytes / kGiB);
    return s;
}

ImVec4 risk_color(MemoryRisk r) { return r == MemoryRisk::High ? kErr : r == MemoryRisk::Medium ? kWarn : kOk; }

const spirula::i18n::Msg* risk_label(MemoryRisk r) {
    switch (r) {
        case MemoryRisk::Low:    return &D::memory_risk_low;
        case MemoryRisk::Medium: return &D::memory_risk_medium;
        case MemoryRisk::High:   return &D::memory_risk_high;
        default:                 return nullptr;
    }
}

ImU32 with_alpha(ImVec4 c, float a) { c.w = a; return ImGui::GetColorU32(c); }

void swatch(ImU32 col) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + h * 0.5f, p.y + h * 0.75f), col);
    ImGui::Dummy(ImVec2(h * 0.5f, h));
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
}

// Stacked from the left: this run solid, its growth up to the planned ceiling faint,
// then other programs grey. Whole pixels, one rounded outline: segments stay square.
void bar(float width, double ours, double others, double planned, double capacity, ImVec4 color) {
    const float h = std::floor(ImGui::GetTextLineHeight());
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 p(std::floor(cursor.x), std::floor(cursor.y));
    const float w = std::floor(width);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = ImGui::GetStyle().FrameRounding;
    auto x = [&](double bytes) { return p.x + std::round(w * (float)std::clamp(bytes / capacity, 0.0, 1.0)); };
    const double grown = std::max(planned, ours);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), r);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddRectFilled(ImVec2(x(grown), p.y), ImVec2(x(grown + others), p.y + h), with_alpha(kOthers, 0.45f));
    dl->AddRectFilled(ImVec2(x(ours), p.y), ImVec2(x(grown), p.y + h), with_alpha(color, 0.3f));
    dl->AddRectFilled(p, ImVec2(x(ours), p.y + h), ImGui::GetColorU32(color));
    dl->PopClipRect();
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_Border), r);
}

// One resource over the run: this run's line above what others hold, the planned
// ceiling dashed, capacity in red.
void chart(const std::vector<double>& t, const std::function<double(size_t)>& ours, const std::function<double(size_t)>& others,
           double planned, double capacity, double now_others) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float W = px(420.0f), H = px(110.0f);
    const ImVec2 r0 = ImGui::GetCursorScreenPos(), r1(r0.x + W, r0.y + H);
    ImGui::Dummy(ImVec2(W, H + st.ItemInnerSpacing.y));
    double y_max = std::max(capacity, now_others + planned);
    for (size_t i = 0; i < t.size(); ++i) y_max = std::max(y_max, ours(i) + others(i));
    y_max = std::max(1.0, y_max * 1.05);
    const double span = t.empty() ? 1.0 : std::max(1.0, t.back());
    auto X = [&](double s) { return r0.x + W * (float)std::clamp(s / span, 0.0, 1.0); };
    auto Y = [&](double b) { return r1.y - H * (float)std::clamp(b / y_max, 0.0, 1.0); };
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddRectFilled(r0, r1, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const ImVec4 run = ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
    // Feathered edges between adjacent slices would show as stripes.
    const ImDrawListFlags flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (size_t i = 1; i < t.size(); ++i)
        dl->AddQuadFilled(ImVec2(X(t[i - 1]), Y(0)), ImVec2(X(t[i - 1]), Y(others(i - 1))),
                          ImVec2(X(t[i]), Y(others(i))), ImVec2(X(t[i]), Y(0)), with_alpha(kOthers, 0.35f));
    dl->Flags = flags;
    for (size_t i = 1; i < t.size(); ++i) {
        dl->AddLine(ImVec2(X(t[i - 1]), Y(ours(i - 1) + others(i - 1))), ImVec2(X(t[i]), Y(ours(i) + others(i))),
                    ImGui::GetColorU32(run), px(2.0f));
    }
    const float ceiling = Y(now_others + planned), dash = px(4.0f);
    for (float x = r0.x; x < r1.x; x += 2 * dash)
        dl->AddLine(ImVec2(x, ceiling), ImVec2(std::min(r1.x, x + dash), ceiling), with_alpha(run, 0.9f), px(1.5f));
    if (capacity > 0) dl->AddLine(ImVec2(r0.x, Y(capacity)), ImVec2(r1.x, Y(capacity)), ImGui::GetColorU32(kErr), px(1.5f));
    dl->AddRect(r0, r1, grid);
}

}  // namespace

void DenseMemoryView::reset() {
    latest_ = {}; history_.clear();
    polled_at_ = started_at_ = -1.0; sequence_ = 0; have_ = false;
}

void DenseMemoryView::poll(const std::string& progress_dir) {
    const double now = ImGui::GetTime();
    if (polled_at_ >= 0.0 && now - polled_at_ < 0.5) return;
    polled_at_ = now;
    spirula::dense::MemoryReport report;
    if (!spirula::dense::read_memory_report(progress_dir, report) || report.sequence == sequence_) return;
    if (report.sequence < sequence_) { history_.clear(); started_at_ = -1.0; }   // a new run reuses the file
    if (started_at_ < 0.0) started_at_ = now;
    sequence_ = report.sequence; latest_ = report; have_ = true;
    if (history_.size() >= kMaxSamples) {   // halve the resolution rather than drop the start
        for (size_t i = 0; i < history_.size() / 2; ++i) history_[i] = history_[i * 2];
        history_.resize(history_.size() / 2);
    }
    history_.push_back({now - started_at_, (double)report.host_process, (double)report.device_ours, (double)report.device_others});
}

void DenseMemoryView::draw(float x0, float avail) {
    if (!have_) return;
    const auto& m = latest_;
    const MemoryRisk risk = spirula::dense::memory_risk(m);
    const double host_capacity = (double)m.host_process + (double)m.host_available;
    const double host_others = m.host_total > host_capacity ? m.host_total - host_capacity : 0.0;
    auto hover = [&] {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_NoSharedDelay) || !ImGui::BeginTooltip()) return;
        ImGui::PushTextWrapPos(px(440.0f));
        ui::TextDisabled(D::memory_help);
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        std::vector<double> t(history_.size());
        for (size_t i = 0; i < t.size(); ++i) t[i] = history_[i].seconds;
        ui::Text(D::memory_chart_host);
        chart(t, [&](size_t i) { return history_[i].host; }, [&](size_t) { return host_others; },
              (double)m.host_planned, (double)m.host_total, host_others);
        ui::TextColoredWrapped(risk_color(spirula::dense::memory_risk(spirula::dense::host_demand_fraction(m))), D::memory_summary,
                               {gib((double)m.host_process), gib((double)m.host_planned), gib(host_others), gib((double)m.host_total)});
        ui::Text(D::memory_chart_device);
        if (m.device_known && m.device_capacity) {
            chart(t, [&](size_t i) { return history_[i].device; }, [&](size_t i) { return history_[i].others; },
                  (double)m.device_planned, (double)m.device_capacity, (double)m.device_others);
            ui::TextColoredWrapped(risk_color(spirula::dense::memory_risk(spirula::dense::device_demand_fraction(m))), D::memory_summary,
                                   {gib((double)m.device_ours), gib((double)m.device_planned), gib((double)m.device_others),
                                    gib((double)m.device_capacity)});
        } else ui::TextDisabledWrapped(D::memory_device_waiting);
        const ImVec4 run = ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
        swatch(ImGui::GetColorU32(run)); ui::TextDisabled(D::memory_legend_run); ImGui::SameLine();
        swatch(with_alpha(run, 0.9f)); ui::TextDisabled(D::memory_legend_planned); ImGui::SameLine();
        swatch(with_alpha(kOthers, 0.35f)); ui::TextDisabled(G::vram_legend_others); ImGui::SameLine();
        swatch(ImGui::GetColorU32(kErr)); ui::TextDisabled(G::vram_legend_capacity);
        ImGui::EndTooltip();
    };

    const ImGuiStyle& st = ImGui::GetStyle();
    const std::string host_text = gib((double)m.host_process) + " / " + gib((double)m.host_total) + " GiB";
    const bool device = m.device_known && m.device_capacity;
    const std::string device_text = device ? gib((double)m.device_ours) + " / " + gib((double)m.device_capacity) + " GiB" : "";
    const auto* label = risk_label(risk);
    // Right-aligned like the training readout; the bars give way first when the row is short.
    float text_w = ImGui::CalcTextSize("RAM").x + ImGui::CalcTextSize(host_text.c_str()).x + st.ItemInnerSpacing.x;
    if (device) text_w += ImGui::CalcTextSize("VRAM").x + ImGui::CalcTextSize(device_text.c_str()).x +
                          st.ItemInnerSpacing.x + st.ItemSpacing.x * 2;
    if (label) text_w += ImGui::CalcTextSize(label->get()).x + st.ItemSpacing.x;
    const int bars = device ? 2 : 1;
    float bar_w = px(120.0f);
    float target = x0 + avail - text_w - bars * (bar_w + st.ItemInnerSpacing.x) - px(8.0f);
    if (target <= ImGui::GetCursorPosX()) { bar_w = 0.0f; target = x0 + avail - text_w - px(8.0f); }
    if (target > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(target);
    if (label) {
        ui::TextColored(risk_color(risk), *label);
        hover();
        ImGui::SameLine();
    }
    auto resource = [&](const char* name, const char* id, const std::string& text, double ours, double others, double planned,
                        double capacity, double fraction) {
        const ImVec4 color = risk_color(spirula::dense::memory_risk(fraction));
        ui::TextRaw(name);
        hover();
        ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        if (bar_w > 0 && capacity > 0) {
            bar(bar_w, ours, others, planned, capacity, color);
            ui::InvisibleButtonRaw(id, ImVec2(bar_w, ImGui::GetTextLineHeight()));
            hover();
            ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        }
        ui::TextDisabledRaw(text);
        hover();
    };
    resource("RAM", "##dense-ram", host_text, (double)m.host_process, host_others, (double)m.host_planned, (double)m.host_total,
             spirula::dense::host_demand_fraction(m));
    if (device) {
        ImGui::SameLine(0.0f, st.ItemSpacing.x * 2);
        resource("VRAM", "##dense-vram", device_text, (double)m.device_ours, (double)m.device_others, (double)m.device_planned,
                 (double)m.device_capacity, spirula::dense::device_demand_fraction(m));
    }
}

}  // namespace gui
