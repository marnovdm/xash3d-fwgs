/*
vr_client.c - renderer-independent client VR frame policy
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "common.h"
#include "client.h"
#include "input.h"
#include "keydefs.h"
#include "vr_client.h"
#include "vr_game.h"
#include "vr_cs.h"

static CVAR_DEFINE_AUTO( vr_enable, "0", FCVAR_ARCHIVE, "enable desktop OpenXR rendering" );
static CVAR_DEFINE_AUTO( vr_worldscale, "40", FCVAR_ARCHIVE, "game units per physical metre" );
static CVAR_DEFINE_AUTO( vr_control_scheme, "0", FCVAR_ARCHIVE, "Lambda1VR control scheme; values >= 10 use the left weapon hand" );
static CVAR_DEFINE_AUTO( vr_comfort_moving, "0", 0, "whether VR locomotion is active for comfort masking" );
static CVAR_DEFINE_AUTO( vr_turn_angle, "45", FCVAR_ARCHIVE, "snap-turn angle, or smooth-turn speed basis" );
static CVAR_DEFINE_AUTO( vr_smoothturn, "1", FCVAR_ARCHIVE, "use continuous turning instead of snap turning" );
static CVAR_DEFINE_AUTO( vr_walkdirection, "1", FCVAR_ARCHIVE, "movement direction: 0 off-hand, 1 head" );
static CVAR_DEFINE_AUTO( vr_swap_jumpduck, "1", FCVAR_ARCHIVE, "1: primary button (Cross/A) jumps, secondary (Circle/B) ducks; 0: the reverse (original Lambda1VR layout)" );
static CVAR_DEFINE_AUTO( vr_weapon_stick_threshold, "0.85", FCVAR_ARCHIVE, "stick deflection (0-1) that opens/steps the weapon menu; must also be mostly vertical" );
static CVAR_DEFINE_AUTO( vr_ladder_aim, "0", FCVAR_ARCHIVE, "ladder climb direction: 0 look up/down (head), 1 point the off-hand controller" );
static CVAR_DEFINE_AUTO( vr_weapon_stick_up_next, "0", FCVAR_ARCHIVE, "weapon menu stick direction: 0 up = previous weapon, down = next (Lambda1VR); 1 up = next" );
static CVAR_DEFINE_AUTO( vr_weapon_stick_menu, "0", FCVAR_ARCHIVE, "pushing the weapon-hand stick up/down opens the HUD weapon menu; 0 = off (use the Triangle weapon grid)" );
static CVAR_DEFINE_AUTO( vr_weapon_stick_hold, "0.15", FCVAR_ARCHIVE, "seconds the stick must be held up/down to OPEN the weapon menu (steps inside it are immediate)" );
static CVAR_DEFINE_AUTO( vr_weapon_pointer, "1", FCVAR_ARCHIVE, "left Triangle (Y) shows the weapon grid on the off hand while held: point at a weapon with the weapon hand and release to switch; 0 = Triangle is the previous weapon" );
static CVAR_DEFINE_AUTO( vr_laser_doubleclick, "0.35", FCVAR_ARCHIVE, "max seconds between the two off-hand stick clicks that cycle the laser sight (or scope smoothing); one click is use" );
static CVAR_DEFINE_AUTO( vr_notify, "0", FCVAR_ARCHIVE, "show console notify lines (top left) over the VR HUD" );
static CVAR_DEFINE_AUTO( vr_reloadtimeoutms, "200", FCVAR_ARCHIVE, "maximum squeeze tap duration used for reload" );
static CVAR_DEFINE_AUTO( vr_enable_crouching, "0", FCVAR_ARCHIVE,
	"physical crouch standing-height multiplier; active from 0 to 0.98 (0 = off; 0.85 = crouch below 85% of your height)" );
static CVAR_DEFINE_AUTO( vr_crouch_threshold, "0.32", FCVAR_ARCHIVE,
	"deprecated; physical crouch uses vr_enable_crouching standing-height multiplier" );
static CVAR_DEFINE_AUTO( vr_positional_factor, "1", FCVAR_ARCHIVE, "room-scale movement multiplier (vr_lean 0 only)" );
static CVAR_DEFINE_AUTO( vr_lean, "1", FCVAR_ARCHIVE,
	"1 = moving the head sideways moves the view (lean/peek), the body only moves with the stick; 0 = head motion walks the body (room-scale)" );
static CVAR_DEFINE_AUTO( vr_lean_radius, "0.75", FCVAR_ARCHIVE, "vr_lean: metres the view may move from the body; beyond it the body point follows the head" );
static CVAR_DEFINE_AUTO( vr_scope_lens, "1", FCVAR_ARCHIVE,
	"zoomed weapons (crossbow, sniper rifle) show the zoom on a round lens in front of the gun; 0 = darkened view" );
static CVAR_DEFINE_AUTO( vr_lean_resume_center, "1", FCVAR_ARCHIVE,
	"vr_lean: put the body point back under the head (position only) when the game resumes from the menu or a load" );
static CVAR_DEFINE_AUTO( vr_quick_crouchjump, "0", FCVAR_ARCHIVE, "double-tap jump to perform a crouch jump" );
static CVAR_DEFINE_AUTO( vr_gesture_triggered_use, "0", FCVAR_ARCHIVE, "reach away from the body to use with the off hand (fires mid-reach; touch use replaced it)" );
static CVAR_DEFINE_AUTO( vr_use_gesture_boundary, "0.25", FCVAR_ARCHIVE, "horizontal use-gesture boundary in metres" );
static CVAR_DEFINE_AUTO( vr_controller_tracking_haptic, "1", FCVAR_ARCHIVE, "haptic feedback for tracked interaction boundaries" );
static CVAR_DEFINE_AUTO( vr_backpack_weapon, "weapon_crowbar", FCVAR_ARCHIVE, "weapon selected by the dominant-hand backpack gesture" );
static CVAR_DEFINE_AUTO( vr_lasersight, "1", FCVAR_ARCHIVE, "laser sight mode" );
static CVAR_DEFINE_AUTO( vr_mirror_weapons, "0", FCVAR_ARCHIVE, "mirror weapon viewmodels for left-handed use" );
static CVAR_DEFINE_AUTO( vr_weapon_backface_culling, "0", FCVAR_ARCHIVE, "enable back-face culling on weapon viewmodels" );
static CVAR_DEFINE_AUTO( vr_height_adjust, "0.1", FCVAR_ARCHIVE, "additional VR eye height in metres" );
static CVAR_DEFINE_AUTO( vr_headtorch, "0", FCVAR_ARCHIVE, "attach the flashlight beam to the HMD" );
static CVAR_DEFINE_AUTO( vr_reversetorch, "0", FCVAR_ARCHIVE, "reverse the tracked flashlight direction" );
static CVAR_DEFINE_AUTO( vr_flashlight_head, "-1", FCVAR_ARCHIVE, "flashlight button only works with the hand next to the head (1), anywhere (0), or -1 = only in Opposing Force (night vision goggles)" );
static CVAR_DEFINE_AUTO( vr_cinema, "1", FCVAR_ARCHIVE, "show scripted camera views (trigger_camera) on a flat screen in the room; 0 = all around you" );
static CVAR_DEFINE_AUTO( vr_cinema_fade, "0.2", FCVAR_ARCHIVE, "seconds the cinema screen and the world take to fade in/out" );

static ref_vr_frame_t vr_frame;
static ref_vr_pose_t vr_center;
static qboolean vr_frame_begun;
static qboolean vr_ui_trigger_blocked;
static qboolean vr_center_valid;
static ref_vr_pose_t vr_previous_head;
static qboolean vr_previous_head_valid;
static qboolean vr_game_was_paused; /* menu/console/loading last frame, see vr_lean_resume_center */
static vr_usercmd_sidecar_t vr_last_pose; /* the gun pose the game DLL sent last (two-handed aim included) */
static qboolean vr_last_pose_valid;
static vec3_t vr_head_delta;
static uint32_t vr_previous_buttons[REF_VR_MAX_HANDS];
static qboolean vr_turn_latched;
static qboolean vr_weapon_latched;
static qboolean vr_reload_pulse;
static double vr_squeeze_time;
static qboolean vr_ui_pressed;
static qboolean vr_ui_cursor_valid;
static float vr_ui_cursor[2];
static qboolean vr_weapon_in_backpack;
static qboolean vr_offhand_in_backpack;
static qboolean vr_backpack_weapon_active;
static qboolean vr_scoreboard_active;
static qboolean vr_offhand_use_active;
static qboolean vr_jump_held;
static qboolean vr_quick_crouch;
static qboolean vr_physical_crouched;
static void CL_VRBodyCenter( ref_vr_pose_t *center ); /* vr_center, lowered during a physical crouch */
static double vr_last_jump_press;
static qboolean vr_selecting_weapon;
static qboolean vr_confirm_attack_release;
static qboolean vr_stick_selecting; /* stick opened the HUD weapon menu (schemes 0/2) */
static qboolean vr_weapon_trigger_down;
static qboolean vr_offhand_stick_chord; /* both sticks down during this off-hand click: menu chord */
static double vr_offhand_stick_since; /* when the current off-hand click went down, 0 when up */
static double vr_offhand_stick_last; /* press time of the previous plain off-hand click, 0 = none */
static qboolean vr_offhand_stick_second; /* the current click came within vr_laser_doubleclick of the last */
static double vr_screen_button_since; /* single player: when Triangle (Y) went down outside the backpack with the grid opened, 0 = not held */
static qboolean vr_weapon_pointer_open; /* the hold opened the client's pointer weapon grid */
static int vr_weapon_scroll_count;
static qboolean vr_scope_zoom_latched;
static qboolean vr_menu_active;
static qboolean vr_one_controller_shifted;
static qboolean vr_one_controller_squeeze_held;
static qboolean vr_one_controller_jump_mode;
static qboolean vr_one_controller_running;
static qboolean vr_one_controller_duck_toggled;
static double vr_one_controller_jump_time;
static float vr_one_controller_stick[4][2];
static vec3_t vr_body_view_origin;
static float vr_body_view_yaw;
static qboolean vr_body_view_valid;
static struct
{
	qboolean camera;    /* a scripted camera has the view this frame */
	qboolean showing;   /* the screen is up: camera scene, or fading out after one */
	float brightness;   /* screen, 0..1 */
	float world_black;  /* black over the world after a scene, 1..0 */
	double last_time;
} vr_cinema_state;

static void CL_VRTransformVector( const vec3_t in, const ref_vr_pose_t *origin, vec3_t out );

/* Standing eye height assumed for physical crouch: OpenXR LOCAL space has its origin at
 * head height, not on the floor, so only the drop below the recentre height is known. */
#define VR_ASSUMED_STANDING_HEIGHT 1.7f

/* The recentre reference keeps only the head's yaw. Keeping its pitch/roll would tilt the
 * whole world for the session whenever the head was not level at the moment of recentring. */
static void CL_VRFlattenPose( ref_vr_pose_t *pose )
{
	vec3_t forward;
	float length;

	VectorSet( forward, pose->forward[0], pose->forward[1], 0.0f );
	length = VectorNormalizeLength( forward );
	if( length < 0.1f ) /* looking straight up or down: take the heading from the right vector */
	{
		VectorSet( forward, -pose->right[1], pose->right[0], 0.0f );
		VectorNormalize( forward );
	}
	VectorCopy( forward, pose->forward );
	VectorSet( pose->right, forward[1], -forward[0], 0.0f );
	VectorSet( pose->up, 0.0f, 0.0f, 1.0f );
}

static void CL_VRRelativePose( const ref_vr_pose_t *pose, const ref_vr_pose_t *origin, ref_vr_pose_t *out );

