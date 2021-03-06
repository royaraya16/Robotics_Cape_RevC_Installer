/*
Copyright (c) 2014, James Strawson
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer. 
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies, 
either expressed or implied, of the FreeBSD Project.
*/

//	fly.c
//	see README.txt for description and use


/********************************************
* 			Inlcudes & Constants			*
*********************************************/
// Includes
#include <robotics_cape.h>
#include "filter_lib.h"				// for discrete filters and controllers
#include "flight_core_config.h"		// for loading and saving settings
#include "flight_core_logger.h"		// for logging control loop data

// Flight Core Constants
#define CONTROL_HZ 			200		// Run the main control loop at this rate
#define DT 				   .005		// timestep seconds MUST MATCH CONTROL_HZ
#define	STATE_LEN 			32		// number of timesteps to retain data
#define MAX_YAW_COMPONENT	0.21 	// Max control delta the controller can apply
#define MAX_THRUST_COMPONENT 0.8	// upper limit of net thrust input
#define MAX_ROLL_COMPONENT	0.2 	// Max control delta the controller can apply
#define MAX_PITCH_COMPONENT	0.2 	// Max control delta the controller can apply
#define INT_CUTOFF_TH 		0.3		// prevent integrators from running unless flying
#define YAW_CUTOFF_TH 		0.1		// prevent yaw from changing when grounded
#define ARM_TIP_THRESHOLD	0.2		// radians from level to allow arming sequence 
#define LAND_SATURATION 	0.05	// saturation of roll, yaw, pitch controllers
									// while landed

// Flight Stack Constants
#define TIP_THRESHOLD 		1.5		// Kill propellers if it rolls or pitches past this
#define DSM2_LAND_TIMEOUT	0.3 	// seconds before going into emergency land mode
#define DSM2_DISARM_TIMEOUT	5.0		// seconds before disarming motors completely 
#define EMERGENCY_LAND_THR  0.15	// throttle to hold at when emergency landing


/************************************************************************
*	Type Definitions 
*************************************************************************/

/************************************************************************
* 	flight_mode_t
*	
*	user_interface.flight_mode determines how the flight stack behaves
*
*	EMERGENCY_KILL: kill motors and reset the flight core controllers
*
*	EMERGENCY_LAND: slowly decrease altitude in place until touchdown
*	
*	USER_ATTITUDE: gives the user direct joystick control of the inner-loop
*	throttle, yaw rate, and roll/pitch attitude
*
*
*	TODO: future modes
*
*	LOITER: sets the flight_core to position mode and updates the position
*	setpoint based on user inputs such that the user joystick controls 
*	velocity from the perspective of the UAV. This would be the most_useful
*	mode when flying First-Person view.
*	
*	USER_POSITION_CARTESIAN: Similar to jog mode on a CNC mill. The user 
*	controls the global position setpoint using the ARMING location as
*	the origin with positive Y facing forward and X to the right
*
*	USER_POSITION_RADIAL: Left/Right forward/back are from the perspective 
*	of the pilot at takeoff location.
*
************************************************************************/
typedef enum flight_mode_t{
	EMERGENCY_KILL,
	EMERGENCY_LAND,
	USER_ATTITUDE,
	USER_LOITER,
	USER_POSITION_CARTESIAN,
	USER_POSITION_RADIAL,
	TARGET_HOLD,
}flight_mode_t;

/************************************************************************
* 	core_mode_t
*	
*	DISARMED: no signal will ever go to ESCs
*
*	ATTITUDE: The controller will read throttle, roll, pitch, and yaw_rate
*	setpoints so the user has direct control of inner attitude control loop.
*	The yaw controller will still hold an absolute position but the 
*	yaw_setpoint will be updated by the flight_core based on the yaw_rate
*	setpoint.
*
*	POSITION: The controller will instead read the absolute global position
*	inside core_setpoint and modulate attitude to maintain position via
*	successive loop closure. This means the continuously changing attitude
*	setpoint can be read back by other threads.
************************************************************************/
typedef enum core_mode_t{
	DISARMED,
	ATTITUDE,
	POSITION,
}core_mode_t;


/************************************************************************
* 	core_setpoint_t
*	setpoint for the flight_core attitude controller
*	This is controlled by the flight stack and read by the flight core	
************************************************************************/
typedef struct core_setpoint_t{
	
	core_mode_t core_mode;	// see core_state_t declaration
	
	// attitude setpoint
	float throttle;			// desired upward motor thrust
	float roll;				// roll angle (rad)
	float pitch;			// pitch angle (rad)
	float yaw_rate;			// yaw_rate in rad/s
	
	// Cartesian position setpoint from arming location (m)
	float altitude;			// altitude 	
	float position_X;		// horizontal displacement since arming
	float position_Y;		// forward/back displacement since arming
	float yaw;				// yaw angle displacement since arming
}core_setpoint_t;

