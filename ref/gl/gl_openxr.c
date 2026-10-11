/*
gl_openxr.c - desktop OpenXR compositor for ref_gl
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#if XASH_OPENXR

#define XR_USE_GRAPHICS_API_OPENGL

#if defined( _WIN32 )
#define XR_USE_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define XR_USE_PLATFORM_XLIB
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* Avoid including the system GL headers, which conflict with ref_gl's GL shim. */
typedef XID GLXDrawable;
typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;

#ifndef GLX_FBCONFIG_ID
#define GLX_FBCONFIG_ID 0x8013
#endif

extern Display *glXGetCurrentDisplay( void );
extern GLXContext glXGetCurrentContext( void );
extern GLXDrawable glXGetCurrentDrawable( void );
extern int glXQueryContext( Display *display, GLXContext context, int attribute, int *value );
extern GLXFBConfig *glXChooseFBConfig( Display *display, int screen, const int *attributes, int *count );
extern XVisualInfo *glXGetVisualFromFBConfig( Display *display, GLXFBConfig config );
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdlib.h>
#include <string.h>

#include "gl_local.h"

/* gl_export.h intentionally exposes only the legacy constants used by ref_gl. */
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_COLOR_CLEAR_VALUE
#define GL_COLOR_CLEAR_VALUE 0x0C22
#endif
#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX 0x0C10
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT 0x0BA2
#endif

/* Hand HUD panels (vr_hud_hands): the client reports where it drew health, suit and ammo
 * (vr_hud_health_rect / vr_hud_suit_rect / vr_hud_ammo_rect). We copy those boxes out of the UI texture
 * onto two small quads anchored to the controllers (health stacked above suit on the off hand, ammo on
 * the weapon hand) and erase them from the head-locked panel. */
/* The third panel is the pointer weapon grid (client +vr_weaponpointer): on the off hand, out past the status panel,
 * shown while open, with a beam from the weapon hand; where the beam hits is sent back as vr_weapon_pointer_cursor. The
 * client draws the grid in up to three rows when it is wider than the HUD (vr_hud_weapons_rect, _weapons2_, _weapons3_);
 * here they are put side by side again, so the panel is always one row. */
enum { GL_OPENXR_HAND_HUD_STATUS, GL_OPENXR_HAND_HUD_AMMO, GL_OPENXR_HAND_HUD_WEAPONS, GL_OPENXR_HAND_HUD_COUNT };
enum { GL_OPENXR_HUD_BOX_HEALTH, GL_OPENXR_HUD_BOX_SUIT, GL_OPENXR_HUD_BOX_AMMO, GL_OPENXR_HUD_BOX_DAMAGE,
	GL_OPENXR_HUD_BOX_FLASHLIGHT, GL_OPENXR_HUD_BOX_WEAPONS, GL_OPENXR_HUD_BOX_WEAPONS2, GL_OPENXR_HUD_BOX_WEAPONS3, GL_OPENXR_HUD_BOX_COUNT };
#define GL_OPENXR_HAND_HUD_MAX_BOXES 4
/* slots side by side in the hand HUD swapchain, in units of a GL_OPENXR_HAND_HUD_WIDTH-wide image */
static const uint32_t gl_openxr_hand_hud_slot[GL_OPENXR_HAND_HUD_COUNT][2] = { { 0, 512 }, { 512, 512 }, { 1024, 1024 } };
#define GL_OPENXR_HAND_HUD_WIDTH  2048
#define GL_OPENXR_HAND_HUD_HEIGHT 512

/* Cinema screen for scripted cameras (trigger_camera): 16:9 like the menu panel. The engine computes the
 * camera FOV for this aspect (desktop widescreen rules: 4:3 vertical FOV kept, wider horizontally). */
#define GL_OPENXR_CINEMA_WIDTH  1600
#define GL_OPENXR_CINEMA_HEIGHT 900

typedef struct gl_openxr_eye_s
{
	XrSwapchain swapchain;
	uint32_t width;
	uint32_t height;
	uint32_t image_count;
	uint32_t image_index;
	XrSwapchainImageOpenGLKHR *images;
	GLuint *framebuffers;
	GLuint depth_stencil; /* eyes render straight into the swapchain at full size, so they need their own depth */
	qboolean acquired;
	qboolean waited;
	qboolean ready;
} gl_openxr_eye_t;

typedef struct gl_openxr_actions_s
{
	XrActionSet gameplay_set;
	XrPath hand_paths[REF_VR_MAX_HANDS];
	XrAction grip_pose;
	XrAction aim_pose;
	XrAction trigger;
	XrAction squeeze;
	XrAction stick;
	XrAction primary;
	XrAction secondary;
	XrAction menu;
	XrAction stick_click;
	XrAction haptic;
	XrSpace grip_spaces[REF_VR_MAX_HANDS];
	XrSpace aim_spaces[REF_VR_MAX_HANDS];
} gl_openxr_actions_t;

typedef struct gl_openxr_binding_s
{
	XrAction action;
	const char *left_path;
	const char *right_path;
} gl_openxr_binding_t;

typedef struct gl_openxr_state_s
{
	qboolean initialized;
	qboolean failed;
	qboolean session_running;
	qboolean focused;
	qboolean frame_begun;
	qboolean views_valid;
	qboolean submitted_once;
	qboolean ui_submitted_once;
	qboolean tracked_controllers_once;
	qboolean reference_changed;
	qboolean exit_requested;
	int current_eye;
	uint64_t frame_id;

	XrInstance instance;
	XrSystemId system_id;
	XrSession session;
	XrSessionState session_state;
	XrSpace local_space;
	XrSpace head_space;
	XrFrameState frame_state;
	XrView views[REF_VR_MAX_EYES];
	XrPosef head_pose;
	gl_openxr_eye_t eyes[REF_VR_MAX_EYES];
	qboolean mirror_warned;
	gl_openxr_eye_t ui;
	gl_openxr_actions_t actions;
	qboolean ui_active;
	qboolean ui_world_active;
	XrPosef ui_anchor; /* level pose captured when a menu opened (local space, GL_OpenXRPanelAnchor) */
	vec3_t room_forward; /* engine: the game's forward in the room (LOCAL, Xash axes), zero when unknown */
	gl_openxr_eye_t beam; /* tiny solid-colour swapchain for the menu pointer beam */
	gl_openxr_eye_t hud;  /* hand HUD: one slot per panel (gl_openxr_hand_hud_slot) */
	struct
	{
		gl_openxr_eye_t image;  /* the camera view, rendered once per frame like the desktop view */
		qboolean rendering;     /* between BeginCinema and EndCinema */
		qboolean show;          /* engine: a camera scene is up (screen shown, no world) */
		float brightness;       /* engine: 0..1, the screen fades to black below 1 */
		qboolean anchored;
		XrPosef anchor;         /* level pose the screen hangs off, captured when it appeared */
		GLuint copy_fbo, copy_rb; /* last camera image, repeated while the screen fades out */
		qboolean copy_valid;
	} cinema;
	struct
	{
		gl_openxr_eye_t image;  /* zoomed view along the weapon, rendered once per frame */
		qboolean show;          /* engine: zoomed with a scoped weapon */
		qboolean rendering;     /* between BeginScope and EndScope */
		int hand;               /* the weapon hand the lens sits on */
	} scope;
	struct
	{
		qboolean valid;
		int hud_width, hud_height; /* content size in HUD pixels (square on the desktop) */
		int base_width;            /* status panel: width of health + suit; the flashlight and damage icons extend it to the right */
		uint32_t x, width, height; /* the panel's slot in the hand HUD swapchain */
		int box_count;
		struct
		{
			int index;          /* GL_OPENXR_HUD_BOX_* */
			int hud_x, hud_y;   /* where the client drew it, HUD pixels (top left, margin included) */
			int px, py, w, h;   /* where it sits in the panel, HUD pixels from the panel's top left */
		} box[GL_OPENXR_HAND_HUD_MAX_BOXES];
	} hand_hud[GL_OPENXR_HAND_HUD_COUNT];
	struct
	{
		qboolean valid;       /* the weapon grid is up and the weapon hand is tracked: draw the beam */
		qboolean hit;         /* the beam ends on the grid */
		float u, v;           /* where, 0..1 from the top left */
		XrVector3f origin;    /* weapon-hand aim position (local space) */
		XrVector3f end;       /* hit point, or a short way along the ray */
		char cursor[32];      /* last value written to vr_weapon_pointer_cursor */
	} weapon_pointer;
	struct
	{
		qboolean valid;
		float u, v;           /* cursor on the UI panel, 0..1 */
		XrVector3f origin;    /* controller aim position (local space) */
		XrVector3f hit;       /* where the aim ray meets the panel (local space) */
	} ui_pointer[REF_VR_MAX_HANDS];
	struct
	{
		qboolean grip_valid, aim_valid;
		XrPosef grip, aim;     /* local space, this frame's predicted display time */
	} hand_pose[REF_VR_MAX_HANDS];
	int ui_source_width;
	int ui_source_height;
	float ui_clear_color[4];
	float comfort_level;
	uint64_t comfort_frame;
	double haptic_until[REF_VR_MAX_HANDS];
	qboolean haptic_sustained[REF_VR_MAX_HANDS];
} gl_openxr_state_t;

static gl_openxr_state_t xr;
static cvar_t *vr_enable;
static cvar_t *vr_supersampling;
static cvar_t *vr_worldscale;
static cvar_t *vr_ui_distance;
static cvar_t *vr_ui_offset_x;
static cvar_t *vr_ui_height;
static cvar_t *vr_ui_offset_y;
static cvar_t *vr_panel_head_angle;
static cvar_t *vr_ui_world;
static cvar_t *vr_ui_width;
static cvar_t *vr_ui_glow_curve;
static cvar_t *vr_comfort_mask;
static cvar_t *vr_comfort_moving;
static cvar_t *vr_scope_engaged;
static cvar_t *vr_mirror_eye;
static cvar_t *vr_hud_hands;
static cvar_t *vr_hud_hand_scale;
static cvar_t *vr_hud_hand_tilt;
static cvar_t *vr_hud_hand_yaw;
static cvar_t *vr_hud_ammo_scale;
static cvar_t *vr_hud_ammo_tilt;
static cvar_t *vr_hud_ammo_yaw;
static cvar_t *vr_hud_right_flip;
static cvar_t *vr_hud_rect[GL_OPENXR_HUD_BOX_COUNT];
static cvar_t *vr_hud_offset[GL_OPENXR_HAND_HUD_COUNT];
static cvar_t *vr_weapon_pointer_scale;
static cvar_t *vr_scope_size;
static cvar_t *vr_scope_offset;
static cvar_t *vr_weapon_pointer_yaw;

void GL_OpenXRInit( void )
{
	vr_enable = gEngfuncs.Cvar_Get( "vr_enable", "0", FCVAR_ARCHIVE, "enable desktop OpenXR rendering" );
	vr_supersampling = gEngfuncs.Cvar_Get( "vr_supersampling", "1.0", FCVAR_ARCHIVE, "OpenXR eye resolution scale" );
	vr_mirror_eye = gEngfuncs.Cvar_Get( "vr_mirror_eye", "0", 0, "eye shown in the desktop window (0 left, 1 right)" );
	vr_worldscale = gEngfuncs.Cvar_Get( "vr_worldscale", "40", FCVAR_ARCHIVE, "game units per physical metre" );
	vr_ui_distance = gEngfuncs.Cvar_Get( "vr_ui_distance", "2.2", FCVAR_ARCHIVE, "head-relative VR UI distance in metres" );
	vr_ui_offset_x = gEngfuncs.Cvar_Get( "vr_ui_offset_x", "-0.1", FCVAR_ARCHIVE, "VR HUD (head-locked) panel offset to the right in metres (negative = left), clamped to +-1; the menu is centred" );
	vr_ui_height = gEngfuncs.Cvar_Get( "vr_ui_height", "0", FCVAR_ARCHIVE, "VR UI panel height in metres; 0 = from the width and the UI texture aspect (16:9)" );
	vr_ui_offset_y = gEngfuncs.Cvar_Get( "vr_ui_offset_y", "0", FCVAR_ARCHIVE, "VR HUD (head-locked) panel offset upwards in metres (negative = down), clamped to +-1; the menu is centred" );
	vr_panel_head_angle = gEngfuncs.Cvar_Get( "vr_panel_head_angle", "70", FCVAR_ARCHIVE,
		"the menu and the cinema screen appear in the game's forward direction unless your head is turned further than this from it (degrees); then where you look" );
	vr_ui_world = gEngfuncs.Cvar_Get( "vr_ui_world", "0", 0, "set by the engine while a menu/console is open: the UI panel stays put in the room" );
	vr_ui_width = gEngfuncs.Cvar_Get( "vr_ui_width", "2.6", FCVAR_ARCHIVE, "head-relative VR UI width in metres" );
	vr_ui_glow_curve = gEngfuncs.Cvar_Get( "vr_ui_glow_curve", "1.6", FCVAR_ARCHIVE, "VR HUD: opacity of additive (glowing) HUD pixels = brightness^curve; 1 = old look, higher hides faint fringes" );
	vr_comfort_mask = gEngfuncs.Cvar_Get( "vr_comfort_mask", "0.0", FCVAR_ARCHIVE,
		"movement comfort mask: 0 disables it, 1 fully obscures the view edges" );
	vr_comfort_moving = gEngfuncs.Cvar_Get( "vr_comfort_moving", "0", 0,
		"internal VR movement state" );
	vr_scope_engaged = gEngfuncs.Cvar_Get( "vr_scope_engaged", "0", 0,
		"internal VR scope state" );
	vr_hud_hands = gEngfuncs.Cvar_Get( "vr_hud_hands", "1", FCVAR_ARCHIVE,
		"show health/suit on the off hand and ammo on the weapon hand instead of on the head-locked HUD" );
	vr_hud_hand_scale = gEngfuncs.Cvar_Get( "vr_hud_hand_scale", "0.0005", FCVAR_ARCHIVE,
		"health/suit panel size: metres per HUD pixel (0.0005 = HUD digits about 1.2 cm tall)" );
	vr_hud_hand_tilt = gEngfuncs.Cvar_Get( "vr_hud_hand_tilt", "-20", FCVAR_ARCHIVE,
		"health/suit panel and weapon grid tilt in degrees about the controller's sideways axis (negative = face more upwards)" );
	vr_hud_hand_yaw = gEngfuncs.Cvar_Get( "vr_hud_hand_yaw", "90", FCVAR_ARCHIVE,
		"health/suit panel turned inwards in degrees, mirrored per hand (90 = facing your body, like a watch)" );
	vr_hud_ammo_scale = gEngfuncs.Cvar_Get( "vr_hud_ammo_scale", "0.0005", FCVAR_ARCHIVE,
		"ammo panel size, like vr_hud_hand_scale" );
	vr_hud_ammo_tilt = gEngfuncs.Cvar_Get( "vr_hud_ammo_tilt", "0", FCVAR_ARCHIVE,
		"ammo panel tilt, like vr_hud_hand_tilt" );
	vr_hud_ammo_yaw = gEngfuncs.Cvar_Get( "vr_hud_ammo_yaw", "20", FCVAR_ARCHIVE,
		"ammo panel turned inwards, like vr_hud_hand_yaw" );
	vr_hud_right_flip = gEngfuncs.Cvar_Get( "vr_hud_right_flip", "0", FCVAR_ARCHIVE,
		"turn the right-hand HUD panel 180 degrees in its plane, so both panels read forwards along the arm when turned inwards" );
	vr_hud_offset[GL_OPENXR_HAND_HUD_STATUS] = gEngfuncs.Cvar_Get( "vr_hud_status_offset", "0.025 0.035 -0.02", FCVAR_ARCHIVE,
		"health/suit panel position from the in-game off hand (flashlight), metres along the aim axes: right up back" );
	vr_hud_offset[GL_OPENXR_HAND_HUD_AMMO] = gEngfuncs.Cvar_Get( "vr_hud_ammo_offset", "-0.01 0.02 -0.05", FCVAR_ARCHIVE,
		"ammo panel position from the weapon-hand palm, metres along the aim axes: right up back" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_HEALTH] = gEngfuncs.Cvar_Get( "vr_hud_health_rect", "", 0,
		"set by the client: health HUD box \"x y w h screenwidth screenheight\"" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_SUIT] = gEngfuncs.Cvar_Get( "vr_hud_suit_rect", "", 0,
		"set by the client: suit HUD box \"x y w h screenwidth screenheight\"" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_AMMO] = gEngfuncs.Cvar_Get( "vr_hud_ammo_rect", "", 0,
		"set by the client: ammo HUD box \"x y w h screenwidth screenheight\"" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_DAMAGE] = gEngfuncs.Cvar_Get( "vr_hud_damage_rect", "", 0,
		"set by the client: damage-type icons (drowning, freezing, ...) \"x y w h screenwidth screenheight\"" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_FLASHLIGHT] = gEngfuncs.Cvar_Get( "vr_hud_flashlight_rect", "", 0,
		"set by the client: flashlight icon and battery \"x y w h screenwidth screenheight\"" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_WEAPONS] = gEngfuncs.Cvar_Get( "vr_hud_weapons_rect", "", 0,
		"set by the client: pointer weapon grid, first row \"x y w h screenwidth screenheight\"" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_WEAPONS2] = gEngfuncs.Cvar_Get( "vr_hud_weapons2_rect", "", 0,
		"set by the client: pointer weapon grid, second row (shown right of the first)" );
	vr_hud_rect[GL_OPENXR_HUD_BOX_WEAPONS3] = gEngfuncs.Cvar_Get( "vr_hud_weapons3_rect", "", 0,
		"set by the client: pointer weapon grid, third row (shown right of the second)" );
	vr_hud_offset[GL_OPENXR_HAND_HUD_WEAPONS] = gEngfuncs.Cvar_Get( "vr_weapon_pointer_offset", "0 0.035 -0.35", FCVAR_ARCHIVE,
		"pointer weapon grid centre from the in-game off hand (flashlight), metres along the aim axes: right up back (-0.35 = out past the flashlight)" );
	vr_weapon_pointer_scale = gEngfuncs.Cvar_Get( "vr_weapon_pointer_scale", "0.00065", FCVAR_ARCHIVE,
		"pointer weapon grid size: metres per HUD pixel (0.00065 = weapon icons about 10 cm wide)" );
	vr_weapon_pointer_yaw = gEngfuncs.Cvar_Get( "vr_weapon_pointer_yaw", "40", FCVAR_ARCHIVE,
		"pointer weapon grid turn in degrees, like vr_hud_hand_yaw (90 = facing your body like the health panel; less turns its far end inwards and its face towards you)" );
	vr_scope_size = gEngfuncs.Cvar_Get( "vr_scope_size", "0.05", FCVAR_ARCHIVE,
		"scope lens diameter in metres (Counter-Strike: the zoomed view on a lens in front of the weapon hand)" );
	vr_scope_offset = gEngfuncs.Cvar_Get( "vr_scope_offset", "0 0.06 -0.08", FCVAR_ARCHIVE,
		"scope lens centre from the weapon-hand aim pose, metres along its axes: right up back (negative back = in front)" );
	gEngfuncs.Cvar_Get( "vr_weapon_pointer_cursor", "", 0,
		"set by the renderer: \"x y\" HUD pixels the weapon hand points at on the weapon grid, empty when off it" );
}

