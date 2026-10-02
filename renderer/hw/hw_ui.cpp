#include "hw_ui.h"
#include "hw_settings.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool ready, open, drawn;
    SDL_Window *win;
    ImGuiStyle base;
    float applied_scale;
    int scale;
    int lang;
    bool patch_found[2];
    char patch_path[2][1024];
    float volume_pct;
} U;

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) fprintf(stderr, "ac3: ui: vulkan error %d\n", (int)r);
}

static void apply_scale(void) {
    float dpi = SDL_GetWindowDisplayScale(U.win);
    if (dpi <= 0.0f) dpi = 1.0f;
    ImGuiStyle &st = ImGui::GetStyle();
    st = U.base;
    st.ScaleAllSizes(dpi * ac3_ui.ui_scale);
    st.FontScaleMain = ac3_ui.ui_scale;
    st.FontScaleDpi = dpi;
    U.applied_scale = dpi * ac3_ui.ui_scale;
}

static void find_patches(void) {
    for (int d = 0; d < 2; d++) {
        U.patch_path[d][0] = 0;
        U.patch_found[d] = ac3_en_find_patch(d + 1, ac3_lang_patch_dir(), U.patch_path[d],
                                             sizeof U.patch_path[d]) != 0;
    }
}

int hw_ui_init(const hw_ui_vk_t *vk) {
    memset(&U, 0, sizeof U);
    U.win = vk->window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.GrabRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    U.base = st;
    apply_scale();
    if (!ImGui_ImplSDL3_InitForVulkan(vk->window)) {
        ImGui::DestroyContext();
        return 0;
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = vk->api_version;
    ii.Instance = vk->instance;
    ii.PhysicalDevice = vk->phys;
    ii.Device = vk->dev;
    ii.QueueFamily = vk->qfamily;
    ii.Queue = vk->queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    ii.MinImageCount = vk->min_images < 2 ? 2 : vk->min_images;
    ii.ImageCount = vk->images < ii.MinImageCount ? ii.MinImageCount : vk->images;
    ii.PipelineInfoMain.RenderPass = vk->pass;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return 0;
    }
    U.scale = ac3_hd.scale;
    U.lang = ac3_lang_english() ? 1 : 0;
    U.volume_pct = ac3_ui.volume * 100.0f;
    find_patches();
    U.ready = true;
    printf("ui: F1 opens the settings menu\n");
    return 1;
}

void hw_ui_shutdown(void) {
    if (!U.ready) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    U.ready = false;
}

int hw_ui_open(void) { return U.ready && U.open; }

void hw_ui_set_open(int open) {
    if (!U.ready) return;
    U.open = open != 0;
    if (U.open) {
        U.scale = ac3_hd.scale;
        U.volume_pct = ac3_ui.volume * 100.0f;
        find_patches();
    }
}

void hw_ui_set_image_count(uint32_t min_images) {
    if (U.ready) ImGui_ImplVulkan_SetMinImageCount(min_images < 2 ? 2 : min_images);
}

int hw_ui_event(const SDL_Event *ev) {
    if (!U.ready) return 0;
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        if (ev->key.key == SDLK_F1) {
            hw_ui_set_open(!U.open);
            return 1;
        }
        if (U.open && ev->key.key == SDLK_ESCAPE) {
            hw_ui_set_open(0);
            return 1;
        }
    }
    ImGui_ImplSDL3_ProcessEvent(ev);
    if (!U.open) return 0;
    const ImGuiIO &io = ImGui::GetIO();
    bool key = ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP ||
               ev->type == SDL_EVENT_TEXT_INPUT;
    bool mouse = ev->type == SDL_EVENT_MOUSE_MOTION || ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                 ev->type == SDL_EVENT_MOUSE_BUTTON_UP || ev->type == SDL_EVENT_MOUSE_WHEEL;
    return (key && io.WantCaptureKeyboard) || (mouse && io.WantCaptureMouse);
}

static void save(void) { hw_settings_save(); }

static bool flag(const char *label, int *v, const char *tip) {
    bool b = *v != 0;
    bool changed = ImGui::Checkbox(label, &b);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    if (changed) {
        *v = b ? 1 : 0;
        save();
    }
    return changed;
}

