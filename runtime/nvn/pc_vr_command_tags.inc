// Part of nvn.h (D133): PC VR's tags on recorded draws: certificate, screen, pane role, layer draw.
// Included where it stands by nvn.h, so it compiles in nvn.h's translation unit as before.

static inline bool nvn_direct_integer_draw(uint32_t op) {
    return op == CMD_DRAW_ARRAYS || op == CMD_DRAW_ELEMENTS;
}

static inline void nvn_set_render_certificate(CmdRec *command, uint64_t id) {
    if (!command) return;
    if (!nvn_direct_integer_draw(command->op)) {
        if (command->op != CMD_DRAW_ARRAYS_INDIRECT && command->op != CMD_DRAW_ELEMENTS_INDIRECT) return;
        id = 0;
    }
    memcpy(&command->f[2], &id, sizeof id);
}

static inline uint64_t nvn_render_certificate(const CmdRec *command) {
    uint64_t id = 0;
    if (command && nvn_direct_integer_draw(command->op)) memcpy(&id, &command->f[2], sizeof id);
    return id;
}

// PC VR (D088): the game UI screen that recorded an integer draw, its id + 1 (0: none; runtime/pc_vr_hud.c), in f[0]
// (a draw's s0, which nothing reads).
static inline void nvn_set_screen_tag(CmdRec *command, uint32_t tag) {
    if (command && nvn_direct_integer_draw(command->op)) memcpy(&command->f[0], &tag, sizeof tag);
}

static inline uint32_t nvn_screen_tag(const CmdRec *command) {
    uint32_t tag = 0;
    if (command && nvn_direct_integer_draw(command->op)) memcpy(&tag, &command->f[0], sizeof tag);
    return tag & 0xffff;
}

// PC VR: the role of the UI pane that recorded an integer draw (PcVrPaneRole, runtime/pc_vr_hud.h, six roles; 0:
// none), in the screen tag's bits 16 to 19.
static inline uint32_t nvn_pane_role(const CmdRec *command) {
    uint32_t tag = 0;
    if (command && nvn_direct_integer_draw(command->op)) memcpy(&tag, &command->f[0], sizeof tag);
    return tag >> 16 & 0xf;
}

// PC VR: what an integer draw is to the eye-image producer (PcVrLayerDraw with its layer's flags,
// runtime/hle/pc_vr_render_trace.h PC_VR_LAYER_DRAW_BITS; 0: nothing), in the screen tag's bits 20 to 31.
static inline uint32_t nvn_layer_draw(const CmdRec *command) {
    uint32_t tag = 0;
    if (command && nvn_direct_integer_draw(command->op)) memcpy(&tag, &command->f[0], sizeof tag);
    return tag >> 20;
}

// PC VR (D088): the marker report an integer draw's pane was positioned at (runtime/pc_vr_hud.c pc_vr_marker_drawing;
// 0: none), in f[1] (a draw's s1, which nothing reads).
static inline void nvn_set_marker_tag(CmdRec *command, uint32_t seq) {
    if (command && nvn_direct_integer_draw(command->op)) memcpy(&command->f[1], &seq, sizeof seq);
}

static inline uint32_t nvn_marker_tag(const CmdRec *command) {
    uint32_t seq = 0;
    if (command && nvn_direct_integer_draw(command->op)) memcpy(&seq, &command->f[1], sizeof seq);
    return seq;
}
