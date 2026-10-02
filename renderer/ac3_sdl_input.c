#include "ac3_input.h"
#include <SDL3/SDL.h>
#include <stdio.h>

static SDL_Gamepad *pad;
static SDL_JoystickID pad_id;

static u8 axis_to_pad(Sint16 v) {
    int x = (v + 32768) * 255 / 65535;
    return (u8)(x < 0 ? 0 : (x > 255 ? 255 : x));
}

#define STICK_DEADZONE 3200

static Sint16 deadzone(Sint16 v) {
    return (v > -STICK_DEADZONE && v < STICK_DEADZONE) ? 0 : v;
}

void ac3_input_open(void) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "ac3: gamepad subsystem unavailable: %s\n", SDL_GetError());
        return;
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids && count > 0) {
        pad = SDL_OpenGamepad(ids[0]);
        if (pad) {
            pad_id = ids[0];
            printf("input: gamepad \"%s\"\n", SDL_GetGamepadName(pad));
        }
    }
    if (ids) SDL_free(ids);
    if (!pad)
        printf("input: keyboard (arrows=D-pad, Enter=Start, RShift=Select,\n"
               "       X=Cross Z=Square S=Triangle C=Circle, Q/E=L1/R1, "
               "1/3=L2/R2)\n");
}

void ac3_input_close(void) {
    if (pad) {
        SDL_CloseGamepad(pad);
        pad = NULL;
    }
}

void ac3_input_event(const SDL_Event *ev) {
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
        pad = SDL_OpenGamepad(ev->gdevice.which);
        if (pad) {
            pad_id = ev->gdevice.which;
            printf("input: gamepad connected -- \"%s\"\n", SDL_GetGamepadName(pad));
        }
    } else if (ev->type == SDL_EVENT_GAMEPAD_REMOVED && pad &&
               ev->gdevice.which == pad_id) {
        SDL_CloseGamepad(pad);
        pad = NULL;
        printf("input: gamepad disconnected\n");
    }
}

static int suppressed;

void ac3_input_suppress(int on) { suppressed = on; }

void ac3_input_poll(void) {
    ac3_pad_t *p = &ac3_pad[0];
    u16 b = 0;

    p->connected = 1;
    p->lx = p->ly = p->rx = p->ry = 0x80;

    if (pad) {
        p->type = AC3_PAD_ANALOG;

        struct { SDL_GamepadButton sdl; u16 psx; } map[] = {
            { SDL_GAMEPAD_BUTTON_SOUTH,          AC3_BTN_CROSS    },
            { SDL_GAMEPAD_BUTTON_EAST,           AC3_BTN_CIRCLE   },
            { SDL_GAMEPAD_BUTTON_WEST,           AC3_BTN_SQUARE   },
            { SDL_GAMEPAD_BUTTON_NORTH,          AC3_BTN_TRIANGLE },
            { SDL_GAMEPAD_BUTTON_START,          AC3_BTN_START    },
            { SDL_GAMEPAD_BUTTON_BACK,           AC3_BTN_SELECT   },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  AC3_BTN_L1       },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, AC3_BTN_R1       },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK,     AC3_BTN_L3       },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    AC3_BTN_R3       },
            { SDL_GAMEPAD_BUTTON_DPAD_UP,        AC3_BTN_UP       },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN,      AC3_BTN_DOWN     },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT,      AC3_BTN_LEFT     },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     AC3_BTN_RIGHT    },
        };
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GetGamepadButton(pad, map[i].sdl)) b |= map[i].psx;

        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
            b |= AC3_BTN_L2;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
            b |= AC3_BTN_R2;

        p->lx = axis_to_pad(deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX)));
        p->ly = axis_to_pad(deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)));
        p->rx = axis_to_pad(deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX)));
        p->ry = axis_to_pad(deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)));
    } else {
        p->type = AC3_PAD_DIGITAL;
        const bool *k = SDL_GetKeyboardState(NULL);
        if (!k) { p->buttons = 0; return; }

        if (k[SDL_SCANCODE_UP])     b |= AC3_BTN_UP;
        if (k[SDL_SCANCODE_DOWN])   b |= AC3_BTN_DOWN;
        if (k[SDL_SCANCODE_LEFT])   b |= AC3_BTN_LEFT;
        if (k[SDL_SCANCODE_RIGHT])  b |= AC3_BTN_RIGHT;
        if (k[SDL_SCANCODE_RETURN]) b |= AC3_BTN_START;
        if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE])
                                    b |= AC3_BTN_SELECT;
        if (k[SDL_SCANCODE_X])      b |= AC3_BTN_CROSS;
        if (k[SDL_SCANCODE_C])      b |= AC3_BTN_CIRCLE;
        if (k[SDL_SCANCODE_Z])      b |= AC3_BTN_SQUARE;
        if (k[SDL_SCANCODE_V])      b |= AC3_BTN_TRIANGLE;
        if (k[SDL_SCANCODE_Q])      b |= AC3_BTN_L1;
        if (k[SDL_SCANCODE_E])      b |= AC3_BTN_R1;
        if (k[SDL_SCANCODE_1])      b |= AC3_BTN_L2;
        if (k[SDL_SCANCODE_3])      b |= AC3_BTN_R2;

        if (k[SDL_SCANCODE_A]) p->lx = 0x00;
        if (k[SDL_SCANCODE_D]) p->lx = 0xFF;
        if (k[SDL_SCANCODE_W]) p->ly = 0x00;
        if (k[SDL_SCANCODE_S]) p->ly = 0xFF;
    }

    if (suppressed) {
        b = 0;
        p->lx = p->ly = p->rx = p->ry = 0x80;
    }
    p->buttons = b;
}