/* Debug: where a tilted horizon could come from (recentre reference, raw head, body view angles). */
static void CL_VRPoseInfo_f( void )
{
	vec3_t angles;
	ref_vr_pose_t relative;

	Con_Printf( "cl.viewangles (body) %.1f %.1f %.1f\n", cl.viewangles[PITCH], cl.viewangles[YAW], cl.viewangles[ROLL] );
	if( !vr_center_valid || !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
	{
		Con_Printf( "no valid VR centre/head\n" );
		return;
	}
	VectorsAngles( vr_center.forward, vr_center.right, vr_center.up, angles );
	Con_Printf( "centre angles %.1f %.1f %.1f pos %.2f %.2f %.2f\n", angles[PITCH], angles[YAW], angles[ROLL],
		vr_center.position[0], vr_center.position[1], vr_center.position[2] );
	VectorsAngles( vr_frame.head.forward, vr_frame.head.right, vr_frame.head.up, angles );
	Con_Printf( "raw head (LOCAL space) %.1f %.1f %.1f pos %.2f %.2f %.2f\n", angles[PITCH], angles[YAW], angles[ROLL],
		vr_frame.head.position[0], vr_frame.head.position[1], vr_frame.head.position[2] );
	CL_VRRelativePose( &vr_frame.head, &vr_center, &relative );
	VectorsAngles( relative.forward, relative.right, relative.up, angles );
	Con_Printf( "head rel. to centre %.1f %.1f %.1f\n", angles[PITCH], angles[YAW], angles[ROLL] );
	if( vr_last_pose_valid )
		Con_Printf( "sidecar gun (view angles) %.1f %.1f %.1f, offset %.1f %.1f %.1f; scope lens %d\n",
			-vr_last_pose.weapon_angles[0] / 128.0f, vr_last_pose.weapon_angles[1] / 128.0f,
			vr_last_pose.weapon_angles[2] / 128.0f, vr_last_pose.weapon_position[0] / 8.0f,
			vr_last_pose.weapon_position[1] / 8.0f, vr_last_pose.weapon_position[2] / 8.0f, CL_VRScopeLensActive( ));
}

/* Debug: pulse both controllers so haptics can be tested without game events. */
static void CL_VRTestHaptic_f( void )
{
	float amplitude = Cmd_Argc() > 1 ? Q_atof( Cmd_Argv( 1 )) : 1.0f;
	float frequency = Cmd_Argc() > 2 ? Q_atof( Cmd_Argv( 2 )) : 0.0f;

	for( int hand = 0; hand < REF_VR_MAX_HANDS; ++hand )
	{
		/* stop first: a previous pulse would otherwise make this one a no-op */
		CL_VRHaptic( hand, 0.0f, 0.0f, 0.0f );
		Con_Printf( "vr_testhaptic: hand %d amplitude %.2f frequency %.0f -> %s\n", hand, amplitude, frequency,
			CL_VRHaptic( hand, 0.5f, frequency, amplitude ) ? "accepted" : "FAILED" );
	}
}

static void CL_VRRecenter_f( void )
{
	vr_center_valid = false; /* the next valid head pose becomes the new reference */
}

void CL_VRRegisterCvars( void )
{
	Cvar_RegisterVariable( &vr_enable );
	Cvar_RegisterVariable( &vr_worldscale );
	Cvar_RegisterVariable( &vr_control_scheme );
	Cvar_RegisterVariable( &vr_comfort_moving );
	Cvar_RegisterVariable( &vr_turn_angle );
	Cvar_RegisterVariable( &vr_smoothturn );
	Cvar_RegisterVariable( &vr_walkdirection );
	Cvar_RegisterVariable( &vr_reloadtimeoutms );
	Cvar_RegisterVariable( &vr_enable_crouching );
	Cvar_RegisterVariable( &vr_crouch_threshold );
	Cvar_RegisterVariable( &vr_positional_factor );
	Cvar_RegisterVariable( &vr_lean );
	Cvar_RegisterVariable( &vr_lean_radius );
	Cvar_RegisterVariable( &vr_lean_resume_center );
	Cvar_RegisterVariable( &vr_scope_lens );
	Cvar_RegisterVariable( &vr_quick_crouchjump );
	Cvar_RegisterVariable( &vr_gesture_triggered_use );
	Cvar_RegisterVariable( &vr_use_gesture_boundary );
	Cvar_RegisterVariable( &vr_controller_tracking_haptic );
	Cvar_RegisterVariable( &vr_backpack_weapon );
	Cvar_RegisterVariable( &vr_lasersight );
	Cvar_RegisterVariable( &vr_mirror_weapons );
	Cvar_RegisterVariable( &vr_weapon_backface_culling );
	Cvar_RegisterVariable( &vr_height_adjust );
	Cvar_RegisterVariable( &vr_swap_jumpduck );
	Cvar_RegisterVariable( &vr_ladder_aim );
	Cvar_RegisterVariable( &vr_weapon_stick_hold );
	Cvar_RegisterVariable( &vr_weapon_stick_menu );
	Cvar_RegisterVariable( &vr_weapon_stick_up_next );
	Cvar_RegisterVariable( &vr_laser_doubleclick );
	Cvar_RegisterVariable( &vr_weapon_pointer );
	Cvar_RegisterVariable( &vr_notify );
	Cvar_RegisterVariable( &vr_weapon_stick_threshold );
	Cvar_RegisterVariable( &vr_headtorch );
	Cvar_RegisterVariable( &vr_reversetorch );
	Cvar_RegisterVariable( &vr_flashlight_head );
	Cvar_RegisterVariable( &vr_cinema );
	Cvar_RegisterVariable( &vr_cinema_fade );
	CL_VRCSRegisterCvars();
	Cmd_AddCommand( "vr_poseinfo", CL_VRPoseInfo_f, "print VR head/centre/body angles (tilt debugging)" );
	Cmd_AddCommand( "vr_testhaptic", CL_VRTestHaptic_f, "pulse both controllers for 0.5 s: vr_testhaptic [amplitude 0-1] [frequency Hz]" );
	Cmd_AddCommand( "vr_recenter", CL_VRRecenter_f, "make the current head position and heading the VR reference" );
}

static qboolean CL_VRHandPosition( int hand, vec3_t position )
{
	const ref_vr_hand_t *source = &vr_frame.hands[hand];

	if( FBitSet( source->flags, REF_VR_HAND_GRIP_VALID ))
	{
		VectorCopy( source->grip.position, position );
		return true;
	}
	if( FBitSet( source->flags, REF_VR_HAND_AIM_VALID ))
	{
		VectorCopy( source->aim.position, position );
		return true;
	}
	return false;
}

static qboolean CL_VRIsBackpack( int hand )
{
	vec3_t position, delta, local;
	float horizontal;

	if( !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ) || !CL_VRHandPosition( hand, position ))
		return false;
	VectorSubtract( position, vr_frame.head.position, delta );
	if( VectorLength( delta ) > 0.4f )
		return false;
	CL_VRTransformVector( delta, &vr_frame.head, local );
	horizontal = sqrt( local[0] * local[0] + local[1] * local[1] );
	return horizontal > 0.001f && local[0] < -0.5f * horizontal;
}

/* Hand beside the head (at the ear, as when switching on head-worn goggles): within 25 cm of the head and
 * more to the side than in front or behind, so it never overlaps the backpack zone behind the head. */
static qboolean CL_VRIsAtHead( int hand )
{
	vec3_t position, delta, local;

	if( !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ) || !CL_VRHandPosition( hand, position ))
		return false;
	VectorSubtract( position, vr_frame.head.position, delta );
	if( VectorLength( delta ) > 0.25f )
		return false;
	CL_VRTransformVector( delta, &vr_frame.head, local );
	return fabs( local[1] ) > fabs( local[0] );
}

/* Opposing Force has no crowbar in single player: its melee weapon is the pipe wrench. A setting still at the
 * crowbar default grabs that instead. */
static const char *CL_VRBackpackWeapon( void )
{
	if( !Q_stricmp( vr_backpack_weapon.string, "weapon_crowbar" ) && !Q_stricmp( GI->gamefolder, "gearbox" ))
		return "weapon_pipewrench";
	return vr_backpack_weapon.string;
}

static qboolean CL_VRFlashlightNeedsHead( void )
{
	if( vr_flashlight_head.value < 0.0f )
		return !Q_stricmp( GI->gamefolder, "gearbox" );
	return vr_flashlight_head.value != 0.0f;
}

static qboolean CL_VRHandSqueezePressed( const ref_vr_hand_t *hand )
{
	return hand && FBitSet( hand->flags, REF_VR_HAND_SQUEEZE_PRESSED );
}

static qboolean CL_VRPhysicalCrouchState( qboolean latched, float multiplier, float standing_height,
	float head_height )
{
	if( multiplier <= 0.0f || multiplier >= 0.98f )
		return false;
	if( !latched )
		return head_height < standing_height * multiplier;
	return head_height <= standing_height * ( multiplier + 0.02f );
}

static void CL_VRResetPhysicalCrouch( void )
{
	vr_physical_crouched = false;
}

static void CL_VRUpdatePhysicalCrouch( void )
{
	if( !vr_center_valid || !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
	{
		CL_VRResetPhysicalCrouch();
		return;
	}
	vr_physical_crouched = CL_VRPhysicalCrouchState( vr_physical_crouched, vr_enable_crouching.value,
		VR_ASSUMED_STANDING_HEIGHT,
		VR_ASSUMED_STANDING_HEIGHT - ( vr_center.position[2] - vr_frame.head.position[2] ));
}

/* Lean: the head is free to move horizontally up to radius (metres) from the body point (the
 * recentre position); further out the body point is pulled after the head by the excess only. */
static void CL_VRLeanLeash( vec3_t center, const vec3_t head, float radius )
{
	float dx = head[0] - center[0], dy = head[1] - center[1];
	float distance = sqrt( dx * dx + dy * dy );

	if( radius < 0.0f ) radius = 0.0f;
	if( distance <= radius )
		return;
	center[0] += dx * ( distance - radius ) / distance;
	center[1] += dy * ( distance - radius ) / distance;
}

/* Counter-Strike places its hands from the camera and keeps the room-scale walk */
static qboolean CL_VRLeanActive( void )
{
	return vr_lean.value != 0.0f && !CL_VRCSMode();
}

static qboolean CL_VRUseGesture( int hand )
{
	vec3_t position, delta;

	if( !vr_gesture_triggered_use.value || !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ) ||
		!CL_VRHandPosition( hand, position ))
		return false;
	VectorSubtract( position, vr_frame.head.position, delta );
	return sqrt( delta[0] * delta[0] + delta[1] * delta[1] ) > vr_use_gesture_boundary.value;
}

static void CL_VRCommand( const char *command )
{
	char buffer[256];
	Q_snprintf( buffer, sizeof( buffer ), "%s\n", command );
	Cbuf_AddText( buffer );
}

static qboolean CL_VRUIActive( void )
{
	/* GoldSrc VGUI dialogs keep key_game while requesting a visible cursor.
	 * Reuse the same cursor ownership that disables desktop mouse look. */
	return cls.key_dest != key_game || host.mouse_visible;
}

static void CL_VRUpdateUITrigger( float trigger, qboolean ui_active )
{
	/* Called only with a focused, valid action sample. Focus loss may release
	 * a VGUI button and close its dialog, but is not a physical trigger release. */
	vr_ui_trigger_blocked = trigger >= 0.55f && ( vr_ui_trigger_blocked || ui_active );
}

static void CL_VRResetUIPointer( void )
{
	if( vr_ui_pressed && vr_ui_cursor_valid )
		IN_TouchEvent( event_up, 0, vr_ui_cursor[0], vr_ui_cursor[1], 0.0f, 0.0f );
	vr_ui_pressed = false;
	vr_ui_cursor_valid = false;
}

/* Close the pointer weapon grid without switching (menu opened, focus lost) and forget a held Triangle. */
static void CL_VRCancelWeaponPointer( void )
{
	if( vr_weapon_pointer_open )
		Cbuf_AddText( "-vr_weaponpointer cancel\n" );
	vr_weapon_pointer_open = false;
	vr_screen_button_since = 0.0;
}

