/*
vr_cs.c - Counter-Strike VR mode for the CSVR client DLL
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

The CSVR client (Team Beef's VR fork of cs16-client) does its VR work in the stock game code
and reads everything else from vr_* cvars its Quest engine sets (engine/common/host_vr.c
there). This file is that engine half on top of our OpenXR layer (vr_client.c):

- the player's view angles ARE the weapon hand's aim (body yaw + the hand's yaw in the room),
  so shooting, recoil, bots and netcode stay stock; the camera gets the head instead
  (CL_VRApplyHeadPose asks CL_VRCSBodyYaw for the body yaw);
- controller buttons run console commands from vr_button_* cvars (CSVR's names and scheme,
  including the hold-for-alternative layer);
- the client places the weapon and the off hand from vr_weapon_* / vr_hand_* offsets, which
  are measured here from the VR recentre point (CSVR measured them from the head, which
  misplaced the weapon by however far the head had moved from the body).
*/

#include "common.h"
#include "client.h"
#include "input.h"
#include "keydefs.h"
#include "cl_tent.h"
#include "mod_local.h"
#include "vr_client.h"
#include "vr_cs.h"

/* written by the engine, read by the client */
static CVAR_DEFINE_AUTO( vr_gamemode, "0", 0, "CS VR: set by the engine, 1 while playing (no menu or console)" );
static CVAR_DEFINE_AUTO( vr_hand_active, "0", 0, "CS VR: off-hand pointing action active" );
static CVAR_DEFINE_AUTO( vr_hand_x, "0", 0, "CS VR: off hand from the body, units right (negated)" );
static CVAR_DEFINE_AUTO( vr_hand_y, "0", 0, "CS VR: off hand from the body, units forward (negated)" );
static CVAR_DEFINE_AUTO( vr_hand_z, "0", 0, "CS VR: off hand from the body, units up" );
static CVAR_DEFINE_AUTO( vr_hand_pitch, "0", 0, "CS VR: off-hand pitch" );
static CVAR_DEFINE_AUTO( vr_hand_yaw, "0", 0, "CS VR: off-hand yaw relative to the weapon hand" );
static CVAR_DEFINE_AUTO( vr_hand_roll, "0", 0, "CS VR: off-hand roll" );
static CVAR_DEFINE_AUTO( vr_hmd_pitch, "0", 0, "CS VR: head pitch" );
static CVAR_DEFINE_AUTO( vr_hmd_yaw, "0", 0, "CS VR: head yaw in the game" );
static CVAR_DEFINE_AUTO( vr_hmd_roll, "0", 0, "CS VR: head roll" );
static CVAR_DEFINE_AUTO( vr_offset_x, "0", 0, "CS VR: camera offset (unused, the camera is placed by the engine)" );
static CVAR_DEFINE_AUTO( vr_offset_y, "0", 0, "CS VR: camera offset (unused, the camera is placed by the engine)" );
static CVAR_DEFINE_AUTO( vr_offset_z, "0", 0, "CS VR: camera height offset subtracted from the off hand" );
static CVAR_DEFINE_AUTO( vr_weapon_x, "0", 0, "CS VR: weapon hand from the body, units right (negated)" );
static CVAR_DEFINE_AUTO( vr_weapon_y, "0", 0, "CS VR: weapon hand from the body, units forward (negated)" );
static CVAR_DEFINE_AUTO( vr_weapon_z, "0", 0, "CS VR: weapon hand from the body, units up" );
static CVAR_DEFINE_AUTO( vr_weapon_roll, "0", 0, "CS VR: weapon-hand roll" );
static CVAR_DEFINE_AUTO( vr_weapon_anim, "1", 0, "CS VR: 0 = the client skips weapon animations (motion controls)" );
static CVAR_DEFINE_AUTO( vr_weapon_throw_active, "0", 0, "CS VR: a grenade throw is being aimed" );
static CVAR_DEFINE_AUTO( vr_weapon_throw_pitch, "0", 0, "CS VR: grenade throw pitch" );
static CVAR_DEFINE_AUTO( vr_weapon_throw_yaw, "0", 0, "CS VR: grenade throw yaw" );
static CVAR_DEFINE_AUTO( vr_xhair_x, "0", 0, "CS VR: crosshair offset from the screen centre" );
static CVAR_DEFINE_AUTO( vr_xhair_y, "0", 0, "CS VR: crosshair offset from the screen centre" );
static CVAR_DEFINE_AUTO( vr_zoomed, "0", 0, "CS VR: the client is zoomed" );
static CVAR_DEFINE_AUTO( vr_superzoomed, "0", 0, "CS VR: the client is zoomed twice" );
static CVAR_DEFINE_AUTO( vr_zoomfix, "0", 0, "CS VR: the scope overlay is not drawn (zoom by motion)" );
static CVAR_DEFINE_AUTO( vr_zoom_by_motion, "0", 0, "CS VR: the current zoom came from the motion gesture" );
static CVAR_DEFINE_AUTO( vr_stereo_side, "0", 0, "CS VR: eye being drawn" );
static CVAR_DEFINE_AUTO( vr_vignette_amount, "0", 0, "CS VR: comfort vignette amount" );
static CVAR_DEFINE_AUTO( vr_force2d, "0", 0, "CS VR: the server asks for the flat UI (bot map analysis)" );
static CVAR_DEFINE_AUTO( vr_hand_click, "0", 0, "CS VR: off-hand use button held" );
static CVAR_DEFINE_AUTO( vr_hand_stretch, "0", 0, "CS VR: off hand stretched forward" );
static CVAR_DEFINE_AUTO( vr_player_pitch, "0", 0, "CS VR: rendered view pitch" );
static CVAR_DEFINE_AUTO( vr_player_yaw, "0", 0, "CS VR: rendered view yaw" );

