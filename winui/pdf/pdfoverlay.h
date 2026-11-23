#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace yiyi::pdf {

struct OverlayText {
    std::wstring text;
    std::wstring source;
    double x = 0, y = 0, width = 0, height = 0;
    double fontSize = 12;
    std::array<std::uint8_t, 3> color{};
    bool serif = false, bold = false, centered = false, code = false, fixed = false, styled = false;
};

struct OverlayLayoutData;

class OverlayLayout {
public:
    std::wstring error;
    bool fits() const { return static_cast<bool>(m_data); }

private:
    friend class OverlayRenderer;
    std::shared_ptr<const OverlayLayoutData> m_data;
};

// Layout coordinates are page-relative PDF points, with an origin at the top left.
class OverlayRenderer {
public:
    explicit OverlayRenderer(const std::filesystem::path& runtimeDirectory);
    ~OverlayRenderer();
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    OverlayLayout fit(const OverlayText& block, double pageWidth, double pageHeight) const;
    std::vector<std::uint8_t> render(double pageWidth, double pageHeight,
                                     const std::vector<OverlayLayout>& layouts) const;

private:
    struct Fonts;
    std::unique_ptr<Fonts> m_fonts;
};

}
