/*
fakexr.c - a minimal OpenXR runtime for testing the engine without a headset
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Loaded by the OpenXR loader like any runtime (XR_RUNTIME_JSON=fakexr.json). It implements the
subset of OpenXR 1.0 + XR_KHR_opengl_enable the engine uses. Head and controller poses and button
states come from a control FIFO (FAKEXR_CONTROL); on request the runtime composites the submitted
layers (projection + quads) the way a real compositor would and writes the result to a PNG.

Control commands, one per line (angles in degrees, positions in metres, OpenXR LOCAL space:
+X right, +Y up, -Z forward; pitch + = up, yaw + = left, roll + = tilt left):
  head  <x> <y> <z> <pitch> <yaw> <roll>
  aim   left|right <x> <y> <z> <pitch> <yaw> <roll>   controller aim pose (grip follows, see FAKEXR_GRIP_PITCH)
  grip  left|right <x> <y> <z> <pitch> <yaw> <roll>   override the grip pose
  track left|right 0|1                                controller tracked or not
  input left|right <component> <value> [<y>]          e.g. "input right trigger/value 1", "input left thumbstick 0 1",
                                                      "input right a/click 1" (path under /user/hand/<hand>/input/)
  move  head|left|right <x> <y> <z> <pitch> <yaw> <roll> <seconds> [smooth|linear]
                                                      move there over time (smooth = ease in/out, the default);
                                                      the matching linear/angular velocity is reported meanwhile
  look  <pitch> <yaw> <roll> [left|right]             turn the head to these angles about that eye (default left):
                                                      the eye stays put, a pure rotation of its view (no parallax)
  vel   left|right <vx> <vy> <vz>                     extra linear velocity (m/s), added to a move's own
  state focused|visible|synchronized                  session state, stepping through the ones between: visible =
                                                      dashboard (inputs inactive), synchronized = headset asleep
                                                      (shouldRender false)
  fov   left|right|both <left> <right> <up> <down>    field of view handed out, degrees (left/down negative)
  status                                              log poses, active inputs, session state, measured fps and the
                                                      projection check (below)
  shot  <file.png> [both|left|right] [layers] [world] [rect=x0,y0,x1,y1] [px=<n>]
                                                      composite the next submitted frame (written atomically) plus
                                                      <file>.json (every layer's space, pose, size and box in the
                                                      PNG); layers = each quad's image too; rect = that part of the
                                                      eye only (pixels of a normal shot), px = its longer side;
                                                      world = projection layers only (no panels)
  exit                                                ask the application to end the session
Haptic pulses are logged ("fakexr: haptic ..."). Every submitted frame is checked against what LocateViews handed
out: each projection view's fov and eye pose, and whether its image's pixel aspect fits that fov; a difference is
logged as "fakexr: projection mismatch ..." (an app rendering one aspect/fov while declaring another).
Environment: FAKEXR_CONTROL (FIFO path), FAKEXR_EYE_SIZE (default 960), FAKEXR_FPS (default 90, 0 = unthrottled),
FAKEXR_PROFILE (default /interaction_profiles/oculus/touch_controller), FAKEXR_GRIP_PITCH (aim -> grip pitch, default 0),
FAKEXR_IPD (default 0.064), FAKEXR_VERBOSE (1 = log every call that fails).
*/

#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define EXPORT __attribute__(( visibility( "default" )))
#define ARRAY_COUNT( a ) ( sizeof( a ) / sizeof(( a )[0] ))
#define DEG2RAD( a ) (( a ) * (float)M_PI / 180.0f )
#define TO_HANDLE( T, p ) ((T)(uintptr_t)( p ))
#define FROM_HANDLE( T, h ) ((T *)(uintptr_t)( h ))

/* GL entry points beyond 1.1 */
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_CLAMP_TO_EDGE 0x812F

static struct
{
	void ( *GenFramebuffers )( GLsizei, GLuint * );
	void ( *DeleteFramebuffers )( GLsizei, const GLuint * );
	void ( *BindFramebuffer )( GLenum, GLuint );
	void ( *FramebufferTexture2D )( GLenum, GLenum, GLenum, GLuint, GLint );
	GLenum ( *CheckFramebufferStatus )( GLenum );
	void ( *ActiveTexture )( GLenum );
	void ( *UseProgram )( GLuint );
	void ( *BindBuffer )( GLenum, GLuint );
	int loaded;
} gl;

/* ---------- small math ---------- */

typedef struct { float m[16]; } mat4; /* column-major */

static XrQuaternionf QuatMul( XrQuaternionf a, XrQuaternionf b )
{
	XrQuaternionf r;
	r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	return r;
}

static XrQuaternionf QuatConj( XrQuaternionf q )
{
	XrQuaternionf r = { -q.x, -q.y, -q.z, q.w };
	return r;
}

static XrVector3f QuatRotate( XrQuaternionf q, XrVector3f v )
{
	XrQuaternionf p = { v.x, v.y, v.z, 0.0f };
	XrQuaternionf r = QuatMul( QuatMul( q, p ), QuatConj( q ));
	XrVector3f out = { r.x, r.y, r.z };
	return out;
}

static XrQuaternionf QuatAxisAngle( float x, float y, float z, float angle )
{
	float s = sinf( angle * 0.5f );
	XrQuaternionf q = { x * s, y * s, z * s, cosf( angle * 0.5f ) };
	return q;
}

/* yaw about +Y, then pitch about +X, then roll about +Z (all degrees) */
static XrQuaternionf QuatFromAngles( float pitch, float yaw, float roll )
{
	XrQuaternionf qy = QuatAxisAngle( 0, 1, 0, DEG2RAD( yaw ));
	XrQuaternionf qx = QuatAxisAngle( 1, 0, 0, DEG2RAD( pitch ));
	XrQuaternionf qz = QuatAxisAngle( 0, 0, 1, DEG2RAD( roll ));
	return QuatMul( QuatMul( qy, qx ), qz );
}

/* inverse of QuatFromAngles, in degrees */
static void QuatToAngles( XrQuaternionf q, float *pitch, float *yaw, float *roll )
{
	float r02 = 2 * ( q.x * q.z + q.w * q.y ), r22 = 1 - 2 * ( q.x * q.x + q.y * q.y );
	float r10 = 2 * ( q.x * q.y + q.w * q.z ), r11 = 1 - 2 * ( q.x * q.x + q.z * q.z );
	float r12 = 2 * ( q.y * q.z - q.w * q.x );

	r12 = r12 > 1.0f ? 1.0f : r12 < -1.0f ? -1.0f : r12;
	*pitch = asinf( -r12 ) * 180.0f / (float)M_PI;
	*yaw = atan2f( r02, r22 ) * 180.0f / (float)M_PI;
	*roll = atan2f( r10, r11 ) * 180.0f / (float)M_PI;
}

/* rotation taking a to b (b = r * a) as a unit axis and an angle in radians, the short way round */
static float QuatDelta( XrQuaternionf a, XrQuaternionf b, XrVector3f *axis )
{
	XrQuaternionf r = QuatMul( b, QuatConj( a ));
	float s;

	if( r.w < 0 )
	{
		r.x = -r.x; r.y = -r.y; r.z = -r.z; r.w = -r.w;
	}
	s = sqrtf( r.x * r.x + r.y * r.y + r.z * r.z );
	if( s < 1e-6f )
	{
		axis->x = 1; axis->y = axis->z = 0;
		return 0.0f;
	}
	axis->x = r.x / s; axis->y = r.y / s; axis->z = r.z / s;
	return 2.0f * atan2f( s, r.w );
}

static XrPosef PoseIdentity( void )
{
	XrPosef p;
	memset( &p, 0, sizeof( p ));
	p.orientation.w = 1.0f;
	return p;
}

static XrPosef PoseMul( XrPosef a, XrPosef b ) /* a * b: b expressed in a's frame */
{
	XrPosef r;
	XrVector3f t = QuatRotate( a.orientation, b.position );
	r.orientation = QuatMul( a.orientation, b.orientation );
	r.position.x = a.position.x + t.x;
	r.position.y = a.position.y + t.y;
	r.position.z = a.position.z + t.z;
	return r;
}

static XrPosef PoseInv( XrPosef p )
{
	XrPosef r;
	XrVector3f t;
	r.orientation = QuatConj( p.orientation );
	t = QuatRotate( r.orientation, p.position );
	r.position.x = -t.x;
	r.position.y = -t.y;
	r.position.z = -t.z;
	return r;
}

static mat4 Mat4FromPose( XrPosef p )
{
	mat4 m;
	XrQuaternionf q = p.orientation;
	float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

	m.m[0] = 1 - 2 * ( yy + zz ); m.m[4] = 2 * ( xy - wz );     m.m[8] = 2 * ( xz + wy );      m.m[12] = p.position.x;
	m.m[1] = 2 * ( xy + wz );     m.m[5] = 1 - 2 * ( xx + zz ); m.m[9] = 2 * ( yz - wx );      m.m[13] = p.position.y;
	m.m[2] = 2 * ( xz - wy );     m.m[6] = 2 * ( yz + wx );     m.m[10] = 1 - 2 * ( xx + yy ); m.m[14] = p.position.z;
	m.m[3] = 0;                   m.m[7] = 0;                   m.m[11] = 0;                   m.m[15] = 1;
	return m;
}

/* ---------- logging ---------- */

static int verbose;

static void Log( const char *fmt, ... )
{
	va_list args;
	fprintf( stderr, "fakexr: " );
	va_start( args, fmt );
	vfprintf( stderr, fmt, args );
	va_end( args );
	fputc( '\n', stderr );
	fflush( stderr );
}

/* ---------- objects ---------- */

#define MAX_PATHS 512
#define MAX_ACTIONS 64
#define MAX_BINDINGS 256
#define MAX_INPUTS 64
#define MAX_EVENTS 16
#define MAX_SWAPCHAIN_IMAGES 3

typedef struct
{
	XrActionType type;
	char name[XR_MAX_ACTION_NAME_SIZE];
	int alive;
} action_t;

typedef struct
{
	XrPath profile;
	action_t *action;
	XrPath path;
} binding_t;

typedef struct
{
	XrPath path;     /* full input path, e.g. /user/hand/right/input/trigger/value */
	float value[2];
	float synced[2];
	float previous[2];
} input_t;

typedef enum { SPACE_REFERENCE, SPACE_ACTION } space_kind_t;

typedef struct
{
	space_kind_t kind;
	XrReferenceSpaceType reference;
	action_t *action;
	XrPath subaction;
	XrPosef offset;
} space_t;

typedef struct
{
	int64_t format;
	uint32_t width, height;
	GLuint textures[MAX_SWAPCHAIN_IMAGES];
	uint32_t next, acquired;
} swapchain_t;

typedef struct
{
	XrPosef aim, grip;
	XrVector3f velocity; /* linear, LOCAL space, m/s (set with "vel"), added to the motion's own */
	int grip_override;
	int tracked;
} hand_t;

/* a "move" in progress: head (MOTION_HEAD) or a hand's aim pose, from -> to over duration */
#define MOTION_HEAD 2
typedef struct
{
	int active, smooth;
	XrPosef from, to;
	XrTime start, duration;
	XrVector3f linear, angular; /* current velocity, LOCAL space */
} motion_t;

static struct
{
	int instance_alive;
	char paths[MAX_PATHS][XR_MAX_PATH_LENGTH];
	uint32_t path_count;
	action_t actions[MAX_ACTIONS];
	binding_t bindings[MAX_BINDINGS];
	uint32_t binding_count;
	input_t inputs[MAX_INPUTS];
	uint32_t input_count;
	XrEventDataBuffer events[MAX_EVENTS];
	uint32_t event_head, event_count;

	int session_alive, session_running, attached;
	XrPath profile;
	XrPath hand_path[2];

	XrSessionState session_state;
	XrPosef head;
	hand_t hands[2];
	motion_t motion[3]; /* left, right, head */
	float frame_interval; /* measured, smoothed, seconds */
	float ipd;
	uint32_t eye_size;
	float fps;
	float grip_pitch;
	XrFovf fov[2];

