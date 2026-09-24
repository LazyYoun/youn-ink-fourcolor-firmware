#include "i18n.h"

#include "nvs_state.h"

namespace i18n {

namespace {
Language g_language = Language::kZhCN;
bool g_loaded = false;

void EnsureLoaded() {
    if (g_loaded) return;
    g_loaded = true;
    g_language = nvs_state::LoadUiLanguage() == 1 ? Language::kEnUS : Language::kZhCN;
}
}  // namespace

Language GetLanguage() {
    EnsureLoaded();
    return g_language;
}

void SetLanguage(Language lang) {
    g_language = lang;
    g_loaded = true;
    nvs_state::SaveUiLanguage(lang == Language::kEnUS ? 1 : 0);
}

extern const char* const kStringsZhCN[kStringCount];
extern const char* const kStringsEnUS[kStringCount];

const char* Tr(StringId id) {
    const size_t index = static_cast<size_t>(id);
    return GetLanguage() == Language::kEnUS ? kStringsEnUS[index] : kStringsZhCN[index];
}

}  // namespace i18n