static void CL_VRResetInputState( qboolean release_menu )
{
	CL_VRResetUIPointer();
	CL_VRCSReleaseInput();
	Cvar_SetValue( vr_comfort_moving.name, 0.0f );
	if( release_menu )
	{
		if( vr_menu_active )
			Key_Event( K_ESCAPE, false );
		if( vr_scoreboard_active ) Cbuf_AddText( "-showscores\n" );
		if( vr_offhand_use_active ) Cbuf_AddText( "-use2\n" );
	}

	memset( vr_previous_buttons, 0, sizeof( vr_previous_buttons ));
	vr_turn_latched = false;
	vr_weapon_latched = false;
	vr_reload_pulse = false;
	vr_squeeze_time = 0.0;
	vr_weapon_in_backpack = false;
	vr_offhand_in_backpack = false;
	vr_backpack_weapon_active = false;
	vr_scoreboard_active = false;
	vr_offhand_use_active = false;
	vr_jump_held = false;
	vr_quick_crouch = false;
	CL_VRResetPhysicalCrouch();
	vr_last_jump_press = 0.0;
	vr_selecting_weapon = false;
	vr_confirm_attack_release = false;
	vr_stick_selecting = false;
	CL_VRCancelWeaponPointer();
	vr_weapon_trigger_down = false;
	vr_offhand_stick_chord = false;
	vr_offhand_stick_since = 0.0;
	vr_offhand_stick_last = 0.0;
	vr_offhand_stick_second = false;
	vr_weapon_scroll_count = 0;
	vr_scope_zoom_latched = false;
	vr_menu_active = false;
	vr_one_controller_shifted = false;
	vr_one_controller_squeeze_held = false;
	vr_one_controller_jump_mode = false;
	vr_one_controller_running = false;
	vr_one_controller_duck_toggled = false;
	vr_one_controller_jump_time = 0.0;
	memset( vr_one_controller_stick, 0, sizeof( vr_one_controller_stick ));
	vr_body_view_valid = false;
}

static void CL_VRTransformVector( const vec3_t in, const ref_vr_pose_t *origin, vec3_t out )
{
	out[0] = DotProduct( in, origin->forward );
	out[1] = -DotProduct( in, origin->right );
	out[2] = DotProduct( in, origin->up );
}

static void CL_VROneControllerInput( int dominant, int offhand, uint32_t old_dominant_buttons,
	uint32_t old_offhand_buttons )
{
	const ref_vr_hand_t *weapon = &vr_frame.hands[dominant];
	const ref_vr_hand_t *other = &vr_frame.hands[offhand];
	qboolean squeeze = CL_VRHandSqueezePressed( weapon );
	qboolean stick = FBitSet( weapon->buttons, REF_VR_BUTTON_STICK );
	qboolean old_stick = FBitSet( old_dominant_buttons, REF_VR_BUTTON_STICK );

	/* VrInputOne.c uses a short squeeze for reload and its held duration as the
	 * one-controller shift modifier. The shared OpenXR squeeze action supplies
	 * that state identically for either dominant hand. */
	if( squeeze != vr_one_controller_squeeze_held )
	{
		vr_one_controller_squeeze_held = squeeze;
		if( squeeze )
		{
			vr_squeeze_time = 0.0;
			if( !CL_VRIsBackpack( dominant )) vr_squeeze_time = host.realtime;
		}
		else
		{
			if( vr_squeeze_time != 0.0 &&
				( host.realtime - vr_squeeze_time ) * 1000.0 <= vr_reloadtimeoutms.value )
				vr_reload_pulse = true;
			vr_squeeze_time = 0.0;
		}
	}
	vr_one_controller_shifted = squeeze && vr_squeeze_time != 0.0 &&
		( host.realtime - vr_squeeze_time ) * 1000.0 > vr_reloadtimeoutms.value;

	if( !( old_offhand_buttons & REF_VR_BUTTON_STICK ) &&
		FBitSet( other->buttons, REF_VR_BUTTON_STICK ))
		Cvar_SetValue( "vr_lasersight", (int)( vr_lasersight.value + 1.0f ) % 3 );
	if( !( old_offhand_buttons & REF_VR_BUTTON_SECONDARY ) &&
		FBitSet( other->buttons, REF_VR_BUTTON_SECONDARY ))
		Cbuf_AddText( "impulse 100\n" );

	if( vr_one_controller_shifted )
	{
		if( !( old_dominant_buttons & REF_VR_BUTTON_PRIMARY ) &&
			FBitSet( weapon->buttons, REF_VR_BUTTON_PRIMARY ))
		{
			vr_one_controller_jump_mode = !vr_one_controller_jump_mode;
			CL_CenterPrint( vr_one_controller_jump_mode ? "JumpMode:  Duck -> Jump" :
				"JumpMode:  Jump {-> Duck}", -1.0f );
		}
		if( !( old_dominant_buttons & REF_VR_BUTTON_SECONDARY ) &&
			FBitSet( weapon->buttons, REF_VR_BUTTON_SECONDARY ))
		{
			vr_one_controller_running = !vr_one_controller_running;
			CL_CenterPrint( vr_one_controller_running ? "Run Toggle: Enabled" :
				"Run Toggle: Disabled", -1.0f );
		}
	}
	else
	{
		if( stick && !old_stick )
			vr_one_controller_jump_time = host.realtime;
		else if( !stick && old_stick )
			vr_one_controller_jump_time = 0.0;

		/* The Android implementation intentionally toggles duck when the
		 * dominant primary button is released outside the jump macro. */
		if( vr_one_controller_jump_time == 0.0 &&
			( old_dominant_buttons & REF_VR_BUTTON_PRIMARY ) &&
			!FBitSet( weapon->buttons, REF_VR_BUTTON_PRIMARY ))
			vr_one_controller_duck_toggled = !vr_one_controller_duck_toggled;
	}
}

static void CL_VRRelativePose( const ref_vr_pose_t *pose, const ref_vr_pose_t *origin, ref_vr_pose_t *out )
{
	vec3_t delta;

	VectorSubtract( pose->position, origin->position, delta );
	CL_VRTransformVector( delta, origin, out->position );
	CL_VRTransformVector( pose->forward, origin, out->forward );
	CL_VRTransformVector( pose->right, origin, out->right );
	CL_VRTransformVector( pose->up, origin, out->up );
}

static void CL_VRCinemaReset( void )
{
	memset( &vr_cinema_state, 0, sizeof( vr_cinema_state ));
}

/* Cinema mode: while a scripted camera (trigger_camera, svc_setview) has the view, the engine renders it once onto
 * a flat screen in the room and leaves the world black around it. The engine only learns of the camera when the
 * view switches, so the world cuts to black at once and the screen fades in; when the camera ends the screen fades
 * out on its last image, then the world fades in from black. */
static void CL_VRCinemaUpdate( void )
{
	const double now = host.realtime;
	const float fade = vr_cinema_fade.value;
	/* the game's forward places the screen and the menu panel in the renderer (GL_OpenXRPanelAnchor) */
	vec3_t room_forward = { 0.0f, 0.0f, 0.0f };
	float step;

	if( vr_center_valid )
		VectorCopy( vr_center.forward, room_forward );

	step = (float)bound( 0.0, now - vr_cinema_state.last_time, 0.1 );
	step = fade > 0.01f ? step / fade : 1.0f;
	vr_cinema_state.last_time = now;

	/* level changes (c5a1, of7a0 end on a camera) must not leave the screen up */
	if( !vr_cinema.value || cls.state != ca_active || !vr_center_valid || !ref.dllFuncs.R_VRSetCinema )
	{
		CL_VRCinemaReset();
		vr_cinema_state.last_time = now;
		if( ref.dllFuncs.R_VRSetCinema )
			ref.dllFuncs.R_VRSetCinema( false, 0.0f, room_forward );
		return;
	}

	/* same test as the client's V_CalcRefdef: a view entity that is not a player is a camera */
	vr_cinema_state.camera = !cls.spectator && cl.viewentity > cl.maxclients;
	if( vr_cinema_state.camera )
	{
		if( !vr_cinema_state.showing )
		{
			vr_cinema_state.showing = true;
			vr_cinema_state.brightness = 0.0f;
		}
		vr_cinema_state.brightness = Q_min( 1.0f, vr_cinema_state.brightness + step );
		vr_cinema_state.world_black = 1.0f;
	}
	else if( vr_cinema_state.showing )
	{
		vr_cinema_state.brightness -= step;
		if( vr_cinema_state.brightness <= 0.0f )
		{
			vr_cinema_state.brightness = 0.0f;
			vr_cinema_state.showing = false;
		}
		vr_cinema_state.world_black = 1.0f;
	}
	else vr_cinema_state.world_black = Q_max( 0.0f, vr_cinema_state.world_black - step );

	ref.dllFuncs.R_VRSetCinema( vr_cinema_state.showing, vr_cinema_state.brightness, room_forward );
}

qboolean CL_VRCinemaShowing( void )
{
	return vr_cinema_state.showing;
}

qboolean CL_VRCinemaCamera( void )
{
	return vr_cinema_state.showing && vr_cinema_state.camera;
}

qboolean CL_VRBeginCinema( int *width, int *height )
{
	return CL_VRShouldRender() && ref.dllFuncs.R_VRBeginCinema && ref.dllFuncs.R_VRBeginCinema( width, height );
}

void CL_VRSetScope( qboolean show, int hand )
{
	if( vr_frame_begun && ref.dllFuncs.R_VRSetScope )
		ref.dllFuncs.R_VRSetScope( show, hand );
}

qboolean CL_VRBeginScope( int *width, int *height )
{
	return CL_VRShouldRender() && ref.dllFuncs.R_VRBeginScope && ref.dllFuncs.R_VRBeginScope( width, height );
}

void CL_VREndScope( void )
{
	if( ref.dllFuncs.R_VREndScope )
		ref.dllFuncs.R_VREndScope( refState.width, refState.height );
}

void CL_VREndCinema( void )
{
	if( ref.dllFuncs.R_VREndCinema )
		ref.dllFuncs.R_VREndCinema( refState.width, refState.height );
}

/* After a camera scene: the world fades in from black (between CL_VRBeginEye and CL_VREndEye). */
void CL_VRDrawCinemaWorldFade( void )
{
	byte rgba[4] = { 0, 0, 0, 0 };

	if( vr_cinema_state.world_black <= 0.0f || !ref.dllFuncs.R_VRDrawEyeFade )
		return;
	rgba[3] = (byte)( 255.0f * Q_min( 1.0f, vr_cinema_state.world_black ) + 0.5f );
	ref.dllFuncs.R_VRDrawEyeFade( rgba, false );
}