	int control_fd;
	char control_buffer[4096];
	size_t control_length;

	uint64_t frame_index;
	XrTime last_display_time;
	struct timespec last_wait;

	char shot_path[1024];
	int shot_eyes; /* bit 0 left, bit 1 right */
	int shot_layers; /* also write every quad layer's image */
	int shot_world; /* projection layers only (no panels) */
	float shot_rect[4]; /* part of each eye to show: u0 v0 u1 v1, 0..1, top-left origin (zoom) */
	uint32_t shot_width, shot_height; /* output size per eye */
	GLuint compose_fbo, compose_texture;
	uint32_t compose_width, compose_height;
	uint64_t haptic_count;

	/* projection check: what LocateViews gave (LOCAL eye poses + fov) vs what each frame says it rendered */
	XrPosef given_pose[2];
	XrFovf given_fov[2];
	int views_given;
	uint64_t projection_frames, projection_bad_frames;
	float worst_fov_deg, worst_pos_mm, worst_rot_deg, worst_aspect;
	char last_projection_problem[256];
	struct timespec last_projection_log;
	struct { float fov_deg, pos_mm, rot_deg, aspect; int valid; } frame_check[2]; /* this frame, for the shot json */
} rt;

static const char *PathString( XrPath path )
{
	if( path == XR_NULL_PATH || path > rt.path_count )
		return "";
	return rt.paths[path - 1];
}

static XrPath PathFind( const char *string )
{
	for( uint32_t i = 0; i < rt.path_count; ++i )
	{
		if( !strcmp( rt.paths[i], string ))
			return i + 1;
	}
	return XR_NULL_PATH;
}

static XrPath PathIntern( const char *string )
{
	XrPath path = PathFind( string );

	if( path != XR_NULL_PATH || rt.path_count >= MAX_PATHS || strlen( string ) >= XR_MAX_PATH_LENGTH )
		return path;
	strcpy( rt.paths[rt.path_count++], string );
	return rt.path_count;
}

static int HandFromPath( XrPath path )
{
	if( path == XR_NULL_PATH )
		return -1;
	if( path == rt.hand_path[0] )
		return 0;
	if( path == rt.hand_path[1] )
		return 1;
	return -1;
}

static int PathIsUnderHand( XrPath path, int hand )
{
	const char *prefix = hand ? "/user/hand/right/" : "/user/hand/left/";
	return !strncmp( PathString( path ), prefix, strlen( prefix ));
}

static void PushEvent( const void *event, size_t size )
{
	XrEventDataBuffer *slot;

	if( rt.event_count >= MAX_EVENTS )
		return;
	slot = &rt.events[( rt.event_head + rt.event_count ) % MAX_EVENTS];
	memset( slot, 0, sizeof( *slot ));
	memcpy( slot, event, size );
	rt.event_count++;
}

static const char *SessionStateName( XrSessionState state );

