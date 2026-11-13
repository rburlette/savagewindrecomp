#ifndef SAVAGEWIND_PC_VR_CAMERA_UPDATE_H
#define SAVAGEWIND_PC_VR_CAMERA_UPDATE_H

// Call once per boot after dispatch_init and pc_vr_head_look_init, before guest threads start. With PC VR and head
// look on, the eight camera types' update entries hand their camera controller to head look before running unchanged.
void pc_vr_camera_update_init(void);

#endif