/************************************************************************
* 	core_state_t
*	contains most recent values reported by the flight_core
*	Should only be written to by the flight core after initialization		
************************************************************************/
typedef struct core_state_t{
	unsigned long control_loops; 	// number of loops since flight core started
	float altitude;					// altitude estimate (m)
	float roll;						// current roll angle (rad)
	float pitch;					// current pitch angle (rad)
	float yaw;						// current yaw angle (rad)
	float last_yaw;					// previous value for crossover detection
	
	float dAltitude;				// first derivative of altitude (m/s)
	float dRoll;					// first derivative of roll (rad/s)
	float dPitch;					// first derivative of pitch (rad/s)
	float dYaw;						// first derivative of yaw (rad/s)
	
	float v_batt;					// main battery pack voltage
	float positionX;				// estimate of X displacement from takeoff (m)
	float positionY;				// estimate of Y displacement from takeoff (m)
	
	float alt_err; 					// current and previous altitudes error
	float dRoll_err; 				// current and previous roll error
	float dPitch_err;				// current pitch error
	float yaw_err;  			 	// current  yaw error
	
	discrete_filter roll_ctrl;		// feedback controller for angular velocity
	discrete_filter pitch_ctrl;		// feedback controller for angular velocity
	discrete_filter yaw_ctrl;		// feedback controller for DMP yaw
	
	float alt_err_integrator; 		// current and previous altitudes error
	float dRoll_err_integrator; 	// current and previous roll error
	float dPitch_err_integrator;	// current and previous pitch error
	float imu_roll_err;
	float imu_pitch_err;
	float yaw_err_integrator;   	// current and previous yaw error
	float control_u[4];				// control outputs  alt,roll,pitch,yaw
	float esc_out[4];				// normalized (0-1) outputs to 4 motors
	int num_yaw_spins; 				// remember number of spins around Z
	float imu_yaw_on_takeoff;		// raw yaw value read on takeoff
}core_state_t;

/************************************************************************
* 	user_interface_t
*	represents current command by the user which may be populated from 
*	DSM2, mavlink, or any other communication.
************************************************************************/
typedef struct user_interface_t{
	// this is the user commanded flight_mode. 
	// flight stack reads this into flight_mode except in the
	// case of loss of communication or emergency landing
	flight_mode_t flight_mode;  
	
	// All sticks scaled from -1 to 1
	float throttle_stick; 	// positive up
	float yaw_stick;		// positive to the right, CW yaw
	float roll_stick;		// positive to the right
	float pitch_stick;		// positive up
	
	// kill_switch == 0 means ARMED
	// kill_switch != 0 mean emergency kill and disarm
	int kill_switch;

}user_interface_t;


/************************************************************************
* 	options_t
*	holds user enabled options from command line arguments
*	ints are non-zero if feature is enabled
************************************************************************/
typedef struct options_t{
	int logging; // enable saving a log file for each flight
	int mavlink; // enable mavlink over UDP
	char ground_ip[24]; 
	int mode_0;	 // mode to use for DSM2 ch6 mode switch
	int mode_1;  // mode to use when switch is in position 1
	int quiet;	 // enable quiet mode (disable printf thread)
}options_t;

/************************************************************************
* 	Function declarations				
************************************************************************/
// regular functions
int initialize_core();
int wait_for_arming_sequence();
int disarm();
int load_default_core_config();
int on_pause_press();
int print_flight_mode(flight_mode_t mode);

//threads
void* flight_stack(void* ptr);
void* mavlink_sender(void* ptr);
void* safety_thread_func(void* ptr);
void* DSM2_watcher(void* ptr);
void* led_manager(void* ptr);
void* printf_thread_func(void* ptr);

// hardware interrupt routines
int flight_core();


/************************************************************************
* 	Global Variables				
************************************************************************/
options_t 				options;
core_config_t 			core_config;
core_setpoint_t 		core_setpoint;
core_state_t 			core_state;
user_interface_t		user_interface;
core_logger_t			core_logger;


/************************************************************************
*	initialize_filters()
*	setup of feedback controllers used in flight core
************************************************************************/
int initialize_filters(){
	core_state.roll_ctrl = generatePID(
							core_config.Droll_KP,
							core_config.Droll_KI,
							core_config.Droll_KD,
							.015,
							DT);
							
	core_state.pitch_ctrl = generatePID(
							core_config.Dpitch_KP,
							core_config.Dpitch_KI,
							core_config.Dpitch_KD,
							.015,
							DT);
	core_state.yaw_ctrl = generatePID(
							core_config.yaw_KP,
							core_config.yaw_KI,
							core_config.yaw_KD,
							.015,
							DT);
	zeroFilter(&core_state.roll_ctrl);
	zeroFilter(&core_state.pitch_ctrl);
	zeroFilter(&core_state.yaw_ctrl);
	
	return 0;
}