void CL_VRFrameBegin( void )
{
	qboolean old_menu, menu, paused;
	uint32_t old_offhand_buttons, old_dominant_buttons;
	int dominant, offhand, variant;

	if( vr_frame_begun )
		CL_VRFrameEnd();

	memset( &vr_frame, 0, sizeof( vr_frame ));
	vr_frame.version = REF_VR_FRAME_VERSION;
	vr_frame.struct_size = sizeof( vr_frame );
	vr_frame_begun = false;
	CL_VRCSBots();

	if( !ref.initialized || !ref.dllFuncs.R_VRFrameBegin )
	{
		CL_VRResetInputState( true );
		CL_VRCinemaReset();
		return;
	}

	{
		/* menu/console open: tell the renderer to keep the UI panel fixed in the room. Also while
		 * a map loads/connects: the menu closes on "New game" and the panel (menu image / loading
		 * plaque) used to snap to the head until the level was running. */
		convar_t *ui_world = Cvar_FindVar( "vr_ui_world" );
		const qboolean want = CL_VRUIActive() || cls.state != ca_active || cls.disable_screen != 0.0f;
		if( ui_world && ( ui_world->value != 0.0f ) != want )
			Cvar_DirectSet( ui_world, want ? "1" : "0" );
	}
	vr_frame_begun = ref.dllFuncs.R_VRFrameBegin( &vr_frame );
	if( !vr_frame_begun || !FBitSet( vr_frame.flags, REF_VR_FRAME_SESSION_ACTIVE ))
	{
		vr_center_valid = false;
		vr_previous_head_valid = false;
		VectorClear( vr_head_delta );
		CL_VRResetInputState( true );
		CL_VRCinemaReset();
		return;
	}

	/* VR session is running from here on (CL_VRIsActive() is false before R_VRFrameBegin) */
	{
		/* SDL also sees the Sense controllers as a gamepad: its look/move axes and button binds
		 * would act on top of the VR controls (right stick up/down pitched the body). */
		convar_t *joy = Cvar_FindVar( "joy_enable" );
		if( joy && joy->value != 0.0f )
		{
			Cvar_DirectSet( joy, "0" );
			Con_Printf( "VR: SDL gamepad input disabled (joy_enable 0); the VR controllers are read via OpenXR\n" );
		}
	}
	{
		/* the desktop window rarely has focus in VR; muting on focus loss silenced the game */
		convar_t *mute = Cvar_FindVar( "snd_mute_losefocus" );
		if( mute && mute->value != 0.0f )
		{
			Cvar_DirectSet( mute, "0" );
			Con_Printf( "VR: snd_mute_losefocus 0 (sound keeps playing when the desktop window loses focus)\n" );
		}
	}

	if( FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ) &&
		( !vr_center_valid || FBitSet( vr_frame.flags, REF_VR_FRAME_REFERENCE_CHANGED )))
	{
		vr_center = vr_frame.head;
		CL_VRFlattenPose( &vr_center );
		vr_center_valid = true;
		vr_previous_head_valid = false;
		VectorClear( vr_head_delta );
	}

	paused = CL_VRUIActive() || cls.state != ca_active || cls.disable_screen != 0.0f;
	if( FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
	{
		if( vr_previous_head_valid )
		{
			VectorSubtract( vr_frame.head.position, vr_previous_head.position, vr_head_delta );
			if( CL_VRLeanActive( ))
			{
				/* the view shows the head's offset from the body (lean/peek); hands are measured
				 * from the same point, so they stay on the real hands */
				CL_VRLeanLeash( vr_center.position, vr_frame.head.position, vr_lean_radius.value );
				/* The leash leaves the body point up to the radius away from the head after a walk
				 * around the room (to the desk and back), and every reach is then that much short or
				 * long. Resuming play re-centres it; the heading stays, so the world does not turn. */
				if( vr_game_was_paused && !paused && vr_lean_resume_center.value != 0.0f )
				{
					vr_center.position[0] = vr_frame.head.position[0];
					vr_center.position[1] = vr_frame.head.position[1];
				}
			}
			else
			{
				/* Prediction consumes horizontal room-scale motion this frame. Keep the
				 * rendered offset for vertical crouching, but follow horizontal motion. */
				vr_center.position[0] += vr_head_delta[0];
				vr_center.position[1] += vr_head_delta[1];
			}
		}
		else VectorClear( vr_head_delta );
		vr_previous_head = vr_frame.head;
		vr_previous_head_valid = true;
		vr_game_was_paused = paused;
	}
	CL_VRCinemaUpdate();

	dominant = vr_control_scheme.value >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
	offhand = dominant == REF_VR_HAND_LEFT ? REF_VR_HAND_RIGHT : REF_VR_HAND_LEFT;
	/* Lambda1VR keeps the legacy viewmodel/game-DLL handedness cvar in lockstep
	 * with its control scheme. Preserve that contract for aware clients. */
	if( Cvar_FindVar( "hand" )) /* not every game has it (Counter-Strike) */
		Cvar_SetValue( "hand", dominant == REF_VR_HAND_LEFT ? 1.0f : 0.0f );
	if( !FBitSet( vr_frame.flags, REF_VR_FRAME_FOCUSED ) ||
		!FBitSet( vr_frame.flags, REF_VR_FRAME_ACTIONS_VALID ))
	{
		CL_VRResetInputState( true );
		return;
	}

	CL_VRUpdateUITrigger( vr_frame.hands[dominant].trigger, CL_VRUIActive() );
	if( CL_VRUIActive() )
	{
		ref_vr_hand_t *pointer = &vr_frame.hands[dominant];
		qboolean pressed = pointer->trigger >= 0.55f;

		if( FBitSet( pointer->flags, REF_VR_HAND_UI_VALID ))
		{
			float dx = vr_ui_cursor_valid ? pointer->ui_cursor[0] - vr_ui_cursor[0] : 0.0f;
			float dy = vr_ui_cursor_valid ? pointer->ui_cursor[1] - vr_ui_cursor[1] : 0.0f;
			vr_ui_cursor[0] = pointer->ui_cursor[0];
			vr_ui_cursor[1] = pointer->ui_cursor[1];
			vr_ui_cursor_valid = true;
			IN_TouchEvent( event_motion, 0, vr_ui_cursor[0], vr_ui_cursor[1], dx, dy );
			if( pressed != vr_ui_pressed )
				IN_TouchEvent( pressed ? event_down : event_up, 0,
					vr_ui_cursor[0], vr_ui_cursor[1], 0.0f, 0.0f );
			vr_ui_pressed = pressed;
		}
		else CL_VRResetUIPointer();
	}
	else CL_VRResetUIPointer();

	old_menu = vr_menu_active;
	menu = (( vr_frame.hands[0].buttons | vr_frame.hands[1].buttons ) & REF_VR_BUTTON_MENU ) != 0 ||
		( FBitSet( vr_frame.hands[0].buttons, REF_VR_BUTTON_STICK ) &&
		FBitSet( vr_frame.hands[1].buttons, REF_VR_BUTTON_STICK ));
	/* the menu chord is two stick clicks: mark it here, before the UI early return below, so the
	 * off-hand release afterwards is not taken as an L3 laser toggle (the menu opens/closes a
	 * frame later, and the stick-up frames were never seen by the chord check further down) */
	if( FBitSet( vr_frame.hands[0].buttons, REF_VR_BUTTON_STICK ) &&
		FBitSet( vr_frame.hands[1].buttons, REF_VR_BUTTON_STICK ))
		vr_offhand_stick_chord = true;
	if( menu != old_menu ) Key_Event( K_ESCAPE, menu );
	vr_menu_active = menu;
	if( !CL_VRCSMode( ))
	{
		CL_VRSetScope( CL_VRScopeLensActive(), vr_control_scheme.value >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT );
		if( Cvar_VariableValue( "vr_scope_magnifier" ) == 0.0f )
			Cvar_SetValue( "vr_scope_magnifier", 1.0f );
	}
	else if( Cvar_VariableValue( "vr_scope_magnifier" ) != 0.0f )
		Cvar_SetValue( "vr_scope_magnifier", 0.0f );
	if( CL_VRCSMode( ))
	{
		/* Counter-Strike (CSVR client): buttons, aim and hands are handled in vr_cs.c */
		CL_VRCSFrame( CL_VRUIActive( ));
		for( int i = 0; i < REF_VR_MAX_HANDS; ++i )
			vr_previous_buttons[i] = vr_frame.hands[i].buttons;
		return;
	}
	if( CL_VRUIActive() )
	{
		CL_VRCancelWeaponPointer();
		for( int i = 0; i < REF_VR_MAX_HANDS; ++i )
			vr_previous_buttons[i] = vr_frame.hands[i].buttons;
		return;
	}

	old_offhand_buttons = vr_previous_buttons[offhand];
	old_dominant_buttons = vr_previous_buttons[dominant];
	variant = (int)vr_control_scheme.value % 10;
	if( variant < 0 || variant > 3 ) variant = 0;
	if( variant == 3 )
	{
		qboolean use;

		CL_VROneControllerInput( dominant, offhand, old_dominant_buttons, old_offhand_buttons );
		use = CL_VRUseGesture( offhand ) && Cvar_VariableValue( "vr_weapon_stabilised" ) == 0.0f &&
			Cmd_Exists( "+use2" );
		if( use != vr_offhand_use_active )
			Cbuf_AddText( use ? "+use2\n" : "-use2\n" );
		vr_offhand_use_active = use;

		for( int i = 0; i < REF_VR_MAX_HANDS; ++i )
			vr_previous_buttons[i] = vr_frame.hands[i].buttons;
		return;
	}
	/* The off-hand stick click is use from the off hand (like R3 for the dominant hand, see the
	 * +use2 block below); a double click cycles the laser sight while unscoped, or scope smoothing
	 * while scoped (Lambda1VR used a single click for that). The toggle is decided on the second
	 * release and skipped if the dominant stick went down during either click: the two sticks of
	 * the menu chord never go down in the same frame. */
	if( FBitSet( vr_frame.hands[offhand].buttons, REF_VR_BUTTON_STICK ))
	{
		if( FBitSet( vr_frame.hands[dominant].buttons, REF_VR_BUTTON_STICK ))
			vr_offhand_stick_chord = true;
		if( vr_offhand_stick_since == 0.0 )
		{
			vr_offhand_stick_since = host.realtime;
			vr_offhand_stick_second = vr_offhand_stick_last != 0.0 &&
				host.realtime - vr_offhand_stick_last <= vr_laser_doubleclick.value;
		}
	}
	else if( vr_offhand_stick_since != 0.0 )
	{
		if( vr_offhand_stick_chord )
			vr_offhand_stick_last = 0.0;
		else if( vr_offhand_stick_second )
		{
			vr_offhand_stick_last = 0.0;
			if( Cvar_VariableValue( "vr_scope_engaged" ) != 0.0f )
				Cvar_SetValue( "vr_scope_stabilise",
					Cvar_VariableValue( "vr_scope_stabilise" ) == 0.0f ? 1.0f : 0.0f );
			else
				Cvar_SetValue( vr_lasersight.name, (int)( vr_lasersight.value + 1.0f ) % 3 );
		}
		else vr_offhand_stick_last = vr_offhand_stick_since;
		vr_offhand_stick_chord = false;
		vr_offhand_stick_since = 0.0;
		vr_offhand_stick_second = false;
	}
	else vr_offhand_stick_chord = false; /* chord pressed and released while the menu was up */
	if( vr_confirm_attack_release )
	{
		Cbuf_AddText( "-attack\n" );
		vr_confirm_attack_release = false;
	}
	if( variant == 1 &&
		FBitSet( old_offhand_buttons ^ vr_frame.hands[offhand].buttons, REF_VR_BUTTON_SECONDARY ))
	{
		vr_selecting_weapon = FBitSet( vr_frame.hands[offhand].buttons, REF_VR_BUTTON_SECONDARY );
		if( vr_selecting_weapon )
		{
			vr_weapon_scroll_count = 1;
			Cbuf_AddText( "invprev\n" );
		}
		else
		{
			if( vr_weapon_scroll_count == 0 ) Cbuf_AddText( "cancelselect\n" );
			else
			{
				Cbuf_AddText( "+attack\n" );
				vr_confirm_attack_release = true;
			}
			vr_weapon_scroll_count = 0;
		}
	}
	else if( variant != 1 ) vr_selecting_weapon = false;

	{
		qboolean weapon_backpack = CL_VRIsBackpack( dominant );
		qboolean offhand_backpack = CL_VRIsBackpack( offhand );

		if( vr_controller_tracking_haptic.value && weapon_backpack && !vr_weapon_in_backpack && !vr_backpack_weapon_active )
			CL_VRHaptic( dominant, 0.04f, 0.0f, 0.5f );
		if( vr_controller_tracking_haptic.value && offhand_backpack && !vr_offhand_in_backpack )
			CL_VRHaptic( offhand, 0.04f, 0.0f, 0.5f );
		vr_weapon_in_backpack = weapon_backpack;
		vr_offhand_in_backpack = offhand_backpack;
	}

	if( vr_backpack_weapon_active )
	{
		if( !CL_VRHandSqueezePressed( &vr_frame.hands[dominant] ))
		{
			Cbuf_AddText( "lastinv\n" );
			vr_backpack_weapon_active = false;
		}
	}
	else if( vr_weapon_in_backpack && CL_VRHandSqueezePressed( &vr_frame.hands[dominant] ))
	{
		CL_VRCommand( CL_VRBackpackWeapon( ));
		CL_VRHaptic( dominant, 0.08f, 0.0f, 0.8f );
		vr_backpack_weapon_active = true;
		vr_squeeze_time = 0.0;
	}
	else if( !vr_weapon_in_backpack )
	{
		if( CL_VRHandSqueezePressed( &vr_frame.hands[dominant] ) && vr_squeeze_time == 0.0 )
			vr_squeeze_time = host.realtime;
		else if( !CL_VRHandSqueezePressed( &vr_frame.hands[dominant] ) && vr_squeeze_time != 0.0 )
		{
			/* a short grip on a ladder rung is climbing (client DLL sets vr_climbing bit 1), not reload */
			if(( host.realtime - vr_squeeze_time ) * 1000.0 <= vr_reloadtimeoutms.value &&
				!( (int)Cvar_VariableValue( "vr_climbing" ) & 1 ))
			{
				vr_reload_pulse = true;
				CL_VRHaptic( dominant, 0.04f, 0.0f, 0.5f );
			}
			vr_squeeze_time = 0.0;
		}
	}

	if( vr_offhand_in_backpack && cl.maxclients <= 1 )
	{
		if( !( old_offhand_buttons & REF_VR_BUTTON_PRIMARY ) &&
			( vr_frame.hands[offhand].buttons & REF_VR_BUTTON_PRIMARY ))
		{
			/* feedback: the gesture is done blind behind the shoulder */
			Cbuf_AddText( "savequick\n" );
			CL_VRHaptic( offhand, 0.0f, 0.0f, 0.0f );
			CL_VRHaptic( offhand, 0.25f, 0.0f, 0.9f );
		}
		if( !( old_offhand_buttons & REF_VR_BUTTON_SECONDARY ) &&
			( vr_frame.hands[offhand].buttons & REF_VR_BUTTON_SECONDARY ))
		{
			Cbuf_AddText( "loadquick\n" );
			CL_VRHaptic( offhand, 0.0f, 0.0f, 0.0f );
			CL_VRHaptic( offhand, 0.25f, 0.0f, 0.9f );
		}
	}
	else
	{
		int flashlight_hand = variant == 2 ? dominant : offhand;
		uint32_t old_flashlight = flashlight_hand == dominant ? old_dominant_buttons : old_offhand_buttons;
		if(( old_flashlight & REF_VR_BUTTON_PRIMARY ) &&
			!( vr_frame.hands[flashlight_hand].buttons & REF_VR_BUTTON_PRIMARY ))
		{
			if( !CL_VRFlashlightNeedsHead( ))
				Cbuf_AddText( "impulse 100\n" );
			else if( CL_VRIsAtHead( flashlight_hand ))
			{
				Cbuf_AddText( "impulse 100\n" );
				CL_VRHaptic( flashlight_hand, 0.06f, 0.0f, 0.6f );
			}
		}

		if( variant == 0 || variant == 2 )
		{
			int screen_hand = variant == 2 ? dominant : offhand;
			uint32_t old_screen = screen_hand == dominant ? old_dominant_buttons : old_offhand_buttons;
			if( !( old_screen & REF_VR_BUTTON_SECONDARY ) &&
				( vr_frame.hands[screen_hand].buttons & REF_VR_BUTTON_SECONDARY ))
			{
				/* multiplayer: scoreboard (Lambda1VR); single player has none, so the button
				 * (left Triangle / Y) opens the pointer weapon grid while held (release handled
				 * below), or without the grid switches to the previous weapon */
				if( cl.maxclients > 1 )
				{
					vr_scoreboard_active = !vr_scoreboard_active;
					Cbuf_AddText( vr_scoreboard_active ? "+showscores\n" : "-showscores\n" );
				}
				else if( vr_weapon_pointer.value != 0.0f && Cmd_Exists( "+vr_weaponpointer" ))
				{
					Cbuf_AddText( "+vr_weaponpointer\n" );
					vr_weapon_pointer_open = true;
					vr_screen_button_since = host.realtime;
				}
				else Cbuf_AddText( "lastinv\n" );
			}
		}
	}

	/* Triangle release closes the grid (switching to the weapon pointed at). Tracked outside the
	 * branch above so moving the hand behind the shoulder while holding still sees the release. */
	if( vr_screen_button_since != 0.0 )
	{
		int screen_hand = variant == 2 ? dominant : offhand;

		if( !FBitSet( vr_frame.hands[screen_hand].buttons, REF_VR_BUTTON_SECONDARY ))
		{
			if( vr_weapon_pointer_open )
				Cbuf_AddText( "-vr_weaponpointer\n" );
			vr_weapon_pointer_open = false;
			vr_screen_button_since = 0.0;
		}
	}

	{
		int jump_hand = variant == 2 ? offhand : dominant;
		uint32_t old_jump = jump_hand == dominant ? old_dominant_buttons : old_offhand_buttons;
		uint32_t jump_button = vr_swap_jumpduck.value ? REF_VR_BUTTON_PRIMARY : REF_VR_BUTTON_SECONDARY;
		qboolean jump = FBitSet( vr_frame.hands[jump_hand].buttons, jump_button );
		qboolean old_jump_pressed = FBitSet( old_jump, jump_button );

		if( jump && !old_jump_pressed )
		{
			vr_quick_crouch = vr_quick_crouchjump.value && vr_last_jump_press > 0.0 &&
				( host.realtime - vr_last_jump_press ) <= 0.3;
			vr_last_jump_press = host.realtime;
			vr_jump_held = true;
		}
		else if( !jump && old_jump_pressed )
		{
			vr_jump_held = false;
			vr_quick_crouch = false;
		}
	}

	{
		qboolean click = FBitSet( vr_frame.hands[offhand].buttons, REF_VR_BUTTON_STICK ) &&
			!vr_offhand_stick_chord;
		qboolean use = ( click || CL_VRUseGesture( offhand )) &&
			Cvar_VariableValue( "vr_weapon_stabilised" ) == 0.0f && Cmd_Exists( "+use2" );
		if( use != vr_offhand_use_active )
			Cbuf_AddText( use ? "+use2\n" : "-use2\n" );
		vr_offhand_use_active = use;
	}

	if( variant == 1 )
	{
		float axis = vr_frame.hands[dominant].stick[0];
		if( vr_selecting_weapon && fabs( axis ) > 0.4f )
		{
			if( !vr_weapon_latched )
			{
				Cbuf_AddText( axis > 0.0f ? "invprev\n" : "invnext\n" );
				vr_weapon_scroll_count += axis > 0.0f ? 1 : -1;
				vr_weapon_latched = true;
			}
		}
		else vr_weapon_latched = false;
	}
	else if( Cvar_VariableValue( "vr_scope_engaged" ) != 0.0f )
	{
		float axis = vr_frame.hands[offhand].stick[1];
		if( fabs( axis ) > 0.6f )
		{
			if( !vr_scope_zoom_latched )
			{
				Cbuf_AddText( axis > 0.0f ? "impulse 104\n" : "impulse 105\n" );
				vr_scope_zoom_latched = true;
			}
		}
		else vr_scope_zoom_latched = false;
	}
	else if( vr_weapon_stick_menu.value == 0.0f )
		vr_stick_selecting = false; /* stick menu off: up/down does nothing */
	else
	{
		int selector = variant == 2 ? offhand : dominant;
		/* Deadzone: a deliberate, mostly vertical push opens the weapon menu; diagonal
		 * pushes while turning or a stick resting near the threshold do not. The latch
		 * only releases once the stick is back near the centre (hysteresis). */
		float threshold = vr_weapon_stick_threshold.value;
		float xaxis = vr_frame.hands[selector].stick[0];
		float yaxis = vr_frame.hands[selector].stick[1];
		qboolean choose = ( fabs( yaxis ) > threshold && fabs( yaxis ) > 2.0f * fabs( xaxis )) ||
			( vr_turn_angle.value == 0.0f && fabs( xaxis ) > threshold && fabs( xaxis ) > 2.0f * fabs( yaxis ));
		qboolean centred = fabs( xaxis ) < 0.3f && fabs( yaxis ) < 0.3f;
		static double choose_since, last_step;

		/* Opening the menu needs the push held for vr_weapon_stick_hold seconds, so brushing the
		 * stick while turning/moving does not open it; once open, each push steps immediately. */
		if( !choose ) choose_since = 0.0;
		else if( choose_since == 0.0 ) choose_since = host.realtime;
		if( vr_stick_selecting && host.realtime - last_step > 4.0 )
			vr_stick_selecting = false;	/* menu may have closed some other way: require the hold again */
		if( choose && !vr_stick_selecting && !vr_weapon_latched &&
			host.realtime - choose_since < vr_weapon_stick_hold.value )
			choose = false;

		if( choose )
		{
			if( !vr_weapon_latched )
			{
				/* stick up = previous weapon, down = next, like Lambda1VR; vr_weapon_stick_up_next reverses it */
				const qboolean next = ( yaxis > 0.0f ) == ( vr_weapon_stick_up_next.value != 0.0f );
				const char *select = fabs( yaxis ) > threshold ? ( next ? "invnext\n" : "invprev\n" ) :
					( xaxis > 0.0f ? "invprevslot\n" : "invnextslot\n" );
				Cbuf_AddText( select );
				vr_weapon_latched = true;
				vr_stick_selecting = true;
				last_step = host.realtime;
			}
		}
		else if( centred ) vr_weapon_latched = false;
	}

	/* The HUD weapon menu confirms on the client's +attack key state
	 * (CL_ButtonBits), which the trigger never touches: it only sets IN_ATTACK in
	 * the usercmd. Confirm like scheme 1 does, and keep the trigger from also
	 * firing until it is released. */
	{
		qboolean trigger_down = vr_frame.hands[dominant].trigger >= 0.55f;

		if( variant != 1 && vr_stick_selecting && trigger_down && !vr_weapon_trigger_down )
		{
			Cbuf_AddText( "+attack\n" );
			vr_confirm_attack_release = true;
			vr_stick_selecting = false;
			vr_ui_trigger_blocked = true;
		}
		vr_weapon_trigger_down = trigger_down;
	}

	for( int i = 0; i < REF_VR_MAX_HANDS; ++i )
		vr_previous_buttons[i] = vr_frame.hands[i].buttons;
}

