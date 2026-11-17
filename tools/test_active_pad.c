// The game's controller follows the gamepad in use (#455 / #456: a two-port adapter is two gamepads, the working
// controller in port 2). Two SDL virtual gamepads, real SDL events, the real input.c. Never touches a physical pad.
#include <SDL3/SDL.h>
static void input_reset(void);
static void input_load_user_mappings(void);
static bool input_user_blocked(const SDL_Event *e);
static int input_pad_family(void);
static void input_handle_event(const SDL_Event *e);
static void input_frame(uint64_t frame);
static uint64_t input_buttons_now(void);
static void input_assign_begin(int player);
static int input_assign_waiting(void);
static bool input_assign_event(const SDL_Event *e);
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#include "../runtime/hle/input.c"
#include "../runtime/services/hid_set.c"
#undef NDEBUG
#include <assert.h>

bool menu_is_open(void) { return false; }
Settings g_settings;
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { abort(); }
KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) { abort(); }
Service *service_new(const char *n, ServiceHandler h, void *state) { abort(); }
void ipc_return_object(IpcResp *r, Service *s) { abort(); }
static int in_play;  // (the game: on a file, not at the title - drop-in joins happen only then)
int game_in_play(void) { return in_play; }
int cheats_link_health(int *max) { abort(); }
int bind_label_family(void) { abort(); }
int16_t g_bind_pad[BIND_PAD_COUNT], g_bind_key[BIND_KEY_COUNT];  // (A on the bottom button, as by default)
int bind_pad_host(int slot) { return g_bind_pad[slot]; }  // (by position: Confirm button "Right")
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
void settings_save(void) {}
// Local co-op as runtime/hle/coop.c has it for input.c: on or off (the Players setting), the players in (a bit each,
// Player 1's always), the joins asked and not yet carried out. A join is carried out on the game thread (coop_tick ->
// coop_join): here admit(); a leave at once.
static bool coop_on;
static uint32_t joined = 1, want;
bool coop_enabled(void) { return coop_on; }
uint32_t coop_joined(void) { return coop_on ? joined : 1; }
uint32_t coop_join_pending(void) { return want; }
int rt_coop_players(void) { return coop_on ? 2 : 1; }
void coop_join_want(int i) { want |= 1u << i; }
void coop_leave_want(int i) { joined &= ~(1u << i); }
static void admit(void) { joined |= want, want = 0; }

static SDL_JoystickID attach(const char *name) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD, d.naxes = 6, d.nbuttons = 15, d.axis_mask = 0x3f, d.button_mask = 0x7fff;
    d.name = name;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    assert(id && SDL_OpenJoystick(id));  // (open already, as the start screen leaves every controller)
    return id;
}
static void pump(void) {  // the event loop: every event to input.c, as ui_run does
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) input_handle_event(&e);
}
static void button(SDL_JoystickID id, int b, bool down) {
    SDL_Joystick *j = SDL_GetJoystickFromID(id);
    assert(j && SDL_SetJoystickVirtualButton(j, b, down));
    pump();
}
static void axis(SDL_JoystickID id, int a, int16_t v) {
    SDL_Joystick *j = SDL_GetJoystickFromID(id);
    assert(j && SDL_SetJoystickVirtualAxis(j, a, v));
    pump();
}
static uint64_t south(void) { return pad_bits(SDL_GAMEPAD_BUTTON_SOUTH); }