/************************************************************************
*	flight_core()
*	Hardware Interrupt-Driven Flight Control Loop
*	- read sensor values
*	- estimate system state
*	- read setpoint from flight_stack
*	- if is position mode, calculate a new attitude setpoint
* 	- otherwise use user attitude setpoint
*	- calculate and send ESC commands
************************************************************************/
int flight_core(){
	// remember previous core_mode to detect transition from DISARMED
	static core_mode_t previous_core_mode;
	int i;	// general purpose
	
	/************************************************************************
	*	Begin control loop if there was a valid interrupt with new IMU data
	************************************************************************/
	if (mpu9150_read(&mpu) == 0) {
		
		/************************************************************************
		*	Estimate system state if DISARMED or not
		************************************************************************/
		// // march system state history one step
		// for(i=(STATE_LEN-1);i>0;i--){
			// core_state.altitude[i] = core_state.altitude[i-1];
			// core_state.roll[i] = core_state.roll[i-1];
			// core_state.pitch[i] = core_state.pitch[i-1];
			// core_state.yaw[i] = core_state.yaw[i-1];
			// core_state.alt_err[i] = core_state.alt_err[i-1];
			// core_state.dRoll_err[i] = core_state.dRoll_err[i-1];
			// core_state.dPitch_err[i] = core_state.dPitch_err[i-1];
			// core_state.yaw_err[i] = core_state.yaw_err[i-1];
		// }
		
		// collect new IMU roll/pitch data
		// positive roll right according to right hand rule
		// MPU9150 driver has incorrect minus sign on Y axis, correct for it here
		// positive pitch backwards according to right hand rule
		core_state.roll  = -(mpu.fusedEuler[VEC3_Y] - core_state.imu_roll_err);
		core_state.pitch =   mpu.fusedEuler[VEC3_X] - core_state.imu_pitch_err;

		
		// current roll/pitch/yaw rates straight from gyro 
		// converted to rad/s with default FUll scale range
		// raw gyro matches sign on MPU9150 coordinate system, unlike Euler angle
		core_state.dRoll  = mpu.rawGyro[VEC3_Y] * GYRO_FSR * DEGREE_TO_RAD / 32767.0;
		core_state.dPitch = mpu.rawGyro[VEC3_X] * GYRO_FSR * DEGREE_TO_RAD / 32767.0;
		core_state.dYaw	  = mpu.rawGyro[VEC3_Z] * GYRO_FSR * DEGREE_TO_RAD / 32767.0;
		
		// if this is the first loop since being armed, reset yaw trim
		if(previous_core_mode == DISARMED && 
			core_setpoint.core_mode != DISARMED)
		{	
			core_state.num_yaw_spins = 0;
			core_state.imu_yaw_on_takeoff = mpu.fusedEuler[VEC3_Z];
		}
		float new_yaw = -(mpu.fusedEuler[VEC3_Z] - core_state.imu_yaw_on_takeoff) + (
													core_state.num_yaw_spins*2*PI);
		
		// detect the crossover point at Z = +-PI
		if(new_yaw - core_state.last_yaw > 6){
			core_state.num_yaw_spins -= 1;
		}
		else if(new_yaw - core_state.last_yaw < -6){
			core_state.num_yaw_spins += 1;
		}
		
		// record new yaw compensating for full rotation
		core_state.last_yaw = core_state.yaw;
		core_state.yaw = -(mpu.fusedEuler[VEC3_Z] - core_state.imu_yaw_on_takeoff) +
												(core_state.num_yaw_spins*2*PI);
		

		/************************************************************************
		* 	manage the setpoints based on attitude or position mode
		************************************************************************/
		switch(core_setpoint.core_mode){
		
			/************************************************************************
			*	in Position control mode, evaluate an outer loop controller to
			*	change the attitude setpoint. Discard user attitude setpoints
			************************************************************************/
			case POSITION:
				// TODO: outer loop position controller
				break;
				
			/************************************************************************
			*	in attitude control mode, user has direct control over throttle
			*	roll, and pitch angles. Absolute yaw setpoint gets updated at
			*	user-commanded yaw_rate
			************************************************************************/
			case ATTITUDE:
				// only when flying, update the yaw setpoint
				if(core_setpoint.throttle > YAW_CUTOFF_TH){
					core_setpoint.yaw += DT*core_setpoint.yaw_rate;
				}
				
				break;
				
			/************************************************************************
			*	if disarmed, reset controllers and return
			************************************************************************/
			case DISARMED:
				core_state.dRoll_err_integrator  = 0;
				core_state.dPitch_err_integrator = 0;
				core_state.yaw_err_integrator = 0;
				zeroFilter(&core_state.roll_ctrl);
				zeroFilter(&core_state.pitch_ctrl);
				core_setpoint.yaw=0;
				memset(&core_state.esc_out,0,16);
				previous_core_mode = DISARMED;
				return 0;
				break;		//should never get here
				
			default:
				break;		//should never get here
		}
		
		
		/************************************************************************
		* 	Finally run the attitude feedback controllers
		************************************************************************/
		float u[4];		// normalized throttle, roll, pitch, yaw control components 
		
		/************************************************************************
		*	Throttle Controller
		************************************************************************/
		// compensate for roll/pitch angle to maintain Z thrust
		float throttle_compensation;
		throttle_compensation = 1 / cos(core_state.roll);
		throttle_compensation *= 1 / cos(core_state.pitch);
		float thr = core_setpoint.throttle*(MAX_THRUST_COMPONENT-core_config.idle_speed)
						+ core_config.idle_speed;
		
		u[0] = throttle_compensation * thr;
		
		/************************************************************************
		*	Roll & Pitch Controllers
		************************************************************************/
		float dRoll_setpoint = (core_setpoint.roll - core_state.roll) *
														core_config.roll_rate_per_rad;
		float dPitch_setpoint = (core_setpoint.pitch - core_state.pitch) *
														core_config.pitch_rate_per_rad;
		core_state.dRoll_err  = dRoll_setpoint  - core_state.dRoll;
		core_state.dPitch_err = dPitch_setpoint - core_state.dPitch;
		
		// // if last state was DISARMED, then errors will all be 0.
		// // make the previous error the same
		// if(previous_core_mode == DISARMED){
			// preFillFilter(&core_state.roll_ctrl, core_state.dRoll_err);
			// preFillFilter(&core_state.pitch_ctrl, core_state.dPitch_err);
		// }
		
		// only run integrator if airborne 
		// TODO: proper landing/takeoff detection
		if(u[0] > INT_CUTOFF_TH){
			core_state.dRoll_err_integrator  += core_state.dRoll_err  * DT;
			core_state.dPitch_err_integrator += core_state.dPitch_err * DT;
		}
		
				
		marchFilter(&core_state.roll_ctrl, core_state.dRoll_err);
		marchFilter(&core_state.pitch_ctrl, core_state.dPitch_err);
		
		if(core_setpoint.throttle<0.1){
			saturateFilter(&core_state.roll_ctrl, -LAND_SATURATION,LAND_SATURATION);
			saturateFilter(&core_state.pitch_ctrl, -LAND_SATURATION, LAND_SATURATION);
		}
		else{
			saturateFilter(&core_state.roll_ctrl, -MAX_ROLL_COMPONENT, MAX_ROLL_COMPONENT);
			saturateFilter(&core_state.pitch_ctrl, -MAX_PITCH_COMPONENT, MAX_PITCH_COMPONENT);
		}
		
		u[1] = core_state.roll_ctrl.current_output;
		u[2] = core_state.pitch_ctrl.current_output;
		
		
		/************************************************************************
		*	Yaw Controller
		************************************************************************/
		core_state.yaw_err = core_setpoint.yaw - core_state.yaw;
		
		// only run integrator if airborne 
		if(u[0] > INT_CUTOFF_TH){
			core_state.yaw_err_integrator += core_state.yaw_err * DT;
		}

		marchFilter(&core_state.yaw_ctrl, core_state.yaw_err);
		
		if(core_setpoint.throttle<0.1){
			saturateFilter(&core_state.yaw_ctrl, -LAND_SATURATION,LAND_SATURATION);
		}
		else{
			saturateFilter(&core_state.yaw_ctrl, -MAX_YAW_COMPONENT, MAX_YAW_COMPONENT);
		}
		u[3] = core_state.yaw_ctrl.current_output;
		
		/************************************************************************
		*  Mixing for arducopter/pixhawk X-quadrator layout
		*  CW 3	  1 CCW			
		* 	   \ /				Y
		*	   / \            	|_ X
		* CCW 2	  4 CW
		************************************************************************/
		float new_esc[4];
		
		new_esc[0]=u[0]-u[1]+u[2]-u[3];
		new_esc[1]=u[0]+u[1]-u[2]-u[3];
		new_esc[2]=u[0]+u[1]+u[2]+u[3];
		new_esc[3]=u[0]-u[1]-u[2]+u[3];	
		
		/************************************************************************
		*	Prevent saturation under heavy vertical acceleration by reducing all
		*	outputs evenly such that the largest doesn't exceed 1
		************************************************************************/
		// find control output limits 
		float largest_value = 0;
		float smallest_value = 1;
		for(i=0;i<4;i++){
			if(new_esc[i]>largest_value){
				largest_value = new_esc[i];

			}
			if(new_esc[i]<smallest_value){
				smallest_value=new_esc[i];
			}
		}
		// if upper saturation would have occurred, reduce all outputs evenly
		if(largest_value>1){
			for(i=0;i<4;i++){
			float offset = largest_value - 1;
				new_esc[i]-=offset;
			}
		}
			
		/************************************************************************
		*	Send a servo pulse immediately at the end of the control loop.
		*	Intended to update ESCs exactly once per control timestep
		*	also record this action to core_state.new_esc_out[] for telemetry
		************************************************************************/
		
		// if this is the first time armed, make sure to send minimum 
		// pulse width to prevent ESCs from going into calibration
		
		if(previous_core_mode == DISARMED){
			for(i=0;i<4;i++){
				send_servo_pulse_normalized(i+1,0);
			}
		}
		else{
			for(i=0;i<4;i++){
				if(new_esc[i]>1.0){
					new_esc[i]=1.0;
				}
				else if(new_esc[i]<0){
					new_esc[i]=0;
				}
				send_servo_pulse_normalized(i+1,new_esc[i]);
				core_state.esc_out[i] = new_esc[i];
				core_state.control_u[i] = u[i];		
			}
		}	
		
		// log some useful data if armed and flying
		core_log_entry_t new_entry;
		new_entry.num_loops	= core_state.control_loops;
		new_entry.roll		= core_state.roll;
		new_entry.pitch		= core_state.pitch;
		new_entry.yaw		= core_state.yaw;
		new_entry.dRoll		= core_state.dRoll;
		new_entry.dPitch	= core_state.dPitch;
		new_entry.dYaw		= core_state.dYaw;
		new_entry.u_0		= core_state.control_u[0];
		new_entry.u_1		= core_state.control_u[1];
		new_entry.u_2		= core_state.control_u[2];
		new_entry.u_3		= core_state.control_u[3];
		new_entry.esc_1		= core_state.esc_out[0];
		new_entry.esc_2		= core_state.esc_out[1];
		new_entry.esc_3		= core_state.esc_out[2];
		new_entry.esc_4		= core_state.esc_out[3];
		new_entry.v_batt	= core_state.v_batt;
			
		log_core_data(&core_logger, &new_entry);
		
		//remember the last state to detect transition from DISARMED to ARMED
		previous_core_mode = core_setpoint.core_mode;
		core_state.control_loops++;
	}
	return 0;
}