static XrTime NowNs( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (XrTime)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void PushSessionState( XrSessionState state )
{
	XrEventDataSessionStateChanged event;
	memset( &event, 0, sizeof( event ));
	event.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
	event.session = TO_HANDLE( XrSession, &rt.session_alive );
	event.state = state;
	event.time = NowNs();
	PushEvent( &event, sizeof( event ));
	rt.session_state = state;
	Log( "session state %s", SessionStateName( state ));
}

/* ---------- control FIFO ---------- */

static input_t *InputFind( XrPath path, int create )
{
	for( uint32_t i = 0; i < rt.input_count; ++i )
	{
		if( rt.inputs[i].path == path )
			return &rt.inputs[i];
	}
	if( !create || rt.input_count >= MAX_INPUTS )
		return NULL;
	memset( &rt.inputs[rt.input_count], 0, sizeof( rt.inputs[0] ));
	rt.inputs[rt.input_count].path = path;
	return &rt.inputs[rt.input_count++];
}

static int ParseHand( const char *word )
{
	if( !strcmp( word, "left" )) return 0;
	if( !strcmp( word, "right" )) return 1;
	return -1;
}

static XrPosef PoseFromWords( float x, float y, float z, float pitch, float yaw, float roll )
{
	XrPosef p;
	p.position.x = x;
	p.position.y = y;
	p.position.z = z;
	p.orientation = QuatFromAngles( pitch, yaw, roll );
	return p;
}

static void UpdateGrip( int hand )
{
	if( !rt.hands[hand].grip_override )
	{
		XrPosef offset = PoseIdentity();
		offset.orientation = QuatAxisAngle( 1, 0, 0, DEG2RAD( rt.grip_pitch ));
		rt.hands[hand].grip = PoseMul( rt.hands[hand].aim, offset );
	}
}

static XrTime NowNs( void );
static void PushSessionState( XrSessionState state );
static XrPosef EyePose( int eye );

/* the pose a motion slot drives */
static XrPosef *MotionTarget( int slot )
{
	return slot == MOTION_HEAD ? &rt.head : &rt.hands[slot].aim;
}

static void SetMotionPose( int slot, XrPosef pose )
{
	if( slot != MOTION_HEAD && rt.hands[slot].grip_override )
	{
		/* an explicit grip stays rigidly attached to the aim pose */
		XrPosef rel = PoseMul( PoseInv( rt.hands[slot].aim ), rt.hands[slot].grip );
		rt.hands[slot].grip = PoseMul( pose, rel );
	}
	*MotionTarget( slot ) = pose;
	if( slot != MOTION_HEAD )
		UpdateGrip( slot );
}

/* advance every move to time t: interpolated pose plus the matching velocity */
static void MotionUpdate( XrTime t )
{
	for( int slot = 0; slot < 3; ++slot )
	{
		motion_t *m = &rt.motion[slot];
		XrVector3f axis;
		XrPosef pose;
		float u, s, rate, angle;

		if( !m->active )
			continue;
		u = m->duration > 0 ? (float)( t - m->start ) / (float)m->duration : 1.0f;
		u = u < 0 ? 0 : u;
		if( u >= 1.0f )
		{
			SetMotionPose( slot, m->to );
			m->active = 0;
			memset( &m->linear, 0, sizeof( m->linear ));
			memset( &m->angular, 0, sizeof( m->angular ));
			continue;
		}
		/* smooth = smoothstep: starts and stops gently like a real arm; linear = constant speed */
		s = m->smooth ? u * u * ( 3 - 2 * u ) : u;
		rate = ( m->smooth ? 6 * u * ( 1 - u ) : 1.0f ) / ( m->duration / 1e9f );
		angle = QuatDelta( m->from.orientation, m->to.orientation, &axis );
		pose.position.x = m->from.position.x + ( m->to.position.x - m->from.position.x ) * s;
		pose.position.y = m->from.position.y + ( m->to.position.y - m->from.position.y ) * s;
		pose.position.z = m->from.position.z + ( m->to.position.z - m->from.position.z ) * s;
		pose.orientation = QuatMul( QuatAxisAngle( axis.x, axis.y, axis.z, angle * s ), m->from.orientation );
		SetMotionPose( slot, pose );
		m->linear.x = ( m->to.position.x - m->from.position.x ) * rate;
		m->linear.y = ( m->to.position.y - m->from.position.y ) * rate;
		m->linear.z = ( m->to.position.z - m->from.position.z ) * rate;
		m->angular.x = axis.x * angle * rate;
		m->angular.y = axis.y * angle * rate;
		m->angular.z = axis.z * angle * rate;
	}
}

static const char *SessionStateName( XrSessionState state )
{
	switch( state )
	{
	case XR_SESSION_STATE_IDLE: return "idle";
	case XR_SESSION_STATE_READY: return "ready";
	case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
	case XR_SESSION_STATE_VISIBLE: return "visible";
	case XR_SESSION_STATE_FOCUSED: return "focused";
	case XR_SESSION_STATE_STOPPING: return "stopping";
	case XR_SESSION_STATE_EXITING: return "exiting";
	default: return "unknown";
	}
}

/* walk synchronized <-> visible <-> focused one step at a time, like a real runtime */
static void SetRunningState( XrSessionState target )
{
	if( !rt.session_running )
	{
		Log( "state: session not running" );
		return;
	}
	if( rt.session_state < XR_SESSION_STATE_SYNCHRONIZED || rt.session_state > XR_SESSION_STATE_FOCUSED )
	{
		Log( "state: session is %s", SessionStateName( rt.session_state ));
		return;
	}
	while( rt.session_state != target )
	{
		XrSessionState next = rt.session_state < target ? rt.session_state + 1 : rt.session_state - 1;
		PushSessionState( next );
	}
}

static void LogPose( const char *name, XrPosef p, int tracked )
{
	float pitch, yaw, roll;
	QuatToAngles( p.orientation, &pitch, &yaw, &roll );
	Log( "  %-10s %6.3f %6.3f %6.3f  %7.1f %7.1f %7.1f%s", name, p.position.x, p.position.y, p.position.z,
		pitch, yaw, roll, tracked ? "" : "  (not tracked)" );
}

static void ControlCommand( char *line )
{
	char word[64], side[16], component[200], mode[16];
	float v[7];
	int hand;

	if( sscanf( line, "%63s", word ) != 1 || word[0] == '#' )
		return;

	if( !strcmp( word, "head" ) && sscanf( line, "%*s %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5] ) == 6 )
	{
		rt.motion[MOTION_HEAD].active = 0;
		memset( &rt.motion[MOTION_HEAD].linear, 0, sizeof( rt.motion[MOTION_HEAD].linear ));
		memset( &rt.motion[MOTION_HEAD].angular, 0, sizeof( rt.motion[MOTION_HEAD].angular ));
		rt.head = PoseFromWords( v[0], v[1], v[2], v[3], v[4], v[5] );
	}
	else if( !strcmp( word, "move" ) && sscanf( line, "%*s %15s %f %f %f %f %f %f %f", side,
		&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6] ) == 8 &&
		(( hand = ParseHand( side )) >= 0 || !strcmp( side, "head" )) && v[6] >= 0 )
	{
		int slot = hand >= 0 ? hand : MOTION_HEAD;
		motion_t *m = &rt.motion[slot];

		mode[0] = '\0';
		sscanf( line, "%*s %*s %*f %*f %*f %*f %*f %*f %*f %15s", mode );
		m->from = *MotionTarget( slot );
		m->to = PoseFromWords( v[0], v[1], v[2], v[3], v[4], v[5] );
		m->start = NowNs();
		m->duration = (XrTime)( v[6] * 1e9f );
		m->smooth = strcmp( mode, "linear" ) != 0;
		m->active = 1;
	}
	else if( !strcmp( word, "state" ) && sscanf( line, "%*s %15s", side ) == 1 )
	{
		if( !strcmp( side, "focused" )) SetRunningState( XR_SESSION_STATE_FOCUSED );
		else if( !strcmp( side, "visible" )) SetRunningState( XR_SESSION_STATE_VISIBLE );
		else if( !strcmp( side, "synchronized" )) SetRunningState( XR_SESSION_STATE_SYNCHRONIZED );
		else Log( "state: focused, visible or synchronized" );
	}
	else if( !strcmp( word, "look" ) && sscanf( line, "%*s %f %f %f", &v[0], &v[1], &v[2] ) == 3 )
	{
		/* turn the head to these angles about one eye (default left), so that eye does not move: a pure rotation
		 * of its view, no parallax (for comparing shots) */
		int eye = 0;
		XrPosef eye_pose, offset = PoseIdentity();
		XrVector3f r;

		if( sscanf( line, "%*s %*f %*f %*f %15s", side ) == 1 && !strcmp( side, "right" ))
			eye = 1;
		eye_pose = EyePose( eye );
		offset.position.x = eye ? rt.ipd * 0.5f : -rt.ipd * 0.5f;
		rt.motion[MOTION_HEAD].active = 0;
		memset( &rt.motion[MOTION_HEAD].linear, 0, sizeof( rt.motion[MOTION_HEAD].linear ));
		memset( &rt.motion[MOTION_HEAD].angular, 0, sizeof( rt.motion[MOTION_HEAD].angular ));
		rt.head.orientation = QuatFromAngles( v[0], v[1], v[2] );
		r = QuatRotate( rt.head.orientation, offset.position );
		rt.head.position.x = eye_pose.position.x - r.x;
		rt.head.position.y = eye_pose.position.y - r.y;
		rt.head.position.z = eye_pose.position.z - r.z;
	}
	else if( !strcmp( word, "fov" ) && sscanf( line, "%*s %15s %f %f %f %f", side, &v[0], &v[1], &v[2], &v[3] ) == 5 )
	{
		/* fov left|right|both <left> <right> <up> <down> in degrees, as OpenXR angles (left/down negative) */
		for( int eye = 0; eye < 2; ++eye )
		{
			if(( eye == 0 && !strcmp( side, "right" )) || ( eye == 1 && !strcmp( side, "left" )))
				continue;
			rt.fov[eye].angleLeft = DEG2RAD( v[0] );
			rt.fov[eye].angleRight = DEG2RAD( v[1] );
			rt.fov[eye].angleUp = DEG2RAD( v[2] );
			rt.fov[eye].angleDown = DEG2RAD( v[3] );
		}
	}
	else if( !strcmp( word, "status" ))
	{
		Log( "status: frame %llu, %.1f fps measured, session %s, %llu haptic pulses",
			(unsigned long long)rt.frame_index, rt.frame_interval > 0 ? 1.0f / rt.frame_interval : 0.0f,
			SessionStateName( rt.session_state ), (unsigned long long)rt.haptic_count );
		Log( "  projection: %llu of %llu frames off; worst fov %.2f deg, pose %.1f mm %.2f deg, pixel aspect %.3f",
			(unsigned long long)rt.projection_bad_frames, (unsigned long long)rt.projection_frames,
			rt.worst_fov_deg, rt.worst_pos_mm, rt.worst_rot_deg, rt.worst_aspect > 0 ? rt.worst_aspect : 1.0f );
		LogPose( "head", rt.head, 1 );
		LogPose( "left aim", rt.hands[0].aim, rt.hands[0].tracked );
		LogPose( "left grip", rt.hands[0].grip, rt.hands[0].tracked );
		LogPose( "right aim", rt.hands[1].aim, rt.hands[1].tracked );
		LogPose( "right grip", rt.hands[1].grip, rt.hands[1].tracked );
		for( uint32_t i = 0; i < rt.input_count; ++i )
		{
			if( rt.inputs[i].value[0] != 0.0f || rt.inputs[i].value[1] != 0.0f )
				Log( "  input %s = %g %g", PathString( rt.inputs[i].path ), rt.inputs[i].value[0], rt.inputs[i].value[1] );
		}
	}
	else if(( !strcmp( word, "aim" ) || !strcmp( word, "grip" )) &&
		sscanf( line, "%*s %15s %f %f %f %f %f %f", side, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5] ) == 7 &&
		( hand = ParseHand( side )) >= 0 )
	{
		rt.motion[hand].active = 0;
		memset( &rt.motion[hand].linear, 0, sizeof( rt.motion[hand].linear ));
		memset( &rt.motion[hand].angular, 0, sizeof( rt.motion[hand].angular ));
		if( word[0] == 'a' )
			rt.hands[hand].aim = PoseFromWords( v[0], v[1], v[2], v[3], v[4], v[5] );
		else
		{
			rt.hands[hand].grip = PoseFromWords( v[0], v[1], v[2], v[3], v[4], v[5] );
			rt.hands[hand].grip_override = 1;
		}
		UpdateGrip( hand );
	}
	else if( !strcmp( word, "vel" ) && sscanf( line, "%*s %15s %f %f %f", side, &v[0], &v[1], &v[2] ) == 4 &&
		( hand = ParseHand( side )) >= 0 )
	{
		rt.hands[hand].velocity.x = v[0];
		rt.hands[hand].velocity.y = v[1];
		rt.hands[hand].velocity.z = v[2];
	}
	else if( !strcmp( word, "track" ) && sscanf( line, "%*s %15s %f", side, &v[0] ) == 2 && ( hand = ParseHand( side )) >= 0 )
		rt.hands[hand].tracked = v[0] != 0.0f;
	else if( !strcmp( word, "input" ) && sscanf( line, "%*s %15s %199s %f", side, component, &v[0] ) == 3 &&
		( hand = ParseHand( side )) >= 0 )
	{
		char full[XR_MAX_PATH_LENGTH];
		input_t *input;

		v[1] = 0.0f;
		sscanf( line, "%*s %*s %*s %*f %f", &v[1] );
		snprintf( full, sizeof( full ), "/user/hand/%s/input/%s", side, component );
		input = InputFind( PathIntern( full ), 1 );
		if( input )
		{
			input->value[0] = v[0];
			input->value[1] = v[1];
		}
	}
	else if( !strcmp( word, "shot" ) && sscanf( line, "%*s %1023s", rt.shot_path ) == 1 )
	{
		/* shot <file> [both|left|right] [layers] [rect=x0,y0,x1,y1] [px=<width>]
		 * rect: the part of the eye to render, in pixels of a normal shot (eye size, top-left origin); the
		 * runtime re-projects it at px wide, so panels come out at their full texture detail */
		char *token, *save = NULL;
		float r[4], px = 0;
		int words = 0;

		rt.shot_eyes = 3;
		rt.shot_layers = 0;
		rt.shot_world = 0;
		rt.shot_rect[0] = rt.shot_rect[1] = 0.0f;
		rt.shot_rect[2] = rt.shot_rect[3] = 1.0f;
		for( token = strtok_r( line, " \t", &save ); token; token = strtok_r( NULL, " \t", &save ))
		{
			if( words++ < 2 )
				continue; /* "shot" and the file */
			if( !strcmp( token, "left" )) rt.shot_eyes = 1;
			else if( !strcmp( token, "right" )) rt.shot_eyes = 2;
			else if( !strcmp( token, "both" )) rt.shot_eyes = 3;
			else if( !strcmp( token, "layers" )) rt.shot_layers = 1;
			else if( !strcmp( token, "world" )) rt.shot_world = 1;
			else if( sscanf( token, "rect=%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3] ) == 4 && r[2] > r[0] && r[3] > r[1] )
			{
				for( int k = 0; k < 4; ++k )
					rt.shot_rect[k] = r[k] / rt.eye_size;
			}
			else if( sscanf( token, "px=%f", &px ) == 1 ) {}
			else Log( "shot: ignoring '%s'", token );
		}
		{
			/* px = the longer side of the output (per eye); the other follows the rect's aspect */
			float aspect = ( rt.shot_rect[2] - rt.shot_rect[0] ) / ( rt.shot_rect[3] - rt.shot_rect[1] );
			uint32_t longer = px >= 16 && px <= 8192 ? (uint32_t)px : rt.eye_size;
			rt.shot_width = aspect >= 1.0f ? longer : (uint32_t)( longer * aspect + 0.5f );
			rt.shot_height = aspect >= 1.0f ? (uint32_t)( longer / aspect + 0.5f ) : longer;
			if( rt.shot_width < 1 ) rt.shot_width = 1;
			if( rt.shot_height < 1 ) rt.shot_height = 1;
		}
	}
	else if( !strcmp( word, "exit" ))
	{
		if( rt.session_alive )
			PushSessionState( XR_SESSION_STATE_STOPPING );
	}
	else
		Log( "unknown or malformed control command: %s", line );
}

static void ControlPoll( void )
{
	ssize_t got;

	if( rt.control_fd < 0 )
		return;
	for( ;; )
	{
		char *newline;

		got = read( rt.control_fd, rt.control_buffer + rt.control_length, sizeof( rt.control_buffer ) - 1 - rt.control_length );
		if( got <= 0 )
			break;
		rt.control_length += (size_t)got;
		rt.control_buffer[rt.control_length] = '\0';
		while(( newline = strchr( rt.control_buffer, '\n' )) != NULL )
		{
			size_t used = (size_t)( newline - rt.control_buffer ) + 1;
			*newline = '\0';
			ControlCommand( rt.control_buffer );
			memmove( rt.control_buffer, rt.control_buffer + used, rt.control_length - used + 1 );
			rt.control_length -= used;
		}
		if( rt.control_length >= sizeof( rt.control_buffer ) - 1 )
			rt.control_length = 0; /* overlong line, drop it */
	}
}

/* ---------- PNG writer (stored deflate, no dependencies) ---------- */

static uint32_t Crc32( uint32_t crc, const uint8_t *data, size_t length )
{
	static uint32_t table[256];
	static int ready;

	if( !ready )
	{
		for( uint32_t i = 0; i < 256; ++i )
		{
			uint32_t c = i;
			for( int k = 0; k < 8; ++k )
				c = ( c & 1 ) ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;
			table[i] = c;
		}
		ready = 1;
	}
	crc = ~crc;
	while( length-- )
		crc = table[( crc ^ *data++ ) & 0xFF] ^ ( crc >> 8 );
	return ~crc;
}

static void PutBE32( uint8_t *out, uint32_t value )
{
	out[0] = (uint8_t)( value >> 24 );
	out[1] = (uint8_t)( value >> 16 );
	out[2] = (uint8_t)( value >> 8 );
	out[3] = (uint8_t)value;
}

static void WriteChunk( FILE *f, const char *type, const uint8_t *data, uint32_t length )
{
	uint8_t header[8];
	uint8_t crc_bytes[4];
	uint32_t crc;

	PutBE32( header, length );
	memcpy( header + 4, type, 4 );
	fwrite( header, 1, 8, f );
	if( length )
		fwrite( data, 1, length, f );
	crc = Crc32( 0, header + 4, 4 );
	crc = Crc32( crc, data, length );
	PutBE32( crc_bytes, crc );
	fwrite( crc_bytes, 1, 4, f );
}

/* rgba: bottom-up rows (glReadPixels), written top-down as RGB */
static int WritePNG( const char *path, const uint8_t *rgba, uint32_t width, uint32_t height )
{
	size_t row = (size_t)width * 3 + 1;
	size_t raw_size = row * height;
	uLongf z_size = compressBound( (uLong)raw_size );
	uint8_t *raw = malloc( raw_size );
	uint8_t *z = malloc( z_size );
	uint8_t ihdr[13];
	char temp[1100];
	FILE *f = NULL;

	if( raw && z )
	{
		/* bottom-up GL rows -> top-down PNG rows, RGBA -> RGB, filter "sub" per row (compresses far better) */
		for( uint32_t y = 0; y < height; ++y )
		{
			const uint8_t *src = rgba + (size_t)( height - 1 - y ) * width * 4;
			uint8_t *dst = raw + y * row;
			*dst++ = 1;
			for( uint32_t x = 0; x < width; ++x )
			{
				for( int c = 0; c < 3; ++c )
					*dst++ = (uint8_t)( src[x * 4 + c] - ( x ? src[( x - 1 ) * 4 + c] : 0 ));
			}
		}
		if( compress2( z, &z_size, raw, (uLong)raw_size, 6 ) != Z_OK )
			z_size = 0;
	}
	if( z_size )
	{
		PutBE32( ihdr, width );
		PutBE32( ihdr + 4, height );
		ihdr[8] = 8;  /* bit depth */
		ihdr[9] = 2;  /* RGB */
		ihdr[10] = ihdr[11] = ihdr[12] = 0;

		snprintf( temp, sizeof( temp ), "%s.tmp", path );
		f = fopen( temp, "wb" );
		if( f )
		{
			static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
			fwrite( signature, 1, 8, f );
			WriteChunk( f, "IHDR", ihdr, 13 );
			WriteChunk( f, "IDAT", z, (uint32_t)z_size );
			WriteChunk( f, "IEND", NULL, 0 );
			fclose( f );
			rename( temp, path );
		}
	}
	free( raw );
	free( z );
	return f != NULL;
}

/* ---------- poses ---------- */

static XrPosef EyePose( int eye )
{
	XrPosef offset = PoseIdentity();
	offset.position.x = eye ? rt.ipd * 0.5f : -rt.ipd * 0.5f;
	return PoseMul( rt.head, offset );
}

/* pose of a space in LOCAL; returns 0 when it is not tracked */
static int SpacePose( const space_t *space, XrPosef *out )
{
	XrPosef base;

	if( space->kind == SPACE_REFERENCE )
	{
		base = space->reference == XR_REFERENCE_SPACE_TYPE_VIEW ? rt.head : PoseIdentity();
	}
	else
	{
		int hand = HandFromPath( space->subaction );
		int is_aim = 0, bound = 0;

		if( hand < 0 || !rt.hands[hand].tracked )
			return 0;
		for( uint32_t i = 0; i < rt.binding_count; ++i )
		{
			const binding_t *binding = &rt.bindings[i];
			if( binding->profile != rt.profile || binding->action != space->action || !PathIsUnderHand( binding->path, hand ))
				continue;
			bound = 1;
			is_aim = strstr( PathString( binding->path ), "/aim/pose" ) != NULL;
			break;
		}
		if( !bound )
			return 0;
		base = is_aim ? rt.hands[hand].aim : rt.hands[hand].grip;
	}
	*out = PoseMul( base, space->offset );
	return 1;
}

/* ---------- compositor ---------- */

static void *GLProc( const char *name )
{
	return (void *)glXGetProcAddressARB( (const GLubyte *)name );
}

static int LoadGL( void )
{
	if( gl.loaded )
		return gl.loaded > 0;
	gl.GenFramebuffers = GLProc( "glGenFramebuffers" );
	gl.DeleteFramebuffers = GLProc( "glDeleteFramebuffers" );
	gl.BindFramebuffer = GLProc( "glBindFramebuffer" );
	gl.FramebufferTexture2D = GLProc( "glFramebufferTexture2D" );
	gl.CheckFramebufferStatus = GLProc( "glCheckFramebufferStatus" );
	gl.ActiveTexture = GLProc( "glActiveTexture" );
	gl.UseProgram = GLProc( "glUseProgram" );
	gl.BindBuffer = GLProc( "glBindBuffer" );
	gl.loaded = ( gl.GenFramebuffers && gl.BindFramebuffer && gl.FramebufferTexture2D && gl.CheckFramebufferStatus &&
		gl.ActiveTexture && gl.UseProgram && gl.BindBuffer ) ? 1 : -1;
	if( gl.loaded < 0 )
		Log( "missing GL framebuffer functions, screenshots disabled" );
	return gl.loaded > 0;
}

static void ProjectionForFov( XrFovf fov, float near_z, float far_z )
{
	float l = tanf( fov.angleLeft ) * near_z, r = tanf( fov.angleRight ) * near_z;
	float b = tanf( fov.angleDown ) * near_z, t = tanf( fov.angleUp ) * near_z;
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glFrustum( l, r, b, t, near_z, far_z );
}

/* the eye's field of view narrowed to the shot rectangle */
static XrFovf ShotFov( int eye )
{
	XrFovf fov = rt.fov[eye], out;
	float tl = tanf( fov.angleLeft ), tr = tanf( fov.angleRight ), tu = tanf( fov.angleUp ), td = tanf( fov.angleDown );
	out.angleLeft = atanf( tl + rt.shot_rect[0] * ( tr - tl ));
	out.angleRight = atanf( tl + rt.shot_rect[2] * ( tr - tl ));
	out.angleUp = atanf( tu - rt.shot_rect[1] * ( tu - td ));
	out.angleDown = atanf( tu - rt.shot_rect[3] * ( tu - td ));
	return out;
}

static const space_t *SpaceFromHandle( XrSpace space )
{
	return FROM_HANDLE( const space_t, space );
}

static void BindSubImage( const XrSwapchainSubImage *sub, float uv[4] )
{
	const swapchain_t *chain = FROM_HANDLE( const swapchain_t, sub->swapchain );
	uint32_t index = chain->acquired; /* last released image */

	glBindTexture( GL_TEXTURE_2D, chain->textures[index] );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	uv[0] = (float)sub->imageRect.offset.x / chain->width;
	uv[1] = (float)sub->imageRect.offset.y / chain->height;
	uv[2] = (float)( sub->imageRect.offset.x + sub->imageRect.extent.width ) / chain->width;
	uv[3] = (float)( sub->imageRect.offset.y + sub->imageRect.extent.height ) / chain->height;
}

static void SetLayerBlend( XrCompositionLayerFlags flags )
{
	if( !( flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT ))
	{
		glDisable( GL_BLEND );
		return;
	}
	glEnable( GL_BLEND );
	if( flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT )
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	else
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
}

static void ComposeEye( int eye, const XrFrameEndInfo *info, int x_offset )
{
	XrPosef eye_pose = EyePose( eye );
	XrPosef view = PoseInv( eye_pose );

	glViewport( x_offset, 0, rt.shot_width, rt.shot_height );
	for( uint32_t i = 0; i < info->layerCount; ++i )
	{
		const XrCompositionLayerBaseHeader *base = info->layers[i];
		float uv[4];

		if( base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION )
		{
			const XrCompositionLayerProjection *projection = (const XrCompositionLayerProjection *)base;
			const XrCompositionLayerProjectionView *pv;

			if( projection->viewCount <= (uint32_t)eye )
				continue;
			pv = &projection->views[eye];
			/* the poses do not move between the app's render and here, so no reprojection: draw it full-eye */
			BindSubImage( &pv->subImage, uv );
			SetLayerBlend( projection->layerFlags );
			glMatrixMode( GL_PROJECTION );
			glLoadIdentity();
			/* GL texture rows run bottom-up: the rect's top (v0) is the high end */
			glOrtho( rt.shot_rect[0], rt.shot_rect[2], 1.0f - rt.shot_rect[3], 1.0f - rt.shot_rect[1], -1, 1 );
			glMatrixMode( GL_MODELVIEW );
			glLoadIdentity();
			glBegin( GL_QUADS );
			glTexCoord2f( uv[0], uv[1] ); glVertex2f( 0, 0 );
			glTexCoord2f( uv[2], uv[1] ); glVertex2f( 1, 0 );
			glTexCoord2f( uv[2], uv[3] ); glVertex2f( 1, 1 );
			glTexCoord2f( uv[0], uv[3] ); glVertex2f( 0, 1 );
			glEnd();
		}
		else if( base->type == XR_TYPE_COMPOSITION_LAYER_QUAD && !rt.shot_world )
		{
			const XrCompositionLayerQuad *quad = (const XrCompositionLayerQuad *)base;
			XrPosef space_pose, quad_pose;
			mat4 model, view_m;
			float w = quad->size.width * 0.5f, h = quad->size.height * 0.5f;

			if( quad->eyeVisibility == XR_EYE_VISIBILITY_LEFT && eye != 0 )
				continue;
			if( quad->eyeVisibility == XR_EYE_VISIBILITY_RIGHT && eye != 1 )
				continue;
			if( !SpacePose( SpaceFromHandle( quad->space ), &space_pose ))
				continue;
			quad_pose = PoseMul( space_pose, quad->pose );
			BindSubImage( &quad->subImage, uv );
			SetLayerBlend( quad->layerFlags );
			ProjectionForFov( ShotFov( eye ), 0.02f, 100.0f );
			glMatrixMode( GL_MODELVIEW );
			view_m = Mat4FromPose( view );
			model = Mat4FromPose( quad_pose );
			glLoadMatrixf( view_m.m );
			glMultMatrixf( model.m );
			glBegin( GL_QUADS );
			glTexCoord2f( uv[0], uv[1] ); glVertex3f( -w, -h, 0 );
			glTexCoord2f( uv[2], uv[1] ); glVertex3f( w, -h, 0 );
			glTexCoord2f( uv[2], uv[3] ); glVertex3f( w, h, 0 );
			glTexCoord2f( uv[0], uv[3] ); glVertex3f( -w, h, 0 );
			glEnd();
		}
	}
}

static void Compose( const XrFrameEndInfo *info )
{
	GLint draw_fbo = 0, read_fbo = 0, program = 0, active = 0, pack_buffer = 0, viewport[4];
	GLboolean srgb;
	uint32_t width = rt.shot_width * (( rt.shot_eyes == 3 ) ? 2 : 1), height = rt.shot_height;
	uint8_t *pixels;
	int x = 0;

	if( !LoadGL( ))
		return;

	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo );
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_fbo );
	glGetIntegerv( GL_CURRENT_PROGRAM, &program );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &active );
	glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer );
	glGetIntegerv( GL_VIEWPORT, viewport );
	srgb = glIsEnabled( GL_FRAMEBUFFER_SRGB );
	glPushAttrib( GL_ALL_ATTRIB_BITS );
	glPushClientAttrib( GL_CLIENT_ALL_ATTRIB_BITS );
	glMatrixMode( GL_TEXTURE ); glPushMatrix(); glLoadIdentity();
	glMatrixMode( GL_PROJECTION ); glPushMatrix();
	glMatrixMode( GL_MODELVIEW ); glPushMatrix();

	if( !rt.compose_fbo || rt.compose_width != width || rt.compose_height != height )
	{
		if( rt.compose_fbo )
		{
			gl.DeleteFramebuffers( 1, &rt.compose_fbo );
			glDeleteTextures( 1, &rt.compose_texture );
		}
		glGenTextures( 1, &rt.compose_texture );
		glBindTexture( GL_TEXTURE_2D, rt.compose_texture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		gl.GenFramebuffers( 1, &rt.compose_fbo );
		gl.BindFramebuffer( GL_FRAMEBUFFER, rt.compose_fbo );
		gl.FramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.compose_texture, 0 );
		if( gl.CheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			Log( "compose framebuffer incomplete" );
		rt.compose_width = width;
		rt.compose_height = height;
	}

	gl.UseProgram( 0 );
	gl.BindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	for( int unit = 3; unit >= 0; --unit )
	{
		gl.ActiveTexture( GL_TEXTURE0 + unit );
		glDisable( GL_TEXTURE_2D );
		glMatrixMode( GL_TEXTURE );
		glLoadIdentity();
	}
	glEnable( GL_TEXTURE_2D );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_ALPHA_TEST );
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_STENCIL_TEST );
	glDisable( GL_FOG );
	glDisable( GL_LIGHTING );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glColor4f( 1, 1, 1, 1 );
	glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

	/* sRGB target + sRGB swapchains: blending happens in linear space like a real compositor */
	gl.BindFramebuffer( GL_FRAMEBUFFER, rt.compose_fbo );
	glEnable( GL_FRAMEBUFFER_SRGB );
	glViewport( 0, 0, width, height );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT );
	for( int eye = 0; eye < 2; ++eye )
	{
		if( !( rt.shot_eyes & ( 1 << eye )))
			continue;
		ComposeEye( eye, info, x );
		x += rt.shot_width;
	}

	pixels = malloc( (size_t)width * height * 4 );
	if( pixels )
	{
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
		if( WritePNG( rt.shot_path, pixels, width, height ))
			Log( "shot %s (%ux%u, frame %llu, %u layers)", rt.shot_path, width, height,
				(unsigned long long)rt.frame_index, info->layerCount );
		else
			Log( "could not write %s", rt.shot_path );
		free( pixels );
	}

	/* every quad layer's image on its own: <shot>.layerN.png (raw texels, sRGB values as stored) */
	for( uint32_t i = 0; rt.shot_layers && i < info->layerCount; ++i )
	{
		const XrCompositionLayerQuad *quad = (const XrCompositionLayerQuad *)info->layers[i];
		const swapchain_t *chain;
		char path[1100];
		uint8_t *texels;

		if( quad->type != XR_TYPE_COMPOSITION_LAYER_QUAD )
			continue;
		chain = FROM_HANDLE( const swapchain_t, quad->subImage.swapchain );
		texels = malloc( (size_t)chain->width * chain->height * 4 );
		if( !texels )
			continue;
		glBindTexture( GL_TEXTURE_2D, chain->textures[chain->acquired] );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels );
		snprintf( path, sizeof( path ), "%.*s.layer%u.png", (int)strlen( rt.shot_path ) - 4, rt.shot_path, i );
		if( WritePNG( path, texels, chain->width, chain->height ))
			Log( "layer %u (%ux%u, %.3fx%.3f m) -> %s", i, chain->width, chain->height, quad->size.width, quad->size.height, path );
		free( texels );
	}

	glMatrixMode( GL_MODELVIEW ); glPopMatrix();
	glMatrixMode( GL_PROJECTION ); glPopMatrix();
	glMatrixMode( GL_TEXTURE ); glPopMatrix();
	glPopClientAttrib();
	glPopAttrib();
	if( srgb ) glEnable( GL_FRAMEBUFFER_SRGB ); else glDisable( GL_FRAMEBUFFER_SRGB );
	gl.BindFramebuffer( GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo );
	gl.BindFramebuffer( GL_READ_FRAMEBUFFER, (GLuint)read_fbo );
	gl.BindBuffer( GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer );
	gl.ActiveTexture( (GLenum)active );
	gl.UseProgram( (GLuint)program );
	glViewport( viewport[0], viewport[1], viewport[2], viewport[3] );
}



