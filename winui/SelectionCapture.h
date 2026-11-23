#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace yiyi {

enum class SelectionStatus : std::uint32_t {
    Captured, Empty, Protected, SourceChanged, Unavailable, TooLong,
    ProbeFailed, TimedOut, ClipboardChanged, ClipboardUnavailable, CopyFailed
};

struct SelectionResult {
    SelectionStatus status;
    std::wstring text;
    HWND source;
    bool automatic;
};

class SelectionCapture {
public:
    SelectionCapture();
    ~SelectionCapture();
    SelectionCapture(SelectionCapture const&) = delete;
    SelectionCapture& operator=(SelectionCapture const&) = delete;

    bool request(HWND clipboardOwner, HWND source, POINT point, bool automatic);
    std::optional<SelectionResult> poll();
    void cancel();
    bool busy() const;
    static int runProbe(int argc, wchar_t* const* argv);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

}