/************************************************************************
*	flight_stack()
*	Translates the flight mode and user controls from user_interface
*	into setpoints for the flight_core position and attitude controller
*
*	If the core gets disarmed by another thread, flight_stack manages
*	recognizing the rearming sequence
*
*	The flight_core only takes setpoint values for feedback control, 
************************************************************************/
void* flight_stack(void* ptr){
	flight_mode_t previous_flight_mode; // remember to detect when mode changes
	
	
	// run until state indicates thread should close
	while(get_state()!=EXITING){
		
		// if the user swapped modes, print to console
		if(previous_flight_mode != user_interface.flight_mode){
			print_flight_mode(user_interface.flight_mode);
		}
		
		// shutdown core on emergency kill mode or kill switch
		if(user_interface.flight_mode == EMERGENCY_KILL ||
		   user_interface.kill_switch != 0)
		{
			disarm();
		}
		
		// if the core got disarmed, wait for arming sequence 
		if(core_setpoint.core_mode == DISARMED){
			wait_for_arming_sequence();
			// any future pre-flight checks or routines go here
			
		}
		
		// kill switches seem to be fine
		// switch behaviour based on user flight mode
		else{
			switch(user_interface.flight_mode){
			// Raw attitude mode lets user control the inner attitude loop directly
			case USER_ATTITUDE:
				core_setpoint.core_mode = ATTITUDE;
				
				// translate throttle stick (-1,1) to throttle (0,1)
				core_setpoint.throttle = (user_interface.throttle_stick + 1.0)/2.0;
				
				// scale roll and pitch angle by max setpoint in rad
				core_setpoint.roll		= user_interface.roll_stick *
											core_config.max_roll_setpoint;
				core_setpoint.pitch		= user_interface.pitch_stick *
											core_config.max_pitch_setpoint;
				// scale yaw_rate by max yaw rate in rad/s
				core_setpoint.yaw_rate	= user_interface.yaw_stick *
											core_config.max_yaw_rate;
				break;
				
			// emergency land just sets the throttle low for now
			// TODO: gently lower altitude till landing detected 
			case EMERGENCY_LAND:
				core_setpoint.core_mode = ATTITUDE;
				core_setpoint.throttle  = EMERGENCY_LAND_THR;
				core_setpoint.roll		= 0;
				core_setpoint.pitch		= 0;
				core_setpoint.yaw_rate	= 0;
				break;
			
			// TODO: other modes
			case USER_LOITER:
				break;
			case USER_POSITION_CARTESIAN:
				break;
			case USER_POSITION_RADIAL:
				break; 
			case TARGET_HOLD:
				break;
			default:
				break;
			}
		}
		
		// record previous flight mode to detect changes
		previous_flight_mode = user_interface.flight_mode; 
		usleep(10000); // ~100hz loop, could be faster
	}
	return NULL;
}