/* ---------- projection check ---------- */

#define RAD2DEG( a ) (( a ) * 180.0f / (float)M_PI )

/* Compare every projection view the app submits with what LocateViews handed out: field of view, eye pose, and
 * whether the image's pixel aspect fits the declared field of view (a squashed/stretched world otherwise). Logs
 * "fakexr: projection mismatch ..." (at most once a second, and whenever the problem changes) and counts frames. */
static void CheckProjection( const XrFrameEndInfo *info )
{
	char problem[256] = "";

	rt.frame_check[0].valid = rt.frame_check[1].valid = 0;
	if( !rt.views_given )
		return;
	for( uint32_t i = 0; i < info->layerCount; ++i )
	{
		const XrCompositionLayerProjection *proj = (const XrCompositionLayerProjection *)info->layers[i];
		XrPosef space_pose;

		if( proj->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION || !SpacePose( SpaceFromHandle( proj->space ), &space_pose ))
			continue;
		for( uint32_t eye = 0; eye < proj->viewCount && eye < 2; ++eye )
		{
			const XrCompositionLayerProjectionView *pv = &proj->views[eye];
			XrPosef pose = PoseMul( space_pose, pv->pose );
			XrFovf g = rt.given_fov[eye], f = pv->fov;
			XrVector3f axis;
			float fov_deg = 0, pos_mm, rot_deg, aspect, a[4], b[4];
			float w = (float)pv->subImage.imageRect.extent.width, h = (float)pv->subImage.imageRect.extent.height;

			a[0] = g.angleLeft; a[1] = g.angleRight; a[2] = g.angleUp; a[3] = g.angleDown;
			b[0] = f.angleLeft; b[1] = f.angleRight; b[2] = f.angleUp; b[3] = f.angleDown;
			for( int k = 0; k < 4; ++k )
				fov_deg = fmaxf( fov_deg, fabsf( RAD2DEG( a[k] - b[k] )));
			pos_mm = 1000.0f * sqrtf(( pose.position.x - rt.given_pose[eye].position.x ) * ( pose.position.x - rt.given_pose[eye].position.x ) +
				( pose.position.y - rt.given_pose[eye].position.y ) * ( pose.position.y - rt.given_pose[eye].position.y ) +
				( pose.position.z - rt.given_pose[eye].position.z ) * ( pose.position.z - rt.given_pose[eye].position.z ));
			rot_deg = RAD2DEG( QuatDelta( rt.given_pose[eye].orientation, pose.orientation, &axis ));
			/* pixel aspect: (image w / h) against (tan extent x / tan extent y) of the fov it says it used */
			aspect = ( h > 0 ) ? ( w / h ) / (( tanf( f.angleRight ) - tanf( f.angleLeft )) / ( tanf( f.angleUp ) - tanf( f.angleDown ))) : 1.0f;

			rt.frame_check[eye].fov_deg = fov_deg;
			rt.frame_check[eye].pos_mm = pos_mm;
			rt.frame_check[eye].rot_deg = rot_deg;
			rt.frame_check[eye].aspect = aspect;
			rt.frame_check[eye].valid = 1;
			rt.worst_fov_deg = fmaxf( rt.worst_fov_deg, fov_deg );
			rt.worst_pos_mm = fmaxf( rt.worst_pos_mm, pos_mm );
			rt.worst_rot_deg = fmaxf( rt.worst_rot_deg, rot_deg );
			if( fabsf( aspect - 1.0f ) > fabsf( rt.worst_aspect - 1.0f ))
				rt.worst_aspect = aspect;

			if( !problem[0] && ( fov_deg > 0.05f || pos_mm > 0.5f || rot_deg > 0.05f || fabsf( aspect - 1.0f ) > 0.01f ))
			{
				snprintf( problem, sizeof( problem ),
					"%s eye: fov given %.1f/%.1f/%.1f/%.1f submitted %.1f/%.1f/%.1f/%.1f deg (l/r/u/d), pose off %.1f mm %.2f deg, "
					"pixel aspect %.3f (image %ux%u)", eye ? "right" : "left",
					RAD2DEG( g.angleLeft ), RAD2DEG( g.angleRight ), RAD2DEG( g.angleUp ), RAD2DEG( g.angleDown ),
					RAD2DEG( f.angleLeft ), RAD2DEG( f.angleRight ), RAD2DEG( f.angleUp ), RAD2DEG( f.angleDown ),
					pos_mm, rot_deg, aspect, pv->subImage.imageRect.extent.width, pv->subImage.imageRect.extent.height );
			}
		}
	}
	rt.projection_frames++;
	if( problem[0] )
	{
		struct timespec now;
		clock_gettime( CLOCK_MONOTONIC, &now );
		rt.projection_bad_frames++;
		if( strcmp( problem, rt.last_projection_problem ) || now.tv_sec - rt.last_projection_log.tv_sec >= 1 )
		{
			Log( "projection mismatch (frame %llu): %s", (unsigned long long)rt.frame_index, problem );
			rt.last_projection_log = now;
		}
	}
	snprintf( rt.last_projection_problem, sizeof( rt.last_projection_problem ), "%s", problem );
}

