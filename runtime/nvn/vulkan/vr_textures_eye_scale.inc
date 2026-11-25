// Part of textures.c (D133): PC VR's eye targets' scale.
// Included where it stands by textures.c, so it compiles in textures.c's translation unit as before.

// PC VR eye scale (1 = off, up to 5: 4000x4500), exact decimal text only. The eye chain's shape (0.87..0.91, the
// 800x900 Labo eye targets and their fractions) is otherwise never scaled. In PC VR the scale's pixels take the
// OpenXR runtime's view shape (pc_vr.c pc_vr_eye_size: 2451x2644 at 3.0 on Link), one factor per axis, so each eye
// renders in the shape the headset shows it in; uniform (Labo's 8:9) only without a discovered runtime.
// The eye targets' scale from the eye size now (pc_vr_eye_size, which follows PC VR's DLAA modes) and the render
// ratio (vr_dlaa.h: with DLSS the eyes render that many times smaller, brought up to the eye size at their placement).
// A changed scale makes the scaled eye targets again at their next use (rs_stale). false: no eye size.
static bool eye_scale_from_size(void) {
    unsigned width, height;
    if (!pc_vr_eye_size(&width, &height)) return false;
    const double ratio = vr_dlaa_render_ratio();
    const float scale_x = (float)(width / ratio) / 800.0f, scale_y = (float)(height / ratio) / 900.0f;
    if (scale_x == g_rs[RS_EYE][0] && scale_y == g_rs[RS_EYE][1]) return true;
    g_rs[RS_EYE][0] = scale_x;
    g_rs[RS_EYE][1] = scale_y;
    LOG("[pc-vr-eye-size] exact eye renders %ux%u (%s); final halves match, poses/FOV unchanged\n", width, height,
        pc_vr_config()->play.eye_width ? "SAVAGEWIND_PC_VR_EYE_SIZE" : "the eye scale's pixels in the runtime's view shape");
    if (ratio > 1)
        LOG("[pc-vr-eye-size] eyes rendered %.4g times smaller (%ldx%ld), DLSS bringing them up to %ux%u\n", ratio,
            lround(width / ratio), lround(height / ratio), width, height);
    return true;
}

static bool eye_scale_by_size;  // (the eye targets are scaled to the eye size: eye_scale_from_size)

static bool eye_scale_on(void) {
    static int on = -1;
    if (on >= 0) return on;
    on = 0;
    if (eye_scale_from_size()) {
        eye_scale_by_size = true;
        on = 1;
        return on;
    }
    // (SAVAGEWIND_PC_VR_EYE_SCALE: the configuration takes a decimal in (1, 5], and leaves 1 otherwise)
    const PcVrConfig *config = pc_vr_config();
    const double scale = config->play.eye_scale;
    if (scale > 1.0) {
        g_rs[RS_EYE][0] = g_rs[RS_EYE][1] = (float)scale;
        on = 1;
        LOG("[pc-vr-eye-scale] Labo eye targets scaled %.3fx (800x900 -> %ldx%ld)\n", scale,
            lround(800 * scale), lround(900 * scale));
    } else if (config->settings_ignored >> PC_VR_SETTING_EYE_SCALE & 1) {
        LOG("[pc-vr-eye-scale] ignored SAVAGEWIND_PC_VR_EYE_SCALE (needs a decimal in (1, 5])\n");
    }
    return on;
}

// (vr_dlaa.c, at a present that changed its mode) The eye targets' scale again from the eye size and render ratio now.
void vk_eye_scale_refresh(void) {
    if (eye_scale_on() && eye_scale_by_size) eye_scale_from_size();
}