/************************************************************************
*	wait_for_arming_sequence()
*	
*	blocking_function that returns after the user has released the
*	kill_switch and toggled the throttle stick up and down
************************************************************************/
int wait_for_arming_sequence(){
	int i;
START:
	// wait for level MAV before starting
	while(fabs(core_state.roll)>ARM_TIP_THRESHOLD ||
		fabs(core_state.pitch)>ARM_TIP_THRESHOLD){
		usleep(100000);
		if(get_state()==EXITING)return 0;
	} 	  
	
	while(user_interface.kill_switch){ 
		usleep(100000);
		if(get_state()==EXITING)return 0;
	}
 	
	//wait for throttle down
	while(user_interface.throttle_stick > -0.9){ 
		usleep(100000);
		if(get_state()==EXITING)return 0;}
	
	//wait for throttle up
	while(user_interface.throttle_stick<.9){ 
		usleep(100000);
		if(get_state()==EXITING)return 0;}
	
	//wait for throttle down
	while(user_interface.throttle_stick > -0.9){ 
		usleep(100000);
		if(get_state()==EXITING)return 0;
	}
	
	if(fabs(core_state.roll)>ARM_TIP_THRESHOLD ||
		fabs(core_state.pitch)>ARM_TIP_THRESHOLD){
		printf("\nRestart arming sequence with level MAV\n");
		goto START;
	} 
	
	// wake ESCs up at minimum throttle to avoid calibration mode
	// flight_core also sends one minimum pulse at first when armed
	for(i=0; i<10; i++){
		send_servo_pulse_normalized(1,0);
		send_servo_pulse_normalized(2,0); 
		send_servo_pulse_normalized(3,0);
		send_servo_pulse_normalized(4,0);
		usleep(5000);
	}
	
	// load fresh settings if edited while disarmed
	load_core_config(&core_config);
	initialize_filters();
		
	core_setpoint.core_mode = ATTITUDE;
	printf("\n\nARMED!!\n");
	setRED(LOW);
	return 0;
}