/* ---------- shot metadata (<shot>.json) ---------- */

static void JsonPose( FILE *f, XrPosef p )
{
	float pitch, yaw, roll;
	QuatToAngles( p.orientation, &pitch, &yaw, &roll );
	fprintf( f, "{\"pos\": [%.4f, %.4f, %.4f], \"ang\": [%.2f, %.2f, %.2f]}",
		p.position.x, p.position.y, p.position.z, pitch, yaw, roll );
}

static void JsonSpace( FILE *f, const space_t *space )
{
	if( space->kind == SPACE_REFERENCE )
	{
		fprintf( f, "\"%s\"", space->reference == XR_REFERENCE_SPACE_TYPE_VIEW ? "view" :
			space->reference == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : "local" );
		return;
	}
	{
		int hand = HandFromPath( space->subaction ), is_aim = 0;
		for( uint32_t i = 0; i < rt.binding_count; ++i )
		{
			const binding_t *b = &rt.bindings[i];
			if( b->profile == rt.profile && b->action == space->action && hand >= 0 && PathIsUnderHand( b->path, hand ))
			{
				is_aim = strstr( PathString( b->path ), "/aim/pose" ) != NULL;
				break;
			}
		}
		fprintf( f, "\"%s %s %s\"", hand == 0 ? "left" : hand == 1 ? "right" : "?",
			is_aim ? "aim" : "grip", space->action ? space->action->name : "" );
	}
}

/* pixel box of a quad in the shot PNG for one eye (top-left origin); 0 when it is not in front of the eye */
static int QuadScreenBox( const XrPosef *quad_pose, XrExtent2Df size, int eye, int x_offset, float box[4] )
{
	XrPosef view = PoseInv( EyePose( eye ));
	XrFovf fov = ShotFov( eye );
	float tl = tanf( fov.angleLeft ), tr = tanf( fov.angleRight ), tu = tanf( fov.angleUp ), td = tanf( fov.angleDown );

	box[0] = box[1] = 1e9f;
	box[2] = box[3] = -1e9f;
	for( int c = 0; c < 4; ++c )
	{
		XrPosef corner = PoseIdentity();
		XrVector3f p;
		float u, v;

		corner.position.x = ( c & 1 ? 0.5f : -0.5f ) * size.width;
		corner.position.y = ( c & 2 ? 0.5f : -0.5f ) * size.height;
		p = PoseMul( view, PoseMul( *quad_pose, corner )).position;
		if( p.z > -0.01f )
			return 0;
		u = ( p.x / -p.z - tl ) / ( tr - tl );
		v = ( p.y / -p.z - td ) / ( tu - td );
		u = x_offset + u * rt.shot_width;
		v = ( 1.0f - v ) * rt.shot_height;
		if( u < box[0] ) box[0] = u;
		if( v < box[1] ) box[1] = v;
		if( u > box[2] ) box[2] = u;
		if( v > box[3] ) box[3] = v;
	}
	return 1;
}

