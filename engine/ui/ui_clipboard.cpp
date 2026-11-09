#include "ui/ui_clipboard.h"

namespace gryce_engine::ui {

namespace {
ClipboardGetFn get_fn = nullptr;
ClipboardSetFn set_fn = nullptr;
} // namespace

void set_clipboard_callbacks(ClipboardGetFn get, ClipboardSetFn set) {
    get_fn = get;
    set_fn = set;
}

std::string get_clipboard_text() {
    return get_fn ? get_fn() : std::string{};
}

void set_clipboard_text(std::string text) {
    if (set_fn) set_fn(text);
}

} // namespace gryce_engine::ui