/************************************************************************
*	disarm()
*	
*	emergency disarm mode
************************************************************************/
int disarm(){
	if(core_setpoint.core_mode != DISARMED){
		printf("\n\nDISARMED\n");
	}
	core_setpoint.core_mode = DISARMED;
	setRED(1);
	setGRN(0); 
	return 0;
}



/************************************************************************
*	on_pause_press
*	If the user holds the pause button for a second, exit cleanly
*	disarm on momentary press
************************************************************************/
int on_pause_press(){
	disarm();
	int i=0;
	do{
		usleep(100000);
		if(get_pause_button_state() == LOW){
			return 0; //user let go before time-out
		}
		i++;
	}while(i<10);
	//user held the button down long enough, exit cleanly
	set_state(EXITING);
	return 0;
}

/************************************************************************
*	mavlink_sender
*	send mavlink heartbeat and IMU attitude packets
************************************************************************/
void* mavlink_sender(void* ptr){
	uint8_t buf[MAV_BUF_LEN];
	mavlink_message_t msg;
	uint16_t len;
	while(get_state() != EXITING){
		
		// send heartbeat
		memset(buf, 0, MAV_BUF_LEN);
		mavlink_msg_heartbeat_pack(1, 200, &msg, MAV_TYPE_HELICOPTER, MAV_AUTOPILOT_GENERIC, MAV_MODE_GUIDED_ARMED, 0, MAV_STATE_ACTIVE);
		len = mavlink_msg_to_send_buffer(buf, &msg);
		sendto(sock, buf, len, 0, (struct sockaddr*)&gcAddr, sizeof(struct sockaddr_in));
		
		//send attitude
		memset(buf, 0, MAV_BUF_LEN);
		mavlink_msg_attitude_pack(1, 200, &msg, microsSinceEpoch(), 
											core_state.roll, 
											core_state.pitch,
											core_state.yaw, 
											core_state.dRoll,
											core_state.dPitch,
											core_state.dYaw);
		len = mavlink_msg_to_send_buffer(buf, &msg);
		sendto(sock, buf, len, 0, (struct sockaddr*)&gcAddr, sizeof(struct sockaddr_in));
		
		usleep(100000); // 10 hz
	}
	return NULL;
}

/************************************************************************
*	Safety thread 
*	check for rollover 
*	TODO: check for low battery too
************************************************************************/
void* safety_thread_func(void* ptr){
	while(get_state()!=EXITING){
		// check for tipover
		if(core_setpoint.core_mode != DISARMED){
			if(	fabs(core_state.roll)>TIP_THRESHOLD ||
				fabs(core_state.pitch)>TIP_THRESHOLD)
			{
				printf("\nTIP DETECTED\n");
				disarm();
			}
		}
		usleep(50000); // check at ~20hz
	}
	return NULL;
}