static void WriteShotJSON( const XrFrameEndInfo *info )
{
	char path[1100], temp[1110];
	FILE *f;

	snprintf( path, sizeof( path ), "%.*s.json", (int)strlen( rt.shot_path ) - 4, rt.shot_path );
	snprintf( temp, sizeof( temp ), "%s.tmp", path );
	if( !( f = fopen( temp, "w" )))
		return;
	fprintf( f, "{\n\"frame\": %llu, \"session\": \"%s\", \"eye_size\": %u, \"eyes\": \"%s\",\n",
		(unsigned long long)rt.frame_index, SessionStateName( rt.session_state ), rt.eye_size,
		rt.shot_eyes == 3 ? "both" : rt.shot_eyes == 2 ? "right" : "left" );
	fprintf( f, "\"shot_size\": [%u, %u], \"rect\": [%.1f, %.1f, %.1f, %.1f],\n", rt.shot_width, rt.shot_height,
		rt.shot_rect[0] * rt.eye_size, rt.shot_rect[1] * rt.eye_size, rt.shot_rect[2] * rt.eye_size, rt.shot_rect[3] * rt.eye_size );
	fprintf( f, "\"world_only\": %s,\n", rt.shot_world ? "true" : "false" );
	for( int eye = 0; eye < 2; ++eye )
	{
		XrFovf fv = rt.fov[eye];
		fprintf( f, "\"eye_%s\": {\"pose\": ", eye ? "right" : "left" );
		JsonPose( f, EyePose( eye ));
		fprintf( f, ", \"fov\": [%.4f, %.4f, %.4f, %.4f]", RAD2DEG( fv.angleLeft ), RAD2DEG( fv.angleRight ),
			RAD2DEG( fv.angleUp ), RAD2DEG( fv.angleDown ));
		if( rt.frame_check[eye].valid )
			fprintf( f, ", \"check\": {\"fov_deg\": %.3f, \"pos_mm\": %.2f, \"rot_deg\": %.3f, \"pixel_aspect\": %.4f}",
				rt.frame_check[eye].fov_deg, rt.frame_check[eye].pos_mm, rt.frame_check[eye].rot_deg, rt.frame_check[eye].aspect );
		fprintf( f, "},\n" );
	}
	fprintf( f, "\"head\": " ); JsonPose( f, rt.head );
	for( int h = 0; h < 2; ++h )
	{
		fprintf( f, ",\n\"%s\": {\"tracked\": %s, \"aim\": ", h ? "right" : "left", rt.hands[h].tracked ? "true" : "false" );
		JsonPose( f, rt.hands[h].aim );
		fprintf( f, ", \"grip\": " );
		JsonPose( f, rt.hands[h].grip );
		fprintf( f, ", \"moving\": %s}", rt.motion[h].active ? "true" : "false" );
	}
	fprintf( f, ",\n\"layers\": [" );
	for( uint32_t i = 0; i < info->layerCount; ++i )
	{
		const XrCompositionLayerBaseHeader *base = info->layers[i];

		fprintf( f, "%s\n  {\"index\": %u, \"flags\": %llu", i ? "," : "", i, (unsigned long long)base->layerFlags );
		if( base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION )
		{
			const XrCompositionLayerProjection *proj = (const XrCompositionLayerProjection *)base;
			fprintf( f, ", \"type\": \"projection\", \"space\": " );
			JsonSpace( f, SpaceFromHandle( proj->space ));
			fprintf( f, ", \"views\": %u", proj->viewCount );
		}
		else if( base->type == XR_TYPE_COMPOSITION_LAYER_QUAD )
		{
			const XrCompositionLayerQuad *quad = (const XrCompositionLayerQuad *)base;
			const swapchain_t *chain = FROM_HANDLE( const swapchain_t, quad->subImage.swapchain );
			XrPosef space_pose, pose;
			int tracked = SpacePose( SpaceFromHandle( quad->space ), &space_pose );

			fprintf( f, ", \"type\": \"quad\", \"space\": " );
			JsonSpace( f, SpaceFromHandle( quad->space ));
			fprintf( f, ", \"tracked\": %s, \"eye\": \"%s\", \"size\": [%.4f, %.4f], \"image\": [%u, %u, %d, %d, %d, %d]",
				tracked ? "true" : "false",
				quad->eyeVisibility == XR_EYE_VISIBILITY_LEFT ? "left" : quad->eyeVisibility == XR_EYE_VISIBILITY_RIGHT ? "right" : "both",
				quad->size.width, quad->size.height, chain->width, chain->height,
				quad->subImage.imageRect.offset.x, quad->subImage.imageRect.offset.y,
				quad->subImage.imageRect.extent.width, quad->subImage.imageRect.extent.height );
			if( tracked )
			{
				int x = 0;
				pose = PoseMul( space_pose, quad->pose );
				fprintf( f, ",\n   \"pose\": " );
				JsonPose( f, pose );
				fprintf( f, ", \"from_head\": " );
				JsonPose( f, PoseMul( PoseInv( rt.head ), pose ));
				/* where it lands in the PNG of this shot: [x0, y0, x1, y1], may reach outside the eye */
				for( int eye = 0; eye < 2; ++eye )
				{
					float box[4];
					if( !( rt.shot_eyes & ( 1 << eye )))
						continue;
					fprintf( f, ", \"png_%s\": ", eye ? "right" : "left" );
					if( QuadScreenBox( &pose, quad->size, eye, x, box ))
						fprintf( f, "[%.0f, %.0f, %.0f, %.0f]", box[0], box[1], box[2], box[3] );
					else
						fprintf( f, "null" );
					x += rt.shot_width;
				}
			}
		}
		else
			fprintf( f, ", \"type\": \"other\", \"structure\": %d", (int)base->type );
		fprintf( f, "}" );
	}
	fprintf( f, "\n]\n}\n" );
	fclose( f );
	rename( temp, path );
}

/* ---------- OpenXR: instance ---------- */

#define CHECK_TYPE( p, t ) do { if( !( p ) || ( p )->type != ( t )) return XR_ERROR_VALIDATION_FAILURE; } while( 0 )

static XrResult XRAPI_CALL EnumerateInstanceExtensionProperties( const char *layer, uint32_t capacity, uint32_t *count,
	XrExtensionProperties *props )
{
	(void)layer;
	*count = 1;
	if( !capacity )
		return XR_SUCCESS;
	if( capacity < 1 )
		return XR_ERROR_SIZE_INSUFFICIENT;
	strcpy( props[0].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME );
	props[0].extensionVersion = XR_KHR_opengl_enable_SPEC_VERSION;
	return XR_SUCCESS;
}

static void InitDefaults( void )
{
	const char *env;
	float l = DEG2RAD( 52.0f ), r = DEG2RAD( 44.0f ), u = DEG2RAD( 48.0f ), d;

	/* the down angle makes the vertical extent equal the horizontal one: the recommended image is square, so
	 * pixels stay square (it was 52 deg, which made every image 6 % taller per pixel than wide) */
	d = atanf( tanf( l ) + tanf( r ) - tanf( u ));

	memset( &rt, 0, sizeof( rt ));
	rt.control_fd = -1;
	rt.ipd = ( env = getenv( "FAKEXR_IPD" )) ? (float)atof( env ) : 0.064f;
	rt.eye_size = ( env = getenv( "FAKEXR_EYE_SIZE" )) ? (uint32_t)atoi( env ) : 960;
	if( rt.eye_size < 64 || rt.eye_size > 4096 )
		rt.eye_size = 960;
	rt.fps = ( env = getenv( "FAKEXR_FPS" )) ? (float)atof( env ) : 90.0f;
	rt.grip_pitch = ( env = getenv( "FAKEXR_GRIP_PITCH" )) ? (float)atof( env ) : 0.0f;
	verbose = ( env = getenv( "FAKEXR_VERBOSE" )) && atoi( env );

	/* asymmetric like a real headset: each eye sees further outwards */
	rt.fov[0].angleLeft = -l; rt.fov[0].angleRight = r; rt.fov[0].angleUp = u; rt.fov[0].angleDown = -d;
	rt.fov[1].angleLeft = -r; rt.fov[1].angleRight = l; rt.fov[1].angleUp = u; rt.fov[1].angleDown = -d;

	/* standing still at the LOCAL origin, controllers held in front, pointing forward */
	rt.head = PoseIdentity();
	rt.hands[0].aim = PoseFromWords( -0.2f, -0.45f, -0.35f, 0, 0, 0 );
	rt.hands[1].aim = PoseFromWords( 0.2f, -0.45f, -0.35f, 0, 0, 0 );
	rt.hands[0].tracked = rt.hands[1].tracked = 1;
	UpdateGrip( 0 );
	UpdateGrip( 1 );

	if(( env = getenv( "FAKEXR_CONTROL" )) && *env )
	{
		/* read-write: we never see EOF when the writer goes away */
		rt.control_fd = open( env, O_RDWR | O_NONBLOCK );
		if( rt.control_fd < 0 )
			Log( "cannot open control FIFO %s: %s", env, strerror( errno ));
	}
}

