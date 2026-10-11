/*
vr_cs.h - Counter-Strike VR mode for the CSVR client DLL
Copyright (C) 2026 Xash3D FWGS contributors
*/

#ifndef VR_CS_H
#define VR_CS_H

/* active when the game directory is cstrike/czero (decided when the cvars are registered) */
qboolean CL_VRCSMode( void );
void CL_VRCSRegisterCvars( void );
/* per frame after the shared VR input (UI pointer, menu button); ui_active: menu/VGUI/console up */
void CL_VRCSFrame( qboolean ui_active );
/* release held buttons (focus lost, menu opened) */
void CL_VRCSReleaseInput( void );
/* usercmd: view angles = the weapon hand, movement; head_delta = this frame's head movement in
 * the recentre frame (metres, forward/left/up) */
void CL_VRCSAppendMove( float frametime, usercmd_t *cmd, const vec3_t head_delta );
/* the body's yaw: the view yaw carries the weapon hand's yaw on top of it */
float CL_VRCSBodyYaw( void );
/* dead or spectating: the spectator camera owns the view angles, the head pose goes on top of them */
qboolean CL_VRCSFreeView( void );
/* zoomed with a scoped weapon: the engine renders the scope lens */
qboolean CL_VRCSScopeActive( void );
/* every frame, VR or not: bot settings for offline games (vr_cs_bots*) */
void CL_VRCSBots( void );
/* the server set the view angles (spawn, teleport): turn the body so the gun points that way */
void CL_VRCSSetAngle( float view_yaw );

#endif /* VR_CS_H */
