#pragma once

#include <string>
#include <vector>

#include "engine/render/UiSkin.h"
#include "engine/render/ui/UiTypes.h"

namespace UiDialogs {

struct Entry {
    enum class Kind { Ok, YesNo } kind = Kind::Ok;
    std::string title;
    std::string text;
    DialogCallback callback;
};

class Stack {
public:
    void pushOk(const char* title, const char* text, DialogCallback cb);
    void pushYesNo(const char* title, const char* text, DialogCallback cb);
    bool hasModal() const { return !stack_.empty(); }
    void draw(const UiSkin::Assets& skin);

private:
    std::vector<Entry> stack_;
    void finish(DialogResult result);
};

}  // namespace UiDialogs