/* written by the client */
static CVAR_DEFINE_AUTO( vr_hand_swap, "0", 0, "CS VR: dual elites, the left gun is the active one" );
static CVAR_DEFINE_AUTO( vr_haptics_weapon, "0", 0, "CS VR: weapon fired, haptic strength (reset by the engine)" );
static CVAR_DEFINE_AUTO( vr_haptics_yaw, "0", 0, "CS VR: direction damage came from" );
static CVAR_DEFINE_AUTO( vr_ladder, "0", 0, "CS VR: the player is on a ladder" );
static CVAR_DEFINE_AUTO( vr_shielded, "0", 0, "CS VR: the shield is raised" );
static CVAR_DEFINE_AUTO( vr_spectator, "0", 0, "CS VR: spectator mode" );
static CVAR_DEFINE_AUTO( vr_weapon_calibration_update, "0", 0, "CS VR: write the weapon calibration" );
static CVAR_DEFINE_AUTO( vr_weapon_pivot_name, "", 0, "CS VR: weapon model the pivot is for" );
static CVAR_DEFINE_AUTO( vr_weapon_pivot_x, "0", 0, "CS VR: weapon grip offset" );
static CVAR_DEFINE_AUTO( vr_weapon_pivot_y, "0", 0, "CS VR: weapon grip offset" );
static CVAR_DEFINE_AUTO( vr_weapon_pivot_z, "0", 0, "CS VR: weapon grip offset" );
static CVAR_DEFINE_AUTO( vr_weapon_pivot_pitch, "0", 0, "CS VR: weapon grip pitch" );
static CVAR_DEFINE_AUTO( vr_weapon_pivot_yaw, "0", 0, "CS VR: weapon grip yaw" );
static CVAR_DEFINE_AUTO( vr_weapon_pivot_scale, "0", 0, "CS VR: weapon model scale" );