static void GL_OpenXRLogResult( const char *operation, XrResult result )
{
	char text[XR_MAX_RESULT_STRING_SIZE] = { 0 };

	if( xr.instance != XR_NULL_HANDLE && XR_FAILED( xrResultToString( xr.instance, result, text )))
		text[0] = '\0';

	if( text[0] )
		gEngfuncs.Con_Printf( S_ERROR "OpenXR: %s failed: %d (%s)\n", operation, (int)result, text );
	else
		gEngfuncs.Con_Printf( S_ERROR "OpenXR: %s failed: %d\n", operation, (int)result );
}

static qboolean GL_OpenXRCheck( XrResult result, const char *operation )
{
	if( XR_SUCCEEDED( result ))
		return true;

	GL_OpenXRLogResult( operation, result );
	return false;
}

static qboolean GL_OpenXRCreateAction( XrAction *action, XrActionType type, const char *name, const char *localized_name )
{
	XrActionCreateInfo create_info = { XR_TYPE_ACTION_CREATE_INFO };

	create_info.actionType = type;
	create_info.countSubactionPaths = REF_VR_MAX_HANDS;
	create_info.subactionPaths = xr.actions.hand_paths;
	Q_strncpy( create_info.actionName, name, sizeof( create_info.actionName ));
	Q_strncpy( create_info.localizedActionName, localized_name, sizeof( create_info.localizedActionName ));
	return GL_OpenXRCheck( xrCreateAction( xr.actions.gameplay_set, &create_info, action ), name );
}