static void tab_display(void) {
    int fs = (SDL_GetWindowFlags(U.win) & SDL_WINDOW_FULLSCREEN) ? 1 : 0;
    ac3_ui.fullscreen = fs;
    if (flag("Fullscreen (F11)", &ac3_ui.fullscreen, nullptr))
        ac3_render_set_fullscreen(ac3_ui.fullscreen);
    if (flag("VSync", &ac3_ui.vsync, "Off lets the picture tear, for the lowest latency."))
        ac3_render_set_vsync(ac3_ui.vsync);
    flag("Run at the console's speed", &ac3_ui.frame_limit,
         "59.94 fields a second, as on the PlayStation. Off runs the game as fast as it will go.");
    flag("Widescreen 16:9", &ac3_hd.widescreen,
         "A wider view while flying; menus, briefings and movies stay 4:3.");
    flag("Performance overlay", &ac3_ui.show_stats, nullptr);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::SliderFloat("Menu size", &ac3_ui.ui_scale, 0.75f, 2.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
}

static void tab_graphics(void) {
    ImGui::SeparatorText("Internal resolution");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::SliderInt("##scale", &U.scale, 1, HW_MAX_SCALE, "%dx");
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d for a 320 x 240 screen", 320 * U.scale, 240 * U.scale);
    if (U.scale != hw_scale()) {
        if (ImGui::Button("Apply")) {
            ac3_hd.scale = U.scale;
            ac3_render_request_scale(U.scale);
            save();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("running at %dx", hw_scale());
    }
    ImGui::SeparatorText("Geometry");
    if (flag("Perspective-correct textures", &ac3_hd.perspective,
             "Off gives the original affine (swimming) textures."))
        hw_set_perspective(ac3_hd.perspective);
    int pg = ac3_hd.pgxp;
    if (flag("Precise vertices (PGXP)", &pg,
             "Off snaps every vertex to whole pixels, as the console did."))
        hw_set_pgxp(pg);
}

static void tab_effects(void) {
    flag("World effects (F7)", &ac3_hd.fx, "Everything below at once.");
    ImGui::BeginDisabled(!ac3_hd.fx);
    ImGui::Indent();
    flag("Atmosphere and haze", &ac3_hd.atmosphere,
         "Off brings back the game's own fog, and with it the game's own draw distance.");
    ImGui::BeginDisabled(!ac3_hd.atmosphere);
    flag("Draw distance to the horizon", &ac3_hd.far_terrain,
         "The terrain map ray-marched out to the horizon of a round earth. Needs the atmosphere.");
    ImGui::EndDisabled();
    flag("Volumetric clouds", &ac3_hd.clouds, nullptr);
    flag("Simulated ocean", &ac3_hd.ocean, nullptr);
    flag("Forest", &ac3_hd.forest, nullptr);
    flag("Global illumination", &ac3_hd.gi, "Ambient occlusion, bounce light and cloud shadows.");
    flag("HDR sun and lens flare", &ac3_hd.sun, "Off keeps the game's own sun sprite and flare.");
    ImGui::Unindent();
    ImGui::EndDisabled();
}

static void tab_game(void) {
    ImGui::SeparatorText("Language");
    int before = U.lang;
    ImGui::RadioButton("Japanese (original)", &U.lang, 0);
    ImGui::RadioButton("English (Team NEMO translation v0.9b3)", &U.lang, 1);
    if (U.lang != before) {
        ac3_settings_set("language", U.lang ? "en" : "ja");
        ac3_settings_save();
    }
    if (U.lang == 1 && (!U.patch_found[0] || !U.patch_found[1])) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                           "The Team NEMO patch was not found for disc %s.",
                           !U.patch_found[0] && !U.patch_found[1] ? "1 or 2" :
                           !U.patch_found[0] ? "1" : "2");
        ImGui::TextWrapped("Put the \"[ProjectNEMO] ... Patches+Extras\" folder in the game's folder, "
                           "or its .xdelta files in game/translation. The disc images are not changed.");
    }
    ImGui::Text("Running in %s.", ac3_disc_english_active() ? "English" : "Japanese");
    if (U.lang != ac3_disc_english_active()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
                           "The language changes when the game starts again.");
        if (ImGui::Button("Restart now")) ac3_render_request_restart(U.lang);
        ImGui::SetItemTooltip("Anything not yet saved to a memory card is lost.");
    }
    if (ImGui::TreeNode("Patch files")) {
        for (int d = 0; d < 2; d++)
            ImGui::TextWrapped("Disc %d: %s", d + 1, U.patch_found[d] ? U.patch_path[d] : "not found");
        ImGui::TreePop();
    }
    ImGui::SeparatorText("Menu");
    flag("Pause the game while this menu is open", &ac3_ui.pause_on_menu, nullptr);
}