int main(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    const char *off[] = {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_IOKIT, SDL_HINT_JOYSTICK_MFI, SDL_HINT_JOYSTICK_RAWINPUT,
                         SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_DIRECTINPUT,
                         SDL_HINT_XINPUT_ENABLED};
    for (size_t i = 0; i < sizeof off / sizeof *off; i++) SDL_SetHint(off[i], "0");  // (physical controllers: left alone)
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    for (int i = 0; i < BIND_PAD_COUNT; i++) g_bind_pad[i] = -1;
    g_bind_pad[BIND_A] = SDL_GAMEPAD_BUTTON_SOUTH;
    SDL_JoystickID p1 = attach("port 1 (virtual)"), p2 = attach("port 2 (virtual)");
    SDL_Event none = {0};
    input_handle_event(&none);  // (the first event: connected pads opened, the first one is the game's)
    pump();
    assert(input_active_pad() == p1);

    // held on port 2: the game's pad switches and the hold stays held (#456), not just a tap
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == p2 && (pad_buttons & south()));
    pump(); pump();
    assert(pad_buttons & south());
    // port 2's stick moves Link (#455)
    axis(p2, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    assert(pad_lx == 32767);
    axis(p2, SDL_GAMEPAD_AXIS_LEFTX, 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    assert(!(pad_buttons & south()));

    // a noisy stick on port 1: drift doesn't take the pad, nor does a full push while port 2 is in use
    axis(p1, SDL_GAMEPAD_AXIS_LEFTX, 9000);
    assert(input_active_pad() == p2 && pad_lx == 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    axis(p1, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    assert(input_active_pad() == p2 && pad_lx == 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    axis(p1, SDL_GAMEPAD_AXIS_LEFTX, 0);
    // a clear stick push on port 1 while port 2 rests does
    axis(p1, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    assert(input_active_pad() == p1 && pad_ly > 32000);
    axis(p1, SDL_GAMEPAD_AXIS_LEFTY, 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == p2);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, false);

    // the game's pad unplugged: the other one takes over
    assert(SDL_DetachVirtualJoystick(p2));
    pump();
    assert(input_active_pad() == p1);
    button(p1, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(pad_buttons & south());
    button(p1, SDL_GAMEPAD_BUTTON_SOUTH, false);
    assert(SDL_DetachVirtualJoystick(p1));
    pump();
    assert(input_active_pad() == 0 && !pad_buttons);

    // local co-op, drop-in (co-op on, in play): Player 1's pad is the first; the others wait - a press or a stick of
    // theirs reaches nobody and doesn't take Player 1's pad
    coop_on = true, in_play = 1;
    SDL_JoystickID c1 = attach("co-op 1 (virtual)"), c2 = attach("co-op 2 (virtual)"), c3 = attach("spare (virtual)");
    pump();
    assert(input_active_pad() == c1 && coop_slot_of(c2) < 0 && coop_slot_of(c3) < 0);
    button(c2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == c1 && !(pad_buttons & south()) && !coop_btn[0]);
    button(c2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    axis(c3, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    assert(input_active_pad() == c1 && pad_lx == 0);
    axis(c3, SDL_GAMEPAD_AXIS_LEFTX, 0);
    // a waiting pad's Plus held JOIN_HOLD_MS asks to join, as Player 2 (the first free); let go sooner, nothing
    button(c2, SDL_GAMEPAD_BUTTON_START, true);
    SDL_Delay(JOIN_HOLD_MS / 2);
    input_pad_poll();
    button(c2, SDL_GAMEPAD_BUTTON_START, false);
    SDL_Delay(JOIN_HOLD_MS);
    input_pad_poll();
    assert(!want && coop_slot_of(c2) < 0);
    button(c2, SDL_GAMEPAD_BUTTON_START, true);
    SDL_Delay(JOIN_HOLD_MS + 50);
    input_pad_poll();
    assert(want == 1u << 1 && coop_slot_of(c2) == 0);
    button(c2, SDL_GAMEPAD_BUTTON_START, false);
    input_pad_poll();  // (asked, not in yet: his pad is kept)
    assert(coop_slot_of(c2) == 0);
    admit();
    input_pad_poll();
    assert(coop_slot_of(c2) == 0);
    // in: his presses and stick are his, never the game's (Player 1's) pad
    button(c2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == c1 && !(pad_buttons & south()) && (coop_btn[0] & 1ull << BIND_A));
    button(c2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    axis(c2, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    assert(input_active_pad() == c1 && pad_lx == 0 && coop_axis[0][0] == 32767);
    axis(c2, SDL_GAMEPAD_AXIS_LEFTX, 0);
    // a quick tap on his pad - pressed and released before the events are read - still reaches him
    SDL_Joystick *j2 = SDL_GetJoystickFromID(c2);
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_SOUTH, true));
    SDL_UpdateJoysticks();
    assert(SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_SOUTH, false));
    pump();
    assert(!(coop_btn[0] & 1ull << BIND_A) && (coop_tap[0] & 1ull << BIND_A));
    // Minus held with L (a colour step): no leave, no ring
    int colour = g_settings.coop_colour[1];
    button(c2, SDL_GAMEPAD_BUTTON_BACK, true);
    button(c2, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    button(c2, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    assert(g_settings.coop_colour[1] == (colour + COOP_COLOURS - 1) % COOP_COLOURS);
    SDL_Delay(LEAVE_WAIT_MS + LEAVE_HOLD_MS + 50);
    input_pad_poll();
    assert(joined >> 1 & 1 && coop_leave_progress(2) == 0);
    button(c2, SDL_GAMEPAD_BUTTON_BACK, false);
    // Minus held on its own: no ring for LEAVE_WAIT_MS, then it fills over LEAVE_HOLD_MS, then he is out and his
    // pad waits again
    button(c2, SDL_GAMEPAD_BUTTON_BACK, true);
    assert(coop_leave_progress(2) == 0);
    SDL_Delay(LEAVE_WAIT_MS + LEAVE_HOLD_MS / 2);
    input_pad_poll();
    float half = coop_leave_progress(2);
    assert(joined >> 1 & 1 && half > 0.2f && half < 0.8f);
    SDL_Delay(LEAVE_HOLD_MS / 2 + 50);
    input_pad_poll();
    assert(!(joined >> 1 & 1));
    button(c2, SDL_GAMEPAD_BUTTON_BACK, false);
    input_pad_poll();
    assert(coop_slot_of(c2) < 0 && input_active_pad() == c1);
    // in again (its own number again: remembered as Player 2's); Player 1's pad unplugged: a waiting one takes over,
    // never a player's
    button(c2, SDL_GAMEPAD_BUTTON_START, true);
    SDL_Delay(JOIN_HOLD_MS + 50);
    input_pad_poll();
    button(c2, SDL_GAMEPAD_BUTTON_START, false);
    admit();
    assert(coop_slot_of(c2) == 0);
    assert(SDL_DetachVirtualJoystick(c1));
    pump();
    assert(input_active_pad() == c3 && coop_slot_of(c2) == 0);
    // co-op set Off: Player 2's pad is let go but stays open, and a press makes it the game's
    coop_on = false;
    input_pad_poll();
    assert(coop_slot_of(c2) < 0 && SDL_GetGamepadFromID(c2));
    button(c2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == c2 && (pad_buttons & south()));
    button(c2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    // co-op on but not in play (the title, the file select): no one waits - a press takes the game's pad, as alone
    coop_on = true, in_play = 0;
    button(c3, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == c3 && coop_slot_of(c3) < 0);
    button(c3, SDL_GAMEPAD_BUTTON_SOUTH, false);
    assert(SDL_DetachVirtualJoystick(c2) && SDL_DetachVirtualJoystick(c3));
    pump();
    SDL_Quit();
    puts("PASS: press / stick on port 2 takes the game's pad and holds stay held; drift and in-use guard; unplug fallback;"
         " drop-in co-op: waiting pads reach nobody, Plus held joins, a player's pad never the game's, quick taps, Minus held"
         " leaves (the ring's timing; not with a colour step), co-op off lets go");
    return 0;
}