static qboolean GL_OpenXRCreateActions( void )
{
	XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	const char *hand_paths[REF_VR_MAX_HANDS] = { "/user/hand/left", "/user/hand/right" };

	Q_strncpy( set_info.actionSetName, "gameplay", sizeof( set_info.actionSetName ));
	Q_strncpy( set_info.localizedActionSetName, "Gameplay", sizeof( set_info.localizedActionSetName ));
	if( !GL_OpenXRCheck( xrCreateActionSet( xr.instance, &set_info, &xr.actions.gameplay_set ), "xrCreateActionSet(gameplay)" ))
		return false;

	for( int i = 0; i < REF_VR_MAX_HANDS; ++i )
	{
		if( !GL_OpenXRCheck( xrStringToPath( xr.instance, hand_paths[i], &xr.actions.hand_paths[i] ), "xrStringToPath(hand)" ))
			return false;
	}

	return GL_OpenXRCreateAction( &xr.actions.grip_pose, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose" ) &&
		GL_OpenXRCreateAction( &xr.actions.aim_pose, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose" ) &&
		GL_OpenXRCreateAction( &xr.actions.trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger" ) &&
		GL_OpenXRCreateAction( &xr.actions.squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Squeeze" ) &&
		GL_OpenXRCreateAction( &xr.actions.stick, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Stick" ) &&
		GL_OpenXRCreateAction( &xr.actions.primary, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "Primary" ) &&
		GL_OpenXRCreateAction( &xr.actions.secondary, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "Secondary" ) &&
		GL_OpenXRCreateAction( &xr.actions.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu" ) &&
		GL_OpenXRCreateAction( &xr.actions.stick_click, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick Click" ) &&
		GL_OpenXRCreateAction( &xr.actions.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic" );
}

static void GL_OpenXRSuggestBindings( const char *profile_name, const gl_openxr_binding_t *bindings, uint32_t binding_count )
{
	XrActionSuggestedBinding suggested[32];
	XrInteractionProfileSuggestedBinding suggest_info = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	XrPath profile;
	uint32_t suggested_count = 0;

	if( !GL_OpenXRCheck( xrStringToPath( xr.instance, profile_name, &profile ), "xrStringToPath(profile)" ))
		return;

	for( uint32_t i = 0; i < binding_count; ++i )
	{
		for( int hand = 0; hand < REF_VR_MAX_HANDS; ++hand )
		{
			const char *path_name = hand == REF_VR_HAND_LEFT ? bindings[i].left_path : bindings[i].right_path;

			if( !path_name )
				continue;
			if( suggested_count >= ARRAYSIZE( suggested ))
				return;
			if( !GL_OpenXRCheck( xrStringToPath( xr.instance, path_name, &suggested[suggested_count].binding ),
				"xrStringToPath(binding)" ))
				continue;
			suggested[suggested_count].action = bindings[i].action;
			++suggested_count;
		}
	}

	if( !suggested_count )
		return;
	suggest_info.interactionProfile = profile;
	suggest_info.countSuggestedBindings = suggested_count;
	suggest_info.suggestedBindings = suggested;
	GL_OpenXRCheck( xrSuggestInteractionProfileBindings( xr.instance, &suggest_info ), "xrSuggestInteractionProfileBindings" );
}

static void GL_OpenXRSuggestInteractionProfiles( void )
{
	const gl_openxr_binding_t simple_bindings[] = {
		{ xr.actions.grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose" },
		{ xr.actions.aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose" },
		{ xr.actions.trigger, "/user/hand/left/input/select/click", "/user/hand/right/input/select/click" },
		{ xr.actions.menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click" },
		{ xr.actions.haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic" },
	};
	const gl_openxr_binding_t touch_bindings[] = {
		{ xr.actions.grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose" },
		{ xr.actions.aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose" },
		{ xr.actions.trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value" },
		{ xr.actions.squeeze, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value" },
		{ xr.actions.stick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick" },
		{ xr.actions.primary, "/user/hand/left/input/x/click", "/user/hand/right/input/a/click" },
		{ xr.actions.secondary, "/user/hand/left/input/y/click", "/user/hand/right/input/b/click" },
		{ xr.actions.menu, "/user/hand/left/input/menu/click", NULL },
		{ xr.actions.stick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click" },
		{ xr.actions.haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic" },
	};
	const gl_openxr_binding_t index_bindings[] = {
		{ xr.actions.grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose" },
		{ xr.actions.aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose" },
		{ xr.actions.trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value" },
		{ xr.actions.squeeze, "/user/hand/left/input/squeeze/force", "/user/hand/right/input/squeeze/force" },
		{ xr.actions.stick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick" },
		{ xr.actions.primary, "/user/hand/left/input/a/click", "/user/hand/right/input/a/click" },
		{ xr.actions.secondary, "/user/hand/left/input/b/click", "/user/hand/right/input/b/click" },
		{ xr.actions.menu, "/user/hand/left/input/system/click", "/user/hand/right/input/system/click" },
		{ xr.actions.stick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click" },
		{ xr.actions.haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic" },
	};
	const gl_openxr_binding_t motion_bindings[] = {
		{ xr.actions.grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose" },
		{ xr.actions.aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose" },
		{ xr.actions.trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value" },
		{ xr.actions.stick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick" },
		{ xr.actions.primary, "/user/hand/left/input/squeeze/click", "/user/hand/right/input/squeeze/click" },
		{ xr.actions.secondary, "/user/hand/left/input/trackpad/click", "/user/hand/right/input/trackpad/click" },
		{ xr.actions.menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click" },
		{ xr.actions.stick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click" },
		{ xr.actions.haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic" },
	};
	const gl_openxr_binding_t vive_bindings[] = {
		{ xr.actions.grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose" },
		{ xr.actions.aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose" },
		{ xr.actions.trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value" },
		{ xr.actions.stick, "/user/hand/left/input/trackpad", "/user/hand/right/input/trackpad" },
		{ xr.actions.primary, "/user/hand/left/input/trigger/click", "/user/hand/right/input/trigger/click" },
		{ xr.actions.secondary, "/user/hand/left/input/squeeze/click", "/user/hand/right/input/squeeze/click" },
		{ xr.actions.menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click" },
		{ xr.actions.stick_click, "/user/hand/left/input/trackpad/click", "/user/hand/right/input/trackpad/click" },
		{ xr.actions.haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic" },
	};

	GL_OpenXRSuggestBindings( "/interaction_profiles/khr/simple_controller", simple_bindings, ARRAYSIZE( simple_bindings ));
	GL_OpenXRSuggestBindings( "/interaction_profiles/oculus/touch_controller", touch_bindings, ARRAYSIZE( touch_bindings ));
	GL_OpenXRSuggestBindings( "/interaction_profiles/valve/index_controller", index_bindings, ARRAYSIZE( index_bindings ));
	GL_OpenXRSuggestBindings( "/interaction_profiles/microsoft/motion_controller", motion_bindings, ARRAYSIZE( motion_bindings ));
	GL_OpenXRSuggestBindings( "/interaction_profiles/htc/vive_controller", vive_bindings, ARRAYSIZE( vive_bindings ));
}

static qboolean GL_OpenXRCreateActionSpaces( void )
{
	XrActionSpaceCreateInfo space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };

	space_info.poseInActionSpace.orientation.w = 1.0f;
	for( int hand = 0; hand < REF_VR_MAX_HANDS; ++hand )
	{
		space_info.subactionPath = xr.actions.hand_paths[hand];
		space_info.action = xr.actions.grip_pose;
		if( !GL_OpenXRCheck( xrCreateActionSpace( xr.session, &space_info, &xr.actions.grip_spaces[hand] ),
			"xrCreateActionSpace(grip)" ))
			return false;
		space_info.action = xr.actions.aim_pose;
		if( !GL_OpenXRCheck( xrCreateActionSpace( xr.session, &space_info, &xr.actions.aim_spaces[hand] ),
			"xrCreateActionSpace(aim)" ))
			return false;
	}
	return true;
}

/* The UI quad layer is composited with premultiplied alpha. GoldSrc draws most HUD
 * elements and title/credit text additively; with one blend function for colour and
 * alpha those writes also set the layer's alpha to 1, turning every sprite into a black
 * box over the world. While the UI layer is bound, route pglBlendFunc through
 * glBlendFuncSeparate: additive keeps the destination alpha (pure glow), normal alpha
 * blending accumulates coverage. */
static void (APIENTRY *gl_openxr_blendfunc)( GLenum sfactor, GLenum dfactor );
static void (APIENTRY *gl_openxr_blendfunc_separate)( GLenum srgb, GLenum drgb, GLenum salpha, GLenum dalpha );
static GLenum gl_openxr_blend_src = GL_ONE, gl_openxr_blend_dst = GL_ZERO;
static qboolean gl_openxr_ui_blend;

static void GL_OpenXRApplyBlend( void )
{
	GLenum src = gl_openxr_blend_src, dst = gl_openxr_blend_dst;

	if( !gl_openxr_ui_blend || !gl_openxr_blendfunc_separate )
		gl_openxr_blendfunc( src, dst );
	else if( dst == GL_ONE )
		gl_openxr_blendfunc_separate( src, dst, GL_ZERO, GL_ONE );
	else if( src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA )
		gl_openxr_blendfunc_separate( src, dst, GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	else gl_openxr_blendfunc( src, dst );
}

static void APIENTRY GL_OpenXRBlendFunc( GLenum sfactor, GLenum dfactor )
{
	gl_openxr_blend_src = sfactor;
	gl_openxr_blend_dst = dfactor;
	GL_OpenXRApplyBlend();
}

static void GL_OpenXRSetUIBlend( qboolean active )
{
	if( !gl_openxr_blendfunc )
		return;
	gl_openxr_ui_blend = active;
	GL_OpenXRApplyBlend();
}

static qboolean GL_OpenXRLoadGLFunctions( void )
{
	if( !gl_openxr_blendfunc && pglBlendFunc && pglBlendFunc != GL_OpenXRBlendFunc )
	{
		gl_openxr_blendfunc = pglBlendFunc;
		gl_openxr_blendfunc_separate = gEngfuncs.GL_GetProcAddress( "glBlendFuncSeparate" );
		pglBlendFunc = GL_OpenXRBlendFunc;
	}
	if( !pglGenFramebuffers ) pglGenFramebuffers = gEngfuncs.GL_GetProcAddress( "glGenFramebuffers" );
	if( !pglDeleteFramebuffers ) pglDeleteFramebuffers = gEngfuncs.GL_GetProcAddress( "glDeleteFramebuffers" );
	if( !pglBindFramebuffer ) pglBindFramebuffer = gEngfuncs.GL_GetProcAddress( "glBindFramebuffer" );
	if( !pglFramebufferTexture2D ) pglFramebufferTexture2D = gEngfuncs.GL_GetProcAddress( "glFramebufferTexture2D" );
	if( !pglCheckFramebufferStatus ) pglCheckFramebufferStatus = gEngfuncs.GL_GetProcAddress( "glCheckFramebufferStatus" );
	if( !pglBlitFramebuffer ) pglBlitFramebuffer = gEngfuncs.GL_GetProcAddress( "glBlitFramebuffer" );
	if( !pglGenRenderbuffers ) pglGenRenderbuffers = gEngfuncs.GL_GetProcAddress( "glGenRenderbuffers" );
	if( !pglDeleteRenderbuffers ) pglDeleteRenderbuffers = gEngfuncs.GL_GetProcAddress( "glDeleteRenderbuffers" );
	if( !pglBindRenderbuffer ) pglBindRenderbuffer = gEngfuncs.GL_GetProcAddress( "glBindRenderbuffer" );
	if( !pglRenderbufferStorage ) pglRenderbufferStorage = gEngfuncs.GL_GetProcAddress( "glRenderbufferStorage" );
	if( !pglFramebufferRenderbuffer ) pglFramebufferRenderbuffer = gEngfuncs.GL_GetProcAddress( "glFramebufferRenderbuffer" );

	if( pglGenFramebuffers && pglDeleteFramebuffers && pglBindFramebuffer &&
		pglFramebufferTexture2D && pglCheckFramebufferStatus && pglBlitFramebuffer &&
		pglGenRenderbuffers && pglDeleteRenderbuffers && pglBindRenderbuffer &&
		pglRenderbufferStorage && pglFramebufferRenderbuffer )
		return true;

	gEngfuncs.Con_Printf( S_ERROR "OpenXR: required framebuffer functions are unavailable\n" );
	return false;
}

static XrQuaternionf GL_OpenXRQuatConjugate( XrQuaternionf q )
{
	XrQuaternionf out = { -q.x, -q.y, -q.z, q.w };
	return out;
}

static XrQuaternionf GL_OpenXRQuatMultiply( XrQuaternionf a, XrQuaternionf b )
{
	XrQuaternionf out = {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
	return out;
}

static XrVector3f GL_OpenXRRotateVector( XrQuaternionf q, XrVector3f v )
{
	XrQuaternionf p = { v.x, v.y, v.z, 0.0f };
	XrQuaternionf rotated = GL_OpenXRQuatMultiply( GL_OpenXRQuatMultiply( q, p ), GL_OpenXRQuatConjugate( q ));
	XrVector3f out = { rotated.x, rotated.y, rotated.z };
	return out;
}

static XrPosef GL_OpenXRPoseInverse( XrPosef pose )
{
	XrPosef out;
	out.orientation = GL_OpenXRQuatConjugate( pose.orientation );
	XrVector3f negative = { -pose.position.x, -pose.position.y, -pose.position.z };
	out.position = GL_OpenXRRotateVector( out.orientation, negative );
	return out;
}

static XrPosef GL_OpenXRPoseMultiply( XrPosef a, XrPosef b )
{
	XrPosef out;
	XrVector3f position = GL_OpenXRRotateVector( a.orientation, b.position );
	out.orientation = GL_OpenXRQuatMultiply( a.orientation, b.orientation );
	out.position.x = a.position.x + position.x;
	out.position.y = a.position.y + position.y;
	out.position.z = a.position.z + position.z;
	return out;
}

static void GL_OpenXRMapVector( const XrVector3f *in, vec3_t out )
{
	// OpenXR right/up/back maps to Xash forward/left/up.
	out[0] = -in->z;
	out[1] = -in->x;
	out[2] = in->y;
}

static void GL_OpenXRQuatToBasis( XrQuaternionf q, vec3_t forward, vec3_t right, vec3_t up )
{
	const XrVector3f xr_forward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f xr_right = { 1.0f, 0.0f, 0.0f };
	const XrVector3f xr_up = { 0.0f, 1.0f, 0.0f };
	XrVector3f rotated_forward = GL_OpenXRRotateVector( q, xr_forward );
	XrVector3f rotated_right = GL_OpenXRRotateVector( q, xr_right );
	XrVector3f rotated_up = GL_OpenXRRotateVector( q, xr_up );

	GL_OpenXRMapVector( &rotated_forward, forward );
	GL_OpenXRMapVector( &rotated_right, right );
	GL_OpenXRMapVector( &rotated_up, up );
	VectorNormalize( forward );
	VectorNormalize( right );
	VectorNormalize( up );
}

static void GL_OpenXRFillPose( const XrPosef *pose, ref_vr_pose_t *out )
{
	GL_OpenXRMapVector( &pose->position, out->position );
	GL_OpenXRQuatToBasis( pose->orientation, out->forward, out->right, out->up );
}

static qboolean GL_OpenXRGetBooleanAction( XrAction action, XrPath hand_path, qboolean *value )
{
	XrActionStateGetInfo get_info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };

	get_info.action = action;
	get_info.subactionPath = hand_path;
	if( !GL_OpenXRCheck( xrGetActionStateBoolean( xr.session, &get_info, &state ), "xrGetActionStateBoolean" ))
		return false;
	*value = state.isActive && state.currentState;
	return true;
}

static qboolean GL_OpenXRGetFloatAction( XrAction action, XrPath hand_path, float *value )
{
	XrActionStateGetInfo get_info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };

	get_info.action = action;
	get_info.subactionPath = hand_path;
	if( !GL_OpenXRCheck( xrGetActionStateFloat( xr.session, &get_info, &state ), "xrGetActionStateFloat" ))
		return false;
	*value = state.isActive ? state.currentState : 0.0f;
	return true;
}

static qboolean GL_OpenXRGetVector2Action( XrAction action, XrPath hand_path, float value[2] )
{
	XrActionStateGetInfo get_info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };

	get_info.action = action;
	get_info.subactionPath = hand_path;
	if( !GL_OpenXRCheck( xrGetActionStateVector2f( xr.session, &get_info, &state ), "xrGetActionStateVector2f" ))
		return false;
	value[0] = state.isActive ? state.currentState.x : 0.0f;
	value[1] = state.isActive ? state.currentState.y : 0.0f;
	return true;
}

static float GL_OpenXRUIHeight( float width )
{
	if( vr_ui_height->value > 0.0f )
		return bound( 0.25f, vr_ui_height->value, 8.0f );
	return width * (float)xr.ui.height / (float)xr.ui.width;
}

/* Reference pose the UI panel hangs off: the head (HUD) or the anchor captured when a menu opened. */
static XrPosef GL_OpenXRUIReference( void )
{
	return xr.ui_world_active ? xr.ui_anchor : xr.head_pose;
}

/* Heading of a pose about +Y (0 = facing -Z); looking straight up or down it comes from the right vector. */
static float GL_OpenXRPoseYaw( const XrPosef *pose )
{
	const XrVector3f forward = { 0.0f, 0.0f, -1.0f }, right = { 1.0f, 0.0f, 0.0f };
	XrVector3f f = GL_OpenXRRotateVector( pose->orientation, forward );
	XrVector3f r;

	if( f.x * f.x + f.z * f.z > 0.01f )
		return atan2f( -f.x, -f.z );
	r = GL_OpenXRRotateVector( pose->orientation, right );
	return atan2f( -r.z, r.x );
}

/* Level (yaw-only) pose, so the menu and the cinema screen do not tilt with the head. */
static XrPosef GL_OpenXRYawPose( const XrVector3f *position, float yaw )
{
	XrPosef out;

	out.position = *position;
	out.orientation.x = 0.0f;
	out.orientation.y = sinf( yaw * 0.5f );
	out.orientation.z = 0.0f;
	out.orientation.w = cosf( yaw * 0.5f );
	return out;
}

/* Where the menu and the cinema screen hang when they appear: level at the head, facing the game's forward (the way
 * you faced at recentre, passed by the engine), unless the head is turned more than vr_panel_head_angle away from
 * it, then where you look. They often appear unasked (camera scenes, loading, the main menu), when you may be
 * looking anywhere. */
static XrPosef GL_OpenXRPanelAnchor( void )
{
	const float head_yaw = GL_OpenXRPoseYaw( &xr.head_pose );
	float room_yaw, diff;

	if( xr.room_forward[0] * xr.room_forward[0] + xr.room_forward[1] * xr.room_forward[1] < 0.01f )
		return GL_OpenXRYawPose( &xr.head_pose.position, head_yaw );
	/* Xash forward (x, y) = OpenXR (-z, -x): a turn of yaw about +Y */
	room_yaw = atan2f( xr.room_forward[1], xr.room_forward[0] );
	diff = fabsf( remainderf( head_yaw - room_yaw, 2.0f * (float)M_PI ));
	if( RAD2DEG( diff ) > bound( 0.0f, vr_panel_head_angle->value, 180.0f ))
		return GL_OpenXRYawPose( &xr.head_pose.position, head_yaw );
	return GL_OpenXRYawPose( &xr.head_pose.position, room_yaw );
}

static void GL_OpenXRFillUICursor( int hand, const XrPosef *aim, ref_vr_hand_t *out )
{
	const XrPosef reference = GL_OpenXRUIReference();
	XrVector3f world_origin;
	XrVector3f aim_forward = { 0.0f, 0.0f, -1.0f };
	XrVector3f world_direction;
	XrVector3f local_origin;
	XrVector3f local_direction;
	float distance = bound( 0.2f, vr_ui_distance->value, 10.0f );
	float width = bound( 0.25f, vr_ui_width->value, 8.0f );
	float height = GL_OpenXRUIHeight( width );
	float t, x, y;

	world_origin.x = aim->position.x - reference.position.x;
	world_origin.y = aim->position.y - reference.position.y;
	world_origin.z = aim->position.z - reference.position.z;
	world_direction = GL_OpenXRRotateVector( aim->orientation, aim_forward );
	local_origin = GL_OpenXRRotateVector( GL_OpenXRQuatConjugate( reference.orientation ), world_origin );
	local_direction = GL_OpenXRRotateVector( GL_OpenXRQuatConjugate( reference.orientation ), world_direction );

	if( local_direction.z >= -0.0001f )
		return;
	t = ( -distance - local_origin.z ) / local_direction.z;
	if( t <= 0.0f )
		return;
	x = local_origin.x + local_direction.x * t;
	y = local_origin.y + local_direction.y * t;
	if( !xr.ui_world_active )
	{
		/* the offsets move only the head-locked HUD (tuned for it); the menu is centred */
		x -= bound( -1.0f, vr_ui_offset_x->value, 1.0f );
		y -= bound( -1.0f, vr_ui_offset_y->value, 1.0f );
	}
	out->ui_cursor[0] = x / width + 0.5f;
	out->ui_cursor[1] = 0.5f - y / height;
	if( out->ui_cursor[0] >= 0.0f && out->ui_cursor[0] <= 1.0f &&
		out->ui_cursor[1] >= 0.0f && out->ui_cursor[1] <= 1.0f )
	{
		XrVector3f panel_hit, world_hit;
		SetBits( out->flags, REF_VR_HAND_UI_VALID );
		/* remember the ray for the menu pointer dot and beam */
		panel_hit.x = local_origin.x + local_direction.x * t;
		panel_hit.y = local_origin.y + local_direction.y * t;
		panel_hit.z = -distance;
		world_hit = GL_OpenXRRotateVector( reference.orientation, panel_hit );
		xr.ui_pointer[hand].valid = true;
		xr.ui_pointer[hand].u = out->ui_cursor[0];
		xr.ui_pointer[hand].v = out->ui_cursor[1];
		xr.ui_pointer[hand].origin = aim->position;
		xr.ui_pointer[hand].hit.x = reference.position.x + world_hit.x;
		xr.ui_pointer[hand].hit.y = reference.position.y + world_hit.y;
		xr.ui_pointer[hand].hit.z = reference.position.z + world_hit.z;
	}
}

static qboolean GL_OpenXRFillHandPose( int hand, XrAction action, XrSpace space, uint32_t flag, qboolean include_velocity,
	ref_vr_hand_t *out )
{
	XrActionStateGetInfo get_info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
	XrSpaceVelocity velocity = { XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

	get_info.action = action;
	get_info.subactionPath = xr.actions.hand_paths[hand];
	if( !GL_OpenXRCheck( xrGetActionStatePose( xr.session, &get_info, &state ), "xrGetActionStatePose" ))
		return false;
	if( !state.isActive )
		return true;
	if( include_velocity )
		location.next = &velocity;
	if( !GL_OpenXRCheck( xrLocateSpace( space, xr.local_space, xr.frame_state.predictedDisplayTime, &location ),
		"xrLocateSpace(controller)" ))
		return false;
	/* A controller out of tracking only loses its own pose flag; failing here would
	   drop the whole frame's actions (both sticks, all buttons) on every tracking hiccup. */
	if(( location.locationFlags & required ) != required )
		return true;

	GL_OpenXRFillPose( &location.pose, flag == REF_VR_HAND_GRIP_VALID ? &out->grip : &out->aim );
	SetBits( out->flags, flag );
	if( flag == REF_VR_HAND_GRIP_VALID )
	{
		xr.hand_pose[hand].grip = location.pose;
		xr.hand_pose[hand].grip_valid = true;
	}
	else
	{
		xr.hand_pose[hand].aim = location.pose;
		xr.hand_pose[hand].aim_valid = true;
	}
	if( flag == REF_VR_HAND_AIM_VALID && xr.views_valid )
		GL_OpenXRFillUICursor( hand, &location.pose, out );
	if( include_velocity )
	{
		if( velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT )
			GL_OpenXRMapVector( &velocity.linearVelocity, out->linear_velocity );
		if( velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT )
			GL_OpenXRMapVector( &velocity.angularVelocity, out->angular_velocity );
	}
	return true;
}

static qboolean GL_OpenXRFillHands( ref_vr_hand_t hands[REF_VR_MAX_HANDS] )
{
	qboolean valid = true;

	for( int hand = 0; hand < REF_VR_MAX_HANDS; ++hand )
	{
		qboolean pressed;

		valid = GL_OpenXRFillHandPose( hand, xr.actions.grip_pose, xr.actions.grip_spaces[hand],
			REF_VR_HAND_GRIP_VALID, true, &hands[hand] ) && valid;
		valid = GL_OpenXRFillHandPose( hand, xr.actions.aim_pose, xr.actions.aim_spaces[hand],
			REF_VR_HAND_AIM_VALID, false, &hands[hand] ) && valid;
		valid = GL_OpenXRGetFloatAction( xr.actions.trigger, xr.actions.hand_paths[hand], &hands[hand].trigger ) && valid;
		valid = GL_OpenXRGetFloatAction( xr.actions.squeeze, xr.actions.hand_paths[hand], &hands[hand].squeeze ) && valid;
		if( REF_VR_SQUEEZE_PRESSED( hands[hand].squeeze ))
			SetBits( hands[hand].flags, REF_VR_HAND_SQUEEZE_PRESSED );
		valid = GL_OpenXRGetVector2Action( xr.actions.stick, xr.actions.hand_paths[hand], hands[hand].stick ) && valid;
		if( GL_OpenXRGetBooleanAction( xr.actions.primary, xr.actions.hand_paths[hand], &pressed ))
		{
			if( pressed ) SetBits( hands[hand].buttons, REF_VR_BUTTON_PRIMARY );
		}
		else valid = false;
		if( GL_OpenXRGetBooleanAction( xr.actions.secondary, xr.actions.hand_paths[hand], &pressed ))
		{
			if( pressed ) SetBits( hands[hand].buttons, REF_VR_BUTTON_SECONDARY );
		}
		else valid = false;
		if( GL_OpenXRGetBooleanAction( xr.actions.menu, xr.actions.hand_paths[hand], &pressed ))
		{
			if( pressed ) SetBits( hands[hand].buttons, REF_VR_BUTTON_MENU );
		}
		else valid = false;
		if( GL_OpenXRGetBooleanAction( xr.actions.stick_click, xr.actions.hand_paths[hand], &pressed ))
		{
			if( pressed ) SetBits( hands[hand].buttons, REF_VR_BUTTON_STICK );
		}
		else valid = false;
	}
	return valid;
}

static qboolean GL_OpenXRSyncActions( void )
{
	XrActiveActionSet active_set = { xr.actions.gameplay_set, XR_NULL_PATH };
	XrActionsSyncInfo sync_info = { XR_TYPE_ACTIONS_SYNC_INFO };

	sync_info.countActiveActionSets = 1;
	sync_info.activeActionSets = &active_set;
	return GL_OpenXRCheck( xrSyncActions( xr.session, &sync_info ), "xrSyncActions" );
}

static void GL_OpenXRDestroyEye( gl_openxr_eye_t *eye )
{
	if( eye->acquired && eye->waited && eye->swapchain != XR_NULL_HANDLE )
	{
		XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		GL_OpenXRCheck( xrReleaseSwapchainImage( eye->swapchain, &release_info ), "xrReleaseSwapchainImage(shutdown)" );
	}

	if( eye->framebuffers )
		pglDeleteFramebuffers( eye->image_count, eye->framebuffers );
	if( eye->depth_stencil )
		pglDeleteRenderbuffers( 1, &eye->depth_stencil );
	if( eye->swapchain != XR_NULL_HANDLE )
		GL_OpenXRCheck( xrDestroySwapchain( eye->swapchain ), "xrDestroySwapchain" );

	free( eye->framebuffers );
	free( eye->images );
	memset( eye, 0, sizeof( *eye ));
}

void GL_OpenXRShutdown( void )
{
	if( xr.ui_active && xr.initialized )
	{
		pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		pglViewport( 0, 0, xr.ui_source_width, xr.ui_source_height );
		pglClearColor( xr.ui_clear_color[0], xr.ui_clear_color[1], xr.ui_clear_color[2], xr.ui_clear_color[3] );
		xr.ui_active = false;
	}

	/* OpenXR requires every waited swapchain image to be released before its
	 * frame/session is ended. This also unwinds an interrupted eye/UI pass. */
	for( int i = 0; i <= REF_VR_MAX_EYES + 2; ++i )
	{
		gl_openxr_eye_t *eye = i == REF_VR_MAX_EYES ? &xr.ui : i == REF_VR_MAX_EYES + 1 ? &xr.cinema.image :
			i > REF_VR_MAX_EYES ? &xr.scope.image : &xr.eyes[i];
		if( eye->acquired && eye->waited && eye->swapchain != XR_NULL_HANDLE )
		{
			XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			GL_OpenXRCheck( xrReleaseSwapchainImage( eye->swapchain, &release_info ),
				"xrReleaseSwapchainImage(shutdown)" );
			eye->acquired = false;
			eye->waited = false;
		}
	}

	if( xr.frame_begun && xr.session != XR_NULL_HANDLE )
	{
		XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
		end_info.displayTime = xr.frame_state.predictedDisplayTime;
		end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		GL_OpenXRCheck( xrEndFrame( xr.session, &end_info ), "xrEndFrame(shutdown)" );
	}

	if( xr.initialized )
		pglFinish();

	for( int i = 0; i < REF_VR_MAX_EYES; ++i )
		GL_OpenXRDestroyEye( &xr.eyes[i] );
	GL_OpenXRDestroyEye( &xr.ui );
	GL_OpenXRDestroyEye( &xr.beam );
	GL_OpenXRDestroyEye( &xr.hud );
	GL_OpenXRDestroyEye( &xr.cinema.image );
	GL_OpenXRDestroyEye( &xr.scope.image );
	if( xr.cinema.copy_fbo )
		pglDeleteFramebuffers( 1, &xr.cinema.copy_fbo );
	if( xr.cinema.copy_rb )
		pglDeleteRenderbuffers( 1, &xr.cinema.copy_rb );

	for( int i = 0; i < REF_VR_MAX_HANDS; ++i )
	{
		if( xr.actions.aim_spaces[i] != XR_NULL_HANDLE )
			GL_OpenXRCheck( xrDestroySpace( xr.actions.aim_spaces[i] ), "xrDestroySpace(aim)" );
		if( xr.actions.grip_spaces[i] != XR_NULL_HANDLE )
			GL_OpenXRCheck( xrDestroySpace( xr.actions.grip_spaces[i] ), "xrDestroySpace(grip)" );
	}
	if( xr.head_space != XR_NULL_HANDLE )
		GL_OpenXRCheck( xrDestroySpace( xr.head_space ), "xrDestroySpace(head)" );
	if( xr.local_space != XR_NULL_HANDLE )
		GL_OpenXRCheck( xrDestroySpace( xr.local_space ), "xrDestroySpace(local)" );
	if( xr.session != XR_NULL_HANDLE )
		GL_OpenXRCheck( xrDestroySession( xr.session ), "xrDestroySession" );
	if( xr.actions.gameplay_set != XR_NULL_HANDLE )
		GL_OpenXRCheck( xrDestroyActionSet( xr.actions.gameplay_set ), "xrDestroyActionSet(gameplay)" );
	if( xr.instance != XR_NULL_HANDLE )
		GL_OpenXRCheck( xrDestroyInstance( xr.instance ), "xrDestroyInstance" );

	memset( &xr, 0, sizeof( xr ));
	xr.current_eye = -1;
}

static qboolean GL_OpenXRCreateEye( gl_openxr_eye_t *eye, const XrViewConfigurationView *view, int64_t format )
{
	float scale = bound( 0.5f, vr_supersampling->value, 2.0f );
	XrSwapchainCreateInfo create_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	uint32_t image_count = 0;

	eye->width = (uint32_t)( view->recommendedImageRectWidth * scale );
	eye->height = (uint32_t)( view->recommendedImageRectHeight * scale );
	create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	create_info.format = format;
	create_info.sampleCount = 1;
	create_info.width = eye->width;
	create_info.height = eye->height;
	create_info.faceCount = 1;
	create_info.arraySize = 1;
	create_info.mipCount = 1;

	if( !GL_OpenXRCheck( xrCreateSwapchain( xr.session, &create_info, &eye->swapchain ), "xrCreateSwapchain" ))
		return false;
	if( !GL_OpenXRCheck( xrEnumerateSwapchainImages( eye->swapchain, 0, &image_count, NULL ), "xrEnumerateSwapchainImages(count)" ))
		return false;

	eye->images = calloc( image_count, sizeof( *eye->images ));
	eye->framebuffers = calloc( image_count, sizeof( *eye->framebuffers ));
	if( !eye->images || !eye->framebuffers )
		return false;

	eye->image_count = image_count;
	for( uint32_t i = 0; i < image_count; ++i )
		eye->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;

	if( !GL_OpenXRCheck( xrEnumerateSwapchainImages( eye->swapchain, image_count, &image_count,
		(XrSwapchainImageBaseHeader *)eye->images ), "xrEnumerateSwapchainImages" ))
		return false;

	pglGenRenderbuffers( 1, &eye->depth_stencil );
	pglBindRenderbuffer( GL_RENDERBUFFER, eye->depth_stencil );
	pglRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, eye->width, eye->height );
	pglBindRenderbuffer( GL_RENDERBUFFER, 0 );

	pglGenFramebuffers( image_count, eye->framebuffers );
	for( uint32_t i = 0; i < image_count; ++i )
	{
		pglBindFramebuffer( GL_FRAMEBUFFER, eye->framebuffers[i] );
		pglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye->images[i].image, 0 );
		pglFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, eye->depth_stencil );
		if( pglCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			gEngfuncs.Con_Printf( S_ERROR "OpenXR: incomplete eye framebuffer %u\n", i );
			pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
			return false;
		}
	}
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	return true;
}

static qboolean GL_OpenXRInitialize( void )
{
	const char *extensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
	XrInstanceCreateInfo instance_info = { XR_TYPE_INSTANCE_CREATE_INFO };
	XrSystemGetInfo system_info = { XR_TYPE_SYSTEM_GET_INFO };
	PFN_xrGetOpenGLGraphicsRequirementsKHR get_requirements = NULL;
	XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
#if defined( _WIN32 )
	XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
#else
	XrGraphicsBindingOpenGLXlibKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
#endif
	XrSessionCreateInfo session_info = { XR_TYPE_SESSION_CREATE_INFO };
	XrReferenceSpaceCreateInfo space_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	XrViewConfigurationView view_configs[REF_VR_MAX_EYES];
	uint32_t view_count = 0;
	uint32_t format_count = 0;
	int64_t *formats = NULL;
	int64_t selected_format = 0;
#if !defined( _WIN32 )
	int fbconfig_id = 0;
	int config_count = 0;
	GLXFBConfig *configs = NULL;
	XVisualInfo *visual = NULL;
#endif
	qboolean success = false;

	memset( &xr, 0, sizeof( xr ));
	xr.current_eye = -1;

	Q_strncpy( instance_info.applicationInfo.applicationName, "Xash3D FWGS VR", XR_MAX_APPLICATION_NAME_SIZE );
	instance_info.applicationInfo.applicationVersion = 1;
	Q_strncpy( instance_info.applicationInfo.engineName, "Xash3D FWGS", XR_MAX_ENGINE_NAME_SIZE );
	instance_info.applicationInfo.engineVersion = 1;
	instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0; // SteamVR rejects 1.1 (XR_ERROR_API_VERSION_UNSUPPORTED)
	instance_info.enabledExtensionCount = ARRAYSIZE( extensions );
	instance_info.enabledExtensionNames = extensions;

	if( !GL_OpenXRCheck( xrCreateInstance( &instance_info, &xr.instance ), "xrCreateInstance" ))
		goto cleanup;
	if( !GL_OpenXRCreateActions() )
		goto cleanup;
	GL_OpenXRSuggestInteractionProfiles();

	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if( !GL_OpenXRCheck( xrGetSystem( xr.instance, &system_info, &xr.system_id ), "xrGetSystem" ))
		goto cleanup;
	if( !GL_OpenXRCheck( xrGetInstanceProcAddr( xr.instance, "xrGetOpenGLGraphicsRequirementsKHR",
		(PFN_xrVoidFunction *)&get_requirements ), "xrGetInstanceProcAddr(graphics requirements)" ) || !get_requirements )
		goto cleanup;
	if( !GL_OpenXRCheck( get_requirements( xr.instance, xr.system_id, &requirements ), "xrGetOpenGLGraphicsRequirementsKHR" ))
		goto cleanup;
	if( !GL_OpenXRLoadGLFunctions() )
		goto cleanup;

#if defined( _WIN32 )
	binding.hDC = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();
	if( !binding.hDC || !binding.hGLRC )
	{
		gEngfuncs.Con_Printf( S_ERROR "OpenXR: a current WGL device and rendering context are required\n" );
		goto cleanup;
	}
#else
	binding.xDisplay = glXGetCurrentDisplay();
	binding.glxContext = glXGetCurrentContext();
	binding.glxDrawable = glXGetCurrentDrawable();
	if( !binding.xDisplay || !binding.glxContext || !binding.glxDrawable )
	{
		gEngfuncs.Con_Printf( S_ERROR "OpenXR: a current GLX display, context, and drawable are required\n" );
		goto cleanup;
	}
	if( glXQueryContext( binding.xDisplay, binding.glxContext, GLX_FBCONFIG_ID, &fbconfig_id ) != Success )
	{
		gEngfuncs.Con_Printf( S_ERROR "OpenXR: failed to query the current GLX framebuffer configuration\n" );
		goto cleanup;
	}
	{
		int attributes[] = { GLX_FBCONFIG_ID, fbconfig_id, None };
		configs = glXChooseFBConfig( binding.xDisplay, DefaultScreen( binding.xDisplay ), attributes, &config_count );
	}
	if( !configs || config_count < 1 )
		goto cleanup;
	binding.glxFBConfig = configs[0];
	visual = glXGetVisualFromFBConfig( binding.xDisplay, binding.glxFBConfig );
	if( !visual )
		goto cleanup;
	binding.visualid = visual->visualid;
#endif

	session_info.next = &binding;
	session_info.systemId = xr.system_id;
	if( !GL_OpenXRCheck( xrCreateSession( xr.instance, &session_info, &xr.session ), "xrCreateSession" ))
		goto cleanup;
	{
		XrSessionActionSetsAttachInfo attach_info = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		attach_info.countActionSets = 1;
		attach_info.actionSets = &xr.actions.gameplay_set;
		if( !GL_OpenXRCheck( xrAttachSessionActionSets( xr.session, &attach_info ), "xrAttachSessionActionSets" ))
			goto cleanup;
	}
	if( !GL_OpenXRCreateActionSpaces() )
		goto cleanup;

	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	if( !GL_OpenXRCheck( xrCreateReferenceSpace( xr.session, &space_info, &xr.local_space ), "xrCreateReferenceSpace(local)" ))
		goto cleanup;
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if( !GL_OpenXRCheck( xrCreateReferenceSpace( xr.session, &space_info, &xr.head_space ), "xrCreateReferenceSpace(view)" ))
		goto cleanup;

	memset( view_configs, 0, sizeof( view_configs ));
	for( int i = 0; i < REF_VR_MAX_EYES; ++i )
		view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	if( !GL_OpenXRCheck( xrEnumerateViewConfigurationViews( xr.instance, xr.system_id,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, REF_VR_MAX_EYES, &view_count, view_configs ),
		"xrEnumerateViewConfigurationViews" ) || view_count != REF_VR_MAX_EYES )
		goto cleanup;

	if( !GL_OpenXRCheck( xrEnumerateSwapchainFormats( xr.session, 0, &format_count, NULL ), "xrEnumerateSwapchainFormats(count)" ))
		goto cleanup;
	formats = calloc( format_count, sizeof( *formats ));
	if( !formats )
		goto cleanup;
	if( !GL_OpenXRCheck( xrEnumerateSwapchainFormats( xr.session, format_count, &format_count, formats ), "xrEnumerateSwapchainFormats" ))
		goto cleanup;
	for( uint32_t i = 0; i < format_count; ++i )
	{
		if( formats[i] == GL_SRGB8_ALPHA8 )
		{
			selected_format = formats[i];
			break;
		}
		if( !selected_format && formats[i] == GL_RGBA8 )
			selected_format = formats[i];
	}
	if( !selected_format )
	{
		gEngfuncs.Con_Printf( S_ERROR "OpenXR: runtime exposes no usable RGBA8 swapchain format\n" );
		goto cleanup;
	}

	for( int i = 0; i < REF_VR_MAX_EYES; ++i )
	{
		xr.views[i].type = XR_TYPE_VIEW;
		if( !GL_OpenXRCreateEye( &xr.eyes[i], &view_configs[i], selected_format ))
			goto cleanup;
	}
	{
		XrViewConfigurationView ui_config = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
		ui_config.recommendedImageRectWidth = 1280;
		ui_config.recommendedImageRectHeight = 720;
		if( !GL_OpenXRCreateEye( &xr.ui, &ui_config, selected_format ))
			goto cleanup;
	}
	{
		/* optional: without it the menu just has no pointer beam */
		XrViewConfigurationView beam_config = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
		beam_config.recommendedImageRectWidth = 8;
		beam_config.recommendedImageRectHeight = 8;
		if( !GL_OpenXRCreateEye( &xr.beam, &beam_config, selected_format ))
		{
			GL_OpenXRDestroyEye( &xr.beam );
			memset( &xr.beam, 0, sizeof( xr.beam ));
		}
	}

	{
		/* optional: without it the HUD simply stays on the head-locked panel */
		XrViewConfigurationView hud_config = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
		float scale = bound( 0.5f, vr_supersampling->value, 2.0f );
		hud_config.recommendedImageRectWidth = (uint32_t)( GL_OPENXR_HAND_HUD_WIDTH / scale );
		hud_config.recommendedImageRectHeight = (uint32_t)( GL_OPENXR_HAND_HUD_HEIGHT / scale );
		if( !GL_OpenXRCreateEye( &xr.hud, &hud_config, selected_format ))
		{
			GL_OpenXRDestroyEye( &xr.hud );
			memset( &xr.hud, 0, sizeof( xr.hud ));
		}
	}

	{
		/* optional: without it scripted cameras are shown all around you as before (vr_cinema 0) */
		XrViewConfigurationView cinema_config = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
		float scale = bound( 0.5f, vr_supersampling->value, 2.0f );
		cinema_config.recommendedImageRectWidth = (uint32_t)( GL_OPENXR_CINEMA_WIDTH / scale );
		cinema_config.recommendedImageRectHeight = (uint32_t)( GL_OPENXR_CINEMA_HEIGHT / scale );
		if( GL_OpenXRCreateEye( &xr.cinema.image, &cinema_config, selected_format ))
		{
			pglGenRenderbuffers( 1, &xr.cinema.copy_rb );
			pglBindRenderbuffer( GL_RENDERBUFFER, xr.cinema.copy_rb );
			pglRenderbufferStorage( GL_RENDERBUFFER, (GLenum)selected_format, xr.cinema.image.width, xr.cinema.image.height );
			pglBindRenderbuffer( GL_RENDERBUFFER, 0 );
			pglGenFramebuffers( 1, &xr.cinema.copy_fbo );
			pglBindFramebuffer( GL_FRAMEBUFFER, xr.cinema.copy_fbo );
			pglFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, xr.cinema.copy_rb );
			if( pglCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			{
				/* the screen still works, it just goes black at once when the camera ends */
				pglDeleteFramebuffers( 1, &xr.cinema.copy_fbo );
				pglDeleteRenderbuffers( 1, &xr.cinema.copy_rb );
				xr.cinema.copy_fbo = xr.cinema.copy_rb = 0;
			}
			pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		}
		else
		{
			GL_OpenXRDestroyEye( &xr.cinema.image );
			memset( &xr.cinema.image, 0, sizeof( xr.cinema.image ));
		}
	}

	{
		/* optional: without it zoomed weapons just show the unzoomed view */
		XrViewConfigurationView scope_config = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
		float scale = bound( 0.5f, vr_supersampling->value, 2.0f );
		scope_config.recommendedImageRectWidth = (uint32_t)( 512 / scale );
		scope_config.recommendedImageRectHeight = (uint32_t)( 512 / scale );
		if( !GL_OpenXRCreateEye( &xr.scope.image, &scope_config, selected_format ))
		{
			GL_OpenXRDestroyEye( &xr.scope.image );
			memset( &xr.scope.image, 0, sizeof( xr.scope.image ));
		}
	}

	xr.initialized = true;
#if defined( _WIN32 )
	gEngfuncs.Con_Printf( "OpenXR: initialized Windows/WGL stereo session (%ux%u per eye)\n",
		xr.eyes[0].width, xr.eyes[0].height );
#else
	gEngfuncs.Con_Printf( "OpenXR: initialized Linux/GLX stereo session (%ux%u per eye)\n",
		xr.eyes[0].width, xr.eyes[0].height );
#endif
	success = true;

cleanup:
#if !defined( _WIN32 )
	if( visual ) XFree( visual );
	if( configs ) XFree( configs );
#endif
	free( formats );
	if( !success )
	{
		GL_OpenXRShutdown();
		xr.failed = true;
	}
	return success;
}

static void GL_OpenXRLogInteractionProfiles( void )
{
	static const char *hand_names[REF_VR_MAX_HANDS] = { "left", "right" };
	char profile_name[XR_MAX_PATH_LENGTH];

	for( int hand = 0; hand < REF_VR_MAX_HANDS; ++hand )
	{
		XrInteractionProfileState profile = { XR_TYPE_INTERACTION_PROFILE_STATE };
		uint32_t length = 0;

		if( !GL_OpenXRCheck( xrGetCurrentInteractionProfile( xr.session,
			xr.actions.hand_paths[hand], &profile ), "xrGetCurrentInteractionProfile" ))
			continue;
		if( profile.interactionProfile == XR_NULL_PATH )
		{
			gEngfuncs.Con_Printf( "OpenXR: %s hand has no active interaction profile\n", hand_names[hand] );
			continue;
		}
		if( GL_OpenXRCheck( xrPathToString( xr.instance, profile.interactionProfile,
			sizeof( profile_name ), &length, profile_name ), "xrPathToString(interaction profile)" ))
			gEngfuncs.Con_Printf( "OpenXR: %s hand interaction profile: %s\n", hand_names[hand], profile_name );
	}
}

static void GL_OpenXRPollEvents( void )
{
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	XrResult result;

	while(( result = xrPollEvent( xr.instance, &event )) == XR_SUCCESS )
	{
		if( event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING )
			xr.exit_requested = true;
		else if( event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING )
			xr.reference_changed = true;
		else if( event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED )
		{
			const XrEventDataInteractionProfileChanged *changed =
				(const XrEventDataInteractionProfileChanged *)&event;
			if( changed->session == xr.session )
				GL_OpenXRLogInteractionProfiles();
		}
		else if( event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
		{
			const XrEventDataSessionStateChanged *changed = (const XrEventDataSessionStateChanged *)&event;
			xr.session_state = changed->state;
			switch( changed->state )
			{
			case XR_SESSION_STATE_READY:
				if( !xr.session_running )
				{
					XrSessionBeginInfo begin_info = { XR_TYPE_SESSION_BEGIN_INFO };
					begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if( GL_OpenXRCheck( xrBeginSession( xr.session, &begin_info ), "xrBeginSession" ))
						xr.session_running = true;
				}
				break;
			case XR_SESSION_STATE_STOPPING:
				if( xr.session_running )
				{
					GL_OpenXRCheck( xrEndSession( xr.session ), "xrEndSession" );
					xr.session_running = false;
				}
				memset( xr.haptic_until, 0, sizeof( xr.haptic_until ));
				memset( xr.haptic_sustained, 0, sizeof( xr.haptic_sustained ));
				break;
			case XR_SESSION_STATE_FOCUSED:
				xr.focused = true;
				break;
			case XR_SESSION_STATE_VISIBLE:
			case XR_SESSION_STATE_SYNCHRONIZED:
				xr.focused = false;
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				xr.exit_requested = true;
				xr.session_running = false;
				break;
			default:
				break;
			}
		}

		memset( &event, 0, sizeof( event ));
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
	}

	if( result != XR_EVENT_UNAVAILABLE )
		GL_OpenXRCheck( result, "xrPollEvent" );
}

qboolean GL_OpenXRFrameBegin( ref_vr_frame_t *frame )
{
	XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
	XrViewLocateInfo locate_info = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState view_state = { XR_TYPE_VIEW_STATE };
	XrSpaceLocation head_location = { XR_TYPE_SPACE_LOCATION };
	uint32_t view_count = 0;
	XrResult result;
	qboolean actions_synced;

	if( xr.frame_begun )
		GL_OpenXRFrameEnd();

	if( !vr_enable )
		GL_OpenXRInit();

	if( frame )
	{
		memset( frame, 0, sizeof( *frame ));
		frame->version = REF_VR_FRAME_VERSION;
		frame->struct_size = sizeof( *frame );
	}

	if( !vr_enable->value )
	{
		if( xr.initialized ) GL_OpenXRShutdown();
		xr.failed = false;
		return false;
	}
	if( xr.failed || ( !xr.initialized && !GL_OpenXRInitialize() ))
		return false;

	GL_OpenXRPollEvents();
	if( xr.exit_requested )
	{
		gEngfuncs.Con_Printf( "OpenXR: runtime requested session shutdown; toggle vr_enable to retry\n" );
		GL_OpenXRShutdown();
		xr.failed = true;
		return false;
	}
	if( !xr.session_running )
		return false;

	memset( &xr.frame_state, 0, sizeof( xr.frame_state ));
	xr.frame_state.type = XR_TYPE_FRAME_STATE;
	if( !GL_OpenXRCheck( xrWaitFrame( xr.session, &wait_info, &xr.frame_state ), "xrWaitFrame" ))
		return false;
	result = xrBeginFrame( xr.session, &begin_info );
	if( !GL_OpenXRCheck( result, "xrBeginFrame" ))
		return false;
	if( result == XR_FRAME_DISCARDED )
		xr.frame_state.shouldRender = XR_FALSE;

	xr.frame_begun = true;
	xr.views_valid = false;
	xr.current_eye = -1;
	xr.ui_active = false;
	xr.ui.ready = false;
	xr.hud.ready = false;
	xr.cinema.image.ready = false;
	xr.cinema.rendering = false;
	xr.scope.image.ready = false;
	xr.scope.rendering = false;
	for( int i = 0; i < REF_VR_MAX_EYES; ++i )
		xr.eyes[i].ready = false;
	actions_synced = GL_OpenXRSyncActions();

	locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate_info.displayTime = xr.frame_state.predictedDisplayTime;
	locate_info.space = xr.local_space;
	result = xrLocateViews( xr.session, &locate_info, &view_state, REF_VR_MAX_EYES, &view_count, xr.views );
	if( GL_OpenXRCheck( result, "xrLocateViews" ) && view_count == REF_VR_MAX_EYES )
	{
		const XrViewStateFlags required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		xr.views_valid = ( view_state.viewStateFlags & required ) == required;
	}

	result = xrLocateSpace( xr.head_space, xr.local_space, xr.frame_state.predictedDisplayTime, &head_location );
	if( GL_OpenXRCheck( result, "xrLocateSpace(head)" ))
	{
		const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		if(( head_location.locationFlags & required ) == required )
			xr.head_pose = head_location.pose;
		else xr.views_valid = false;
	}
	else xr.views_valid = false;

	if( vr_ui_world->value != 0.0f && xr.views_valid )
	{
		if( !xr.ui_world_active )
			xr.ui_anchor = GL_OpenXRPanelAnchor();
		xr.ui_world_active = true;
	}
	else if( vr_ui_world->value == 0.0f )
		xr.ui_world_active = false;

	for( int i = 0; i < REF_VR_MAX_HANDS; ++i )
	{
		xr.ui_pointer[i].valid = false;
		xr.hand_pose[i].grip_valid = xr.hand_pose[i].aim_valid = false;
	}

	if( frame )
	{
		frame->frame_id = ++xr.frame_id;
		frame->flags = REF_VR_FRAME_SESSION_ACTIVE;
		if( xr.focused ) SetBits( frame->flags, REF_VR_FRAME_FOCUSED );
		if( actions_synced )
			actions_synced = GL_OpenXRFillHands( frame->hands );
		if( actions_synced ) SetBits( frame->flags, REF_VR_FRAME_ACTIONS_VALID );
		if( actions_synced && !xr.tracked_controllers_once )
		{
			const uint32_t required = REF_VR_HAND_GRIP_VALID | REF_VR_HAND_AIM_VALID;
			if(( frame->hands[0].flags & required ) == required &&
				( frame->hands[1].flags & required ) == required )
			{
				xr.tracked_controllers_once = true;
				gEngfuncs.Con_Printf( "OpenXR: first tracked controller frame received\n" );
			}
		}
		if( xr.reference_changed ) SetBits( frame->flags, REF_VR_FRAME_REFERENCE_CHANGED );
		if( xr.views_valid )
		{
			SetBits( frame->flags, REF_VR_FRAME_HEAD_VALID );
			GL_OpenXRFillPose( &xr.head_pose, &frame->head );
			frame->eye_count = REF_VR_MAX_EYES;
		}
		if( xr.frame_state.shouldRender && xr.views_valid )
			SetBits( frame->flags, REF_VR_FRAME_SHOULD_RENDER );
	}
	xr.reference_changed = false;

	return true;
}

qboolean GL_OpenXRBeginEye( int eye )
{
	gl_openxr_eye_t *target;
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

	if( !xr.frame_begun || !xr.frame_state.shouldRender || !xr.views_valid || eye < 0 || eye >= REF_VR_MAX_EYES )
		return false;

	/* Render the eye straight into its swapchain image at the runtime's recommended size
	   instead of into the desktop back buffer (which capped the eye at the window size). */
	target = &xr.eyes[eye];
	if( !GL_OpenXRCheck( xrAcquireSwapchainImage( target->swapchain, &acquire_info, &target->image_index ), "xrAcquireSwapchainImage" ))
		return false;
	target->acquired = true;
	target->waited = false;
	wait_info.timeout = XR_INFINITE_DURATION;
	if( !GL_OpenXRCheck( xrWaitSwapchainImage( target->swapchain, &wait_info ), "xrWaitSwapchainImage" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return false;
	}
	target->waited = true;

	pglBindFramebuffer( GL_FRAMEBUFFER, target->framebuffers[target->image_index] );
	pglViewport( 0, 0, target->width, target->height );
	{
		/* Swapchain images are reused: a view that does not cover every pixel (no sky, the CS spectator
		 * overview map) otherwise shows older frames through it, smearing like dragged windows. */
		GLfloat old_clear[4];
		pglGetFloatv( GL_COLOR_CLEAR_VALUE, old_clear );
		pglPushAttrib( GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		pglDisable( GL_SCISSOR_TEST );
		pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		pglDepthMask( GL_TRUE );
		pglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		pglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
		pglPopAttrib();
		pglClearColor( old_clear[0], old_clear[1], old_clear[2], old_clear[3] );
	}
	xr.current_eye = eye;
	return true;
}

qboolean GL_OpenXRGetEyeSize( int *width, int *height )
{
	if( xr.cinema.rendering )
	{
		*width = xr.cinema.image.width;
		*height = xr.cinema.image.height;
		return true;
	}
	if( xr.current_eye < 0 || xr.current_eye >= REF_VR_MAX_EYES || !xr.eyes[xr.current_eye].acquired )
		return false; /* only between BeginEye and EndEye (xr is zeroed, i.e. current_eye 0, before VR init) */
	*width = xr.eyes[xr.current_eye].width;
	*height = xr.eyes[xr.current_eye].height;
	return true;
}

/* Screen fades (env_fade, death, damage flashes, the expansions' black title screen) are 2D, so in VR they
 * landed on the floating HUD panel as a coloured box. The engine draws them here instead, over the whole
 * eye image, with the same blend as the desktop path (modulate = multiply). */
static void GL_OpenXRDrawFade( const gl_openxr_eye_t *target, const byte *rgba, qboolean modulate );

void GL_OpenXRDrawEyeFade( const byte *rgba, qboolean modulate )
{
	if( !rgba )
		return;
	if( xr.cinema.rendering )
		GL_OpenXRDrawFade( &xr.cinema.image, rgba, modulate ); /* the camera view on the cinema screen */
	else if( xr.current_eye >= 0 && xr.current_eye < REF_VR_MAX_EYES && xr.eyes[xr.current_eye].acquired )
		GL_OpenXRDrawFade( &xr.eyes[xr.current_eye], rgba, modulate );
}

/* Full-image fade over whatever target framebuffer is bound. */
static void GL_OpenXRDrawFade( const gl_openxr_eye_t *target, const byte *rgba, qboolean modulate )
{

	pglPushAttrib( GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT | GL_FOG_BIT );
	pglViewport( 0, 0, target->width, target->height );
	pglMatrixMode( GL_PROJECTION );
	pglPushMatrix();
	pglLoadIdentity();
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadIdentity();
	pglDisable( GL_TEXTURE_2D );
	pglDisable( GL_DEPTH_TEST );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_CULL_FACE );
	pglDisable( GL_FOG );
	pglDisable( GL_SCISSOR_TEST );
	pglDepthMask( GL_FALSE );
	pglEnable( GL_BLEND );
	if( modulate )
		pglBlendFunc( GL_ZERO, GL_SRC_COLOR );
	else pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	pglColor4ub( rgba[0], rgba[1], rgba[2], rgba[3] );
	pglBegin( GL_QUADS );
	pglVertex2f( -1.0f, -1.0f );
	pglVertex2f( 1.0f, -1.0f );
	pglVertex2f( 1.0f, 1.0f );
	pglVertex2f( -1.0f, 1.0f );
	pglEnd();
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglPopAttrib();
	pglColor4ub( 255, 255, 255, 255 );
}

qboolean GL_OpenXREndEye( int eye, int source_width, int source_height )
{
	gl_openxr_eye_t *target;
	GLenum blit_error;
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	if( eye != xr.current_eye || eye < 0 || eye >= REF_VR_MAX_EYES )
		return false;
	target = &xr.eyes[eye];
	xr.current_eye = -1;

	if( !target->acquired || !target->waited || source_width <= 0 || source_height <= 0 )
		return false;

	if( eye == ( vr_mirror_eye->value ? 1 : 0 ))
	{
		/* desktop mirror only; a failure here (e.g. multisampled back buffer) is harmless */
		while( pglGetError() != GL_NO_ERROR ) {}
		pglBindFramebuffer( GL_READ_FRAMEBUFFER, target->framebuffers[target->image_index] );
		pglBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
		pglBlitFramebuffer( 0, 0, target->width, target->height, 0, 0, source_width, source_height,
			GL_COLOR_BUFFER_BIT, GL_LINEAR );
		blit_error = pglGetError();
		if( blit_error != GL_NO_ERROR && !xr.mirror_warned )
		{
			gEngfuncs.Con_Reportf( S_NOTE "OpenXR: desktop mirror blit failed with GL error 0x%x\n", blit_error );
			xr.mirror_warned = true;
		}
	}
	pglBindFramebuffer( GL_FRAMEBUFFER, target->framebuffers[target->image_index] );
	pglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE );
	pglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	pglClear( GL_COLOR_BUFFER_BIT );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	{
		GLboolean scissor_enabled = pglIsEnabled( GL_SCISSOR_TEST );
		GLint old_scissor[4];
		GLfloat old_clear[4];
		float target_level = vr_scope_engaged->value ? 0.75f :
			( vr_comfort_moving->value ? bound( 0.0f, vr_comfort_mask->value, 1.0f ) : 0.0f );
		float step_source = vr_comfort_mask->value > 0.0f ?
			bound( 0.0f, vr_comfort_mask->value, 1.0f ) : 0.75f;

		if( xr.comfort_frame != xr.frame_id )
		{
			float step = step_source * 0.05f;
			xr.comfort_frame = xr.frame_id;
			if( xr.comfort_level < target_level )
				xr.comfort_level = Q_min( target_level, xr.comfort_level + step );
			else if( xr.comfort_level > target_level )
				xr.comfort_level = Q_max( target_level, xr.comfort_level - step );
		}

		if( xr.comfort_level > 0.0f )
		{
			GLint mask_width = (GLint)( target->width * 0.5f * xr.comfort_level );
			GLint mask_height = (GLint)( target->height * 0.5f * xr.comfort_level );

			pglGetIntegerv( GL_SCISSOR_BOX, old_scissor );
			pglGetFloatv( GL_COLOR_CLEAR_VALUE, old_clear );
			pglEnable( GL_SCISSOR_TEST );
			pglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
			pglScissor( 0, 0, target->width, mask_height );
			pglClear( GL_COLOR_BUFFER_BIT );
			pglScissor( 0, target->height - mask_height, target->width, mask_height );
			pglClear( GL_COLOR_BUFFER_BIT );
			pglScissor( 0, 0, mask_width, target->height );
			pglClear( GL_COLOR_BUFFER_BIT );
			pglScissor( target->width - mask_width, 0, mask_width, target->height );
			pglClear( GL_COLOR_BUFFER_BIT );
			pglScissor( old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3] );
			if( !scissor_enabled ) pglDisable( GL_SCISSOR_TEST );
			pglClearColor( old_clear[0], old_clear[1], old_clear[2], old_clear[3] );
		}
	}
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglViewport( 0, 0, source_width, source_height );

	if( !GL_OpenXRCheck( xrReleaseSwapchainImage( target->swapchain, &release_info ), "xrReleaseSwapchainImage" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return false;
	}
	target->acquired = false;
	target->waited = false;
	target->ready = true;
	return true;
}

/* SteamVR drops UI-layer pixels whose alpha is 0, so additively drawn HUD elements (alpha
 * kept at 0 by GL_OpenXRBlendFunc) vanished. Before submitting, rewrite each pixel as
 * unpremultiplied colour with alpha = max( a, r, g, b ): normal blended content (rgb <= a,
 * premultiplied) is unchanged, an additive glow c becomes c over the world at opacity max(c). *
 * 2026-10-06: the layer is sRGB and SteamVR blends it in linear light, so faint additive fringes
 * (1-3/255 around text and sprite boxes, invisible on the desktop) became full-white pixels at ~1%
 * opacity, ~10x brighter than meant: visible lines around HUD text. Glow opacity is now
 * max(c)^vr_ui_glow_curve (1 = old behaviour) with the hue normalised, so fringes drop to 0. */
static struct
{
	qboolean tried, ok;
	GLhandleARB program;
	GLint curve_location;
	GLuint copy;
	int copy_width, copy_height;
} xr_ui_fix;

static qboolean GL_OpenXRInitUIAlphaFix( void )
{
	static const char *vs =
		"void main() { gl_TexCoord[0] = gl_MultiTexCoord0; gl_Position = gl_Vertex; }\n";
	static const char *fs =
		"uniform sampler2D ui;\n"
		"uniform float curve;\n"
		"void main() {\n"
		"	vec4 c = texture2D( ui, gl_TexCoord[0].xy );\n"
		"	float m = max( c.r, max( c.g, c.b ));\n"
		"	if( c.a >= m ) {\n"
		"		gl_FragColor = c.a > 0.0 ? vec4( c.rgb / c.a, c.a ) : vec4( 0.0 );\n"
		"		return;\n"
		"	}\n"
		"	float a = max( c.a, pow( m, curve ));\n"
		"	gl_FragColor = a > 0.5 / 255.0 ? vec4( c.rgb / m, a ) : vec4( 0.0 );\n"
		"}\n";
	GLhandleARB shaders[2];
	GLint status = 0;

	if( xr_ui_fix.tried )
		return xr_ui_fix.ok;
	xr_ui_fix.tried = true;
	if( !pglCreateShaderObjectARB || !pglCreateProgramObjectARB || !pglUseProgramObjectARB || !pglCopyTexSubImage2D )
	{
		gEngfuncs.Con_Printf( S_WARN "OpenXR: no GLSL, VR HUD alpha fix disabled\n" );
		return false;
	}
	shaders[0] = pglCreateShaderObjectARB( GL_VERTEX_SHADER_ARB );
	shaders[1] = pglCreateShaderObjectARB( GL_FRAGMENT_SHADER_ARB );
	pglShaderSourceARB( shaders[0], 1, &vs, NULL );
	pglShaderSourceARB( shaders[1], 1, &fs, NULL );
	xr_ui_fix.program = pglCreateProgramObjectARB();
	for( int i = 0; i < 2; ++i )
	{
		pglCompileShaderARB( shaders[i] );
		pglGetObjectParameterivARB( shaders[i], GL_OBJECT_COMPILE_STATUS_ARB, &status );
		if( !status )
		{
			char log[1024] = "";
			pglGetInfoLogARB( shaders[i], sizeof( log ), NULL, log );
			gEngfuncs.Con_Printf( S_ERROR "OpenXR: VR HUD alpha fix shader: %s\n", log );
			return false;
		}
		pglAttachObjectARB( xr_ui_fix.program, shaders[i] );
	}
	pglLinkProgramARB( xr_ui_fix.program );
	pglGetObjectParameterivARB( xr_ui_fix.program, GL_OBJECT_LINK_STATUS_ARB, &status );
	if( !status )
	{
		gEngfuncs.Con_Printf( S_ERROR "OpenXR: VR HUD alpha fix shader did not link\n" );
		return false;
	}
	pglUseProgramObjectARB( xr_ui_fix.program );
	pglUniform1iARB( pglGetUniformLocationARB( xr_ui_fix.program, "ui" ), 0 );
	xr_ui_fix.curve_location = pglGetUniformLocationARB( xr_ui_fix.program, "curve" );
	pglUseProgramObjectARB( 0 );
	pglGenTextures( 1, &xr_ui_fix.copy );
	xr_ui_fix.ok = true;
	return true;
}

/* Runs with the UI framebuffer bound; leaves GL state (and Xash's state cache) as it found it. */
static void GL_OpenXRFixUIAlpha( void )
{
	GLint old_texture = 0;
	int old_tmu = glState.activeTMU;

	if( !GL_OpenXRInitUIAlphaFix() )
		return;

	GL_SelectTexture( 0 );
	pglGetIntegerv( GL_TEXTURE_BINDING_2D, &old_texture );
	pglPushAttrib( GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_TEXTURE_BIT );
	pglBindTexture( GL_TEXTURE_2D, xr_ui_fix.copy );
	if( xr_ui_fix.copy_width != xr.ui.width || xr_ui_fix.copy_height != xr.ui.height )
	{
		pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, xr.ui.width, xr.ui.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		xr_ui_fix.copy_width = xr.ui.width;
		xr_ui_fix.copy_height = xr.ui.height;
	}
	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, xr.ui.width, xr.ui.height );

	pglDisable( GL_BLEND );
	pglDisable( GL_DEPTH_TEST );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_CULL_FACE );
	pglDisable( GL_SCISSOR_TEST );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglUseProgramObjectARB( xr_ui_fix.program );
	if( xr_ui_fix.curve_location >= 0 && pglUniform1fARB )
	{
		float curve = vr_ui_glow_curve ? vr_ui_glow_curve->value : 1.6f;
		pglUniform1fARB( xr_ui_fix.curve_location, curve > 0.1f ? curve : 1.6f );
	}
	pglBegin( GL_QUADS );
	pglTexCoord2f( 0.0f, 0.0f ); pglVertex2f( -1.0f, -1.0f );
	pglTexCoord2f( 1.0f, 0.0f ); pglVertex2f( 1.0f, -1.0f );
	pglTexCoord2f( 1.0f, 1.0f ); pglVertex2f( 1.0f, 1.0f );
	pglTexCoord2f( 0.0f, 1.0f ); pglVertex2f( -1.0f, 1.0f );
	pglEnd();
	pglUseProgramObjectARB( 0 );

	pglPopAttrib();
	pglBindTexture( GL_TEXTURE_2D, old_texture );
	GL_SelectTexture( old_tmu );
}

/* The menu pointer follows the engine's dominant hand (right unless a left-handed control scheme). */
static int GL_OpenXRPointerHand( void )
{
	return gEngfuncs.pfnGetCvarFloat( "vr_control_scheme" ) >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
}

/* Cursor dot drawn into the UI panel (2D pass still active, ortho in gpGlobals->width/height units). */
static void GL_OpenXRDrawUIPointerDot( void )
{
	const int hand = GL_OpenXRPointerHand();
	float x, y, radius;

	if( !xr.ui_world_active || !xr.ui_pointer[hand].valid )
		return;
	x = xr.ui_pointer[hand].u * gpGlobals->width;
	y = xr.ui_pointer[hand].v * gpGlobals->height;
	radius = gpGlobals->width * 0.007f;

	pglPushAttrib( GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT );
	pglDisable( GL_TEXTURE_2D );
	pglDisable( GL_BLEND );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_DEPTH_TEST );
	pglDisable( GL_CULL_FACE );
	for( int ring = 0; ring < 2; ++ring )
	{
		const float r = ring ? radius * 0.45f : radius;
		if( ring ) pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		else pglColor4f( 0.9f, 0.15f, 0.12f, 1.0f );
		pglBegin( GL_TRIANGLE_FAN );
		pglVertex2f( x, y );
		for( int i = 0; i <= 20; ++i )
		{
			const float a = (float)i * ( 2.0f * (float)M_PI / 20.0f );
			pglVertex2f( x + cosf( a ) * r, y + sinf( a ) * r );
		}
		pglEnd();
	}
	pglPopAttrib();
}

/* Fill the beam swapchain with a solid colour; the quad layer stretches it along the ray. */
static qboolean GL_OpenXRPrepareBeam( void )
{
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	GLfloat old_clear[4];
	GLint old_viewport[4];

	if( xr.beam.swapchain == XR_NULL_HANDLE )
		return false;
	if( !GL_OpenXRCheck( xrAcquireSwapchainImage( xr.beam.swapchain, &acquire_info, &xr.beam.image_index ),
		"xrAcquireSwapchainImage(beam)" ))
		return false;
	wait_info.timeout = XR_INFINITE_DURATION;
	if( !GL_OpenXRCheck( xrWaitSwapchainImage( xr.beam.swapchain, &wait_info ), "xrWaitSwapchainImage(beam)" ))
		return false;
	pglGetFloatv( GL_COLOR_CLEAR_VALUE, old_clear );
	pglGetIntegerv( GL_VIEWPORT, old_viewport );
	pglBindFramebuffer( GL_FRAMEBUFFER, xr.beam.framebuffers[xr.beam.image_index] );
	pglViewport( 0, 0, xr.beam.width, xr.beam.height );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglClearColor( 0.95f, 0.2f, 0.15f, 0.85f );
	pglClear( GL_COLOR_BUFFER_BIT );
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglViewport( old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3] );
	pglClearColor( old_clear[0], old_clear[1], old_clear[2], old_clear[3] );
	return GL_OpenXRCheck( xrReleaseSwapchainImage( xr.beam.swapchain, &release_info ), "xrReleaseSwapchainImage(beam)" );
}

/* Rotation with the given orthonormal axes as its columns. */
static XrQuaternionf GL_OpenXRQuatFromBasis( XrVector3f xa, XrVector3f ya, XrVector3f za )
{
	const float m00 = xa.x, m01 = ya.x, m02 = za.x;
	const float m10 = xa.y, m11 = ya.y, m12 = za.y;
	const float m20 = xa.z, m21 = ya.z, m22 = za.z;
	const float trace = m00 + m11 + m22;
	XrQuaternionf q;
	float sq;

	if( trace > 0.0f )
	{
		sq = sqrtf( trace + 1.0f ) * 2.0f;
		q.w = 0.25f * sq;
		q.x = ( m21 - m12 ) / sq;
		q.y = ( m02 - m20 ) / sq;
		q.z = ( m10 - m01 ) / sq;
	}
	else if( m00 > m11 && m00 > m22 )
	{
		sq = sqrtf( 1.0f + m00 - m11 - m22 ) * 2.0f;
		q.w = ( m21 - m12 ) / sq;
		q.x = 0.25f * sq;
		q.y = ( m01 + m10 ) / sq;
		q.z = ( m02 + m20 ) / sq;
	}
	else if( m11 > m22 )
	{
		sq = sqrtf( 1.0f + m11 - m00 - m22 ) * 2.0f;
		q.w = ( m02 - m20 ) / sq;
		q.x = ( m01 + m10 ) / sq;
		q.y = 0.25f * sq;
		q.z = ( m12 + m21 ) / sq;
	}
	else
	{
		sq = sqrtf( 1.0f + m22 - m00 - m11 ) * 2.0f;
		q.w = ( m10 - m01 ) / sq;
		q.x = ( m02 + m20 ) / sq;
		q.y = ( m12 + m21 ) / sq;
		q.z = 0.25f * sq;
	}
	return q;
}

/* Quad layer from the controller to the hit point, rotated about the ray to face the head: the menu pointer, or
 * the weapon grid pointer (never both: the grid is not shown while a menu is open). */
static qboolean GL_OpenXRBuildBeamLayer( XrCompositionLayerQuad *beam )
{
	const int hand = GL_OpenXRPointerHand();
	XrVector3f o, d, mid, to_head, xa, ya, za;
	float length, dot, len;

	if( xr.ui_world_active && xr.ui_pointer[hand].valid )
	{
		o = xr.ui_pointer[hand].origin;
		d.x = xr.ui_pointer[hand].hit.x - o.x;
		d.y = xr.ui_pointer[hand].hit.y - o.y;
		d.z = xr.ui_pointer[hand].hit.z - o.z;
	}
	else if( !xr.ui_world_active && xr.weapon_pointer.valid )
	{
		o = xr.weapon_pointer.origin;
		d.x = xr.weapon_pointer.end.x - o.x;
		d.y = xr.weapon_pointer.end.y - o.y;
		d.z = xr.weapon_pointer.end.z - o.z;
	}
	else return false;
	length = sqrtf( d.x * d.x + d.y * d.y + d.z * d.z );
	if( length < 0.02f || !GL_OpenXRPrepareBeam() )
		return false;

	ya.x = d.x / length; ya.y = d.y / length; ya.z = d.z / length;
	mid.x = o.x + d.x * 0.5f; mid.y = o.y + d.y * 0.5f; mid.z = o.z + d.z * 0.5f;
	to_head.x = xr.head_pose.position.x - mid.x;
	to_head.y = xr.head_pose.position.y - mid.y;
	to_head.z = xr.head_pose.position.z - mid.z;
	dot = to_head.x * ya.x + to_head.y * ya.y + to_head.z * ya.z;
	za.x = to_head.x - ya.x * dot; za.y = to_head.y - ya.y * dot; za.z = to_head.z - ya.z * dot;
	len = sqrtf( za.x * za.x + za.y * za.y + za.z * za.z );
	if( len < 0.0001f )
		return false; /* looking straight down the beam */
	za.x /= len; za.y /= len; za.z /= len;
	xa.x = ya.y * za.z - ya.z * za.y; /* x = y cross z */
	xa.y = ya.z * za.x - ya.x * za.z;
	xa.z = ya.x * za.y - ya.y * za.x;

	beam->pose.orientation = GL_OpenXRQuatFromBasis( xa, ya, za );
	beam->pose.position = mid;
	beam->space = xr.local_space;
	beam->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
		XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	beam->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	beam->subImage.swapchain = xr.beam.swapchain;
	beam->subImage.imageRect.extent.width = xr.beam.width;
	beam->subImage.imageRect.extent.height = xr.beam.height;
	beam->size.width = 0.004f;
	beam->size.height = length;
	return true;
}

/* "x y w h screenwidth screenheight" (HUD pixels, y down) -> UI texture pixels (GL, y up), with a small margin
 * for the glow around the digits. */
static qboolean GL_OpenXRParseHandHUDRect( const cvar_t *cv, int rect[4], int origin[2], int size[2] )
{
	int x, y, w, h, sw, sh;

	if( !cv || sscanf( cv->string, "%d %d %d %d %d %d", &x, &y, &w, &h, &sw, &sh ) != 6 ||
		w <= 0 || h <= 0 || sw <= 0 || sh <= 0 )
		return false;
	x -= 2; y -= 2; w += 4; h += 4;
	origin[0] = x;
	origin[1] = y;
	size[0] = w;
	size[1] = h;
	rect[0] = bound( 0, x * (int)xr.ui.width / sw, (int)xr.ui.width );
	rect[2] = bound( 0, ( x + w ) * (int)xr.ui.width / sw, (int)xr.ui.width );
	rect[1] = bound( 0, (int)xr.ui.height - ( y + h ) * (int)xr.ui.height / sh, (int)xr.ui.height );
	rect[3] = bound( 0, (int)xr.ui.height - y * (int)xr.ui.height / sh, (int)xr.ui.height );
	return rect[2] > rect[0] && rect[3] > rect[1];
}

/* Cursor dot on the weapon grid, at last frame's hit (the ray is cast against the panel pose in FrameEnd). Runs
 * with the hand HUD framebuffer bound for drawing; the slot is stretched, so the dot is sized in HUD pixels. */
static void GL_OpenXRDrawWeaponPointerDot( void )
{
	const int p = GL_OPENXR_HAND_HUD_WEAPONS;
	float x, y, rx, ry;

	if( !xr.hand_hud[p].valid || !xr.weapon_pointer.hit )
		return;
	rx = 4.0f * xr.hand_hud[p].width / xr.hand_hud[p].hud_width;
	ry = 4.0f * xr.hand_hud[p].height / xr.hand_hud[p].hud_height;
	x = xr.hand_hud[p].x + xr.weapon_pointer.u * xr.hand_hud[p].width;
	y = ( 1.0f - xr.weapon_pointer.v ) * xr.hand_hud[p].height; /* GL: bottom-up */
	pglEnable( GL_SCISSOR_TEST );
	for( int ring = 0; ring < 2; ++ring )
	{
		const float k = ring ? 0.45f : 1.0f;
		if( ring ) pglClearColor( 1.0f, 1.0f, 1.0f, 1.0f );
		else pglClearColor( 0.9f, 0.15f, 0.12f, 1.0f );
		pglScissor( (int)( x - rx * k ), (int)( y - ry * k ), Q_max( 1, (int)( 2.0f * rx * k )), Q_max( 1, (int)( 2.0f * ry * k )));
		pglClear( GL_COLOR_BUFFER_BIT );
	}
	pglDisable( GL_SCISSOR_TEST );
	pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
}

/* Runs with the finished UI framebuffer bound (after the alpha fix-up): copies the status, ammo and weapon grid
 * boxes into the hand HUD swapchain and erases them from the head-locked UI. */
static void GL_OpenXRCaptureHandHUD( void )
{
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	/* boxes per panel, top to bottom */
	/* boxes per panel: stacked top to bottom, or (weapon grid rows) side by side left to right */
	static const int panel_boxes[GL_OPENXR_HAND_HUD_COUNT][GL_OPENXR_HAND_HUD_MAX_BOXES] = {
		/* the flashlight is always there with the suit, so it sits before the damage icons, which come and go;
		 * both extend the panel to the right, the health end stays put */
		{ GL_OPENXR_HUD_BOX_HEALTH, GL_OPENXR_HUD_BOX_SUIT, GL_OPENXR_HUD_BOX_FLASHLIGHT, GL_OPENXR_HUD_BOX_DAMAGE },
		{ GL_OPENXR_HUD_BOX_AMMO, -1, -1, -1 },
		{ GL_OPENXR_HUD_BOX_WEAPONS, GL_OPENXR_HUD_BOX_WEAPONS2, GL_OPENXR_HUD_BOX_WEAPONS3, -1 },
	};
	int rects[GL_OPENXR_HUD_BOX_COUNT][4], origins[GL_OPENXR_HUD_BOX_COUNT][2], sizes[GL_OPENXR_HUD_BOX_COUNT][2];
	qboolean box_valid[GL_OPENXR_HUD_BOX_COUNT];
	float scale[GL_OPENXR_HAND_HUD_COUNT];
	qboolean any = false;

	for( int p = 0; p < GL_OPENXR_HAND_HUD_COUNT; ++p )
		xr.hand_hud[p].valid = false;
	if( xr.ui_world_active || xr.hud.swapchain == XR_NULL_HANDLE )
		return;
	for( int i = 0; i < GL_OPENXR_HUD_BOX_COUNT; ++i )
	{
		/* the weapon grid is only drawn for this panel, so it does not depend on vr_hud_hands */
		box_valid[i] = ( i >= GL_OPENXR_HUD_BOX_WEAPONS || vr_hud_hands->value != 0.0f ) &&
			GL_OpenXRParseHandHUDRect( vr_hud_rect[i], rects[i], origins[i], sizes[i] );
		any = any || box_valid[i];
	}
	if( !any )
		return;
	/* panel content = its boxes stacked (or side by side), stretched to fill the slot */
	for( int p = 0; p < GL_OPENXR_HAND_HUD_COUNT; ++p )
	{
		/* health and suit side by side like the desktop HUD (with a gap), weapon grid rows joined into one row */
		const qboolean side_by_side = p == GL_OPENXR_HAND_HUD_WEAPONS || p == GL_OPENXR_HAND_HUD_STATUS;
		const int gap = p == GL_OPENXR_HAND_HUD_STATUS ? 12 : 0;
		int width = 0, height = 0;
		qboolean base_set = false;
		xr.hand_hud[p].box_count = 0;
		for( int j = 0; j < GL_OPENXR_HAND_HUD_MAX_BOXES; ++j )
		{
			const int b = panel_boxes[p][j];
			int n = xr.hand_hud[p].box_count;
			if( b < 0 || !box_valid[b] )
				continue;
			xr.hand_hud[p].box[n].index = b;
			xr.hand_hud[p].box[n].hud_x = origins[b][0];
			xr.hand_hud[p].box[n].hud_y = origins[b][1];
			xr.hand_hud[p].box[n].w = sizes[b][0];
			xr.hand_hud[p].box[n].h = sizes[b][1];
			if( side_by_side && n > 0 )
				width += gap;
			if( !base_set && ( b == GL_OPENXR_HUD_BOX_FLASHLIGHT || b == GL_OPENXR_HUD_BOX_DAMAGE ))
			{
				xr.hand_hud[p].base_width = Q_max( 0, width - gap );
				base_set = true;
			}
			xr.hand_hud[p].box[n].px = side_by_side ? width : 0;
			xr.hand_hud[p].box[n].py = side_by_side ? 0 : height;
			xr.hand_hud[p].box_count++;
			if( side_by_side )
			{
				width += sizes[b][0];
				height = Q_max( height, sizes[b][1] );
			}
			else
			{
				width = Q_max( width, sizes[b][0] );
				height += sizes[b][1];
			}
		}
		if( width <= 0 || height <= 0 )
			continue;
		if( p == GL_OPENXR_HAND_HUD_STATUS )
		{
			/* vertically centred on each other (the damage icons are taller than the digits) */
			for( int j = 0; j < xr.hand_hud[p].box_count; ++j )
				xr.hand_hud[p].box[j].py = ( height - xr.hand_hud[p].box[j].h ) / 2;
			if( !base_set )
				xr.hand_hud[p].base_width = width;
		}
		/* stretched to fill the whole slot: SteamVR counts imageRect from the top, our content was at the
		 * bottom (GL), so a short panel showed only empty texture. The quad's physical size (HUD pixels times
		 * vr_hud_hand_scale) restores the aspect. */
		scale[p] = (float)xr.hud.height / height;
		xr.hand_hud[p].valid = true;
		xr.hand_hud[p].hud_width = width;
		xr.hand_hud[p].hud_height = height;
		xr.hand_hud[p].x = gl_openxr_hand_hud_slot[p][0] * xr.hud.width / GL_OPENXR_HAND_HUD_WIDTH;
		xr.hand_hud[p].width = gl_openxr_hand_hud_slot[p][1] * xr.hud.width / GL_OPENXR_HAND_HUD_WIDTH;
		xr.hand_hud[p].height = xr.hud.height;
	}

	if( !GL_OpenXRCheck( xrAcquireSwapchainImage( xr.hud.swapchain, &acquire_info, &xr.hud.image_index ),
		"xrAcquireSwapchainImage(hand HUD)" ))
		return;
	wait_info.timeout = XR_INFINITE_DURATION;
	if( !GL_OpenXRCheck( xrWaitSwapchainImage( xr.hud.swapchain, &wait_info ), "xrWaitSwapchainImage(hand HUD)" ))
	{
		xrReleaseSwapchainImage( xr.hud.swapchain, &release_info );
		return;
	}

	pglPushAttrib( GL_ENABLE_BIT | GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT );
	pglDisable( GL_SCISSOR_TEST );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	pglBindFramebuffer( GL_FRAMEBUFFER, xr.hud.framebuffers[xr.hud.image_index] );
	pglViewport( 0, 0, xr.hud.width, xr.hud.height );
	pglClear( GL_COLOR_BUFFER_BIT );
	pglBindFramebuffer( GL_READ_FRAMEBUFFER, xr.ui.framebuffers[xr.ui.image_index] );
	pglBindFramebuffer( GL_DRAW_FRAMEBUFFER, xr.hud.framebuffers[xr.hud.image_index] );
	for( int p = 0; p < GL_OPENXR_HAND_HUD_COUNT; ++p )
	{
		/* GL images are bottom-up: py counts from the top of the slot */
		const float sx = (float)xr.hand_hud[p].width / xr.hand_hud[p].hud_width;
		if( !xr.hand_hud[p].valid )
			continue;
		for( int j = 0; j < xr.hand_hud[p].box_count; ++j )
		{
			const int b = xr.hand_hud[p].box[j].index;
			const int x0 = (int)( xr.hand_hud[p].x + xr.hand_hud[p].box[j].px * sx );
			const int x1 = (int)( xr.hand_hud[p].x + ( xr.hand_hud[p].box[j].px + xr.hand_hud[p].box[j].w ) * sx );
			const int y1 = (int)( xr.hand_hud[p].height - xr.hand_hud[p].box[j].py * scale[p] );
			const int y0 = (int)( y1 - xr.hand_hud[p].box[j].h * scale[p] );
			pglBlitFramebuffer( rects[b][0], rects[b][1], rects[b][2], rects[b][3],
				x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		}
	}
	GL_OpenXRDrawWeaponPointerDot();
	pglBindFramebuffer( GL_FRAMEBUFFER, xr.ui.framebuffers[xr.ui.image_index] );
	pglEnable( GL_SCISSOR_TEST );
	for( int i = 0; i < GL_OPENXR_HUD_BOX_COUNT; ++i )
	{
		if( !box_valid[i] )
			continue;
		pglScissor( rects[i][0], rects[i][1], rects[i][2] - rects[i][0], rects[i][3] - rects[i][1] );
		pglClear( GL_COLOR_BUFFER_BIT );
	}
	pglPopAttrib();

	if( !GL_OpenXRCheck( xrReleaseSwapchainImage( xr.hud.swapchain, &release_info ), "xrReleaseSwapchainImage(hand HUD)" ))
	{
		for( int p = 0; p < GL_OPENXR_HAND_HUD_COUNT; ++p )
			xr.hand_hud[p].valid = false;
		return;
	}
	xr.hud.ready = true;
}

/* The engine raises the eyes by vr_height_adjust, so in the game the guns and the flashlight are that much lower
 * relative to you than the real controllers. The weapon grid and its pointer are shifted down by it to line up
 * with the gun's laser sight and the flashlight model (LOCAL space: +Y up). */
static float GL_OpenXRGameHandDrop( void )
{
	return bound( -1.0f, gEngfuncs.pfnGetCvarFloat( "vr_height_adjust" ), 1.0f );
}

/* Quad for one hand HUD panel, in that controller's grip space so the compositor keeps it on the hand. */
static qboolean GL_OpenXRBuildHandHUDLayer( int index, XrCompositionLayerQuad *quad )
{
	const int weapon_hand = GL_OpenXRPointerHand();
	const int off_hand = weapon_hand == REF_VR_HAND_LEFT ? REF_VR_HAND_RIGHT : REF_VR_HAND_LEFT;
	/* Holding the gun with both hands (vr_weapon_stabilised, set by the client) the off hand is on the gun: the
	 * health panel then docks under the ammo panel, with the ammo panel's size, tilt, turn and position. */
	const qboolean docked = index == GL_OPENXR_HAND_HUD_STATUS && gEngfuncs.pfnGetCvarFloat( "vr_weapon_stabilised" ) != 0.0f;
	const qboolean ammo = index == GL_OPENXR_HAND_HUD_AMMO || docked;
	const int hand = ammo ? weapon_hand : off_hand;
	float metres = bound( 0.00005f, ( index == GL_OPENXR_HAND_HUD_WEAPONS ? vr_weapon_pointer_scale : ammo ? vr_hud_ammo_scale : vr_hud_hand_scale )->value, 0.005f );
	float tilt = DEG2RAD( bound( -90.0f, ( ammo ? vr_hud_ammo_tilt : vr_hud_hand_tilt )->value, 90.0f ));
	/* inwards = towards +X (right) on the left hand, -X on the right hand */
	float yaw = DEG2RAD( bound( -180.0f, ( index == GL_OPENXR_HAND_HUD_WEAPONS ? vr_weapon_pointer_yaw : ammo ? vr_hud_ammo_yaw : vr_hud_hand_yaw )->value, 180.0f )) *
		( hand == REF_VR_HAND_LEFT ? 1.0f : -1.0f );
	XrQuaternionf turn = { 0.0f, 0.0f, 0.0f, 1.0f }, lean = { 0.0f, 0.0f, 0.0f, 1.0f };
	float offset[3] = { 0.0f, 0.0f, 0.0f };

	XrQuaternionf orientation, frame;
	XrVector3f along;

	if( !xr.hud.ready || !xr.hand_hud[index].valid || !xr.hand_pose[hand].grip_valid || !xr.hand_pose[hand].aim_valid )
		return false;
	sscanf( vr_hud_offset[ammo ? GL_OPENXR_HAND_HUD_AMMO : index]->string, "%f %f %f", &offset[0], &offset[1], &offset[2] );

	quad->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad->space = xr.local_space;
	quad->subImage.swapchain = xr.hud.swapchain;
	quad->subImage.imageRect.offset.x = xr.hand_hud[index].x;
	quad->subImage.imageRect.extent.width = xr.hand_hud[index].width;
	quad->subImage.imageRect.extent.height = xr.hand_hud[index].height;
	quad->size.width = metres * xr.hand_hud[index].hud_width;
	quad->size.height = metres * xr.hand_hud[index].hud_height;
	/* Axes of the aim pose (+X right, +Y up, -Z where the gun points; SteamVR's grip pose points along the
	 * handle, steeply down), positioned at the grip (palm). The quad faces +Z (back towards you); it is tilted
	 * about X to face upwards, then turned about Y towards your body (yaw after tilt keeps "up"). */
	lean.x = sinf( tilt * 0.5f );
	lean.w = cosf( tilt * 0.5f );
	turn.y = sinf( yaw * 0.5f );
	turn.w = cosf( yaw * 0.5f );
	orientation = GL_OpenXRQuatMultiply( turn, lean );
	if( hand == REF_VR_HAND_RIGHT && vr_hud_right_flip->value != 0.0f )
	{
		/* turned inwards, the panels are mirror images: the right one read backwards along the arm */
		const XrQuaternionf flip = { 0.0f, 0.0f, 1.0f, 0.0f };
		orientation = GL_OpenXRQuatMultiply( orientation, flip );
	}
	/* the weapon hand follows the gun model, which the client points vr_weapon_pitchadjust degrees off the
	 * controller (-20 = 20 degrees down; skipped there only for two-handed aim) */
	frame = xr.hand_pose[hand].aim.orientation;
	if( ammo )
	{
		const float adjust = DEG2RAD( bound( -90.0f, gEngfuncs.pfnGetCvarFloat( "vr_weapon_pitchadjust" ), 90.0f ));
		const XrQuaternionf pitch = { sinf( adjust * 0.5f ), 0.0f, 0.0f, cosf( adjust * 0.5f ) };
		frame = GL_OpenXRQuatMultiply( frame, pitch );
	}
	quad->pose.orientation = GL_OpenXRQuatMultiply( frame, orientation );
	along.x = bound( -0.5f, offset[0], 0.5f );
	along.y = bound( -0.5f, offset[1], 0.5f );
	along.z = bound( -0.5f, offset[2], 0.5f );
	along = GL_OpenXRRotateVector( frame, along );
	quad->pose.position.x = xr.hand_pose[hand].grip.position.x + along.x;
	quad->pose.position.y = xr.hand_pose[hand].grip.position.y + along.y;
	quad->pose.position.z = xr.hand_pose[hand].grip.position.z + along.z;
	if( !ammo )
		quad->pose.position.y -= GL_OpenXRGameHandDrop(); /* off hand: placed relative to the in-game flashlight */
	if( docked )
	{
		/* below the ammo panel, in its plane, with a small gap */
		const XrVector3f up = { 0.0f, 1.0f, 0.0f };
		const XrVector3f axis = GL_OpenXRRotateVector( quad->pose.orientation, up );
		const float above = xr.hand_hud[GL_OPENXR_HAND_HUD_AMMO].valid ? metres * xr.hand_hud[GL_OPENXR_HAND_HUD_AMMO].hud_height : 0.0f;
		const float shift = 0.5f * ( above + quad->size.height ) + ( above > 0.0f ? 0.005f : 0.0f );
		quad->pose.position.x -= axis.x * shift;
		quad->pose.position.y -= axis.y * shift;
		quad->pose.position.z -= axis.z * shift;
	}
	if( index == GL_OPENXR_HAND_HUD_STATUS && xr.hand_hud[index].hud_width > xr.hand_hud[index].base_width )
	{
		/* the flashlight and damage icons widen the panel to the right only: health and suit stay where they were */
		const XrVector3f right = { 1.0f, 0.0f, 0.0f };
		const XrVector3f axis = GL_OpenXRRotateVector( quad->pose.orientation, right );
		const float shift = 0.5f * metres * ( xr.hand_hud[index].hud_width - xr.hand_hud[index].base_width );
		quad->pose.position.x += axis.x * shift;
		quad->pose.position.y += axis.y * shift;
		quad->pose.position.z += axis.z * shift;
	}
	return true;
}

/* Cast the weapon hand's aim ray at the weapon grid quad (NULL: grid not shown) and tell the client which HUD pixel
 * it hits (vr_weapon_pointer_cursor "x y", empty when off the grid). */
static void GL_OpenXRUpdateWeaponPointer( const XrCompositionLayerQuad *grid )
{
	const int hand = GL_OpenXRPointerHand();
	const int p = GL_OPENXR_HAND_HUD_WEAPONS;
	const XrVector3f forward = { 0.0f, 0.0f, -1.0f }, right = { 1.0f, 0.0f, 0.0f }, up = { 0.0f, 1.0f, 0.0f };
	const XrVector3f back = { 0.0f, 0.0f, 1.0f };
	char cursor[32] = "";
	float reach = 0.6f; /* beam length when it misses the grid */

	xr.weapon_pointer.valid = xr.weapon_pointer.hit = false;
	if( grid && xr.hand_pose[hand].aim_valid )
	{
		/* along the gun model, which the client points vr_weapon_pitchadjust degrees off the controller (as the
		 * ammo panel and the gun's own laser sight do) */
		const float adjust = DEG2RAD( bound( -90.0f, gEngfuncs.pfnGetCvarFloat( "vr_weapon_pitchadjust" ), 90.0f ));
		const XrQuaternionf pitch = { sinf( adjust * 0.5f ), 0.0f, 0.0f, cosf( adjust * 0.5f ) };
		const XrVector3f o = { xr.hand_pose[hand].aim.position.x, xr.hand_pose[hand].aim.position.y - GL_OpenXRGameHandDrop(),
			xr.hand_pose[hand].aim.position.z };
		const XrVector3f d = GL_OpenXRRotateVector( GL_OpenXRQuatMultiply( xr.hand_pose[hand].aim.orientation, pitch ), forward );
		const XrVector3f n = GL_OpenXRRotateVector( grid->pose.orientation, back );
		const float facing = d.x * n.x + d.y * n.y + d.z * n.z;

		xr.weapon_pointer.valid = true;
		xr.weapon_pointer.origin = o;
		if( facing < -0.0001f )
		{
			const XrVector3f c = grid->pose.position;
			const float t = (( c.x - o.x ) * n.x + ( c.y - o.y ) * n.y + ( c.z - o.z ) * n.z ) / facing;
			if( t > 0.0f )
			{
				const XrVector3f xa = GL_OpenXRRotateVector( grid->pose.orientation, right );
				const XrVector3f ya = GL_OpenXRRotateVector( grid->pose.orientation, up );
				const XrVector3f h = { o.x + d.x * t - c.x, o.y + d.y * t - c.y, o.z + d.z * t - c.z };
				const float u = ( h.x * xa.x + h.y * xa.y + h.z * xa.z ) / grid->size.width + 0.5f;
				const float v = 0.5f - ( h.x * ya.x + h.y * ya.y + h.z * ya.z ) / grid->size.height;
				if( u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f )
				{
					xr.weapon_pointer.hit = true;
					xr.weapon_pointer.u = u;
					xr.weapon_pointer.v = v;
					const int px = (int)( u * xr.hand_hud[p].hud_width ), py = (int)( v * xr.hand_hud[p].hud_height );
					reach = t;
					/* panel position -> the row box under it -> where the client drew that */
					for( int j = 0; j < xr.hand_hud[p].box_count; ++j )
					{
						const int bx = px - xr.hand_hud[p].box[j].px, by = py - xr.hand_hud[p].box[j].py;
						if( bx >= 0 && bx < xr.hand_hud[p].box[j].w && by >= 0 && by < xr.hand_hud[p].box[j].h )
						{
							Q_snprintf( cursor, sizeof( cursor ), "%d %d",
								xr.hand_hud[p].box[j].hud_x + bx, xr.hand_hud[p].box[j].hud_y + by );
							break;
						}
					}
				}
			}
		}
		xr.weapon_pointer.end.x = o.x + d.x * reach;
		xr.weapon_pointer.end.y = o.y + d.y * reach;
		xr.weapon_pointer.end.z = o.z + d.z * reach;
		{
			/* the beam starts where the gun's laser sight does: 10 game units out along the barrel */
			const float start = Q_min( 10.0f / Q_max( 1.0f, vr_worldscale->value ), reach * 0.9f );
			xr.weapon_pointer.origin.x = o.x + d.x * start;
			xr.weapon_pointer.origin.y = o.y + d.y * start;
			xr.weapon_pointer.origin.z = o.z + d.z * start;
		}
	}
	if( Q_strcmp( cursor, xr.weapon_pointer.cursor ))
	{
		gEngfuncs.Cvar_Set( "vr_weapon_pointer_cursor", cursor );
		Q_strncpy( xr.weapon_pointer.cursor, cursor, sizeof( xr.weapon_pointer.cursor ));
	}
}

qboolean GL_OpenXRBeginUI( int width, int height )
{
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

	if( !xr.frame_begun || !xr.frame_state.shouldRender || !xr.views_valid || xr.ui_active ||
		width <= 0 || height <= 0 || xr.ui.swapchain == XR_NULL_HANDLE )
		return false;

	if( !GL_OpenXRCheck( xrAcquireSwapchainImage( xr.ui.swapchain, &acquire_info, &xr.ui.image_index ),
		"xrAcquireSwapchainImage(UI)" ))
		return false;
	xr.ui.acquired = true;
	wait_info.timeout = XR_INFINITE_DURATION;
	if( !GL_OpenXRCheck( xrWaitSwapchainImage( xr.ui.swapchain, &wait_info ), "xrWaitSwapchainImage(UI)" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return false;
	}
	xr.ui.waited = true;
	xr.ui_active = true;
	xr.ui_source_width = width;
	xr.ui_source_height = height;
	pglGetFloatv( GL_COLOR_CLEAR_VALUE, xr.ui_clear_color );
	pglBindFramebuffer( GL_FRAMEBUFFER, xr.ui.framebuffers[xr.ui.image_index] );
	pglViewport( 0, 0, xr.ui.width, xr.ui.height );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	pglClear( GL_COLOR_BUFFER_BIT );
	GL_OpenXRSetUIBlend( true );
	return true;
}

void GL_OpenXREndUI( void )
{
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	if( !xr.ui_active )
		return;

	GL_OpenXRDrawUIPointerDot();
	GL_OpenXRFixUIAlpha();
	GL_OpenXRCaptureHandHUD();
	GL_OpenXRSetUIBlend( false );
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglViewport( 0, 0, xr.ui_source_width, xr.ui_source_height );
	pglClearColor( xr.ui_clear_color[0], xr.ui_clear_color[1], xr.ui_clear_color[2], xr.ui_clear_color[3] );
	xr.ui_active = false;
	if( !GL_OpenXRCheck( xrReleaseSwapchainImage( xr.ui.swapchain, &release_info ), "xrReleaseSwapchainImage(UI)" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return;
	}
	xr.ui.acquired = false;
	xr.ui.waited = false;
	xr.ui.ready = true;
}

/* Cinema mode: the engine shows a scripted camera's view on a flat screen in the room (vr_cinema), placed like the
 * menu (GL_OpenXRPanelAnchor) at the menu panel's distance and width, and kept there for the whole scene. */
void GL_OpenXRSetCinema( qboolean show, float brightness, const vec3_t room_forward )
{
	xr.cinema.show = show && xr.cinema.image.swapchain != XR_NULL_HANDLE;
	xr.cinema.brightness = bound( 0.0f, brightness, 1.0f );
	if( room_forward )
		VectorCopy( room_forward, xr.room_forward );
	else VectorClear( xr.room_forward );
	if( !xr.cinema.show )
	{
		xr.cinema.anchored = false;
		xr.cinema.copy_valid = false; /* never repeat the last scene's image in the next one */
		return;
	}
	if( !xr.cinema.anchored && xr.views_valid )
	{
		xr.cinema.anchor = GL_OpenXRPanelAnchor();
		xr.cinema.anchored = true;
	}
}

qboolean GL_OpenXRBeginCinema( int *width, int *height )
{
	gl_openxr_eye_t *target = &xr.cinema.image;
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

	if( !xr.frame_begun || !xr.frame_state.shouldRender || !xr.views_valid || !xr.cinema.show || !xr.cinema.anchored ||
		target->swapchain == XR_NULL_HANDLE || target->acquired || xr.current_eye >= 0 || !width || !height )
		return false;
	if( !GL_OpenXRCheck( xrAcquireSwapchainImage( target->swapchain, &acquire_info, &target->image_index ),
		"xrAcquireSwapchainImage(cinema)" ))
		return false;
	target->acquired = true;
	target->waited = false;
	wait_info.timeout = XR_INFINITE_DURATION;
	if( !GL_OpenXRCheck( xrWaitSwapchainImage( target->swapchain, &wait_info ), "xrWaitSwapchainImage(cinema)" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return false;
	}
	target->waited = true;
	pglBindFramebuffer( GL_FRAMEBUFFER, target->framebuffers[target->image_index] );
	pglViewport( 0, 0, target->width, target->height );
	/* no eye is current: GetEyeFov/ApplyEyePose decline and the 3D path renders like the desktop, once */
	xr.cinema.rendering = true;
	*width = target->width;
	*height = target->height;
	return true;
}

/* Darken the bound cinema image towards black (the screen fading in or out). */
static void GL_OpenXRCinemaBrightness( void )
{
	byte rgba[4] = { 0, 0, 0, 0 };

	if( xr.cinema.brightness >= 1.0f )
		return;
	rgba[3] = (byte)( 255.0f * ( 1.0f - xr.cinema.brightness ) + 0.5f );
	GL_OpenXRDrawFade( &xr.cinema.image, rgba, false );
}

static qboolean GL_OpenXRReleaseCinema( void )
{
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	gl_openxr_eye_t *target = &xr.cinema.image;

	if( !GL_OpenXRCheck( xrReleaseSwapchainImage( target->swapchain, &release_info ), "xrReleaseSwapchainImage(cinema)" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return false;
	}
	target->acquired = false;
	target->waited = false;
	target->ready = true;
	return true;
}

void GL_OpenXREndCinema( int source_width, int source_height )
{
	gl_openxr_eye_t *target = &xr.cinema.image;

	if( !xr.cinema.rendering )
		return;
	xr.cinema.rendering = false;
	if( !target->acquired || !target->waited )
		return;

	/* keep the image: when the camera ends the screen fades out on it (the view is already back on the player) */
	if( xr.cinema.copy_fbo )
	{
		pglBindFramebuffer( GL_READ_FRAMEBUFFER, target->framebuffers[target->image_index] );
		pglBindFramebuffer( GL_DRAW_FRAMEBUFFER, xr.cinema.copy_fbo );
		pglBlitFramebuffer( 0, 0, target->width, target->height, 0, 0, target->width, target->height,
			GL_COLOR_BUFFER_BIT, GL_NEAREST );
		xr.cinema.copy_valid = true;
	}
	if( source_width > 0 && source_height > 0 )
	{
		/* desktop mirror, letterboxed */
		int w = source_width, h = source_width * (int)target->height / (int)target->width;
		GLfloat old_clear[4];

		if( h > source_height )
		{
			h = source_height;
			w = source_height * (int)target->width / (int)target->height;
		}
		while( pglGetError() != GL_NO_ERROR ) {}
		pglGetFloatv( GL_COLOR_CLEAR_VALUE, old_clear );
		pglPushAttrib( GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT );
		pglDisable( GL_SCISSOR_TEST );
		pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		pglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		pglClear( GL_COLOR_BUFFER_BIT );
		pglBindFramebuffer( GL_READ_FRAMEBUFFER, target->framebuffers[target->image_index] );
		pglBlitFramebuffer( 0, 0, target->width, target->height, ( source_width - w ) / 2, ( source_height - h ) / 2,
			( source_width + w ) / 2, ( source_height + h ) / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		pglPopAttrib();
		pglClearColor( old_clear[0], old_clear[1], old_clear[2], old_clear[3] );
		if( pglGetError() != GL_NO_ERROR && !xr.mirror_warned )
		{
			gEngfuncs.Con_Reportf( S_NOTE "OpenXR: desktop mirror blit of the cinema image failed\n" );
			xr.mirror_warned = true;
		}
	}
	pglBindFramebuffer( GL_FRAMEBUFFER, target->framebuffers[target->image_index] );
	GL_OpenXRCinemaBrightness();
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglViewport( 0, 0, source_width, source_height );
	GL_OpenXRReleaseCinema();
}

/* A frame without a new camera image (the scene just ended and the screen fades out): show the kept copy. */
static void GL_OpenXRRepeatCinema( void )
{
	gl_openxr_eye_t *target = &xr.cinema.image;
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	GLint old_viewport[4];

	if( !xr.cinema.copy_valid || target->acquired )
		return;
	if( !GL_OpenXRCheck( xrAcquireSwapchainImage( target->swapchain, &acquire_info, &target->image_index ),
		"xrAcquireSwapchainImage(cinema repeat)" ))
		return;
	target->acquired = true;
	wait_info.timeout = XR_INFINITE_DURATION;
	if( !GL_OpenXRCheck( xrWaitSwapchainImage( target->swapchain, &wait_info ), "xrWaitSwapchainImage(cinema repeat)" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return;
	}
	target->waited = true;
	pglGetIntegerv( GL_VIEWPORT, old_viewport );
	pglBindFramebuffer( GL_READ_FRAMEBUFFER, xr.cinema.copy_fbo );
	pglBindFramebuffer( GL_DRAW_FRAMEBUFFER, target->framebuffers[target->image_index] );
	pglBlitFramebuffer( 0, 0, target->width, target->height, 0, 0, target->width, target->height,
		GL_COLOR_BUFFER_BIT, GL_NEAREST );
	pglBindFramebuffer( GL_FRAMEBUFFER, target->framebuffers[target->image_index] );
	GL_OpenXRCinemaBrightness();
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglViewport( old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3] );
	GL_OpenXRReleaseCinema();
}

static qboolean GL_OpenXRBuildCinemaLayer( XrCompositionLayerQuad *quad )
{
	XrVector3f offset = { 0.0f, 0.0f, 0.0f }, rotated;
	float width;

	if( !xr.cinema.show || !xr.cinema.anchored )
		return false;
	if( !xr.cinema.image.ready )
		GL_OpenXRRepeatCinema();
	if( !xr.initialized || !xr.cinema.image.ready )
		return false; /* nothing released in this scene yet (menu up, first frame): black */

	width = bound( 0.25f, vr_ui_width->value, 8.0f );
	offset.z = -bound( 0.2f, vr_ui_distance->value, 10.0f );
	rotated = GL_OpenXRRotateVector( xr.cinema.anchor.orientation, offset );
	quad->layerFlags = 0; /* opaque */
	quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad->space = xr.local_space;
	quad->pose.orientation = xr.cinema.anchor.orientation;
	quad->pose.position.x = xr.cinema.anchor.position.x + rotated.x;
	quad->pose.position.y = xr.cinema.anchor.position.y + rotated.y;
	quad->pose.position.z = xr.cinema.anchor.position.z + rotated.z;
	quad->subImage.swapchain = xr.cinema.image.swapchain;
	quad->subImage.imageRect.extent.width = xr.cinema.image.width;
	quad->subImage.imageRect.extent.height = xr.cinema.image.height;
	quad->size.width = width;
	quad->size.height = width * (float)xr.cinema.image.height / (float)xr.cinema.image.width;
	return true;
}

/* Scope lens (Counter-Strike): the engine renders the zoomed view along the weapon into a small
 * image once per frame; it is shown as a round lens in front of the weapon hand. */
void GL_OpenXRSetScope( qboolean show, int hand )
{
	xr.scope.show = show && xr.scope.image.swapchain != XR_NULL_HANDLE && hand >= 0 && hand < REF_VR_MAX_HANDS;
	xr.scope.hand = hand;
}

qboolean GL_OpenXRBeginScope( int *width, int *height )
{
	gl_openxr_eye_t *target = &xr.scope.image;
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

	if( !xr.frame_begun || !xr.frame_state.shouldRender || !xr.views_valid || !xr.scope.show ||
		target->swapchain == XR_NULL_HANDLE || target->acquired || xr.current_eye >= 0 || !width || !height )
		return false;
	if( !GL_OpenXRCheck( xrAcquireSwapchainImage( target->swapchain, &acquire_info, &target->image_index ),
		"xrAcquireSwapchainImage(scope)" ))
		return false;
	target->acquired = true;
	target->waited = false;
	wait_info.timeout = XR_INFINITE_DURATION;
	if( !GL_OpenXRCheck( xrWaitSwapchainImage( target->swapchain, &wait_info ), "xrWaitSwapchainImage(scope)" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return false;
	}
	target->waited = true;
	pglBindFramebuffer( GL_FRAMEBUFFER, target->framebuffers[target->image_index] );
	pglViewport( 0, 0, target->width, target->height );
	xr.scope.rendering = true;
	*width = target->width;
	*height = target->height;
	return true;
}

qboolean GL_OpenXRIsRenderingScope( void )
{
	return xr.scope.rendering;
}

/* Round lens with a thin cross: alpha 1 inside the circle, 0 outside (the layer blends with alpha). */
static void GL_OpenXRDrawScopeMask( int width, int height )
{
	const int segments = 64;
	const float radius = 0.5f, line = 1.5f / (float)width;

	pglPushAttrib( GL_ALL_ATTRIB_BITS );
	pglDisable( GL_DEPTH_TEST );
	pglDisable( GL_CULL_FACE );
	pglDisable( GL_TEXTURE_2D );
	pglDisable( GL_BLEND );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_SCISSOR_TEST );
	pglDisable( GL_FOG );
	pglMatrixMode( GL_PROJECTION );
	pglPushMatrix();
	pglLoadIdentity();
	pglOrtho( -0.5f, 0.5f, -0.5f, 0.5f, -1.0f, 1.0f );
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadIdentity();
	pglViewport( 0, 0, width, height );

	/* the world pass leaves arbitrary alpha: make the inside opaque */
	pglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE );
	pglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	pglClear( GL_COLOR_BUFFER_BIT );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	/* outside the circle: transparent */
	pglColor4f( 0.0f, 0.0f, 0.0f, 0.0f );
	pglBegin( GL_TRIANGLE_STRIP );
	for( int i = 0; i <= segments; ++i )
	{
		float a = (float)i / segments * 2.0f * M_PI_F;
		pglVertex2f( cosf( a ) * radius, sinf( a ) * radius );
		pglVertex2f( cosf( a ) * 1.0f, sinf( a ) * 1.0f );
	}
	pglEnd();

	/* black ring and cross */
	pglColor4f( 0.0f, 0.0f, 0.0f, 1.0f );
	pglBegin( GL_TRIANGLE_STRIP );
	for( int i = 0; i <= segments; ++i )
	{
		float a = (float)i / segments * 2.0f * M_PI_F;
		pglVertex2f( cosf( a ) * radius * 0.94f, sinf( a ) * radius * 0.94f );
		pglVertex2f( cosf( a ) * radius, sinf( a ) * radius );
	}
	pglEnd();
	pglBegin( GL_QUADS );
	pglVertex2f( -radius, -line ); pglVertex2f( radius, -line ); pglVertex2f( radius, line ); pglVertex2f( -radius, line );
	pglVertex2f( -line, -radius ); pglVertex2f( line, -radius ); pglVertex2f( line, radius ); pglVertex2f( -line, radius );
	pglEnd();

	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglPopAttrib();
}

void GL_OpenXREndScope( int source_width, int source_height )
{
	gl_openxr_eye_t *target = &xr.scope.image;
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	if( !xr.scope.rendering )
		return;
	xr.scope.rendering = false;
	if( !target->acquired || !target->waited )
		return;
	pglBindFramebuffer( GL_FRAMEBUFFER, target->framebuffers[target->image_index] );
	GL_OpenXRDrawScopeMask( target->width, target->height );
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglViewport( 0, 0, source_width, source_height );
	if( !GL_OpenXRCheck( xrReleaseSwapchainImage( target->swapchain, &release_info ), "xrReleaseSwapchainImage(scope)" ))
	{
		GL_OpenXRShutdown();
		xr.failed = true;
		return;
	}
	target->acquired = false;
	target->waited = false;
	target->ready = true;
}

static qboolean GL_OpenXRBuildScopeLayer( XrCompositionLayerQuad *quad )
{
	XrVector3f offset, rotated;
	float size;

	if( !xr.scope.show || !xr.scope.image.ready || !xr.hand_pose[xr.scope.hand].aim_valid )
		return false;
	if( sscanf( vr_scope_offset->string, "%f %f %f", &offset.x, &offset.y, &offset.z ) != 3 )
		offset.x = 0.0f, offset.y = 0.06f, offset.z = -0.08f;
	size = bound( 0.01f, vr_scope_size->value, 0.5f );
	/* the lens faces back along the aim (a quad faces its +Z), towards the eye behind the gun */
	rotated = GL_OpenXRRotateVector( xr.hand_pose[xr.scope.hand].aim.orientation, offset );
	quad->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad->space = xr.local_space;
	quad->pose.orientation = xr.hand_pose[xr.scope.hand].aim.orientation;
	quad->pose.position.x = xr.hand_pose[xr.scope.hand].aim.position.x + rotated.x;
	quad->pose.position.y = xr.hand_pose[xr.scope.hand].aim.position.y + rotated.y;
	quad->pose.position.z = xr.hand_pose[xr.scope.hand].aim.position.z + rotated.z;
	quad->subImage.swapchain = xr.scope.image.swapchain;
	quad->subImage.imageRect.extent.width = xr.scope.image.width;
	quad->subImage.imageRect.extent.height = xr.scope.image.height;
	quad->size.width = size;
	quad->size.height = size;
	return true;
}

void GL_OpenXRFrameEnd( void )
{
	XrCompositionLayerProjectionView projection_views[REF_VR_MAX_EYES];
	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad ui = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad cinema = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *layers[5 + GL_OPENXR_HAND_HUD_COUNT];
	XrCompositionLayerQuad beam = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad scope = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad hand_hud[GL_OPENXR_HAND_HUD_COUNT];
	XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
	/* The world view and the head-locked UI quad are submitted independently: in the main menu
	 * (no map loaded) no eye is rendered, and requiring both eyes left the headset black. */
	const qboolean frame_ok = xr.frame_begun && xr.frame_state.shouldRender && xr.views_valid;
	qboolean eyes_ok = frame_ok;
	qboolean submit;
	qboolean on_cinema = false;
	uint32_t layer_count = 0;

	if( !xr.frame_begun )
		return;
	if( xr.ui_active )
		GL_OpenXREndUI();
	if( !xr.frame_begun )
		return; /* EndUI may have shut the session down after a release failure. */

	for( int i = 0; i < REF_VR_MAX_EYES; ++i )
	{
		if( xr.eyes[i].acquired && xr.eyes[i].waited )
		{
			XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			GL_OpenXRCheck( xrReleaseSwapchainImage( xr.eyes[i].swapchain, &release_info ),
				"xrReleaseSwapchainImage(interrupted eye)" );
			xr.eyes[i].acquired = false;
			xr.eyes[i].waited = false;
			eyes_ok = false;
		}
	}
	if( xr.scope.image.acquired && xr.scope.image.waited )
	{
		/* interrupted scope render */
		XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		GL_OpenXRCheck( xrReleaseSwapchainImage( xr.scope.image.swapchain, &release_info ),
			"xrReleaseSwapchainImage(interrupted scope)" );
		xr.scope.image.acquired = false;
		xr.scope.image.waited = false;
		xr.scope.rendering = false;
	}
	if( xr.cinema.image.acquired && xr.cinema.image.waited )
	{
		/* interrupted camera render */
		XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		GL_OpenXRCheck( xrReleaseSwapchainImage( xr.cinema.image.swapchain, &release_info ),
			"xrReleaseSwapchainImage(interrupted cinema)" );
		xr.cinema.image.acquired = false;
		xr.cinema.image.waited = false;
		xr.cinema.rendering = false;
	}

	for( int i = 0; i < REF_VR_MAX_EYES; ++i )
		eyes_ok = eyes_ok && xr.eyes[i].ready;

	memset( projection_views, 0, sizeof( projection_views ));
	if( eyes_ok )
	{
		projection.space = xr.local_space;
		projection.viewCount = REF_VR_MAX_EYES;
		projection.views = projection_views;
		for( int i = 0; i < REF_VR_MAX_EYES; ++i )
		{
			projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projection_views[i].pose = xr.views[i].pose;
			projection_views[i].fov = xr.views[i].fov;
			projection_views[i].subImage.swapchain = xr.eyes[i].swapchain;
			projection_views[i].subImage.imageRect.extent.width = xr.eyes[i].width;
			projection_views[i].subImage.imageRect.extent.height = xr.eyes[i].height;
		}
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&projection;
	}
	/* under the UI quad, so game text (chapter titles, game_text) stays readable over the camera view */
	if( frame_ok && GL_OpenXRBuildCinemaLayer( &cinema ))
	{
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&cinema;
		on_cinema = true;
	}
	if( !xr.frame_begun )
		return; /* the repeat may have shut the session down */
	if( frame_ok && xr.ui.ready )
	{
		{
			float width = bound( 0.25f, vr_ui_width->value, 8.0f );
			/* GL_OpenXRFixUIAlpha leaves unpremultiplied colour */
			ui.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
				XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
			ui.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			ui.subImage.swapchain = xr.ui.swapchain;
			ui.subImage.imageRect.extent.width = xr.ui.width;
			ui.subImage.imageRect.extent.height = xr.ui.height;
			{
				XrVector3f offset;
				/* Game text during a camera scene (the c5a1 end credits, chapter titles) goes on the cinema
				 * screen, where the desktop draws it over the picture, 1 cm in front of it; head-locked it
				 * hung off to one side (vr_ui_offset_x/y) and slid across the room-fixed screen. Same size as
				 * the screen: both images are 16:9. */
				const qboolean ui_on_cinema = on_cinema && !xr.ui_world_active;
				offset.x = xr.ui_world_active || ui_on_cinema ? 0.0f : bound( -1.0f, vr_ui_offset_x->value, 1.0f );
				offset.y = xr.ui_world_active || ui_on_cinema ? 0.0f : bound( -1.0f, vr_ui_offset_y->value, 1.0f );
				offset.z = -bound( 0.2f, vr_ui_distance->value, 10.0f );
				if( ui_on_cinema )
				{
					XrVector3f rotated;
					offset.z += 0.01f;
					rotated = GL_OpenXRRotateVector( xr.cinema.anchor.orientation, offset );
					ui.space = xr.local_space;
					ui.pose.orientation = xr.cinema.anchor.orientation;
					ui.pose.position.x = xr.cinema.anchor.position.x + rotated.x;
					ui.pose.position.y = xr.cinema.anchor.position.y + rotated.y;
					ui.pose.position.z = xr.cinema.anchor.position.z + rotated.z;
				}
				else if( xr.ui_world_active )
				{
					/* menu/console: hangs in the room where it opened */
					XrVector3f rotated = GL_OpenXRRotateVector( xr.ui_anchor.orientation, offset );
					ui.space = xr.local_space;
					ui.pose.orientation = xr.ui_anchor.orientation;
					ui.pose.position.x = xr.ui_anchor.position.x + rotated.x;
					ui.pose.position.y = xr.ui_anchor.position.y + rotated.y;
					ui.pose.position.z = xr.ui_anchor.position.z + rotated.z;
				}
				else
				{
					ui.space = xr.head_space;
					ui.pose.orientation.w = 1.0f;
					ui.pose.position = offset;
				}
			}
			ui.size.width = width;
			/* on the cinema screen the text keeps the image's own aspect, like the screen (vr_ui_height is the HUD's) */
			ui.size.height = on_cinema && !xr.ui_world_active ? width * (float)xr.ui.height / (float)xr.ui.width :
				GL_OpenXRUIHeight( width );
			layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&ui;
		}
	}
	{
		qboolean grid = false;
		for( int i = 0; i < GL_OPENXR_HAND_HUD_COUNT; ++i )
		{
			memset( &hand_hud[i], 0, sizeof( hand_hud[i] ));
			hand_hud[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			if( frame_ok && eyes_ok && GL_OpenXRBuildHandHUDLayer( i, &hand_hud[i] ))
			{
				layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&hand_hud[i];
				grid = grid || i == GL_OPENXR_HAND_HUD_WEAPONS;
			}
		}
		GL_OpenXRUpdateWeaponPointer( grid ? &hand_hud[GL_OPENXR_HAND_HUD_WEAPONS] : NULL );
	}
	if( frame_ok && eyes_ok && GL_OpenXRBuildScopeLayer( &scope ))
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&scope;
	if( frame_ok && layer_count > 0 && GL_OpenXRBuildBeamLayer( &beam ))
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&beam;
	submit = layer_count > 0;

	end_info.displayTime = xr.frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = submit ? layer_count : 0;
	end_info.layers = submit ? layers : NULL;
	if( GL_OpenXRCheck( xrEndFrame( xr.session, &end_info ), "xrEndFrame" ) && submit )
	{
		if( eyes_ok && !xr.submitted_once )
		{
			xr.submitted_once = true;
			gEngfuncs.Con_Printf( "OpenXR: first stereo frame submitted\n" );
		}
		if( xr.ui.ready && !xr.ui_submitted_once )
		{
			xr.ui_submitted_once = true;
			gEngfuncs.Con_Printf( "OpenXR: first head-relative UI layer submitted\n" );
		}
	}
	xr.frame_begun = false;
	xr.current_eye = -1;
}

qboolean GL_OpenXRHaptic( int hand, float duration, float frequency, float amplitude )
{
	XrHapticActionInfo action_info = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	float clamped_duration;
	double now;
	qboolean succeeded;

	if( !xr.initialized || !xr.session_running || hand < 0 || hand >= REF_VR_MAX_HANDS ||
		xr.actions.haptic == XR_NULL_HANDLE )
		return false;

	action_info.action = xr.actions.haptic;
	action_info.subactionPath = xr.actions.hand_paths[hand];
	if( duration == 0.0f || amplitude <= 0.0f )
	{
		succeeded = GL_OpenXRCheck( xrStopHapticFeedback( xr.session, &action_info ),
			"xrStopHapticFeedback" );
		if( succeeded )
		{
			xr.haptic_until[hand] = 0.0;
			xr.haptic_sustained[hand] = false;
		}
		return succeeded;
	}

	/* Lambda1VR lets the first finite pulse finish and protects sustained
	 * effects until their explicit stop event. OpenXR owns pulse playback, so
	 * retain only that admission policy rather than re-submitting every frame. */
	now = gEngfuncs.pfnTime();
	if( xr.haptic_sustained[hand] || xr.haptic_until[hand] > now )
		return true;

	/* Lambda1VR uses a negative duration for sustained effects such as the
	 * Egon beam, followed by an explicit zero-duration stop event. */
	if( duration < 0.0f )
		vibration.duration = XR_INFINITE_DURATION;
	else
	{
		clamped_duration = bound( 0.0f, duration, 10.0f );
		vibration.duration = (XrDuration)( clamped_duration * 1000000000.0f );
	}
	vibration.frequency = bound( 0.0f, frequency, 320.0f );
	vibration.amplitude = bound( 0.0f, amplitude, 1.0f );
	succeeded = GL_OpenXRCheck( xrApplyHapticFeedback( xr.session, &action_info,
		(const XrHapticBaseHeader *)&vibration ), "xrApplyHapticFeedback" );
	if( succeeded )
	{
		static qboolean logged;
		if( !logged )
		{
			gEngfuncs.Con_Printf( "OpenXR: first haptic pulse accepted (hand %d, %.2f s, amplitude %.2f)\n",
				hand, duration, vibration.amplitude );
			logged = true;
		}
		xr.haptic_sustained[hand] = duration < 0.0f;
		xr.haptic_until[hand] = duration < 0.0f ? 0.0 : now + clamped_duration;
	}
	return succeeded;
}

qboolean GL_OpenXRGetEyeFov( float fov[4] )
{
	if( !fov || xr.current_eye < 0 || xr.current_eye >= REF_VR_MAX_EYES || !xr.views_valid )
		return false;

	fov[0] = xr.views[xr.current_eye].fov.angleLeft;
	fov[1] = xr.views[xr.current_eye].fov.angleRight;
	fov[2] = xr.views[xr.current_eye].fov.angleDown;
	fov[3] = xr.views[xr.current_eye].fov.angleUp;
	return true;
}

qboolean GL_OpenXRIsSecondaryEye( void )
{
	return xr.current_eye == 1;
}

qboolean GL_OpenXRIsRenderingEye( void )
{
	return xr.current_eye >= 0 && xr.current_eye < REF_VR_MAX_EYES;
}

void GL_OpenXRApplyEyePose( ref_viewpass_t *rvp )
{
	XrPosef relative;
	ref_vr_pose_t eye;
	vec3_t head_forward, head_right, head_up;
	vec3_t eye_forward, eye_right, eye_up;
	vec3_t composed_forward, composed_right, composed_up;

	if( !rvp || xr.current_eye < 0 || xr.current_eye >= REF_VR_MAX_EYES || !xr.views_valid )
		return;

	relative = GL_OpenXRPoseMultiply( GL_OpenXRPoseInverse( xr.head_pose ), xr.views[xr.current_eye].pose );
	GL_OpenXRFillPose( &relative, &eye );
	AngleVectors( rvp->viewangles, head_forward, head_right, head_up );
	VectorMA( rvp->vieworigin, eye.position[0] * vr_worldscale->value, head_forward, rvp->vieworigin );
	VectorMA( rvp->vieworigin, -eye.position[1] * vr_worldscale->value, head_right, rvp->vieworigin );
	VectorMA( rvp->vieworigin, eye.position[2] * vr_worldscale->value, head_up, rvp->vieworigin );

	VectorCopy( eye.forward, eye_forward );
	VectorCopy( eye.right, eye_right );
	VectorCopy( eye.up, eye_up );
	VectorScale( head_forward, eye_forward[0], composed_forward );
	VectorMA( composed_forward, -eye_forward[1], head_right, composed_forward );
	VectorMA( composed_forward, eye_forward[2], head_up, composed_forward );
	VectorScale( head_forward, eye_right[0], composed_right );
	VectorMA( composed_right, -eye_right[1], head_right, composed_right );
	VectorMA( composed_right, eye_right[2], head_up, composed_right );
	VectorScale( head_forward, eye_up[0], composed_up );
	VectorMA( composed_up, -eye_up[1], head_right, composed_up );
	VectorMA( composed_up, eye_up[2], head_up, composed_up );
	VectorsAngles( composed_forward, composed_right, composed_up, rvp->viewangles );
}

#endif // XASH_OPENXR