void CL_VRFrameEnd( void )
{
	if( vr_frame_begun && ref.initialized && ref.dllFuncs.R_VRFrameEnd )
		ref.dllFuncs.R_VRFrameEnd();

	vr_frame_begun = false;
}

qboolean CL_VRShouldRender( void )
{
	return vr_frame_begun && vr_center_valid && vr_frame.eye_count == REF_VR_MAX_EYES &&
		FBitSet( vr_frame.flags, REF_VR_FRAME_SHOULD_RENDER );
}

qboolean CL_VRBeginEye( int eye )
{
	return CL_VRShouldRender() && ref.dllFuncs.R_VRBeginEye && ref.dllFuncs.R_VRBeginEye( eye );
}

qboolean CL_VREndEye( int eye )
{
	return ref.dllFuncs.R_VREndEye && ref.dllFuncs.R_VREndEye( eye, refState.width, refState.height );
}

/* The near plane (4 units) reaches ~10 units out at its corners with the PS VR2's wide outer
 * field of view, plus the eye's half IPD; nearer to a wall than this the view shows through it.
 * The body's eye always has this much room (the player hull is 16 units wide each way). */
#define VR_HEAD_WALL_MARGIN 12.0f

static qboolean CL_VRHeadSegmentClear( const vec3_t from, const vec3_t to )
{
	vec3_t a, b;
	pmtrace_t trace;

	VectorCopy( from, a );
	VectorCopy( to, b );
	trace = CL_TraceLine( a, b, PM_STUDIO_IGNORE );
	return trace.fraction >= 1.0f && !trace.startsolid && !trace.allsolid;
}

/* push a point out of walls so it keeps the margin horizontally in 8 directions; true when it has */
static qboolean CL_VRHeadPushOut( vec3_t point, qboolean move )
{
	qboolean clear = true;
	int i;

	for( i = 0; i < 8; i++ )
	{
		float s, c;
		vec3_t end;
		pmtrace_t trace;

		SinCos( i * ( M_PI_F / 4.0f ), &s, &c );
		VectorSet( end, point[0] + c * VR_HEAD_WALL_MARGIN, point[1] + s * VR_HEAD_WALL_MARGIN, point[2] );
		trace = CL_TraceLine( point, end, PM_STUDIO_IGNORE );
		if( trace.fraction >= 1.0f && !trace.startsolid )
			continue;
		clear = false;
		if( move && !trace.startsolid )
		{
			float back = VR_HEAD_WALL_MARGIN * ( 1.0f - trace.fraction );
			point[0] -= c * back;
			point[1] -= s * back;
		}
	}
	return clear;
}

