/*
gl_openxr.h - private desktop OpenXR backend for ref_gl
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#pragma once

#include "ref_api.h"

#if XASH_OPENXR
void GL_OpenXRInit( void );
qboolean GL_OpenXRFrameBegin( ref_vr_frame_t *frame );
qboolean GL_OpenXRBeginEye( int eye );
qboolean GL_OpenXREndEye( int eye, int source_width, int source_height );
void GL_OpenXRDrawEyeFade( const byte *rgba, qboolean modulate );
qboolean GL_OpenXRBeginUI( int width, int height );
void GL_OpenXREndUI( void );
void GL_OpenXRFrameEnd( void );
qboolean GL_OpenXRHaptic( int hand, float duration, float frequency, float amplitude );
void GL_OpenXRShutdown( void );
qboolean GL_OpenXRGetEyeFov( float fov[4] );
qboolean GL_OpenXRGetEyeSize( int *width, int *height );
qboolean GL_OpenXRIsRenderingEye( void );
qboolean GL_OpenXRIsSecondaryEye( void );
void GL_OpenXRApplyEyePose( struct ref_viewpass_s *rvp );
void GL_OpenXRSetCinema( qboolean show, float brightness, const vec3_t room_forward );
qboolean GL_OpenXRBeginCinema( int *width, int *height );
void GL_OpenXREndCinema( int source_width, int source_height );
#else
static inline void GL_OpenXRInit( void ) {}
static inline qboolean GL_OpenXRFrameBegin( ref_vr_frame_t *frame ) { (void)frame; return false; }
static inline qboolean GL_OpenXRBeginEye( int eye ) { (void)eye; return false; }
static inline void GL_OpenXRDrawEyeFade( const byte *rgba, qboolean modulate ) { (void)rgba; (void)modulate; }
static inline qboolean GL_OpenXREndEye( int eye, int source_width, int source_height )
{
	(void)eye; (void)source_width; (void)source_height; return false;
}
static inline qboolean GL_OpenXRBeginUI( int width, int height ) { (void)width; (void)height; return false; }
static inline void GL_OpenXREndUI( void ) {}
static inline void GL_OpenXRFrameEnd( void ) {}
static inline qboolean GL_OpenXRHaptic( int hand, float duration, float frequency, float amplitude )
{
	(void)hand; (void)duration; (void)frequency; (void)amplitude; return false;
}
static inline void GL_OpenXRShutdown( void ) {}
static inline qboolean GL_OpenXRGetEyeFov( float fov[4] ) { (void)fov; return false; }
static inline qboolean GL_OpenXRGetEyeSize( int *width, int *height ) { (void)width; (void)height; return false; }
static inline qboolean GL_OpenXRIsRenderingEye( void ) { return false; }
static inline qboolean GL_OpenXRIsSecondaryEye( void ) { return false; }
static inline void GL_OpenXRApplyEyePose( struct ref_viewpass_s *rvp ) { (void)rvp; }
static inline void GL_OpenXRSetCinema( qboolean show, float brightness, const vec3_t room_forward )
{
	(void)show; (void)brightness; (void)room_forward;
}
static inline qboolean GL_OpenXRBeginCinema( int *width, int *height ) { (void)width; (void)height; return false; }
static inline void GL_OpenXREndCinema( int source_width, int source_height ) { (void)source_width; (void)source_height; }
#endif