static XrResult XRAPI_CALL CreateInstance( const XrInstanceCreateInfo *info, XrInstance *instance )
{
	CHECK_TYPE( info, XR_TYPE_INSTANCE_CREATE_INFO );
	for( uint32_t i = 0; i < info->enabledExtensionCount; ++i )
	{
		if( strcmp( info->enabledExtensionNames[i], XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ))
			return XR_ERROR_EXTENSION_NOT_PRESENT;
	}
	if( rt.instance_alive )
		return XR_ERROR_LIMIT_REACHED;
	InitDefaults();
	rt.instance_alive = 1;
	rt.profile = PathIntern( ( getenv( "FAKEXR_PROFILE" ) && *getenv( "FAKEXR_PROFILE" )) ?
		getenv( "FAKEXR_PROFILE" ) : "/interaction_profiles/oculus/touch_controller" );
	rt.hand_path[0] = PathIntern( "/user/hand/left" );
	rt.hand_path[1] = PathIntern( "/user/hand/right" );
	*instance = TO_HANDLE( XrInstance, &rt.instance_alive );
	Log( "instance created for \"%s\" (eye %ux%u, %.0f fps, profile %s, control %s)", info->applicationInfo.applicationName,
		rt.eye_size, rt.eye_size, rt.fps, PathString( rt.profile ), rt.control_fd >= 0 ? "on" : "off" );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL DestroyInstance( XrInstance instance )
{
	(void)instance;
	if( rt.control_fd >= 0 )
		close( rt.control_fd );
	rt.control_fd = -1;
	rt.instance_alive = 0;
	Log( "instance destroyed (%llu frames, %llu haptic pulses)", (unsigned long long)rt.frame_index,
		(unsigned long long)rt.haptic_count );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL GetInstanceProperties( XrInstance instance, XrInstanceProperties *props )
{
	(void)instance;
	CHECK_TYPE( props, XR_TYPE_INSTANCE_PROPERTIES );
	props->runtimeVersion = XR_MAKE_VERSION( 0, 1, 0 );
	strcpy( props->runtimeName, "fakexr" );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL ResultToString( XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE] )
{
	(void)instance;
	snprintf( buffer, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", (int)value );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL StructureTypeToString( XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE] )
{
	(void)instance;
	snprintf( buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_STRUCTURE_TYPE_%d", (int)value );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL PollEvent( XrInstance instance, XrEventDataBuffer *event )
{
	(void)instance;
	ControlPoll();
	if( !rt.event_count )
		return XR_EVENT_UNAVAILABLE;
	memcpy( event, &rt.events[rt.event_head], sizeof( *event ));
	rt.event_head = ( rt.event_head + 1 ) % MAX_EVENTS;
	rt.event_count--;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL StringToPath( XrInstance instance, const char *string, XrPath *path )
{
	(void)instance;
	if( !string || string[0] != '/' )
		return XR_ERROR_PATH_FORMAT_INVALID;
	*path = PathIntern( string );
	return *path == XR_NULL_PATH ? XR_ERROR_PATH_COUNT_EXCEEDED : XR_SUCCESS;
}

static XrResult XRAPI_CALL PathToString( XrInstance instance, XrPath path, uint32_t capacity, uint32_t *count, char *buffer )
{
	const char *s = PathString( path );
	(void)instance;
	if( !*s )
		return XR_ERROR_PATH_INVALID;
	*count = (uint32_t)strlen( s ) + 1;
	if( !capacity )
		return XR_SUCCESS;
	if( capacity < *count )
		return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy( buffer, s, *count );
	return XR_SUCCESS;
}

/* ---------- system ---------- */

static XrResult XRAPI_CALL GetSystem( XrInstance instance, const XrSystemGetInfo *info, XrSystemId *system )
{
	(void)instance;
	CHECK_TYPE( info, XR_TYPE_SYSTEM_GET_INFO );
	if( info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY )
		return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
	*system = 1;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL GetSystemProperties( XrInstance instance, XrSystemId system, XrSystemProperties *props )
{
	(void)instance; (void)system;
	CHECK_TYPE( props, XR_TYPE_SYSTEM_PROPERTIES );
	props->systemId = 1;
	props->vendorId = 0;
	strcpy( props->systemName, "fakexr headset" );
	props->graphicsProperties.maxSwapchainImageWidth = 8192;
	props->graphicsProperties.maxSwapchainImageHeight = 8192;
	props->graphicsProperties.maxLayerCount = 16;
	props->trackingProperties.orientationTracking = XR_TRUE;
	props->trackingProperties.positionTracking = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL EnumerateViewConfigurations( XrInstance instance, XrSystemId system, uint32_t capacity,
	uint32_t *count, XrViewConfigurationType *types )
{
	(void)instance; (void)system;
	*count = 1;
	if( capacity >= 1 )
		types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL GetViewConfigurationProperties( XrInstance instance, XrSystemId system, XrViewConfigurationType type,
	XrViewConfigurationProperties *props )
{
	(void)instance; (void)system;
	if( type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO )
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	props->viewConfigurationType = type;
	props->fovMutable = XR_FALSE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL EnumerateViewConfigurationViews( XrInstance instance, XrSystemId system, XrViewConfigurationType type,
	uint32_t capacity, uint32_t *count, XrViewConfigurationView *views )
{
	(void)instance; (void)system;
	if( type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO )
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	*count = 2;
	if( !capacity )
		return XR_SUCCESS;
	if( capacity < 2 )
		return XR_ERROR_SIZE_INSUFFICIENT;
	for( int i = 0; i < 2; ++i )
	{
		views[i].recommendedImageRectWidth = views[i].recommendedImageRectHeight = rt.eye_size;
		views[i].maxImageRectWidth = views[i].maxImageRectHeight = 8192;
		views[i].recommendedSwapchainSampleCount = views[i].maxSwapchainSampleCount = 1;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL EnumerateEnvironmentBlendModes( XrInstance instance, XrSystemId system, XrViewConfigurationType type,
	uint32_t capacity, uint32_t *count, XrEnvironmentBlendMode *modes )
{
	(void)instance; (void)system; (void)type;
	*count = 1;
	if( capacity >= 1 )
		modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL GetOpenGLGraphicsRequirementsKHR( XrInstance instance, XrSystemId system,
	XrGraphicsRequirementsOpenGLKHR *req )
{
	(void)instance; (void)system;
	CHECK_TYPE( req, XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR );
	req->minApiVersionSupported = XR_MAKE_VERSION( 2, 1, 0 );
	req->maxApiVersionSupported = XR_MAKE_VERSION( 4, 6, 0 );
	return XR_SUCCESS;
}

/* ---------- session ---------- */

static XrResult XRAPI_CALL CreateSession( XrInstance instance, const XrSessionCreateInfo *info, XrSession *session )
{
	(void)instance;
	CHECK_TYPE( info, XR_TYPE_SESSION_CREATE_INFO );
	if( rt.session_alive )
		return XR_ERROR_LIMIT_REACHED;
	if( !info->next )
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	rt.session_alive = 1;
	rt.session_running = 0;
	rt.attached = 0;
	*session = TO_HANDLE( XrSession, &rt.session_alive );
	PushSessionState( XR_SESSION_STATE_IDLE );
	PushSessionState( XR_SESSION_STATE_READY );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL DestroySession( XrSession session )
{
	(void)session;
	if( rt.compose_fbo && LoadGL( ))
	{
		gl.DeleteFramebuffers( 1, &rt.compose_fbo );
		glDeleteTextures( 1, &rt.compose_texture );
	}
	rt.compose_fbo = rt.compose_texture = 0;
	rt.compose_width = rt.compose_height = 0;
	rt.session_alive = rt.session_running = 0;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL BeginSession( XrSession session, const XrSessionBeginInfo *info )
{
	(void)session;
	CHECK_TYPE( info, XR_TYPE_SESSION_BEGIN_INFO );
	if( rt.session_running )
		return XR_ERROR_SESSION_RUNNING;
	rt.session_running = 1;
	PushSessionState( XR_SESSION_STATE_SYNCHRONIZED );
	PushSessionState( XR_SESSION_STATE_VISIBLE );
	PushSessionState( XR_SESSION_STATE_FOCUSED );
	if( rt.attached )
	{
		XrEventDataInteractionProfileChanged changed;
		memset( &changed, 0, sizeof( changed ));
		changed.type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
		changed.session = TO_HANDLE( XrSession, &rt.session_alive );
		PushEvent( &changed, sizeof( changed ));
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL EndSession( XrSession session )
{
	(void)session;
	if( !rt.session_running )
		return XR_ERROR_SESSION_NOT_RUNNING;
	rt.session_running = 0;
	PushSessionState( XR_SESSION_STATE_IDLE );
	PushSessionState( XR_SESSION_STATE_EXITING );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL RequestExitSession( XrSession session )
{
	(void)session;
	PushSessionState( XR_SESSION_STATE_STOPPING );
	return XR_SUCCESS;
}

/* ---------- spaces ---------- */

static XrResult XRAPI_CALL EnumerateReferenceSpaces( XrSession session, uint32_t capacity, uint32_t *count, XrReferenceSpaceType *spaces )
{
	(void)session;
	*count = 2;
	if( !capacity )
		return XR_SUCCESS;
	if( capacity < 2 )
		return XR_ERROR_SIZE_INSUFFICIENT;
	spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
	spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL CreateReferenceSpace( XrSession session, const XrReferenceSpaceCreateInfo *info, XrSpace *space )
{
	space_t *s;
	(void)session;
	CHECK_TYPE( info, XR_TYPE_REFERENCE_SPACE_CREATE_INFO );
	if( info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW && info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL )
		return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
	s = calloc( 1, sizeof( *s ));
	if( !s )
		return XR_ERROR_OUT_OF_MEMORY;
	s->kind = SPACE_REFERENCE;
	s->reference = info->referenceSpaceType;
	s->offset = info->poseInReferenceSpace;
	*space = TO_HANDLE( XrSpace, s );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL CreateActionSpace( XrSession session, const XrActionSpaceCreateInfo *info, XrSpace *space )
{
	space_t *s;
	(void)session;
	CHECK_TYPE( info, XR_TYPE_ACTION_SPACE_CREATE_INFO );
	s = calloc( 1, sizeof( *s ));
	if( !s )
		return XR_ERROR_OUT_OF_MEMORY;
	s->kind = SPACE_ACTION;
	s->action = FROM_HANDLE( action_t, info->action );
	s->subaction = info->subactionPath;
	s->offset = info->poseInActionSpace;
	*space = TO_HANDLE( XrSpace, s );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL DestroySpace( XrSpace space )
{
	free( FROM_HANDLE( space_t, space ));
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL LocateSpace( XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location )
{
	XrPosef a, b;
	XrSpaceVelocity *velocity = NULL;
	(void)time;

	CHECK_TYPE( location, XR_TYPE_SPACE_LOCATION );
	for( XrBaseOutStructure *next = (XrBaseOutStructure *)location->next; next; next = next->next )
	{
		if( next->type == XR_TYPE_SPACE_VELOCITY )
			velocity = (XrSpaceVelocity *)next;
	}
	location->locationFlags = 0;
	if( velocity )
	{
		velocity->velocityFlags = 0;
		memset( &velocity->linearVelocity, 0, sizeof( velocity->linearVelocity ));
		memset( &velocity->angularVelocity, 0, sizeof( velocity->angularVelocity ));
	}
	if( !SpacePose( FROM_HANDLE( space_t, space ), &a ) || !SpacePose( FROM_HANDLE( space_t, base ), &b ))
	{
		location->pose = PoseIdentity();
		return XR_SUCCESS;
	}
	location->pose = PoseMul( PoseInv( b ), a );
	location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
		XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	if( velocity )
	{
		const space_t *located = FROM_HANDLE( space_t, space );
		int hand = located->kind == SPACE_ACTION ? HandFromPath( located->subaction ) : -1;

		const motion_t *m = hand >= 0 ? &rt.motion[hand] :
			( located->kind == SPACE_REFERENCE && located->reference == XR_REFERENCE_SPACE_TYPE_VIEW ) ? &rt.motion[MOTION_HEAD] : NULL;

		velocity->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
		/* the base spaces the engine uses (LOCAL/STAGE) are not rotated against LOCAL here */
		if( m )
		{
			velocity->linearVelocity = m->linear;
			velocity->angularVelocity = m->angular;
		}
		if( hand >= 0 )
		{
			velocity->linearVelocity.x += rt.hands[hand].velocity.x;
			velocity->linearVelocity.y += rt.hands[hand].velocity.y;
			velocity->linearVelocity.z += rt.hands[hand].velocity.z;
		}
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL LocateViews( XrSession session, const XrViewLocateInfo *info, XrViewState *state, uint32_t capacity,
	uint32_t *count, XrView *views )
{
	XrPosef base;
	(void)session;

	CHECK_TYPE( info, XR_TYPE_VIEW_LOCATE_INFO );
	*count = 2;
	if( !capacity )
		return XR_SUCCESS;
	if( capacity < 2 )
		return XR_ERROR_SIZE_INSUFFICIENT;
	if( !SpacePose( FROM_HANDLE( space_t, info->space ), &base ))
	{
		state->viewStateFlags = 0;
		return XR_SUCCESS;
	}
	for( int i = 0; i < 2; ++i )
	{
		views[i].pose = PoseMul( PoseInv( base ), EyePose( i ));
		views[i].fov = rt.fov[i];
		rt.given_pose[i] = EyePose( i );
		rt.given_fov[i] = rt.fov[i];
	}
	rt.views_given = 1;
	state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
		XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
	return XR_SUCCESS;
}

/* ---------- swapchains ---------- */

static XrResult XRAPI_CALL EnumerateSwapchainFormats( XrSession session, uint32_t capacity, uint32_t *count, int64_t *formats )
{
	(void)session;
	*count = 2;
	if( !capacity )
		return XR_SUCCESS;
	if( capacity < 2 )
		return XR_ERROR_SIZE_INSUFFICIENT;
	formats[0] = GL_SRGB8_ALPHA8;
	formats[1] = GL_RGBA8;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL CreateSwapchain( XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain )
{
	swapchain_t *chain;
	GLint previous = 0;
	(void)session;

	CHECK_TYPE( info, XR_TYPE_SWAPCHAIN_CREATE_INFO );
	if( info->format != GL_SRGB8_ALPHA8 && info->format != GL_RGBA8 )
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	chain = calloc( 1, sizeof( *chain ));
	if( !chain )
		return XR_ERROR_OUT_OF_MEMORY;
	chain->format = info->format;
	chain->width = info->width;
	chain->height = info->height;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previous );
	glGenTextures( MAX_SWAPCHAIN_IMAGES, chain->textures );
	for( int i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i )
	{
		glBindTexture( GL_TEXTURE_2D, chain->textures[i] );
		glTexImage2D( GL_TEXTURE_2D, 0, (GLint)info->format, info->width, info->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	}
	glBindTexture( GL_TEXTURE_2D, (GLuint)previous );
	*swapchain = TO_HANDLE( XrSwapchain, chain );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL DestroySwapchain( XrSwapchain swapchain )
{
	swapchain_t *chain = FROM_HANDLE( swapchain_t, swapchain );
	glDeleteTextures( MAX_SWAPCHAIN_IMAGES, chain->textures );
	free( chain );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL EnumerateSwapchainImages( XrSwapchain swapchain, uint32_t capacity, uint32_t *count,
	XrSwapchainImageBaseHeader *images )
{
	swapchain_t *chain = FROM_HANDLE( swapchain_t, swapchain );
	XrSwapchainImageOpenGLKHR *gl_images = (XrSwapchainImageOpenGLKHR *)images;

	*count = MAX_SWAPCHAIN_IMAGES;
	if( !capacity )
		return XR_SUCCESS;
	if( capacity < MAX_SWAPCHAIN_IMAGES )
		return XR_ERROR_SIZE_INSUFFICIENT;
	for( int i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i )
		gl_images[i].image = chain->textures[i];
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL AcquireSwapchainImage( XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info, uint32_t *index )
{
	swapchain_t *chain = FROM_HANDLE( swapchain_t, swapchain );
	(void)info;
	*index = chain->next;
	chain->next = ( chain->next + 1 ) % MAX_SWAPCHAIN_IMAGES;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL WaitSwapchainImage( XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info )
{
	(void)swapchain; (void)info;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL ReleaseSwapchainImage( XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info )
{
	swapchain_t *chain = FROM_HANDLE( swapchain_t, swapchain );
	(void)info;
	/* the image the compositor shows is the last one released */
	chain->acquired = ( chain->next + MAX_SWAPCHAIN_IMAGES - 1 ) % MAX_SWAPCHAIN_IMAGES;
	return XR_SUCCESS;
}

/* ---------- frames ---------- */

static XrResult XRAPI_CALL WaitFrame( XrSession session, const XrFrameWaitInfo *info, XrFrameState *state )
{
	XrTime period = rt.fps > 0 ? (XrTime)( 1e9 / rt.fps ) : 11111111;
	(void)session; (void)info;

	CHECK_TYPE( state, XR_TYPE_FRAME_STATE );
	if( rt.fps > 0 && rt.last_wait.tv_sec )
	{
		struct timespec target = rt.last_wait;
		target.tv_nsec += (long)period;
		while( target.tv_nsec >= 1000000000L )
		{
			target.tv_nsec -= 1000000000L;
			target.tv_sec++;
		}
		clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL );
	}
	{
		struct timespec now;
		clock_gettime( CLOCK_MONOTONIC, &now );
		if( rt.last_wait.tv_sec )
		{
			float dt = (float)( now.tv_sec - rt.last_wait.tv_sec ) + ( now.tv_nsec - rt.last_wait.tv_nsec ) / 1e9f;
			rt.frame_interval = rt.frame_interval > 0 ? rt.frame_interval * 0.9f + dt * 0.1f : dt;
		}
		rt.last_wait = now;
	}
	ControlPoll();
	state->predictedDisplayPeriod = period;
	state->predictedDisplayTime = NowNs() + period;
	if( state->predictedDisplayTime <= rt.last_display_time )
		state->predictedDisplayTime = rt.last_display_time + 1;
	rt.last_display_time = state->predictedDisplayTime;
	MotionUpdate( NowNs( ));
	/* synchronized = headset off/asleep: the app keeps its frame loop but need not render */
	state->shouldRender = rt.session_state == XR_SESSION_STATE_VISIBLE || rt.session_state == XR_SESSION_STATE_FOCUSED;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL BeginFrame( XrSession session, const XrFrameBeginInfo *info )
{
	(void)session; (void)info;
	return rt.session_running ? XR_SUCCESS : XR_ERROR_SESSION_NOT_RUNNING;
}

static XrResult XRAPI_CALL EndFrame( XrSession session, const XrFrameEndInfo *info )
{
	(void)session;
	CHECK_TYPE( info, XR_TYPE_FRAME_END_INFO );
	rt.frame_index++;
	CheckProjection( info );
	if( rt.shot_path[0] )
	{
		WriteShotJSON( info ); /* before the PNG: the runner waits for the PNG */
		Compose( info );
		rt.shot_path[0] = '\0';
	}
	return XR_SUCCESS;
}

/* ---------- actions ---------- */

static XrResult XRAPI_CALL CreateActionSet( XrInstance instance, const XrActionSetCreateInfo *info, XrActionSet *set )
{
	(void)instance;
	CHECK_TYPE( info, XR_TYPE_ACTION_SET_CREATE_INFO );
	*set = TO_HANDLE( XrActionSet, &rt.actions ); /* one set is enough for us */
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL DestroyActionSet( XrActionSet set )
{
	(void)set;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL CreateAction( XrActionSet set, const XrActionCreateInfo *info, XrAction *action )
{
	(void)set;
	CHECK_TYPE( info, XR_TYPE_ACTION_CREATE_INFO );
	for( int i = 0; i < MAX_ACTIONS; ++i )
	{
		if( rt.actions[i].alive )
			continue;
		rt.actions[i].alive = 1;
		rt.actions[i].type = info->actionType;
		snprintf( rt.actions[i].name, sizeof( rt.actions[i].name ), "%s", info->actionName );
		*action = TO_HANDLE( XrAction, &rt.actions[i] );
		return XR_SUCCESS;
	}
	return XR_ERROR_LIMIT_REACHED;
}

static XrResult XRAPI_CALL DestroyAction( XrAction action )
{
	FROM_HANDLE( action_t, action )->alive = 0;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL SuggestInteractionProfileBindings( XrInstance instance, const XrInteractionProfileSuggestedBinding *info )
{
	(void)instance;
	CHECK_TYPE( info, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING );
	/* replace earlier suggestions for this profile */
	for( uint32_t i = 0; i < rt.binding_count; )
	{
		if( rt.bindings[i].profile == info->interactionProfile )
			rt.bindings[i] = rt.bindings[--rt.binding_count];
		else
			++i;
	}
	for( uint32_t i = 0; i < info->countSuggestedBindings && rt.binding_count < MAX_BINDINGS; ++i )
	{
		binding_t *b = &rt.bindings[rt.binding_count++];
		b->profile = info->interactionProfile;
		b->action = FROM_HANDLE( action_t, info->suggestedBindings[i].action );
		b->path = info->suggestedBindings[i].binding;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL AttachSessionActionSets( XrSession session, const XrSessionActionSetsAttachInfo *info )
{
	(void)session;
	CHECK_TYPE( info, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO );
	if( rt.attached )
		return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
	rt.attached = 1;
	return XR_SUCCESS;
}

static int ProfileHasBindings( void )
{
	for( uint32_t i = 0; i < rt.binding_count; ++i )
	{
		if( rt.bindings[i].profile == rt.profile )
			return 1;
	}
	return 0;
}

static XrResult XRAPI_CALL GetCurrentInteractionProfile( XrSession session, XrPath top, XrInteractionProfileState *state )
{
	(void)session;
	CHECK_TYPE( state, XR_TYPE_INTERACTION_PROFILE_STATE );
	state->interactionProfile = ( rt.attached && HandFromPath( top ) >= 0 && ProfileHasBindings( )) ? rt.profile : XR_NULL_PATH;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL SyncActions( XrSession session, const XrActionsSyncInfo *info )
{
	(void)session; (void)info;
	if( !rt.attached )
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	ControlPoll();
	for( uint32_t i = 0; i < rt.input_count; ++i )
	{
		memcpy( rt.inputs[i].previous, rt.inputs[i].synced, sizeof( rt.inputs[i].previous ));
		memcpy( rt.inputs[i].synced, rt.inputs[i].value, sizeof( rt.inputs[i].synced ));
	}
	return XR_SUCCESS;
}

/* combined state of every binding of the action in the active profile (for one hand, or both) */
static int ActionValue( const XrActionStateGetInfo *info, float now[2], float before[2] )
{
	action_t *action = FROM_HANDLE( action_t, info->action );
	int hand = HandFromPath( info->subactionPath );
	int bound = 0;

	now[0] = now[1] = before[0] = before[1] = 0.0f;
	/* input belongs to the focused app only (dashboard open / headset off = inactive) */
	if( rt.session_state != XR_SESSION_STATE_FOCUSED )
		return 0;
	for( uint32_t i = 0; i < rt.binding_count; ++i )
	{
		const binding_t *b = &rt.bindings[i];
		const input_t *input;
		int h;

		if( b->profile != rt.profile || b->action != action )
			continue;
		h = PathIsUnderHand( b->path, 1 ) ? 1 : PathIsUnderHand( b->path, 0 ) ? 0 : -1;
		if( hand >= 0 && h != hand )
			continue;
		if( h >= 0 && !rt.hands[h].tracked )
			continue;
		bound = 1;
		input = InputFind( b->path, 0 );
		if( !input )
			continue;
		for( int k = 0; k < 2; ++k )
		{
			if( fabsf( input->synced[k] ) > fabsf( now[k] )) now[k] = input->synced[k];
			if( fabsf( input->previous[k] ) > fabsf( before[k] )) before[k] = input->previous[k];
		}
	}
	return bound;
}

static XrResult XRAPI_CALL GetActionStateBoolean( XrSession session, const XrActionStateGetInfo *info, XrActionStateBoolean *state )
{
	float now[2], before[2];
	(void)session;
	CHECK_TYPE( state, XR_TYPE_ACTION_STATE_BOOLEAN );
	state->isActive = ActionValue( info, now, before );
	state->currentState = now[0] > 0.5f;
	state->changedSinceLastSync = state->currentState != ( before[0] > 0.5f );
	state->lastChangeTime = rt.last_display_time;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL GetActionStateFloat( XrSession session, const XrActionStateGetInfo *info, XrActionStateFloat *state )
{
	float now[2], before[2];
	(void)session;
	CHECK_TYPE( state, XR_TYPE_ACTION_STATE_FLOAT );
	state->isActive = ActionValue( info, now, before );
	state->currentState = now[0];
	state->changedSinceLastSync = now[0] != before[0];
	state->lastChangeTime = rt.last_display_time;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL GetActionStateVector2f( XrSession session, const XrActionStateGetInfo *info, XrActionStateVector2f *state )
{
	float now[2], before[2];
	(void)session;
	CHECK_TYPE( state, XR_TYPE_ACTION_STATE_VECTOR2F );
	state->isActive = ActionValue( info, now, before );
	state->currentState.x = now[0];
	state->currentState.y = now[1];
	state->changedSinceLastSync = now[0] != before[0] || now[1] != before[1];
	state->lastChangeTime = rt.last_display_time;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL GetActionStatePose( XrSession session, const XrActionStateGetInfo *info, XrActionStatePose *state )
{
	float now[2], before[2];
	(void)session;
	CHECK_TYPE( state, XR_TYPE_ACTION_STATE_POSE );
	state->isActive = ActionValue( info, now, before );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL ApplyHapticFeedback( XrSession session, const XrHapticActionInfo *info, const XrHapticBaseHeader *haptic )
{
	const XrHapticVibration *vibration = (const XrHapticVibration *)haptic;
	(void)session;
	rt.haptic_count++;
	Log( "haptic %s: %.3f s, %.0f Hz, amplitude %.2f", PathString( info->subactionPath ),
			vibration->duration == XR_INFINITE_DURATION ? -1.0 : vibration->duration / 1e9,
			vibration->frequency, vibration->amplitude );
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL StopHapticFeedback( XrSession session, const XrHapticActionInfo *info )
{
	(void)session; (void)info;
	return XR_SUCCESS;
}

/* ---------- dispatch ---------- */

static XrResult XRAPI_CALL GetInstanceProcAddr( XrInstance instance, const char *name, PFN_xrVoidFunction *function );

static const struct { const char *name; PFN_xrVoidFunction function; } functions[] = {
#define F( n ) { "xr" #n, (PFN_xrVoidFunction)n }
	F( GetInstanceProcAddr ), F( EnumerateInstanceExtensionProperties ), F( CreateInstance ), F( DestroyInstance ),
	F( GetInstanceProperties ), F( ResultToString ), F( StructureTypeToString ), F( PollEvent ), F( StringToPath ),
	F( PathToString ), F( GetSystem ), F( GetSystemProperties ), F( EnumerateViewConfigurations ),
	F( GetViewConfigurationProperties ), F( EnumerateViewConfigurationViews ), F( EnumerateEnvironmentBlendModes ),
	F( GetOpenGLGraphicsRequirementsKHR ), F( CreateSession ), F( DestroySession ), F( BeginSession ), F( EndSession ),
	F( RequestExitSession ), F( EnumerateReferenceSpaces ), F( CreateReferenceSpace ), F( CreateActionSpace ),
	F( DestroySpace ), F( LocateSpace ), F( LocateViews ), F( EnumerateSwapchainFormats ), F( CreateSwapchain ),
	F( DestroySwapchain ), F( EnumerateSwapchainImages ), F( AcquireSwapchainImage ), F( WaitSwapchainImage ),
	F( ReleaseSwapchainImage ), F( WaitFrame ), F( BeginFrame ), F( EndFrame ), F( CreateActionSet ),
	F( DestroyActionSet ), F( CreateAction ), F( DestroyAction ), F( SuggestInteractionProfileBindings ),
	F( AttachSessionActionSets ), F( GetCurrentInteractionProfile ), F( SyncActions ), F( GetActionStateBoolean ),
	F( GetActionStateFloat ), F( GetActionStateVector2f ), F( GetActionStatePose ), F( ApplyHapticFeedback ),
	F( StopHapticFeedback ),
#undef F
};

static XrResult XRAPI_CALL GetInstanceProcAddr( XrInstance instance, const char *name, PFN_xrVoidFunction *function )
{
	(void)instance;
	for( size_t i = 0; i < ARRAY_COUNT( functions ); ++i )
	{
		if( !strcmp( functions[i].name, name ))
		{
			*function = functions[i].function;
			return XR_SUCCESS;
		}
	}
	*function = NULL;
	if( verbose )
		Log( "unsupported function %s", name );
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface( const XrNegotiateLoaderInfo *loader, XrNegotiateRuntimeRequest *request )
{
	if( !loader || !request || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
		loader->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
		loader->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION )
		return XR_ERROR_INITIALIZATION_FAILED;
	request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	request->runtimeApiVersion = XR_MAKE_VERSION( 1, 0, 34 );
	request->getInstanceProcAddr = GetInstanceProcAddr;
	return XR_SUCCESS;
}