static void CL_VRClampHeadOffset( const vec3_t start, const vec3_t desired, vec3_t clamped )
{
	pmtrace_t trace;
	vec3_t trace_start, trace_end, candidate, delta;
	float length, keep, lo, hi;
	int i;

	VectorCopy( desired, clamped );
	if( cls.spectator || !clgame.pmove )
		return;

	/* Match Lambda1VR's wall pushback: clip room-scale XY at the first obstruction while
	 * preserving the physically requested head Z, stopping the margin short of it (never
	 * behind the start). */
	VectorCopy( start, trace_start );
	VectorCopy( desired, trace_end );
	trace = CL_TraceLine( trace_start, trace_end, PM_STUDIO_IGNORE );
	VectorCopy( desired, candidate );
	if( trace.fraction < 1.0f )
	{
		VectorSubtract( trace.endpos, start, delta );
		length = VectorLength( delta );
		keep = length > VR_HEAD_WALL_MARGIN ? ( length - VR_HEAD_WALL_MARGIN ) / length : 0.0f;
		candidate[0] = start[0] + delta[0] * keep;
		candidate[1] = start[1] + delta[1] * keep;
	}

	/* The lean direction alone misses walls at a slant to it or just past its end: keep the
	 * margin all round (slides along the wall), then make sure the result is still reachable. */
	for( i = 0; i < 2; i++ )
		CL_VRHeadPushOut( candidate, true );
	if( CL_VRHeadPushOut( candidate, false ) && CL_VRHeadSegmentClear( start, candidate ))
	{
		clamped[0] = candidate[0];
		clamped[1] = candidate[1];
		return;
	}

	/* corners/narrow gaps: the furthest point towards the candidate that has the room */
	VectorSubtract( candidate, start, delta );
	lo = 0.0f; hi = 1.0f;
	for( i = 0; i < 6; i++ )
	{
		float mid = ( lo + hi ) * 0.5f;
		vec3_t probe;

		VectorMA( start, mid, delta, probe );
		probe[2] = desired[2];
		if( CL_VRHeadPushOut( probe, false ) && CL_VRHeadSegmentClear( start, probe ))
			lo = mid;
		else hi = mid;
	}
	clamped[0] = start[0] + delta[0] * lo;
	clamped[1] = start[1] + delta[1] * lo;
}