static void tab_audio(void) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::SliderFloat("Volume", &U.volume_pct, 0.0f, 100.0f, "%.0f%%")) {
        ac3_ui.volume = U.volume_pct / 100.0f;
        ac3_audio_set_volume(ac3_ui.volume);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
}

static void tab_info(void) {
    uint32_t prims = 0, precise = 0;
    hw_frame_stats(&prims, &precise);
    float fps = ac3_render_fps();
    ImGui::Text("%.1f fields a second (%.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);
    ImGui::Text("Internal resolution %dx", hw_scale());
    ImGui::Text("%u primitives last frame, %u with precise vertices", prims, precise);
    ImGui::SeparatorText("Keys");
    ImGui::TextUnformatted("F1   this menu\nF7   world effects on and off\nF9   record the next "
                           "frames to captures_hd\nF10  capture the next video frame\n"
                           "F11  fullscreen\nEsc  close this menu, or quit");
    ImGui::SeparatorText("Settings");
    ImGui::TextDisabled("Saved to ac3_settings.ini as they change.");
    if (ImGui::Button("Reset to defaults")) {
        int fs = ac3_ui.fullscreen;
        ac3_hd = ac3_hd_defaults;
        ac3_ui = ac3_ui_defaults;
        ac3_ui.fullscreen = fs;
        hw_set_perspective(ac3_hd.perspective);
        hw_set_pgxp(ac3_hd.pgxp);
        ac3_render_set_vsync(ac3_ui.vsync);
        ac3_audio_set_volume(ac3_ui.volume);
        U.volume_pct = ac3_ui.volume * 100.0f;
        U.scale = ac3_hd.scale;
        if (hw_scale() != ac3_hd.scale) ac3_render_request_scale(ac3_hd.scale);
        save();
    }
}

static void menu(void) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Ace Combat 3 -- settings", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Display")) { tab_display(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Graphics")) { tab_graphics(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("World effects")) { tab_effects(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Game")) { tab_game(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Audio")) { tab_audio(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Info")) { tab_info(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("F1 or Esc closes this menu%s", ac3_ui.pause_on_menu ? "; the game is paused" : "");
    }
    ImGui::End();
    if (!open) hw_ui_set_open(0);
}

static void overlay(void) {
    uint32_t prims = 0, precise = 0;
    hw_frame_stats(&prims, &precise);
    float fps = ac3_render_fps();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##stats", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        ImGui::Text("%.1f fields/s  %.2f ms", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);
        ImGui::Text("%dx  %u prims  %u precise", hw_scale(), prims, precise);
        ImGui::Text("%s%s", ac3_disc_english_active() ? "English" : "Japanese", ac3_hd.fx ? "" : "  effects off");
    }
    ImGui::End();
}

void hw_ui_frame(void) {
    U.drawn = false;
    if (!U.ready || (!U.open && !ac3_ui.show_stats)) return;
    float dpi = SDL_GetWindowDisplayScale(U.win);
    if (dpi <= 0.0f) dpi = 1.0f;
    if (dpi * ac3_ui.ui_scale != U.applied_scale && !ImGui::IsAnyItemActive()) apply_scale();
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (ac3_ui.show_stats) overlay();
    if (U.open) menu();
    ImGui::Render();
    U.drawn = true;
}

void hw_ui_draw(VkCommandBuffer cb) {
    if (!U.drawn) return;
    ImDrawData *dd = ImGui::GetDrawData();
    if (dd) ImGui_ImplVulkan_RenderDrawData(dd, cb);
}
