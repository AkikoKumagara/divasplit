/*
 * divasplit_qm - Quick Menu (hold PS) toggles for divasplit, built on
 * QuickMenuReborn (https://github.com/Ibrahim778/QuickMenuReborn, MIT).
 *
 * Runs inside SceShell, so it can't touch the game directly: it edits
 * ux0:data/divasplit/settings.txt (see settings.h) and the game plugin picks
 * the change up within half a second. The settings shown are the ones of the
 * DIVA game that started last.
 *
 * Install: QuickMenuReborn under *main, then this file into
 * ux0:QuickMenuReborn/ (or ur0:QuickMenuReborn/).
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>
#include <quickmenureborn/qm_reborn.h>

#include "settings.h"

#define ID(x) "divasplit_qm_" x

#define SEPARATOR_ID ID("sep")
#define LOADER_ID    ID("loader")
#define TITLE_ID     ID("title")
#define PRESET_ID    ID("preset")

enum { ROW_AVS, ROW_SYNC, ROW_BAR, ROW_REC, ROW_COUNT };

static const char *const ROW_PLANE[ROW_COUNT] = {ID("row_avs"), ID("row_sync"), ID("row_bar"), ID("row_rec")};
static const char *const ROW_TEXT[ROW_COUNT] = {ID("txt_avs"), ID("txt_sync"), ID("txt_bar"), ID("txt_rec")};
static const char *const ROW_BOX[ROW_COUNT] = {ID("cb_avs"), ID("cb_sync"), ID("cb_bar"), ID("cb_rec")};
static const char *const ROW_LABEL[ROW_COUNT] = {
    "Adaptive vsync (AVS)", "Frame-drop catch-up (SYNC)", "Status bar", "Record to log.csv"};

static int *row_field(game_settings_t *gs, int row) {
    switch (row) {
    case ROW_AVS:  return &gs->avs;
    case ROW_SYNC: return &gs->sync;
    case ROW_BAR:  return &gs->bar;
    default:       return &gs->rec;
    }
}

// before the menu shows the settings, the game that started last
static int current_game(const settings_t *s) {
    return s->active >= 0 ? s->active : GAME_X;
}

static void set_preset_label(int preset) {
    char label[64];
    sceClibSnprintf(label, sizeof(label), "3D preset: %s", PRESETS[preset].name);
    QuickMenuRebornSetWidgetLabel(PRESET_ID, label);
}

// The loader is registered first, so QuickMenuReborn creates it (and calls
// this) before the other widgets: labels and saved checkbox states set here
// are what those widgets get created with.
ONLOAD_HANDLER(on_loader_load) {
    settings_t s;
    settings_load(&s);
    int g = current_game(&s);

    char title[96];
    sceClibSnprintf(title, sizeof(title), "divasplit - %s", SETTINGS_GAME_NAME[g]);
    QuickMenuRebornSetWidgetLabel(TITLE_ID, title);
    set_preset_label(s.game[g].preset);
    for (int row = 0; row < ROW_COUNT; row++)
        QuickMenuRebornSaveCheckBoxState(ROW_BOX[row], *row_field(&s.game[g], row) ? 1 : 0);
}

BUTTON_HANDLER(on_checkbox) {
    int row = (int)(uintptr_t)userDat;
    settings_t s;
    settings_load(&s);
    int g = current_game(&s);
    *row_field(&s.game[g], row) = QuickMenuRebornGetCheckboxValue(ROW_BOX[row]) ? 1 : 0;
    settings_save(&s);
}

BUTTON_HANDLER(on_preset) {
    settings_t s;
    settings_load(&s);
    int g = current_game(&s);
    s.game[g].preset = (s.game[g].preset + 1) % PRESET_COUNT;
    settings_save(&s);
    set_preset_label(s.game[g].preset);
}

void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
    QuickMenuRebornSeparator(SEPARATOR_ID, SCE_SEPARATOR_HEIGHT);

    QuickMenuRebornRegisterWidget(LOADER_ID, NULL, plane);
    QuickMenuRebornSetWidgetSize(LOADER_ID, SCE_PLANE_WIDTH, 1, 0, 0);
    QuickMenuRebornSetWidgetColor(LOADER_ID, 1, 1, 1, 0);
    QuickMenuRebornAssignOnLoadHandler(on_loader_load, LOADER_ID);

    QuickMenuRebornRegisterWidget(TITLE_ID, NULL, text);
    QuickMenuRebornSetWidgetSize(TITLE_ID, SCE_PLANE_WIDTH, 50, 0, 0);
    QuickMenuRebornSetWidgetColor(TITLE_ID, 1, 1, 1, 1);
    QuickMenuRebornSetWidgetLabel(TITLE_ID, "divasplit");

    for (int row = 0; row < ROW_COUNT; row++) {
        QuickMenuRebornRegisterWidget(ROW_PLANE[row], NULL, plane);
        QuickMenuRebornSetWidgetSize(ROW_PLANE[row], SCE_PLANE_WIDTH, 70, 0, 0);
        QuickMenuRebornSetWidgetColor(ROW_PLANE[row], 1, 1, 1, 0);

        QuickMenuRebornRegisterWidget(ROW_TEXT[row], ROW_PLANE[row], text);
        QuickMenuRebornSetWidgetSize(ROW_TEXT[row], 500, 60, 0, 0);
        QuickMenuRebornSetWidgetColor(ROW_TEXT[row], 1, 1, 1, 1);
        QuickMenuRebornSetWidgetPosition(ROW_TEXT[row], -170, 0, 0, 0);
        QuickMenuRebornSetWidgetLabel(ROW_TEXT[row], ROW_LABEL[row]);

        QuickMenuRebornRegisterWidget(ROW_BOX[row], ROW_PLANE[row], check_box);
        QuickMenuRebornSetWidgetSize(ROW_BOX[row], 48, 48, 0, 0);
        QuickMenuRebornSetWidgetColor(ROW_BOX[row], 1, 1, 1, 1);
        QuickMenuRebornSetWidgetPosition(ROW_BOX[row], 350, 0, 0, 0);
        QuickMenuRebornAssignDefaultCheckBoxRecall(ROW_BOX[row]);
        QuickMenuRebornRegisterEventHanlder(ROW_BOX[row], QMR_BUTTON_RELEASE_ID, on_checkbox, (void *)(uintptr_t)row);
    }

    QuickMenuRebornRegisterWidget(PRESET_ID, NULL, button);
    QuickMenuRebornSetWidgetSize(PRESET_ID, 400, 70, 0, 0);
    QuickMenuRebornSetWidgetColor(PRESET_ID, 1, 1, 1, 1);
    QuickMenuRebornRegisterEventHanlder(PRESET_ID, QMR_BUTTON_RELEASE_ID, on_preset, NULL);
    set_preset_label(0);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    QuickMenuRebornUnregisterWidget(PRESET_ID);
    for (int row = ROW_COUNT - 1; row >= 0; row--) {
        QuickMenuRebornUnregisterWidget(ROW_BOX[row]);
        QuickMenuRebornUnregisterWidget(ROW_TEXT[row]);
        QuickMenuRebornUnregisterWidget(ROW_PLANE[row]);
    }
    QuickMenuRebornUnregisterWidget(TITLE_ID);
    QuickMenuRebornUnregisterWidget(LOADER_ID);
    QuickMenuRebornRemoveSeparator(SEPARATOR_ID);
    return SCE_KERNEL_STOP_SUCCESS;
}