/************************************************************************
*	Watch for new DSM2 data and interpret into local user mode
*	Watch for loss of DSM2 radio communication
*	after DSM2_LAND_TIMEOUT, go into emergency land mode
* 	after DSM2_DISARM_TIMEOUT disarm the motors completely 
************************************************************************/
void* DSM2_watcher(void* ptr){
	timespec last_dsm2_time, current_time;
	
	// toggle using_dsm2 to 1 when first packet arrives
	// only check timeouts if this is true
	int using_dsm2; 
	
	while(get_state()!=EXITING){
		// record time and process new data
		clock_gettime(CLOCK_MONOTONIC, &current_time);
		
		switch (is_new_dsm2_data()){
		case 1:	
			using_dsm2 = 1;
			
			// record time and process new data
			clock_gettime(CLOCK_MONOTONIC, &last_dsm2_time);
			// user hit the kill switch, emergency disarm
			if(get_dsm2_ch_normalized(5)<0){
				user_interface.kill_switch = 1;
				
				// it is not strictly necessary to call disarm here
				// since flight_stack checks kill_switch, but in the
				// event of a flight_stack crash this will disarm anyway
				disarm(); 
			}
			else{	
				// user hasn't hit kill switch
				user_interface.kill_switch = 0;
				// configure your radio switch layout here
				user_interface.throttle_stick = get_dsm2_ch_normalized(1);
				// positive roll means tipping right
				user_interface.roll_stick 	= -get_dsm2_ch_normalized(2);
				// positive pitch means tipping backwards
				user_interface.pitch_stick 	= -get_dsm2_ch_normalized(3);
				// positive yaw means turning left
				user_interface.yaw_stick 	= get_dsm2_ch_normalized(4);
				
				// only use ATTITUDE for now
				if(get_dsm2_ch_normalized(6)>0){
					user_interface.flight_mode = USER_ATTITUDE;
				}
				else{
					user_interface.flight_mode = USER_ATTITUDE;
				}
			}
			break;
			
		// No new data, check for time-outs
		case 0:
			if(using_dsm2){
				timespec timeout = diff(last_dsm2_time, current_time);
				float timeout_secs = timeout.tv_sec + (timeout.tv_nsec/1000000000.0);
				
				// if core is armed and timeout met, disarm the core
				if(core_setpoint.core_mode != DISARMED &&
					timeout_secs > DSM2_DISARM_TIMEOUT){
					printf("\n\nlost DSM2 communication for %0.1f seconds", timeout_secs);
					disarm();
				}
				
				// start landing the the cutout is still short
				else if(user_interface.flight_mode != EMERGENCY_LAND &&
							timeout_secs > DSM2_LAND_TIMEOUT){
					printf("\n\nlost DSM2 communication for %0.1f seconds\n", timeout_secs);
					printf("EMERGENCY LANDING\n");
					user_interface.flight_mode = EMERGENCY_LAND;
					user_interface.throttle_stick 	= -1;
					user_interface.roll_stick 		= 0;
					user_interface.pitch_stick 		= 0;
					user_interface.yaw_stick 		= 0;
				}
				break;
			}
		default:
			break;  // should never get here
		}
		
		usleep(10000); // ~ 100hz
	}
	return NULL;
}

/************************************************************************
*	 flash the red LED is armed, or turn on green if disarmed
************************************************************************/
void* led_manager(void* ptr){
	int toggle;
	while (get_state()!=EXITING){
		if(core_setpoint.core_mode == DISARMED){
			if(toggle){
				setRED(LOW);
				toggle = 1;
			}
			else{
				setRED(HIGH);
				toggle = 0;
			}
		}
		else{
			toggle = 0;
			setGRN(HIGH);
			setRED(LOW);
		}
		usleep(500000); //toggle LED every half second
	}
	return NULL;
}

/************************************************************************
*	print a flight mode to console
************************************************************************/
int print_flight_mode(flight_mode_t mode){
	fflush(stdout);
	printf("\nflight_mode: ");
	switch(mode){
	case EMERGENCY_KILL:
		printf("EMERGENCY_KILL\n");
		break;
	case EMERGENCY_LAND:
		printf("EMERGENCY_LAND\n");
		break;
	case USER_ATTITUDE:
		printf("USER_ATTITUDE\n");
		break;
	case USER_LOITER:
		printf("USER_LOITER\n");
		break;
	case USER_POSITION_CARTESIAN:
		printf("USER_POSITION_CARTESIAN\n");
		break;
	case USER_POSITION_RADIAL:
		printf("USER_POSITION_RADIAL\n");
		break;
	case TARGET_HOLD:
		printf("TARGET_HOLD\n");
		break;
	default:
		printf("unknown\n");
		break;
	}
	fflush(stdout);
	return 0;
}

/************************************************************************
*	print stuff to the console
************************************************************************/
void* printf_thread_func(void* ptr){
	int i;
	
	printf("\nTurn your transmitter kill switch UP\n");
	printf("Then move throttle UP then DOWN to arm\n");
	
	while(get_state()!=EXITING){
		printf("\r");
		
		// print core_state
		printf("roll %0.2f ", core_state.roll); 
		printf("pitch %0.2f ", core_state.pitch); 
		printf("yaw %0.2f ", core_state.yaw); 
		
		// printf("dRoll %0.1f ", core_state.dRoll); 
		// printf("dPitch %0.1f ", core_state.dPitch); 
		// printf("dYaw %0.1f ", core_state.dYaw); 
		
		printf("err: R %0.1f ", core_state.dRoll_err); 
		printf("P %0.1f ", core_state.dPitch_err); 
		printf("Y %0.1f ", core_state.yaw_err); 
		
		// // print user inputs
		// printf("user inputs: ");
		// printf("thr %0.1f ", user_interface.throttle_stick); 
		// printf("roll %0.1f ", user_interface.roll_stick); 
		// printf("pitch %0.1f ", user_interface.pitch_stick); 
		// printf("yaw %0.1f ", user_interface.yaw_stick); 
		// printf("kill %d ", user_interface.kill_switch); 
		
		// // print setpoints
		// printf("setpoints: ");
		// printf("roll %0.1f ", core_setpoint.roll); 
		// printf("pitch %0.1f ", core_setpoint.pitch); 
		// printf("yaw: %0.1f ", core_setpoint.yaw); 
		
		// print control outputs
		printf("u: ");
		for(i=0; i<4; i++){
			printf("%0.2f ", core_state.control_u[i]);
		}
		
		// // print outputs to motors
		// printf("esc: ");
		// for(i=0; i<4; i++){
			// printf("%0.2f ", core_state.esc_out[i]);
		// }
			
		fflush(stdout);	
		usleep(200000); // print at ~5hz
	}
	return NULL;
}

