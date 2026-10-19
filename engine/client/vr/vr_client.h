/*
vr_client.h - renderer-independent client VR frame policy
Copyright (C) 2026 Xash3D FWGS contributors
*/

#ifndef VR_CLIENT_H
#define VR_CLIENT_H

#include "ref_api.h"

void CL_VRRegisterCvars( void );
void CL_VRFrameBegin( void );
void CL_VRFrameEnd( void );
qboolean CL_VRShouldRender( void );
qboolean CL_VRBeginEye( int eye );
qboolean CL_VREndEye( int eye );
void CL_VRApplyHeadPose( ref_viewpass_t *rvp );
uint64_t CL_VRFrameId( void );
void CL_VRAppendMove( float frametime, usercmd_t *cmd, qboolean active );
void CL_VRBuildUsercmdSidecar( const usercmd_t *cmd, vr_usercmd_sidecar_t *sample );
qboolean CL_VRHaptic( int hand, float duration, float frequency, float amplitude );
const ref_vr_frame_t *CL_VRGetFrame( void );
qboolean CL_VRIsActive( void );
qboolean CL_VRScopeLensActive( void );
void CL_VRScopeView( ref_viewpass_t *rvp );
qboolean CL_VRGetRecenter( ref_vr_pose_t *center );
float CL_VRGetWorldScale( void );
qboolean CL_VRGetFlashlightPose( vec3_t origin, vec3_t forward );
qboolean CL_VRCinemaShowing( void );
qboolean CL_VRCinemaCamera( void );
qboolean CL_VRBeginCinema( int *width, int *height );
void CL_VREndCinema( void );
void CL_VRDrawCinemaWorldFade( void );
void CL_VRSetScope( qboolean show, int hand );
qboolean CL_VRBeginScope( int *width, int *height );
void CL_VREndScope( void );

#endif /* VR_CLIENT_H */