/* settings */
static CVAR_DEFINE_AUTO( vr_6dof, "1", FCVAR_ARCHIVE, "CS VR: room-scale head movement" );
static CVAR_DEFINE_AUTO( vr_xhair, "0", FCVAR_ARCHIVE, "CS VR: draw the 2D crosshair (it sits on the HUD panel, not where the gun points)" );
static CVAR_DEFINE_AUTO( vr_haptics_enable, "1", FCVAR_ARCHIVE, "CS VR: controller haptics" );
static CVAR_DEFINE_AUTO( vr_aimdot, "1", FCVAR_ARCHIVE, "CS VR: red dot where the shot lands (traced from the eye along the gun, like the server)" );
static CVAR_DEFINE_AUTO( vr_aimdot_scale, "0.1", FCVAR_ARCHIVE, "CS VR: aim dot size (sprite scale per 100 units of distance)" );
static CVAR_DEFINE_AUTO( vr_weapon_calibration_on, "0", FCVAR_ARCHIVE, "CS VR: right stick adjusts the weapon grip (x/y/z/pitch/yaw/scale)" );
static CVAR_DEFINE_AUTO( vr_thumbstick_deadzone_right, "0.8", FCVAR_ARCHIVE, "CS VR: right stick deflection for turning and weapon changes" );
static CVAR_DEFINE_AUTO( vr_button_alt, "vr_button_x", FCVAR_ARCHIVE, "CS VR: button that switches the other buttons to their _alt commands while held" );
static CVAR_DEFINE_AUTO( vr_button_a, "+jump", FCVAR_ARCHIVE, "CS VR: right Cross/A" );
static CVAR_DEFINE_AUTO( vr_button_b, "+duck", FCVAR_ARCHIVE, "CS VR: right Circle/B" );
static CVAR_DEFINE_AUTO( vr_button_x, "drop", FCVAR_ARCHIVE, "CS VR: left Square/X" );
static CVAR_DEFINE_AUTO( vr_button_y, "+vr_scoreboard", FCVAR_ARCHIVE, "CS VR: left Triangle/Y" );
static CVAR_DEFINE_AUTO( vr_button_grip_left, "buy", FCVAR_ARCHIVE, "CS VR: left grip" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_press_left, "", FCVAR_ARCHIVE, "CS VR: left stick click" );
static CVAR_DEFINE_AUTO( vr_button_trigger_left, "+use", FCVAR_ARCHIVE, "CS VR: left trigger" );
static CVAR_DEFINE_AUTO( vr_button_grip_right, "+reload", FCVAR_ARCHIVE, "CS VR: right grip" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_dup_right, "+vr_weapon_prev", FCVAR_ARCHIVE, "CS VR: right stick up" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_ddown_right, "+vr_weapon_next", FCVAR_ARCHIVE, "CS VR: right stick down" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_dleft_right, "+vr_left", FCVAR_ARCHIVE, "CS VR: right stick left" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_dright_right, "+vr_right", FCVAR_ARCHIVE, "CS VR: right stick right" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_press_right, "+attack2", FCVAR_ARCHIVE, "CS VR: right stick click" );
static CVAR_DEFINE_AUTO( vr_button_trigger_right, "+attack", FCVAR_ARCHIVE, "CS VR: right trigger" );
static CVAR_DEFINE_AUTO( vr_button_a_alt, "", FCVAR_ARCHIVE, "CS VR: right Cross/A with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_b_alt, "", FCVAR_ARCHIVE, "CS VR: right Circle/B with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_x_alt, "", FCVAR_ARCHIVE, "CS VR: left Square/X with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_y_alt, "", FCVAR_ARCHIVE, "CS VR: left Triangle/Y with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_grip_left_alt, "", FCVAR_ARCHIVE, "CS VR: left grip with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_press_left_alt, "", FCVAR_ARCHIVE, "CS VR: left stick click with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_trigger_left_alt, "impulse 201", FCVAR_ARCHIVE, "CS VR: left trigger with the alt button held (spray)" );
static CVAR_DEFINE_AUTO( vr_button_grip_right_alt, "nightvision", FCVAR_ARCHIVE, "CS VR: right grip with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_dup_right_alt, "+vr_weapon_prev", FCVAR_ARCHIVE, "CS VR: right stick up with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_ddown_right_alt, "+vr_weapon_next", FCVAR_ARCHIVE, "CS VR: right stick down with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_dleft_right_alt, "+vr_left", FCVAR_ARCHIVE, "CS VR: right stick left with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_dright_right_alt, "+vr_right", FCVAR_ARCHIVE, "CS VR: right stick right with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_thumbstick_press_right_alt, "", FCVAR_ARCHIVE, "CS VR: right stick click with the alt button held" );
static CVAR_DEFINE_AUTO( vr_button_trigger_right_alt, "drop", FCVAR_ARCHIVE, "CS VR: right trigger with the alt button held" );

static convar_t *const vr_cs_cvars[] = {
	&vr_gamemode, &vr_hand_active, &vr_hand_x, &vr_hand_y, &vr_hand_z, &vr_hand_pitch, &vr_hand_yaw, &vr_hand_roll,
	&vr_hmd_pitch, &vr_hmd_yaw, &vr_hmd_roll, &vr_offset_x, &vr_offset_y, &vr_offset_z,
	&vr_weapon_x, &vr_weapon_y, &vr_weapon_z, &vr_weapon_roll, &vr_weapon_anim, &vr_weapon_throw_active,
	&vr_weapon_throw_pitch, &vr_weapon_throw_yaw, &vr_xhair_x, &vr_xhair_y, &vr_zoomed, &vr_superzoomed,
	&vr_zoomfix, &vr_zoom_by_motion, &vr_stereo_side, &vr_vignette_amount, &vr_force2d, &vr_hand_click,
	&vr_hand_stretch, &vr_player_pitch, &vr_player_yaw,
	&vr_hand_swap, &vr_haptics_weapon, &vr_haptics_yaw, &vr_ladder, &vr_shielded, &vr_spectator,
	&vr_weapon_calibration_update, &vr_weapon_pivot_name, &vr_weapon_pivot_x, &vr_weapon_pivot_y,
	&vr_weapon_pivot_z, &vr_weapon_pivot_pitch, &vr_weapon_pivot_yaw, &vr_weapon_pivot_scale,
	&vr_6dof, &vr_xhair, &vr_haptics_enable, &vr_aimdot, &vr_aimdot_scale, &vr_weapon_calibration_on, &vr_thumbstick_deadzone_right,
	&vr_button_alt, &vr_button_a, &vr_button_b, &vr_button_x, &vr_button_y, &vr_button_grip_left,
	&vr_button_thumbstick_press_left, &vr_button_trigger_left, &vr_button_grip_right,
	&vr_button_thumbstick_dup_right, &vr_button_thumbstick_ddown_right, &vr_button_thumbstick_dleft_right,
	&vr_button_thumbstick_dright_right, &vr_button_thumbstick_press_right, &vr_button_trigger_right,
	&vr_button_a_alt, &vr_button_b_alt, &vr_button_x_alt, &vr_button_y_alt, &vr_button_grip_left_alt,
	&vr_button_thumbstick_press_left_alt, &vr_button_trigger_left_alt, &vr_button_grip_right_alt,
	&vr_button_thumbstick_dup_right_alt, &vr_button_thumbstick_ddown_right_alt,
	&vr_button_thumbstick_dleft_right_alt, &vr_button_thumbstick_dright_right_alt,
	&vr_button_thumbstick_press_right_alt, &vr_button_trigger_right_alt,
};

/* controller state as CSVR's button mask */
enum
{
	CS_BUTTON_A        = BIT( 0 ), /* right primary (Cross) / left primary (Square) */
	CS_BUTTON_B        = BIT( 1 ), /* right secondary (Circle) / left secondary (Triangle) */
	CS_BUTTON_TRIGGER  = BIT( 2 ),
	CS_BUTTON_GRIP     = BIT( 3 ),
	CS_BUTTON_STICK    = BIT( 4 ),
	CS_BUTTON_UP       = BIT( 5 ),
	CS_BUTTON_DOWN     = BIT( 6 ),
	CS_BUTTON_LEFT     = BIT( 7 ),
	CS_BUTTON_RIGHT    = BIT( 8 ),
};

static qboolean vr_cs_mode;
static qboolean vr_cs_scope;
static uint32_t vr_cs_last_buttons[REF_VR_MAX_HANDS]; /* [weapon hand, off hand] */
static float vr_cs_gun_yaw;     /* weapon-hand yaw relative to the recentre heading, applied last frame */
static qboolean vr_cs_gun_valid;
/* The body yaw is kept here, not derived from the view angles: the client's recoil code nudges the
 * view angles when firing, and taking that into the body turned the world with every shot. Only
 * turning and the server's setangle change it. */
static float vr_cs_body_yaw;
static qboolean vr_cs_body_valid;
static float vr_cs_stick_input[2]; /* +vr_left/right, +vr_weapon_prev/next */
static qboolean vr_cs_weapon_change_down;
static qboolean vr_cs_turn_latched;
static qboolean vr_cs_attack_from_use;
static int vr_cs_last_health;

qboolean CL_VRCSMode( void )
{
	return vr_cs_mode;
}

void CL_VRCSRegisterCvars( void )
{
	vr_cs_mode = !Q_stricmp( GI->gamefolder, "cstrike" ) || !Q_stricmp( GI->gamefolder, "czero" );
	if( !vr_cs_mode )
		return;
	for( size_t i = 0; i < ARRAYSIZE( vr_cs_cvars ); ++i )
		Cvar_RegisterVariable( vr_cs_cvars[i] );
}

static int CL_VRCSWeaponHand( void )
{
	return Cvar_VariableValue( "vr_control_scheme" ) >= 10.0f ? REF_VR_HAND_LEFT : REF_VR_HAND_RIGHT;
}

/* pose relative to the recentre point: position in metres (forward, left, up), Quake angles */
static qboolean CL_VRCSRelative( const ref_vr_pose_t *pose, vec3_t position, vec3_t angles )
{
	ref_vr_pose_t center;
	vec3_t delta, forward, right, up;

	if( !CL_VRGetRecenter( &center ))
		return false;
	VectorSubtract( pose->position, center.position, delta );
	position[0] = DotProduct( delta, center.forward );
	position[1] = -DotProduct( delta, center.right );
	position[2] = DotProduct( delta, center.up );
	forward[0] = DotProduct( pose->forward, center.forward );
	forward[1] = -DotProduct( pose->forward, center.right );
	forward[2] = DotProduct( pose->forward, center.up );
	right[0] = DotProduct( pose->right, center.forward );
	right[1] = -DotProduct( pose->right, center.right );
	right[2] = DotProduct( pose->right, center.up );
	up[0] = DotProduct( pose->up, center.forward );
	up[1] = -DotProduct( pose->up, center.right );
	up[2] = DotProduct( pose->up, center.up );
	VectorsAngles( forward, right, up, angles );
	return true;
}

/* a - b, wrapped to -180..180 */
static float CL_VRCSAngleDiff( float a, float b )
{
	float d = anglemod( a - b );
	return d > 180.0f ? d - 360.0f : d;
}

static const ref_vr_pose_t *CL_VRCSHandPose( const ref_vr_hand_t *hand )
{
	if( FBitSet( hand->flags, REF_VR_HAND_AIM_VALID ))
		return &hand->aim;
	if( FBitSet( hand->flags, REF_VR_HAND_GRIP_VALID ))
		return &hand->grip;
	return NULL;
}

/* Weapon and off-hand poses after CSVR's swaps: with the shield up the off hand aims, with dual
 * elites the active gun alternates between the hands. */
static qboolean CL_VRCSHands( vec3_t weapon_pos, vec3_t weapon_ang, vec3_t hand_pos, vec3_t hand_ang )
{
	const ref_vr_frame_t *frame = CL_VRGetFrame();
	int weapon_hand = CL_VRCSWeaponHand();
	const ref_vr_pose_t *weapon = CL_VRCSHandPose( &frame->hands[weapon_hand] );
	const ref_vr_pose_t *hand = CL_VRCSHandPose( &frame->hands[!weapon_hand] );

	if( !weapon || !CL_VRCSRelative( weapon, weapon_pos, weapon_ang ))
		return false;
	if( !hand || !CL_VRCSRelative( hand, hand_pos, hand_ang ))
	{
		VectorCopy( weapon_pos, hand_pos );
		VectorCopy( weapon_ang, hand_ang );
	}
	if( vr_shielded.value > 0.0f )
	{
		VectorCopy( hand_pos, weapon_pos );
		VectorCopy( hand_ang, weapon_ang );
	}
	else if( !Q_strcmp( vr_weapon_pivot_name.string, "models/v_elite.mdl" ) && vr_hand_swap.value > 0.5f )
	{
		vec3_t pos, ang;
		VectorCopy( hand_pos, pos );
		VectorCopy( hand_ang, ang );
		VectorCopy( weapon_pos, hand_pos );
		VectorCopy( weapon_ang, hand_ang );
		VectorCopy( pos, weapon_pos );
		VectorCopy( ang, weapon_ang );
	}
	return true;
}

static uint32_t CL_VRCSButtons( const ref_vr_hand_t *hand, float stick_deadzone )
{
	uint32_t buttons = 0;

	if( FBitSet( hand->buttons, REF_VR_BUTTON_PRIMARY )) SetBits( buttons, CS_BUTTON_A );
	if( FBitSet( hand->buttons, REF_VR_BUTTON_SECONDARY )) SetBits( buttons, CS_BUTTON_B );
	if( FBitSet( hand->buttons, REF_VR_BUTTON_STICK )) SetBits( buttons, CS_BUTTON_STICK );
	if( hand->trigger >= 0.55f ) SetBits( buttons, CS_BUTTON_TRIGGER );
	if( FBitSet( hand->flags, REF_VR_HAND_SQUEEZE_PRESSED )) SetBits( buttons, CS_BUTTON_GRIP );
	if( stick_deadzone > 0.0f )
	{
		if( hand->stick[1] > stick_deadzone ) SetBits( buttons, CS_BUTTON_UP );
		else if( hand->stick[1] < -stick_deadzone ) SetBits( buttons, CS_BUTTON_DOWN );
		if( hand->stick[0] > stick_deadzone ) SetBits( buttons, CS_BUTTON_RIGHT );
		else if( hand->stick[0] < -stick_deadzone ) SetBits( buttons, CS_BUTTON_LEFT );
	}
	return buttons;
}

/* one command of a vr_button_* list (CSVR's Host_VRCustomCommand) */
static void CL_VRCSCommand( const char *command, qboolean release )
{
	char line[256];

	if( release )
	{
		if( command[0] != '+' )
			return;
		Q_snprintf( line, sizeof( line ), "-%s", command + 1 );
	}
	else Q_strncpy( line, command, sizeof( line ));

	if( !Q_strcmp( line, "+vr_left" )) vr_cs_stick_input[0] = -1.0f;
	else if( !Q_strcmp( line, "+vr_right" )) vr_cs_stick_input[0] = 1.0f;
	else if( !Q_strcmp( line, "-vr_left" ) || !Q_strcmp( line, "-vr_right" )) vr_cs_stick_input[0] = 0.0f;
	else if( !Q_strcmp( line, "+vr_weapon_next" )) vr_cs_stick_input[1] = -1.0f;
	else if( !Q_strcmp( line, "+vr_weapon_prev" )) vr_cs_stick_input[1] = 1.0f;
	else if( !Q_strcmp( line, "-vr_weapon_next" ) || !Q_strcmp( line, "-vr_weapon_prev" )) vr_cs_stick_input[1] = 0.0f;
	else if( !Q_strcmp( line, "+vr_scoreboard" )) Cbuf_AddText( "+showscores\n" );
	else if( !Q_strcmp( line, "-vr_scoreboard" )) Cbuf_AddText( "-showscores\n" );
	else if( !Q_strcmp( line, "+use" ))
	{
		/* dual elites: the off-hand trigger fires the left gun */
		if( !Q_strcmp( vr_weapon_pivot_name.string, "models/v_elite.mdl" ))
		{
			Cbuf_AddText( "+attack\n" );
			vr_cs_attack_from_use = true;
		}
		else Cvar_DirectSet( &vr_hand_click, "1" );
		Cbuf_AddText( "+use\n" );
	}
	else if( !Q_strcmp( line, "-use" ))
	{
		if( vr_cs_attack_from_use )
		{
			Cbuf_AddText( "-attack\n" );
			vr_cs_attack_from_use = false;
		}
		Cvar_DirectSet( &vr_hand_click, "0" );
		Cbuf_AddText( "-use\n" );
	}
	else
	{
		if( !Q_strcmp( line, "+attack2" ))
			Cvar_DirectSet( &vr_zoom_by_motion, "0" );
		Cbuf_AddText( line );
		Cbuf_AddText( "\n" );
	}
}

/* press/release edge of one button: run its ';'-separated commands (releases only undo '+' ones) */
static void CL_VRCSButton( uint32_t button, uint32_t now, uint32_t before, const convar_t *normal,
	const convar_t *alternative, qboolean alt )
{
	const char *list;
	char command[256];
	size_t length = 0;
	qboolean down = FBitSet( now, button ) != 0, was_down = FBitSet( before, button ) != 0;

	if( down == was_down )
		return;
	list = alt ? alternative->string : normal->string;
	for( const char *p = list; ; ++p )
	{
		if( *p == ';' || *p == '\0' )
		{
			command[length] = '\0';
			while( length > 0 && command[length - 1] == ' ' )
				command[--length] = '\0';
			if( length )
				CL_VRCSCommand( command[0] == ' ' ? command + 1 : command, !down );
			length = 0;
			if( *p == '\0' )
				break;
		}
		else if( length < sizeof( command ) - 1 )
			command[length++] = *p;
	}
}

static qboolean CL_VRCSAltHeld( uint32_t weapon, uint32_t offhand )
{
	static const struct { const char *name; int hand; uint32_t button; } alt_buttons[] = {
		{ "vr_button_x", 1, CS_BUTTON_A }, { "vr_button_y", 1, CS_BUTTON_B },
		{ "vr_button_trigger_left", 1, CS_BUTTON_TRIGGER }, { "vr_button_thumbstick_press_left", 1, CS_BUTTON_STICK },
		{ "vr_button_grip_left", 1, CS_BUTTON_GRIP }, { "vr_button_a", 0, CS_BUTTON_A }, { "vr_button_b", 0, CS_BUTTON_B },
		{ "vr_button_trigger_right", 0, CS_BUTTON_TRIGGER }, { "vr_button_thumbstick_dup_right", 0, CS_BUTTON_UP },
		{ "vr_button_thumbstick_ddown_right", 0, CS_BUTTON_DOWN }, { "vr_button_thumbstick_dleft_right", 0, CS_BUTTON_LEFT },
		{ "vr_button_thumbstick_dright_right", 0, CS_BUTTON_RIGHT }, { "vr_button_thumbstick_press_right", 0, CS_BUTTON_STICK },
		{ "vr_button_grip_right", 0, CS_BUTTON_GRIP },
	};

	for( size_t i = 0; i < ARRAYSIZE( alt_buttons ); ++i )
	{
		if( Q_strcmp( vr_button_alt.string, alt_buttons[i].name ))
			continue;
		return FBitSet( alt_buttons[i].hand ? offhand : weapon, alt_buttons[i].button ) != 0;
	}
	return false;
}

/* weapon = buttons of the weapon hand ("right" in CSVR's names), offhand = the other hand */
static void CL_VRCSButtonMapping( uint32_t weapon, uint32_t offhand )
{
	qboolean alt = CL_VRCSAltHeld( weapon, offhand );
	uint32_t old_offhand = vr_cs_last_buttons[1], old_weapon = vr_cs_last_buttons[0];

	CL_VRCSButton( CS_BUTTON_A, offhand, old_offhand, &vr_button_x, &vr_button_x_alt, alt );
	CL_VRCSButton( CS_BUTTON_B, offhand, old_offhand, &vr_button_y, &vr_button_y_alt, alt );
	CL_VRCSButton( CS_BUTTON_TRIGGER, offhand, old_offhand, &vr_button_trigger_left, &vr_button_trigger_left_alt, alt );
	CL_VRCSButton( CS_BUTTON_STICK, offhand, old_offhand, &vr_button_thumbstick_press_left, &vr_button_thumbstick_press_left_alt, alt );
	CL_VRCSButton( CS_BUTTON_GRIP, offhand, old_offhand, &vr_button_grip_left, &vr_button_grip_left_alt, alt );
	CL_VRCSButton( CS_BUTTON_A, weapon, old_weapon, &vr_button_a, &vr_button_a_alt, alt );
	CL_VRCSButton( CS_BUTTON_B, weapon, old_weapon, &vr_button_b, &vr_button_b_alt, alt );
	CL_VRCSButton( CS_BUTTON_TRIGGER, weapon, old_weapon, &vr_button_trigger_right, &vr_button_trigger_right_alt, alt );
	CL_VRCSButton( CS_BUTTON_UP, weapon, old_weapon, &vr_button_thumbstick_dup_right, &vr_button_thumbstick_dup_right_alt, alt );
	CL_VRCSButton( CS_BUTTON_DOWN, weapon, old_weapon, &vr_button_thumbstick_ddown_right, &vr_button_thumbstick_ddown_right_alt, alt );
	CL_VRCSButton( CS_BUTTON_LEFT, weapon, old_weapon, &vr_button_thumbstick_dleft_right, &vr_button_thumbstick_dleft_right_alt, alt );
	CL_VRCSButton( CS_BUTTON_RIGHT, weapon, old_weapon, &vr_button_thumbstick_dright_right, &vr_button_thumbstick_dright_right_alt, alt );
	CL_VRCSButton( CS_BUTTON_STICK, weapon, old_weapon, &vr_button_thumbstick_press_right, &vr_button_thumbstick_press_right_alt, alt );
	CL_VRCSButton( CS_BUTTON_GRIP, weapon, old_weapon, &vr_button_grip_right, &vr_button_grip_right_alt, alt );
	vr_cs_last_buttons[0] = weapon;
	vr_cs_last_buttons[1] = offhand;
}

qboolean CL_VRCSScopeActive( void )
{
	return vr_cs_mode && vr_cs_scope;
}

void CL_VRCSReleaseInput( void )
{
	if( !vr_cs_mode )
		return;
	vr_cs_scope = false;
	CL_VRSetScope( false, 0 );
	if( vr_cs_last_buttons[0] || vr_cs_last_buttons[1] )
		CL_VRCSButtonMapping( 0, 0 );
	vr_cs_stick_input[0] = vr_cs_stick_input[1] = 0.0f;
	if( vr_cs_weapon_change_down )
		Cbuf_AddText( "-attack\n" );
	vr_cs_weapon_change_down = false;
	vr_cs_turn_latched = false;
	Cvar_DirectSet( &vr_gamemode, "0" );
}

/* CSVR's Host_VRWeaponCalibration: right stick left/right picks the value, up/down changes it */
static qboolean CL_VRCSCalibration( float x, float y )
{
	static const convar_t *axes[] = { &vr_weapon_pivot_x, &vr_weapon_pivot_y, &vr_weapon_pivot_z,
		&vr_weapon_pivot_pitch, &vr_weapon_pivot_yaw, &vr_weapon_pivot_scale };
	static int axis;
	static qboolean axis_down, value_down;
	float deadzone = vr_thumbstick_deadzone_right.value;
	char text[128];

	if( vr_weapon_calibration_on.value <= 0.0f )
		return false;
	if( fabs( x ) > deadzone && !axis_down )
		axis = bound( 0, axis + ( x > 0.0f ? 1 : -1 ), (int)ARRAYSIZE( axes ) - 1 );
	axis_down = fabs( x ) > deadzone;
	if( fabs( y ) > deadzone && !value_down )
	{
		Cvar_DirectSetValue( (convar_t *)axes[axis], axes[axis]->value + ( y > 0.0f ? 0.5f : -0.5f ));
		Cvar_DirectSet( &vr_weapon_calibration_update, "1" );
	}
	value_down = fabs( y ) > deadzone;
	Q_snprintf( text, sizeof( text ), "%s: %.1f", axes[axis]->name, axes[axis]->value );
	CL_CenterPrint( text, 0.5f );
	return true;
}

static void CL_VRCSHaptics( int weapon_hand )
{
	float power = vr_haptics_weapon.value;

	if( vr_haptics_enable.value < 0.5f || vr_spectator.value > 0.5f )
	{
		vr_cs_last_health = cl.local.health;
		return;
	}
	if( power > 0.0f )
	{
		/* dual elites: the gun that fired is in the other hand while the left one is active */
		int hand = vr_hand_swap.value > 0.5f ? !weapon_hand : weapon_hand;
		CL_VRHaptic( hand, 0.0f, 0.0f, 0.0f );
		CL_VRHaptic( hand, bound( 0.03f, power * 0.5f, 0.25f ), 0.0f, bound( 0.3f, power * 2.0f, 1.0f ));
		Cvar_DirectSet( &vr_haptics_weapon, "0" );
	}
	if( cl.local.health < vr_cs_last_health && cl.local.health > 0 )
	{
		float duration = bound( 0.05f, ( vr_cs_last_health - cl.local.health ) / 50.0f, 0.5f );
		for( int hand = 0; hand < REF_VR_MAX_HANDS; ++hand )
		{
			CL_VRHaptic( hand, 0.0f, 0.0f, 0.0f );
			CL_VRHaptic( hand, duration, 0.0f, 0.8f );
		}
	}
	vr_cs_last_health = cl.local.health;
}

/* Weapons without a useful aim point (CSVR hides its crosshair for these too). */
static qboolean CL_VRCSAimlessWeapon( void )
{
	static const char *names[] = { "v_knife.mdl", "v_flashbang.mdl", "v_hegrenade.mdl", "v_smokegrenade.mdl",
		"v_c4.mdl", "v_shield_knife.mdl", "v_shield_flashbang.mdl", "v_shield_hegrenade.mdl", "v_shield_smokegrenade.mdl" };
	const char *weapon = COM_FileWithoutPath( vr_weapon_pivot_name.string );

	if( !weapon[0] )
		return true;
	for( size_t i = 0; i < ARRAYSIZE( names ); ++i )
	{
		if( !Q_stricmp( weapon, names[i] ))
			return true;
	}
	return false;
}

/* A glowing laser dot where the shot lands. The server fires from the eye along the view angles
 * (= the gun's direction), not from the barrel, so trace exactly that. A new temp entity every
 * frame; the previous one is expired when it is still ours (same model and position), so a
 * reused slot after a map change is never touched. */
static TEMPENTITY *vr_cs_dot_ent;
static vec3_t vr_cs_dot_origin;

static void CL_VRCSExpireDot( model_t *dot )
{
	if( vr_cs_dot_ent && vr_cs_dot_ent->entity.model == dot && VectorCompare( vr_cs_dot_ent->entity.origin, vr_cs_dot_origin ))
		vr_cs_dot_ent->die = 0.0f;
	vr_cs_dot_ent = NULL;
}

static void CL_VRCSAimDot( const vec3_t weapon_ang )
{
	vec3_t start, end, angles, forward;
	model_t *dot;
	TEMPENTITY *ent;
	pmtrace_t trace;

	dot = Mod_ForName( "sprites/laserdot.spr", false, false );
	if( !dot )
		return;
	CL_VRCSExpireDot( dot );
	if( vr_aimdot.value == 0.0f || vr_spectator.value > 0.5f || cl.local.health <= 0 ||
		vr_zoomed.value > 0.5f || CL_VRCSAimlessWeapon( ))
		return;

	VectorAdd( cl.simorg, cl.viewheight, start );
	VectorSet( angles, weapon_ang[PITCH], CL_VRCSBodyYaw() + weapon_ang[YAW], 0.0f );
	AngleVectors( angles, forward, NULL, NULL );
	VectorMA( start, 8192.0f, forward, end );
	trace = CL_TraceLine( start, end, PM_STUDIO_BOX );
	if( trace.fraction >= 1.0f )
		return;
	/* pull it off the surface so it is not half inside the wall */
	VectorMA( trace.endpos, -2.0f, forward, end );

	ent = CL_TempEntAlloc( end, dot );
	if( !ent )
		return;
	ent->die = cl.time + 0.1f;
	ent->flags = FTENT_NONE;
	/* additive and depth-tested: glow sprites go through an occlusion test that hid the dot;
	 * scaled with distance instead so it keeps the same angular size */
	ent->entity.curstate.rendermode = kRenderTransAdd;
	ent->entity.curstate.renderfx = kRenderFxNone;
	ent->entity.curstate.renderamt = 255;
	ent->entity.curstate.rendercolor.r = 255;
	ent->entity.curstate.rendercolor.g = 255;
	ent->entity.curstate.rendercolor.b = 255;
	ent->entity.curstate.scale = vr_aimdot_scale.value * trace.fraction * 8192.0f / 100.0f;
	vr_cs_dot_ent = ent;
	VectorCopy( end, vr_cs_dot_origin );
}

/* the offsets the client adds to the eye origin, in the head's flattened frame (see the header) */
static void CL_VRCSSetOffset( convar_t *x, convar_t *y, convar_t *z, const vec3_t position, float head_yaw )
{
	float scale = CL_VRGetWorldScale();
	float sine, cosine;
	float height = Cvar_VariableValue( "vr_height_adjust" );

	SinCos( DEG2RAD( head_yaw ), &sine, &cosine );
	/* head forward = (cos, sin) and right = (sin, -cos) in the recentre frame (forward, left) */
	Cvar_DirectSetValue( x, -scale * ( position[0] * sine - position[1] * cosine ));
	Cvar_DirectSetValue( y, -scale * ( position[0] * cosine + position[1] * sine ));
	Cvar_DirectSetValue( z, scale * ( position[2] + height ));
}

void CL_VRCSFrame( qboolean ui_active )
{
	const ref_vr_frame_t *frame = CL_VRGetFrame();
	int weapon_hand = CL_VRCSWeaponHand();
	vec3_t weapon_pos, weapon_ang, hand_pos, hand_ang, head_pos, head_ang;
	qboolean game = !ui_active && vr_force2d.value < 0.5f && cls.state == ca_active;
	float stick_x, stick_y;

	/* the client reads cl_righthand; the weapon hand follows vr_control_scheme like Lambda1VR */
	if(( Cvar_VariableValue( "cl_righthand" ) != 0.0f ) != ( weapon_hand == REF_VR_HAND_RIGHT ))
		Cvar_SetValue( "cl_righthand", weapon_hand == REF_VR_HAND_RIGHT ? 1.0f : 0.0f );

	if( !game )
	{
		if( cls.state != ca_active )
		{
			vr_cs_body_valid = false; /* new map: take the spawn direction again */
			vr_cs_dot_ent = NULL;
		}
		CL_VRCSReleaseInput();
		return;
	}
	Cvar_DirectSet( &vr_gamemode, "1" );
	{
		/* the classic crosshair ignores vr_xhair; CSVR's engine forces the client's own one too */
		convar_t *xhair = Cvar_FindVar( "xhair_enable" );
		if( xhair && xhair->value == 0.0f )
			Cvar_DirectSet( xhair, "1" );
	}

	/* Zoomed (AWP, Scout, autosnipers, AUG/SG552): the engine renders the zoomed view onto a lens in
	 * front of the weapon hand; the client's 2D scope overlay stays off (vr_zoomfix), the head keeps
	 * its normal view. */
	vr_cs_scope = cl.local.scr_fov > 1.0f && cl.local.scr_fov < 80.0f && vr_spectator.value < 0.5f && cl.local.health > 0;
	if(( vr_zoomed.value != 0.0f ) != vr_cs_scope )
		Cvar_DirectSet( &vr_zoomed, vr_cs_scope ? "1" : "0" );
	if( vr_zoomfix.value == 0.0f )
		Cvar_DirectSet( &vr_zoomfix, "1" );
	CL_VRSetScope( vr_cs_scope, vr_shielded.value > 0.0f ? !weapon_hand : weapon_hand );
	if( vr_xhair_x.value != 0.0f || vr_xhair_y.value != 0.0f )
	{
		Cvar_DirectSet( &vr_xhair_x, "0" );
		Cvar_DirectSet( &vr_xhair_y, "0" );
	}

	stick_x = frame->hands[weapon_hand].stick[0];
	stick_y = frame->hands[weapon_hand].stick[1];
	if( CL_VRCSCalibration( stick_x, stick_y ))
	{
		CL_VRCSButtonMapping( CL_VRCSButtons( &frame->hands[weapon_hand], 0.0f ), CL_VRCSButtons( &frame->hands[!weapon_hand], 0.0f ));
		vr_cs_stick_input[0] = vr_cs_stick_input[1] = 0.0f;
	}
	else
	{
		uint32_t offhand = CL_VRCSButtons( &frame->hands[!weapon_hand], 0.0f );
		/* the left stick moves: its click must not fire while the stick is being pushed */
		if( fabs( frame->hands[!weapon_hand].stick[0] ) > 0.15f || fabs( frame->hands[!weapon_hand].stick[1] ) > 0.15f )
			ClearBits( offhand, CS_BUTTON_STICK & ~vr_cs_last_buttons[1] );
		CL_VRCSButtonMapping( CL_VRCSButtons( &frame->hands[weapon_hand], vr_thumbstick_deadzone_right.value ), offhand );
	}

	/* stick up/down: select the next/previous weapon at once (the HUD weapon menu confirms on +attack) */
	{
		qboolean down = vr_cs_stick_input[1] != 0.0f;
		if( down && !vr_cs_weapon_change_down )
		{
			Cbuf_AddText( vr_cs_stick_input[1] > 0.0f ? "invprev\n" : "invnext\n" );
			Cbuf_AddText( "+attack\n" );
		}
		else if( !down && vr_cs_weapon_change_down )
			Cbuf_AddText( "-attack\n" );
		vr_cs_weapon_change_down = down;
	}

	if( !FBitSet( frame->flags, REF_VR_FRAME_HEAD_VALID ) || !CL_VRCSRelative( &frame->head, head_pos, head_ang ) ||
		!CL_VRCSHands( weapon_pos, weapon_ang, hand_pos, hand_ang ))
		return;

	Cvar_DirectSetValue( &vr_hmd_pitch, head_ang[PITCH] );
	Cvar_DirectSetValue( &vr_hmd_roll, head_ang[ROLL] );
	Cvar_DirectSetValue( &vr_hmd_yaw, CL_VRCSBodyYaw() + head_ang[YAW] );
	Cvar_DirectSetValue( &vr_weapon_roll, weapon_ang[ROLL] );
	Cvar_DirectSetValue( &vr_hand_pitch, hand_ang[PITCH] );
	Cvar_DirectSetValue( &vr_hand_yaw, CL_VRCSAngleDiff( hand_ang[YAW], weapon_ang[YAW] ));
	Cvar_DirectSetValue( &vr_hand_roll, hand_ang[ROLL] );
	CL_VRCSSetOffset( &vr_weapon_x, &vr_weapon_y, &vr_weapon_z, weapon_pos, head_ang[YAW] );
	CL_VRCSSetOffset( &vr_hand_x, &vr_hand_y, &vr_hand_z, hand_pos, head_ang[YAW] );
	CL_VRCSHaptics( weapon_hand );
	CL_VRCSAimDot( weapon_ang );
}

float CL_VRCSBodyYaw( void )
{
	if( !vr_cs_body_valid )
		return anglemod( cl.viewangles[YAW] - ( vr_cs_gun_valid ? vr_cs_gun_yaw : 0.0f ));
	return vr_cs_body_yaw;
}

void CL_VRCSSetAngle( float view_yaw )
{
	if( !vr_cs_mode )
		return;
	vr_cs_body_yaw = anglemod( view_yaw - ( vr_cs_gun_valid ? vr_cs_gun_yaw : 0.0f ));
	vr_cs_body_valid = true;
}

static float CL_VRCSFilterAxis( float value )
{
	const float deadzone = 0.2f;
	float magnitude = fabs( value );

	if( magnitude <= deadzone ) return 0.0f;
	magnitude = ( magnitude - deadzone ) / ( 1.0f - deadzone );
	return copysign( magnitude * magnitude, value );
}

void CL_VRCSAppendMove( float frametime, usercmd_t *cmd, const vec3_t head_delta )
{
	const ref_vr_frame_t *frame = CL_VRGetFrame();
	int weapon_hand = CL_VRCSWeaponHand();
	const ref_vr_hand_t *move = &frame->hands[!weapon_hand];
	vec3_t weapon_pos, weapon_ang, hand_pos, hand_ang, head_pos, head_ang;
	float body_yaw, x, y, heading, sine, cosine, forward_speed, side_speed;

	if( vr_gamemode.value < 0.5f || !CL_VRCSHands( weapon_pos, weapon_ang, hand_pos, hand_ang ))
		return;
	if( !FBitSet( frame->flags, REF_VR_FRAME_HEAD_VALID ) || !CL_VRCSRelative( &frame->head, head_pos, head_ang ))
		VectorCopy( weapon_ang, head_ang );

	/* the gun moves the view on top of the body; turning and the server's setangle move the body */
	body_yaw = CL_VRCSBodyYaw();
	if( vr_cs_stick_input[0] != 0.0f )
	{
		float turn = Cvar_VariableValue( "vr_turn_angle" );
		if( Cvar_VariableValue( "vr_smoothturn" ) != 0.0f )
			body_yaw -= vr_cs_stick_input[0] * turn * 8.0f * frametime * fabs( frame->hands[weapon_hand].stick[0] );
		else if( !vr_cs_turn_latched )
			body_yaw -= vr_cs_stick_input[0] * turn;
		vr_cs_turn_latched = true;
	}
	else vr_cs_turn_latched = false;

	vr_cs_body_yaw = anglemod( body_yaw );
	vr_cs_body_valid = true;
	cmd->viewangles[YAW] = anglemod( body_yaw + weapon_ang[YAW] );
	cmd->viewangles[PITCH] = bound( -89.0f, weapon_ang[PITCH], 89.0f );
	cmd->viewangles[ROLL] = 0.0f;
	vr_cs_gun_yaw = weapon_ang[YAW];
	vr_cs_gun_valid = true;

	/* left stick: walk where the head looks (vr_walkdirection 1) or the off hand points, in the
	 * gun's frame because the view angles are the gun's */
	x = CL_VRCSFilterAxis( move->stick[0] );
	y = CL_VRCSFilterAxis( move->stick[1] );
	if( Cvar_VariableValue( "vr_walkdirection" ) >= 0.5f )
		heading = head_ang[YAW] - weapon_ang[YAW];
	else heading = hand_ang[YAW] - weapon_ang[YAW];
	SinCos( DEG2RAD( heading ), &sine, &cosine );
	forward_speed = Cvar_VariableValue( "cl_forwardspeed" );
	side_speed = Cvar_VariableValue( "cl_sidespeed" );
	if( forward_speed <= 0.0f ) forward_speed = 400.0f;
	if( side_speed <= 0.0f ) side_speed = 400.0f;
	cmd->sidemove += ( x * cosine - y * sine ) * side_speed;
	cmd->forwardmove += ( y * cosine + x * sine ) * forward_speed;

	/* room-scale: the body follows the head (the recentre point moved with it, vr_client.c) */
	if( vr_6dof.value != 0.0f && frametime > 0.0f && head_delta )
	{
		float scale = CL_VRGetWorldScale() / frametime;
		SinCos( DEG2RAD( -weapon_ang[YAW] ), &sine, &cosine );
		cmd->forwardmove += bound( -forward_speed, ( head_delta[0] * cosine - head_delta[1] * sine ) * scale, forward_speed );
		cmd->sidemove -= bound( -side_speed, ( head_delta[0] * sine + head_delta[1] * cosine ) * scale, side_speed );
	}

	if( cl.local.maxspeed > 0.0f )
	{
		float speed = sqrt( cmd->forwardmove * cmd->forwardmove + cmd->sidemove * cmd->sidemove );
		if( speed > cl.local.maxspeed )
		{
			cmd->forwardmove *= cl.local.maxspeed / speed;
			cmd->sidemove *= cl.local.maxspeed / speed;
		}
	}
	VectorCopy( cmd->viewangles, cl.viewangles );
	Cvar_DirectSetValue( &vr_player_pitch, cmd->viewangles[PITCH] );
	Cvar_DirectSetValue( &vr_player_yaw, cmd->viewangles[YAW] );
}
