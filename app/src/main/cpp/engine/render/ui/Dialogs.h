#pragma once

#include <string>
#include <vector>

#include "engine/render/UiSkin.h"
#include "engine/render/ui/UiTypes.h"

namespace UiDialogs {

// Кнопка кастомного диалога: подпись + результат, который придёт в callback. Ok/YesNo —
// просто пресеты поверх этого (одна/две кнопки). Порядок в списке = слева направо.
struct Button {
    std::string label;
    DialogResult result = DialogResult::Ok;
};

// Модальный диалог: заголовок, текст, набор кнопок (>=1). Затемняет экран, блокирует ввод.
struct Entry {
    std::string title;
    std::string text;
    std::vector<Button> buttons;  // >=1
    DialogCallback callback;
};

// Тост — недолгая НЕблокирующая плашка в углу (успех/ошибка/инфо). Не модалка: не затемняет
// экран, hasModal() его не считает, ввод в мир не гейтит. Сам гаснет по таймеру.
struct Toast {
    enum class Kind { Info, Success, Danger } kind = Kind::Info;
    std::string text;
    float remaining = 3.0f;  // сек до исчезновения
    float age = 0.0f;        // сек с появления (для fade-in)
};

class Stack {
public:
    void pushOk(const char* title, const char* text, DialogCallback cb);
    void pushYesNo(const char* title, const char* text, DialogCallback cb);
    // Произвольный набор кнопок (>=1). Пустой список трактуется как одна кнопка «OK».
    void pushCustom(const char* title, const char* text, std::vector<Button> buttons,
                    DialogCallback cb);

    // Неблокирующее уведомление. seconds<=0 -> дефолт (3 c).
    void pushToast(const char* text, Toast::Kind kind = Toast::Kind::Info, float seconds = 0.0f);

    bool hasModal() const { return !stack_.empty(); }  // тосты сюда НЕ входят
    void draw(const UiSkin::Assets& skin);             // рисует модалку (верх стека) и тосты

private:
    std::vector<Entry> stack_;
    std::vector<Toast> toasts_;
    void finish(DialogResult result);
    void drawModal(const UiSkin::Assets& skin);
    void drawToasts();
};

}  // namespace UiDialogs