void CL_VRApplyHeadPose( ref_viewpass_t *rvp )
{
	ref_vr_pose_t relative;
	vec3_t body_forward, body_right, body_up;
	vec3_t composed_forward, composed_right, composed_up;
	vec3_t head_start, desired_origin;

	if( !rvp || !vr_center_valid || !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
	{
		vr_body_view_valid = false;
		return;
	}

	{
		ref_vr_pose_t body_center;
		CL_VRBodyCenter( &body_center );
		CL_VRRelativePose( &vr_frame.head, &body_center, &relative );
	}
	VectorCopy( rvp->vieworigin, vr_body_view_origin );
	/* Counter-Strike: the view angles are the weapon hand's, the body is under them */
	vr_body_view_yaw = CL_VRCSMode() ? CL_VRCSBodyYaw() : rvp->viewangles[YAW];
	vr_body_view_valid = true;
	{
		/* the body carries the head pitch for gameplay (swimming, ladders); the view gets
		 * pitch/roll from the head pose below, so compose with the body yaw only */
		vec3_t body_angles = { 0.0f, vr_body_view_yaw, 0.0f };
		AngleVectors( body_angles, body_forward, body_right, body_up );
	}
	VectorMA( rvp->vieworigin, vr_height_adjust.value * vr_worldscale.value, body_up, rvp->vieworigin );
	VectorCopy( rvp->vieworigin, head_start );
	VectorMA( head_start, relative.position[0] * vr_worldscale.value, body_forward, desired_origin );
	VectorMA( desired_origin, -relative.position[1] * vr_worldscale.value, body_right, desired_origin );
	VectorMA( desired_origin, relative.position[2] * vr_worldscale.value, body_up, desired_origin );
	CL_VRClampHeadOffset( head_start, desired_origin, rvp->vieworigin );

	VectorScale( body_forward, relative.forward[0], composed_forward );
	VectorMA( composed_forward, -relative.forward[1], body_right, composed_forward );
	VectorMA( composed_forward, relative.forward[2], body_up, composed_forward );
	VectorScale( body_forward, relative.right[0], composed_right );
	VectorMA( composed_right, -relative.right[1], body_right, composed_right );
	VectorMA( composed_right, relative.right[2], body_up, composed_right );
	VectorScale( body_forward, relative.up[0], composed_up );
	VectorMA( composed_up, -relative.up[1], body_right, composed_up );
	VectorMA( composed_up, relative.up[2], body_up, composed_up );
	VectorsAngles( composed_forward, composed_right, composed_up, rvp->viewangles );
}

uint64_t CL_VRFrameId( void )
{
	return vr_frame.frame_id;
}

static float CL_VRFilterAxis( float value )
{
	const float deadzone = 0.2f;
	float magnitude = fabs( value );

	if( magnitude <= deadzone ) return 0.0f;
	magnitude = ( magnitude - deadzone ) / ( 1.0f - deadzone );
	return copysign( magnitude * magnitude, value );
}

static void CL_VROneControllerMoveAxis( const ref_vr_hand_t *weapon, float *x, float *y )
{
	float average_x = 0.0f, average_y = 0.0f;
	float distance, filtered;

	for( int i = ARRAYSIZE( vr_one_controller_stick ) - 1; i > 0; --i )
	{
		vr_one_controller_stick[i][0] = vr_one_controller_stick[i - 1][0];
		vr_one_controller_stick[i][1] = vr_one_controller_stick[i - 1][1];
	}
	vr_one_controller_stick[0][0] = weapon->stick[0];
	vr_one_controller_stick[0][1] = weapon->stick[1];
	for( int i = 0; i < ARRAYSIZE( vr_one_controller_stick ); ++i )
	{
		average_x += vr_one_controller_stick[i][0];
		average_y += vr_one_controller_stick[i][1];
	}
	average_x /= ARRAYSIZE( vr_one_controller_stick );
	average_y /= ARRAYSIZE( vr_one_controller_stick );
	distance = sqrt( average_x * average_x + average_y * average_y );
	if( distance <= 0.2f ) filtered = 0.0f;
	else filtered = pow( ( distance - 0.2f ) / 0.8f, 2.2f );
	*x = filtered * average_x;
	*y = filtered * average_y;
}

/* Heading (degrees, relative to the recentre reference) the stick moves along: the off-hand
 * controller or the head (vr_walkdirection). Shared by movement and the ladder direction so both
 * use one frame. */
static float CL_VRMoveHeading( const ref_vr_hand_t *move, int variant )
{
	ref_vr_pose_t relative;
	vec3_t angles;

	if( variant == 3 )
		return 0.0f;
	if( vr_walkdirection.value < 0.5f && FBitSet( move->flags, REF_VR_HAND_AIM_VALID ))
		CL_VRRelativePose( &move->aim, &vr_center, &relative );
	else if( FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
		CL_VRRelativePose( &vr_frame.head, &vr_center, &relative );
	else return 0.0f;
	VectorsAngles( relative.forward, relative.right, relative.up, angles );
	return angles[YAW];
}

void CL_VRAppendMove( float frametime, usercmd_t *cmd, qboolean active )
{
	int dominant, offhand, variant;
	uint32_t duck_button;
	ref_vr_hand_t *weapon, *move, *turn;
	float x, y, heading, sine, cosine;
	float forward_speed, side_speed;
	float key_forward, key_side;
	float stick_x, stick_y;
	vec3_t head_delta;
	ref_vr_pose_t relative_aim, relative_head;
	vec3_t aim_angles, head_angles;

	if( !cmd || !active || CL_VRUIActive() || !CL_VRIsActive() ||
		!FBitSet( vr_frame.flags, REF_VR_FRAME_FOCUSED ) ||
		!FBitSet( vr_frame.flags, REF_VR_FRAME_ACTIONS_VALID ))
	{
		Cvar_SetValue( vr_comfort_moving.name, 0.0f );
		return;
	}

	if( CL_VRCSMode( ))
	{
		CL_VRTransformVector( vr_head_delta, &vr_center, head_delta );
		CL_VRCSAppendMove( frametime, cmd, head_delta );
		return;
	}

	dominant = vr_control_scheme.value >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
	offhand = dominant == REF_VR_HAND_LEFT ? REF_VR_HAND_RIGHT : REF_VR_HAND_LEFT;
	variant = (int)vr_control_scheme.value % 10;
	if( variant < 0 || variant > 3 ) variant = 0;
	weapon = &vr_frame.hands[dominant];
	move = &vr_frame.hands[variant == 2 || variant == 3 ? dominant : offhand];
	turn = &vr_frame.hands[variant == 2 ? offhand : dominant];

	if( variant == 3 )
	{
		if( vr_one_controller_shifted )
			x = y = 0.0f;
		else CL_VROneControllerMoveAxis( weapon, &x, &y );
	}
	else
	{
		x = CL_VRFilterAxis( move->stick[0] );
		y = CL_VRFilterAxis( move->stick[1] );
	}
	if( Cvar_VariableValue( "vr_scope_engaged" ) != 0.0f )
		x = y = 0.0f;
	Cvar_SetValue( vr_comfort_moving.name, fabs( x ) + fabs( y ) > 0.01f ||
		( variant != 3 && vr_smoothturn.value && fabs( turn->stick[0] ) > 0.6f ) ? 1.0f : 0.0f );
	/* cmd->viewangles carries only the body yaw (stick turning); the head yaw is applied
	 * to the view alone. Rotate the stick into the head's (or off-hand's) heading. */
	/* off-hand trigger = walk, like Shift on a keyboard: HL scales keyboard movement by
	 * cl_movespeedkey in the client, which the VR stick bypasses (the IN_RUN bit set below does
	 * nothing in movement code) */
	stick_x = x;	/* movement buttons use the unscaled stick (ladders must work while walking) */
	stick_y = y;
	if( variant != 3 && move->trigger >= 0.55f )
	{
		float walk = Cvar_VariableValue( "cl_movespeedkey" );
		if( walk <= 0.0f || walk > 1.0f ) walk = 0.3f;
		x *= walk;
		y *= walk;
	}
	heading = DEG2RAD( CL_VRMoveHeading( move, variant ));
	SinCos( heading, &sine, &cosine );
	forward_speed = Cvar_VariableValue( "cl_forwardspeed" );
	side_speed = Cvar_VariableValue( "cl_sidespeed" );
	if( forward_speed <= 0.0f ) forward_speed = 400.0f;
	if( side_speed <= 0.0f ) side_speed = 400.0f;
	/* Movement buttons come from deliberate input only: keyboard (already in cmd) and the raw,
	 * unrotated stick. They are all-or-nothing and ladders climb/strafe at full speed on them, so
	 * deriving them from the head-rotated or room-scale totals pressed a strafe button whenever the
	 * head was turned from the body (left while climbing, right while descending) or the player
	 * swayed. The ladder direction uses the same heading as the stick (sidecar ladder yaw). */
	key_forward = cmd->forwardmove;
	key_side = cmd->sidemove;
	cmd->sidemove += ( x * cosine - y * sine ) * side_speed;
	cmd->forwardmove += ( y * cosine + x * sine ) * forward_speed;

	if( frametime > 0.0f && !CL_VRLeanActive( ))
	{
		CL_VRTransformVector( vr_head_delta, &vr_center, head_delta );
		cmd->forwardmove += bound( -forward_speed, head_delta[0] * vr_worldscale.value * vr_positional_factor.value / frametime, forward_speed );
		cmd->sidemove -= bound( -side_speed, head_delta[1] * vr_worldscale.value * vr_positional_factor.value / frametime, side_speed );
	}

	/* The client DLL has already normalized keyboard input. VR is appended at
	 * the engine boundary, so complete the command contract here as well. */
	if( cl.local.maxspeed > 0.0f )
	{
		float speed = sqrt( cmd->forwardmove * cmd->forwardmove +
			cmd->sidemove * cmd->sidemove + cmd->upmove * cmd->upmove );
		if( speed > cl.local.maxspeed )
		{
			float scale = cl.local.maxspeed / speed;
			cmd->forwardmove *= scale;
			cmd->sidemove *= scale;
			cmd->upmove *= scale;
		}
	}
	ClearBits( cmd->buttons, IN_FORWARD | IN_BACK | IN_MOVERIGHT | IN_MOVELEFT );
	if( key_forward > 1.0f || stick_y > 0.3f ) SetBits( cmd->buttons, IN_FORWARD );
	else if( key_forward < -1.0f || stick_y < -0.3f ) SetBits( cmd->buttons, IN_BACK );
	if( key_side > 1.0f || stick_x > 0.3f ) SetBits( cmd->buttons, IN_MOVERIGHT );
	else if( key_side < -1.0f || stick_x < -0.3f ) SetBits( cmd->buttons, IN_MOVELEFT );

	if( variant == 3 )
	{
		if( !vr_ui_trigger_blocked && weapon->trigger >= 0.55f ) SetBits( cmd->buttons, IN_ATTACK );
		if( !vr_one_controller_shifted && FBitSet( weapon->buttons, REF_VR_BUTTON_SECONDARY ))
			SetBits( cmd->buttons, IN_ATTACK2 );
		if( vr_one_controller_running ) SetBits( cmd->buttons, IN_RUN );
		if( vr_one_controller_shifted && FBitSet( weapon->buttons, REF_VR_BUTTON_STICK ))
			SetBits( cmd->buttons, IN_USE );
	}
	else
	{
		if( !vr_ui_trigger_blocked && weapon->trigger >= 0.55f )
			SetBits( cmd->buttons, CL_VRHandSqueezePressed( weapon ) ? IN_ATTACK2 : IN_ATTACK );
		if( move->trigger >= 0.55f ) SetBits( cmd->buttons, IN_RUN );
		if( FBitSet( weapon->buttons, REF_VR_BUTTON_STICK )) SetBits( cmd->buttons, IN_USE );
	}
	if( CL_VRUseGesture( dominant ) && Cvar_VariableValue( "vr_weapon_stabilised" ) == 0.0f )
		SetBits( cmd->buttons, IN_USE );
	duck_button = vr_swap_jumpduck.value ? REF_VR_BUTTON_SECONDARY : REF_VR_BUTTON_PRIMARY;
	if(( variant == 3 ? vr_one_controller_duck_toggled : variant == 2 ? FBitSet( vr_frame.hands[offhand].buttons, duck_button ) :
		FBitSet( weapon->buttons, duck_button )) || vr_quick_crouch )
		SetBits( cmd->buttons, IN_DUCK );
	if( variant == 3 && vr_one_controller_jump_time != 0.0 )
	{
		if( vr_one_controller_jump_mode )
		{
			SetBits( cmd->buttons, IN_DUCK );
			if( host.realtime - vr_one_controller_jump_time > 0.25 ) SetBits( cmd->buttons, IN_JUMP );
		}
		else
		{
			SetBits( cmd->buttons, IN_JUMP );
			if( host.realtime - vr_one_controller_jump_time > 0.25 ) SetBits( cmd->buttons, IN_DUCK );
		}
	}
	else if( vr_jump_held ) SetBits( cmd->buttons, IN_JUMP );
	CL_VRUpdatePhysicalCrouch();
	if( vr_physical_crouched )
		SetBits( cmd->buttons, IN_DUCK );
	if( vr_reload_pulse )
	{
		SetBits( cmd->buttons, IN_RELOAD );
		vr_reload_pulse = false;
	}

	if( variant == 3 )
	{
		float xaxis = weapon->stick[0];
		float yaxis = weapon->stick[1];
		qboolean choose = fabs( yaxis ) > 0.75f || fabs( xaxis ) > 0.75f;

		if( vr_one_controller_shifted && !FBitSet( weapon->buttons, REF_VR_BUTTON_STICK ) && choose )
		{
			if( !vr_weapon_latched )
			{
				if( yaxis > 0.75f ) Cbuf_AddText( "invnext\n" );
				else if( yaxis < -0.75f ) Cbuf_AddText( "invprev\n" );
				else if( xaxis > 0.75f ) Cbuf_AddText( "invprevslot\n" );
				else Cbuf_AddText( "invnextslot\n" );
				vr_weapon_latched = true;
			}
		}
		else vr_weapon_latched = false;
	}
	else if( vr_smoothturn.value )
	{
		x = turn->stick[0];
		if( vr_selecting_weapon ) x = 0.0f;
		if( fabs( x ) > 0.2f )
			cmd->viewangles[YAW] -= CL_VRFilterAxis( x ) * vr_turn_angle.value * 8.0f * frametime;
	}
	else
	{
		x = turn->stick[0];
		if( vr_selecting_weapon ) x = 0.0f;
		if( fabs( x ) > 0.7f )
		{
			if( !vr_turn_latched )
			{
				cmd->viewangles[YAW] -= copysign( vr_turn_angle.value, x );
				vr_turn_latched = true;
			}
		}
		else if( fabs( x ) < 0.2f ) vr_turn_latched = false;
	}

	/* Body pitch = the headset's pitch, set every frame: HL swims and (with vr_ladder_aim 0) climbs
	 * along the view pitch, and nothing else (mouse, SDL gamepad) can accumulate pitch any more.
	 * The rendered view ignores body pitch (CL_VRApplyHeadPose), so it is not applied twice. */
	cmd->viewangles[PITCH] = 0.0f;
	if( FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
	{
		CL_VRRelativePose( &vr_frame.head, &vr_center, &relative_head );
		VectorsAngles( relative_head.forward, relative_head.right, relative_head.up, head_angles );
		cmd->viewangles[PITCH] = bound( -89.0f, head_angles[PITCH], 89.0f );
	}
	cmd->viewangles[ROLL] = 0.0f;
	COM_NormalizeAngles( cmd->viewangles );
	VectorCopy( cmd->viewangles, cl.viewangles );
}

void CL_VRBuildUsercmdSidecar( const usercmd_t *cmd, vr_usercmd_sidecar_t *sample )
{
	const ref_vr_hand_t *offhand;
	ref_vr_pose_t relativeOffhand;
	vec3_t offhandAngles;
	int dominant, support;

	if( !sample )
		return;
	memset( sample, 0, sizeof( *sample ));
	if( !cmd || !CL_VRIsActive() || !vr_center_valid ||
		!FBitSet( vr_frame.flags, REF_VR_FRAME_ACTIONS_VALID ))
		return;

	dominant = vr_control_scheme.value >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
	support = dominant == REF_VR_HAND_LEFT ? REF_VR_HAND_RIGHT : REF_VR_HAND_LEFT;
	offhand = &vr_frame.hands[support];
	if( Cvar_VariableValue( "vr_controller_ladders" ) != 0.0f &&
		( vr_ladder_aim.value < 0.5f ? FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ) :
		FBitSet( offhand->flags, REF_VR_HAND_AIM_VALID )))
	{
		if( vr_ladder_aim.value < 0.5f )
			offhandAngles[PITCH] = cmd->viewangles[PITCH]; /* look up/down to climb (= head pitch) */
		else
		{
			CL_VRRelativePose( &offhand->aim, &vr_center, &relativeOffhand );
			VectorsAngles( relativeOffhand.forward, relativeOffhand.right, relativeOffhand.up, offhandAngles );
		}
		{
			/* yaw in the same frame as the movement buttons (see CL_VRAppendMove); the
			 * off-hand only contributes the climb pitch */
			int variant = (int)vr_control_scheme.value % 10;
			const ref_vr_hand_t *move;
			if( variant < 0 || variant > 3 ) variant = 0;
			move = &vr_frame.hands[variant == 2 || variant == 3 ? dominant : support];
			offhandAngles[YAW] = CL_VRMoveHeading( move, variant ) + cmd->viewangles[YAW];
		}
		COM_NormalizeAngles( offhandAngles );
		if( offhandAngles[PITCH] == offhandAngles[PITCH] && offhandAngles[YAW] == offhandAngles[YAW] )
		{
			sample->version = VR_USERCMD_SIDECAR_VERSION;
			sample->flags = VR_USERCMD_SIDECAR_LADDER_VALID;
			sample->ladder_angles[0] = VR_UsercmdSidecarQuantize( offhandAngles[PITCH], 128.0f );
			sample->ladder_angles[1] = VR_UsercmdSidecarQuantize( offhandAngles[YAW], 128.0f );
		}
	}

	CL_VRGameBuildUsercmdSidecar( cmd, sample );
	vr_last_pose_valid = sample->version == VR_USERCMD_SIDECAR_VERSION && FBitSet( sample->flags, VR_USERCMD_SIDECAR_POSE_VALID );
	if( vr_last_pose_valid )
		vr_last_pose = *sample;
}

/* Zoomed weapons: the zoomed view on a round lens in front of the gun (as Counter-Strike's scope), in
 * place of Lambda1VR's darkened eyes with a square hole and no zoom. */
qboolean CL_VRScopeLensActive( void )
{
	if( CL_VRCSMode( ))
		return CL_VRCSScopeActive();
	return vr_scope_lens.value != 0.0f && vr_last_pose_valid && Cvar_VariableValue( "vr_scope_engaged" ) != 0.0f;
}

/* The lens is a magnifier: it shows what lies behind it, from the eye through its centre, enlarged by the
 * weapon's zoom (scr_fov against default_fov) - so the world around it, its picture and the laser dot line
 * up, and with the gun at the cheek the dot sits mid-lens. (Looking along the gun put a picture of the aim
 * point wherever the lens hung, away from the dot, and at roughly the lens's own angular size: hardly any
 * zoom.) Counter-Strike keeps its own scope view (its client view already looks along the gun). The
 * renderer turns the lens to face the eye (vr_scope_magnifier). */
void CL_VRScopeView( ref_viewpass_t *rvp )
{
	const int dominant = vr_control_scheme.value >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
	const ref_vr_hand_t *hand = &vr_frame.hands[dominant];
	ref_vr_pose_t lens, body_center, rel_lens, rel_head;
	vec3_t offset = { 0.0f, 0.06f, -0.08f }, local, dir;
	float sine, cosine, dist, size, lens_fov, default_fov;
	int i;

	if( CL_VRCSMode( ) || !FBitSet( hand->flags, REF_VR_HAND_AIM_VALID ) || !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
		return;
	if( sscanf( Cvar_VariableString( "vr_scope_offset" ), "%f %f %f", &offset[0], &offset[1], &offset[2] ) != 3 )
		VectorSet( offset, 0.0f, 0.06f, -0.08f );
	/* the offset is in the controller's OpenXR axes: x right, y up, z back */
	lens = hand->aim;
	for( i = 0; i < 3; i++ )
		lens.position[i] += hand->aim.right[i] * offset[0] + hand->aim.up[i] * offset[1] - hand->aim.forward[i] * offset[2];
	CL_VRBodyCenter( &body_center );
	CL_VRRelativePose( &lens, &body_center, &rel_lens );
	CL_VRRelativePose( &vr_frame.head, &body_center, &rel_head );
	VectorSubtract( rel_lens.position, rel_head.position, local );
	SinCos( DEG2RAD( vr_body_view_yaw ), &sine, &cosine );
	dir[0] = local[0] * cosine - local[1] * sine;
	dir[1] = local[0] * sine + local[1] * cosine;
	dir[2] = local[2];
	dist = VectorLength( dir );
	if( dist < 0.02f )
		return;
	rvp->viewangles[PITCH] = -RAD2DEG( atan2( dir[2], sqrt( dir[0] * dir[0] + dir[1] * dir[1] )));
	rvp->viewangles[YAW] = RAD2DEG( atan2( dir[1], dir[0] ));
	rvp->viewangles[ROLL] = 0.0f;

	size = bound( 0.01f, Cvar_VariableValue( "vr_scope_size" ), 0.5f );
	default_fov = Cvar_VariableValue( "default_fov" );
	if( default_fov < 10.0f )
		default_fov = 90.0f;
	lens_fov = RAD2DEG( 2.0f * atan( size * 0.5f / dist ));
	rvp->fov_x = rvp->fov_y = bound( 0.5f, lens_fov * bound( 1.0f, cl.local.scr_fov, default_fov ) / default_fov, 150.0f );
}

qboolean CL_VRHaptic( int hand, float duration, float frequency, float amplitude )
{
	return ref.initialized && ref.dllFuncs.R_VRHaptic &&
		ref.dllFuncs.R_VRHaptic( hand, duration, frequency, amplitude );
}

const ref_vr_frame_t *CL_VRGetFrame( void )
{
	return &vr_frame;
}

qboolean CL_VRIsActive( void )
{
	return vr_frame_begun && FBitSet( vr_frame.flags, REF_VR_FRAME_SESSION_ACTIVE );
}

/* Physical crouch presses duck, and the duck lowers the game's eye while the view and the hands also
 * follow the real head down: the two added up to lying on the floor. While the crouch is physical the
 * reference point is lowered by the duck's drop, so the camera and hands follow the head only. The drop
 * is measured as eye height above the feet (64 standing, 30 ducked): view_ofs alone (28 -> 12) missed
 * the 18 units the origin moves down when the duck completes, a visible snap. */
#define VR_STANDING_VIEWHEIGHT 28.0f /* VEC_VIEW / DEFAULT_VIEWHEIGHT */
static qboolean vr_crouch_lift_active; /* a physical crouch, until the game has stood the player up again */
static double vr_crouch_lift_end;      /* when the physical crouch ended (lift kept for the unduck) */
static void CL_VRBodyCenter( ref_vr_pose_t *center )
{
	const int hull = cl.local.usehull == 1 ? 1 : 0;
	const float standing = VR_STANDING_VIEWHEIGHT - host.player_mins[0][2];
	const float eye = cl.viewheight[2] - host.player_mins[hull][2];

	*center = vr_center;
	/* Standing up, the crouch ends at once but the game unducks a frame or more later (the cleared duck
	 * button goes through prediction first); dropping the lift right away dipped the view by the whole
	 * duck for those frames. Keep it until the eye is back at standing height - at most 0.25 s, for when
	 * the game cannot stand up (a vent: the view must not rise into the ceiling) or the duck button holds. */
	if( vr_physical_crouched )
	{
		vr_crouch_lift_active = true;
		vr_crouch_lift_end = 0.0;
	}
	else if( vr_crouch_lift_active )
	{
		if( vr_crouch_lift_end == 0.0 )
			vr_crouch_lift_end = host.realtime;
		if( eye >= standing - 0.5f || host.realtime - vr_crouch_lift_end > 0.25 )
			vr_crouch_lift_active = false;
	}
	if( vr_crouch_lift_active && !CL_VRCSMode() && vr_worldscale.value > 0.0f )
	{
		float lift = bound( 0.0f, standing - eye, standing );
		center->position[2] -= lift / vr_worldscale.value;
	}
}

qboolean CL_VRGetRecenter( ref_vr_pose_t *center )
{
	if( !center || !vr_center_valid )
		return false;

	CL_VRBodyCenter( center );
	return true;
}

float CL_VRGetWorldScale( void )
{
	return vr_worldscale.value;
}

qboolean CL_VRGetFlashlightPose( vec3_t origin, vec3_t forward )
{
	ref_vr_pose_t relative;
	const ref_vr_pose_t *pose;
	float sine, cosine;
	vec3_t local_origin;

	if( !CL_VRIsActive() || !vr_center_valid || !vr_body_view_valid )
		return false;

	if( vr_headtorch.value != 0.0f )
	{
		if( !FBitSet( vr_frame.flags, REF_VR_FRAME_HEAD_VALID ))
			return false;
		pose = &vr_frame.head;
	}
	else
	{
		int dominant = vr_control_scheme.value >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
		int offhand = dominant == REF_VR_HAND_LEFT ? REF_VR_HAND_RIGHT : REF_VR_HAND_LEFT;
		if( !FBitSet( vr_frame.hands[offhand].flags, REF_VR_HAND_AIM_VALID ))
			return false;
		pose = &vr_frame.hands[offhand].aim;
	}

	{
		ref_vr_pose_t body_center;
		CL_VRBodyCenter( &body_center );
		CL_VRRelativePose( pose, &body_center, &relative );
	}
	SinCos( DEG2RAD( vr_body_view_yaw ), &sine, &cosine );
	local_origin[0] = relative.position[0] * cosine - relative.position[1] * sine;
	local_origin[1] = relative.position[0] * sine + relative.position[1] * cosine;
	local_origin[2] = relative.position[2];
	VectorMA( vr_body_view_origin, vr_worldscale.value, local_origin, origin );

	// Two-handed aim: the off hand holding the torch is steadying the gun, so its own aim ray points
	// up and to the side of the target. Light along the gun instead (the client's stabilised aim is
	// the line from the gun hand to the support hand).
	if( vr_headtorch.value == 0.0f && Cvar_VariableValue( "vr_weapon_stabilised" ) != 0.0f )
	{
		int dominant = vr_control_scheme.value >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
		ref_vr_pose_t gun;
		vec3_t line;

		if( FBitSet( vr_frame.hands[dominant].flags, REF_VR_HAND_AIM_VALID ))
		{
			CL_VRRelativePose( &vr_frame.hands[dominant].aim, &vr_center, &gun );
			VectorSubtract( relative.position, gun.position, line );
			if( VectorLength( line ) > 0.05f )
				VectorCopy( line, relative.forward );
		}
	}

	forward[0] = relative.forward[0] * cosine - relative.forward[1] * sine;
	forward[1] = relative.forward[0] * sine + relative.forward[1] * cosine;
	forward[2] = relative.forward[2];
	if( vr_reversetorch.value != 0.0f )
		VectorNegate( forward, forward );
	VectorNormalize( forward );
	return true;
}

#if XASH_ENGINE_TESTS
#include "tests.h"

void Test_RunVRInputPolicy( void )
{
	int saved_dest = cls.key_dest;
	qboolean saved_cursor = host.mouse_visible;
	cls.key_dest = key_game;
	host.mouse_visible = false;
	TASSERT( !CL_VRUIActive() );
	host.mouse_visible = true;
	TASSERT( CL_VRUIActive() ); /* Sven MOTD/menu, without changing key_dest. */
	host.mouse_visible = false;
	cls.key_dest = key_menu;
	TASSERT( CL_VRUIActive() );
	cls.key_dest = saved_dest;
	host.mouse_visible = saved_cursor;
	CL_VRUpdateUITrigger( 0.0f, false );
	TASSERT( !vr_ui_trigger_blocked );
	CL_VRUpdateUITrigger( 1.0f, true );
	TASSERT( vr_ui_trigger_blocked );
	TASSERT( !REF_VR_SQUEEZE_PRESSED( 0.5f ));
	TASSERT( REF_VR_SQUEEZE_PRESSED( 0.5001f ));
	TASSERT( CL_VRPhysicalCrouchState( false, 0.85f, 1.0f, 0.84f ));
	TASSERT( !CL_VRPhysicalCrouchState( false, 0.85f, 1.0f, 0.86f ));
	TASSERT( CL_VRPhysicalCrouchState( true, 0.85f, 1.0f, 0.86f ));
	TASSERT( !CL_VRPhysicalCrouchState( true, 0.85f, 1.0f, 0.88f ));
	TASSERT( !CL_VRPhysicalCrouchState( true, 0.0f, 1.80f, 1.00f ));
	vr_physical_crouched = true;
	/* Focus and session loss both take this reset path. */
	CL_VRResetInputState( false );
	TASSERT( vr_ui_trigger_blocked ); /* Synthetic pointer-up is not release. */
	CL_VRUpdateUITrigger( 1.0f, false );
	TASSERT( vr_ui_trigger_blocked ); /* UI closed; held trigger is consumed. */
	CL_VRUpdateUITrigger( 0.0f, false );
	TASSERT( !vr_ui_trigger_blocked );
	CL_VRUpdateUITrigger( 1.0f, false );
	TASSERT( !vr_ui_trigger_blocked ); /* Release/repress permits gameplay. */
	TASSERT( !vr_physical_crouched );
	vr_physical_crouched = true;
	vr_center_valid = false;
	CL_VRUpdatePhysicalCrouch();
	TASSERT( !vr_physical_crouched );

	{
		/* Recentre keeps only the yaw: head facing +y (left), pitched 30 degrees down. */
		ref_vr_pose_t pose;
		const float c = cos( DEG2RAD( 30.0f )), s = sin( DEG2RAD( 30.0f ));
		VectorSet( pose.forward, 0.0f, c, -s );
		VectorSet( pose.right, 1.0f, 0.0f, 0.0f );
		VectorSet( pose.up, 0.0f, s, c );
		CL_VRFlattenPose( &pose );
		TASSERT( fabs( pose.forward[0] ) < 0.0001f && fabs( pose.forward[1] - 1.0f ) < 0.0001f && pose.forward[2] == 0.0f );
		TASSERT( fabs( pose.right[0] - 1.0f ) < 0.0001f && fabs( pose.right[1] ) < 0.0001f && pose.right[2] == 0.0f );
		TASSERT( pose.up[0] == 0.0f && pose.up[1] == 0.0f && pose.up[2] == 1.0f );

		/* Looking straight down: the heading comes from the right vector. */
		VectorSet( pose.forward, 0.0f, 0.0f, -1.0f );
		VectorSet( pose.right, 0.0f, -1.0f, 0.0f );
		VectorSet( pose.up, 1.0f, 0.0f, 0.0f );
		CL_VRFlattenPose( &pose );
		TASSERT( fabs( pose.forward[0] - 1.0f ) < 0.0001f && fabs( pose.forward[1] ) < 0.0001f );

		/* Lean leash: free inside the radius, the body point follows by the excess only. */
		{
			vec3_t center = { 1.0f, 2.0f, 1.5f }, head = { 1.3f, 2.0f, 1.2f };
			CL_VRLeanLeash( center, head, 0.5f );
			TASSERT( center[0] == 1.0f && center[1] == 2.0f && center[2] == 1.5f );
			VectorSet( head, 1.0f, 2.8f, 1.2f );
			CL_VRLeanLeash( center, head, 0.5f );
			TASSERT( fabs( center[0] - 1.0f ) < 0.0001f && fabs( center[1] - 2.3f ) < 0.0001f && center[2] == 1.5f );
			VectorSet( head, 4.0f, 6.0f, 1.2f );
			CL_VRLeanLeash( center, head, 0.0f );
			TASSERT( fabs( center[0] - 4.0f ) < 0.0001f && fabs( center[1] - 6.0f ) < 0.0001f );
		}

		/* Physical crouch: relative to the recentre height, not LOCAL space's zero. */
		float saved_crouching = vr_enable_crouching.value;
		vr_enable_crouching.value = 0.85f; /* crouching on (the default is off); not registered in the test host */
		vr_center_valid = true;
		vr_physical_crouched = false;
		vr_center.position[2] = 0.0f;
		vr_frame.flags = REF_VR_FRAME_HEAD_VALID;
		vr_frame.head.position[2] = -0.05f; /* small head bob: stays standing */
		CL_VRUpdatePhysicalCrouch();
		TASSERT( !vr_physical_crouched );
		vr_frame.head.position[2] = -0.40f; /* real crouch */
		CL_VRUpdatePhysicalCrouch();
		TASSERT( vr_physical_crouched );
		vr_enable_crouching.value = saved_crouching;
		vr_center_valid = false;
		vr_physical_crouched = false;
		memset( &vr_frame, 0, sizeof( vr_frame ));
	}
}
#endif