// Turn features on/off base don user options
int parse_arguments(int argc, char* argv[]){
	int c,i;
	
	while ((c = getopt (argc, argv, "lqm")) != -1){
		switch (c){
		case 'l':
			printf("logging enabled\n");
			options.logging=1;
			break;
		case 'q':
			printf("starting in quiet mode\n");
			options.quiet=1;
			break;
		case 'm':
			options.mavlink = 1;
			printf("sending mavlink data\n");
			// see if the user gave an IP address as argument
			// strcpy(options.ground_ip, optarg);
			break;
		case '?':
			if (optopt == 'm'){
				//send to default mav address if no or bad ip argument provided
				strcpy(options.ground_ip, DEFAULT_MAV_ADDRESS);
			}
			else if (isprint(optopt)){
				printf("Unknown option `-%c'.\n", optopt);
				return -1;
			}
			else{
				printf("Unknown option character `\\x%x'.\n",optopt);
				return -1;
			}
		return 0;
		
		default:
			return -1;
		}
    }
	
	// print any non option arguments
	for (i = optind; i < argc; i++){	
		printf ("Non-option argument %s\n", argv[i]);
		return -1;
	}
	
	printf("finished parsing arguments\n");
	return 0;
}

// Main only serves to initialize hardware and spawn threads
int main(int argc, char* argv[]){
	// not all threads may begin depending on user options
	pthread_t mav_send_thread;
	pthread_t led_thread;
	pthread_t safety_thread;
	pthread_t flight_stack_thread;
	pthread_t DSM2_watcher_thread;
	pthread_t printf_thread;
	pthread_t core_logging_thread;
	
	// first check for user options
	if(parse_arguments(argc, argv)<0){
		return -1;
	}

	// always start disarmed
	disarm();
	
	// initialize cape hardware
	if(initialize_cape()<0){
		return -1;
	}
	
	// load flight_core settings
	if(load_core_config(&core_config)){
		printf("WARNING: no configuration file found\n");
		printf("loading default settings\n");
		if(create_default_core_config_file(&core_config)){
			printf("Warning, can't write default core_config file\n");
		}
	}
	
	// listen to pause button for disarm and exit commands
	// do this after hardware initialization so the user 
	// can quit the program in case of crash
	set_pause_pressed_func(&on_pause_press); 
	
	// start uart4 thread in robotics cape library
	if(initialize_dsm2()<0){
		cleanup_cape();
		return -1;
	}
	
	// start filters after loading parameters
	initialize_filters();
	printf("using roll filter constants:");
	printFilterDetails(&core_state.roll_ctrl);
	
	// start a core_log and logging thread
	if(start_core_log(&core_logger)<0){
		printf("WARNING: failed to open a core_log file\n");
	}
	else{
		pthread_create(&core_logging_thread, NULL, core_log_writer, &core_logger);
	}
	
	// start mavlink thread if enabled by user
	if(options.mavlink){
		// open a udp port for mavlink
		// sock and gcAddr are global variables needed to send and receive
		gcAddr = initialize_mavlink_udp(DEFAULT_MAV_ADDRESS, &sock);
		
		// Start thread sending heartbeat and IMU attitude packets
		pthread_create(&mav_send_thread, NULL, mavlink_sender, (void*) NULL);
		printf("Sending Heartbeat Packets\n");
	}

	// Start LED Flasher Thread
	pthread_create(&led_thread, NULL, led_manager, (void*) NULL);
	
	// Start Safety checking thread
	pthread_create(&safety_thread, NULL, safety_thread_func, (void*) NULL);
	
	// Begin flight Stack
	pthread_create(&flight_stack_thread, NULL, flight_stack, (void*) NULL);
	
	// start interpreting dsm2 packets
	pthread_create(&DSM2_watcher_thread, NULL, DSM2_watcher, (void*) NULL);
	
	// Start the real-time interrupt driven control thread
	signed char orientation[9] = ORIENTATION_FLAT;
	if(initialize_imu(CONTROL_HZ, orientation)){
		printf("IMU initialization failed, please reboot\n");
		cleanup_cape();
		return -1;
	}
	set_imu_interrupt_func(&flight_core);
	
	// if the user didn't specify quiet mode, start printing
	if(options.quiet == 0){
		pthread_create(&printf_thread, NULL, printf_thread_func, (void*) NULL);
	}
	
	//chill until something exits the program
	while(get_state()!=EXITING){
		usleep(100000);
	}
	
	// cleanup before closing
	close(sock); 	// mavlink UDP socket
	stop_core_log(&core_logger);// finish writing core_log
	cleanup_cape();	// de-initialize cape hardware
	return 0;
}