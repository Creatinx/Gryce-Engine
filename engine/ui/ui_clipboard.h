#pragma once

#include "export.h"

#include <string>

namespace gryce_engine::ui {

using ClipboardGetFn = std::string (*)();
using ClipboardSetFn = void (*)(const std::string&);

GRYCE_API void set_clipboard_callbacks(ClipboardGetFn get, ClipboardSetFn set);
GRYCE_API std::string get_clipboard_text();
GRYCE_API void set_clipboard_text(std::string text);

} // namespace gryce_engine::ui
