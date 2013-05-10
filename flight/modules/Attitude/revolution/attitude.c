/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Attitude Copter Control Attitude Estimation
 * @brief Acquires sensor data and computes attitude estimate
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       attitude.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include <openpilot.h>

#include "attitude.h"
#include "accels.h"
#include "airspeedsensor.h"
#include "airspeedactual.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "baroaltitude.h"
#include "flightstatus.h"
#include "gpsposition.h"
#include "gpsvelocity.h"
#include "gyros.h"
#include "gyrosbias.h"
#include "homelocation.h"
#include "magnetometer.h"
#include "positionactual.h"
#include "ekfconfiguration.h"
#include "ekfstatevariance.h"
#include "revocalibration.h"
#include "revosettings.h"
#include "velocityactual.h"
#include "taskinfo.h"

#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES 2048
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)
#define FAILSAFE_TIMEOUT_MS 10

// low pass filter configuration to calculate offset
// of barometric altitude sensor
// reasoning: updates at: 10 Hz, tau= 300 s settle time
// exp(-(1/f) / tau ) ~=~ 0.9997
#define BARO_OFFSET_LOWPASS_ALPHA 0.9997f 

// simple IAS to TAS aproximation - 2% increase per 1000ft
// since we do not have flowing air temperature information
#define IAS2TAS(alt) (1.0f + (0.02f*(alt)/304.8f))

// Private types

// Private variables
static xTaskHandle attitudeTaskHandle;

static xQueueHandle gyroQueue;
static xQueueHandle accelQueue;
static xQueueHandle magQueue;
static xQueueHandle airspeedQueue;
static xQueueHandle baroQueue;
static xQueueHandle gpsQueue;
static xQueueHandle gpsVelQueue;

static AttitudeSettingsData attitudeSettings;
static HomeLocationData homeLocation;
static RevoCalibrationData revoCalibration;
static EKFConfigurationData ekfConfiguration;
static RevoSettingsData revoSettings;
static FlightStatusData flightStatus;
const uint32_t SENSOR_QUEUE_SIZE = 10;

static bool volatile variance_error = true;
static bool volatile initialization_required = true;
static uint32_t volatile running_algorithm = 0xffffffff; // we start with no algorithm running

// Private functions
static void AttitudeTask(void *parameters);

static int32_t updateAttitudeComplementary(bool first_run);
static int32_t updateAttitudeINSGPS(bool first_run, bool outdoor_mode);
static void settingsUpdatedCb(UAVObjEvent * objEv);

static int32_t getNED(GPSPositionData * gpsPosition, float * NED);

// check for invalid values
static inline bool invalid(float data) {
	if ( isnan(data) || isinf(data) ){
		return true;
	}
	return false;
}

// check for invalid variance values
static inline bool invalid_var(float data) {
	if ( invalid(data) ) {
		return true;
	}
	if ( data < 1e-15f ) { // var should not be close to zero. And not negative either.
		return true;
	}
	return false;
}

/**
 * API for sensor fusion algorithms:
 * Configure(xQueueHandle gyro, xQueueHandle accel, xQueueHandle mag, xQueueHandle baro)
 *   Stores all the queues the algorithm will pull data from
 * FinalizeSensors() -- before saving the sensors modifies them based on internal state (gyro bias)
 * Update() -- queries queues and updates the attitude estiamte
 */


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeInitialize(void)
{
	AttitudeActualInitialize();
	AirspeedActualInitialize();
	AirspeedSensorInitialize();
	AttitudeSettingsInitialize();
	PositionActualInitialize();
	VelocityActualInitialize();
	RevoSettingsInitialize();
	RevoCalibrationInitialize();
	EKFConfigurationInitialize();
	EKFStateVarianceInitialize();
	FlightStatusInitialize();
	
	// Initialize this here while we aren't setting the homelocation in GPS
	HomeLocationInitialize();

	// Initialize quaternion
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = 1.0f;
	attitude.q2 = 0.0f;
	attitude.q3 = 0.0f;
	attitude.q4 = 0.0f;
	AttitudeActualSet(&attitude);
	
	// Cannot trust the values to init right above if BL runs
	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x = 0.0f;
	gyrosBias.y = 0.0f;
	gyrosBias.z = 0.0f;
	GyrosBiasSet(&gyrosBias);
	
	AttitudeSettingsConnectCallback(&settingsUpdatedCb);
	RevoSettingsConnectCallback(&settingsUpdatedCb);
	RevoCalibrationConnectCallback(&settingsUpdatedCb);
	HomeLocationConnectCallback(&settingsUpdatedCb);
	EKFConfigurationConnectCallback(&settingsUpdatedCb);
	FlightStatusConnectCallback(&settingsUpdatedCb);

	return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeStart(void)
{
	// Create the queues for the sensors
	gyroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	accelQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	magQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	airspeedQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	baroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	gpsQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	gpsVelQueue = xQueueCreate(1, sizeof(UAVObjEvent));

	// Start main task
	xTaskCreate(AttitudeTask, (signed char *)"Attitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &attitudeTaskHandle);
	PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_ATTITUDE, attitudeTaskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_ATTITUDE);
	
	GyrosConnectQueue(gyroQueue);
	AccelsConnectQueue(accelQueue);
	MagnetometerConnectQueue(magQueue);
	AirspeedSensorConnectQueue(airspeedQueue);
	BaroAltitudeConnectQueue(baroQueue);
	GPSPositionConnectQueue(gpsQueue);
	GPSVelocityConnectQueue(gpsVelQueue);

	return 0;
}

MODULE_INITCALL(AttitudeInitialize, AttitudeStart)

/**
 * Module thread, should not return.
 */
static void AttitudeTask(__attribute__((unused)) void *parameters)
{

	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

	// Force settings update to make sure rotation loaded
	settingsUpdatedCb(NULL);

	// Wait for all the sensors be to read
	vTaskDelay(100);

	// Main task loop - TODO: make it run as delayed callback
	while (1) {

		int32_t ret_val = -1;

		bool first_run = false;
		if (initialization_required) {
			initialization_required = false;
			first_run = true;
		}

		// This  function blocks on data queue
		switch (running_algorithm ) {
			case REVOSETTINGS_FUSIONALGORITHM_COMPLEMENTARY:
				ret_val = updateAttitudeComplementary(first_run);
				break;
			case REVOSETTINGS_FUSIONALGORITHM_INSOUTDOOR:
				ret_val = updateAttitudeINSGPS(first_run, true);
				break;
			case REVOSETTINGS_FUSIONALGORITHM_INSINDOOR:
				ret_val = updateAttitudeINSGPS(first_run, false);
				break;
			default:
				AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_CRITICAL);
				break;
		}

		if(ret_val != 0) {
			initialization_required = true;
		}

		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);
	}
}

float accel_mag;
float qmag;
float attitudeDt;
float mag_err[3];
float magKi = 0.000001f;
float magKp = 0.01f;

static int32_t updateAttitudeComplementary(bool first_run)
{
	UAVObjEvent ev;
	GyrosData gyrosData;
	AccelsData accelsData;
	static int32_t timeval;
	float dT;
	static uint8_t init = 0;

	// Wait until the AttitudeRaw object is updated, if a timeout then go to failsafe
	if ( xQueueReceive(gyroQueue, &ev, FAILSAFE_TIMEOUT_MS / portTICK_RATE_MS) != pdTRUE ||
	     xQueueReceive(accelQueue, &ev, 1 / portTICK_RATE_MS) != pdTRUE )
	{
		// When one of these is updated so should the other
		// Do not set attitude timeout warnings in simulation mode
		if (!AttitudeActualReadOnly()){
			AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
			return -1;
		}
	}

	AccelsGet(&accelsData);

	// During initialization and 
	if(first_run) {
#if defined(PIOS_INCLUDE_HMC5883)
		// To initialize we need a valid mag reading
		if ( xQueueReceive(magQueue, &ev, 0 / portTICK_RATE_MS) != pdTRUE )
			return -1;
		MagnetometerData magData;
		MagnetometerGet(&magData);
#else
		MagnetometerData magData;
		magData.x = 100.0f;
		magData.y = 0.0f;
		magData.z = 0.0f;
#endif
		AttitudeActualData attitudeActual;
		AttitudeActualGet(&attitudeActual);
		init = 0;

		// Set initial attitude. Use accels to determine roll and pitch, rotate magnetic measurement accordingly,
		// so pseudo "north" vector can be estimated even if the board is not level
		attitudeActual.Roll = atan2f(-accelsData.y, -accelsData.z);
		float zn = cosf(attitudeActual.Roll) * magData.z + sinf(attitudeActual.Roll) * magData.y;
		float yn = cosf(attitudeActual.Roll) * magData.y - sinf(attitudeActual.Roll) * magData.z;

		// rotate accels z vector according to roll
		float azn = cosf(attitudeActual.Roll) * accelsData.z + sinf(attitudeActual.Roll) * accelsData.y;
		attitudeActual.Pitch = atan2f(accelsData.x, -azn);

		float xn = cosf(attitudeActual.Pitch) * magData.x + sinf(attitudeActual.Pitch) * zn;

		attitudeActual.Yaw = atan2f(-yn, xn);
		// TODO: This is still a hack
		// Put this in a proper generic function in CoordinateConversion.c
		// should take 4 vectors: g (0,0,-9.81), accels, Be (or 1,0,0 if no home loc) and magnetometers (or 1,0,0 if no mags)
		// should calculate the rotation in 3d space using proper cross product math
		// SUBTODO: formulate the math required

		attitudeActual.Roll = RAD2DEG(attitudeActual.Roll);
		attitudeActual.Pitch = RAD2DEG(attitudeActual.Pitch);
		attitudeActual.Yaw = RAD2DEG(attitudeActual.Yaw);

		RPY2Quaternion(&attitudeActual.Roll,&attitudeActual.q1);
		AttitudeActualSet(&attitudeActual);

		timeval = PIOS_DELAY_GetRaw();

		return 0;

	}

	if((init == 0 && xTaskGetTickCount() < 7000) && (xTaskGetTickCount() > 1000)) {
		// For first 7 seconds use accels to get gyro bias
		attitudeSettings.AccelKp = 1.0f;
		attitudeSettings.AccelKi = 0.9f;
		attitudeSettings.YawBiasRate = 0.23f;
		magKp = 1.0f;
	} else if ((attitudeSettings.ZeroDuringArming == ATTITUDESETTINGS_ZERODURINGARMING_TRUE) && (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMING)) {
		attitudeSettings.AccelKp = 1.0f;
		attitudeSettings.AccelKi = 0.9f;
		attitudeSettings.YawBiasRate = 0.23f;
		magKp = 1.0f;
		init = 0;
	} else if (init == 0) {
		// Reload settings (all the rates)
		AttitudeSettingsGet(&attitudeSettings);
		magKp = 0.01f;
		init = 1;
	}

	GyrosGet(&gyrosData);

	// Compute the dT using the cpu clock
	dT = PIOS_DELAY_DiffuS(timeval) / 1000000.0f;
	timeval = PIOS_DELAY_GetRaw();

	float q[4];

	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);

	float grot[3];
	float accel_err[3];

	// Get the current attitude estimate
	quat_copy(&attitudeActual.q1, q);

	// Rotate gravity to body frame and cross with accels
	grot[0] = -(2.0f * (q[1] * q[3] - q[0] * q[2]));
	grot[1] = -(2.0f * (q[2] * q[3] + q[0] * q[1]));
	grot[2] = -(q[0] * q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3]);
	CrossProduct((const float *) &accelsData.x, (const float *) grot, accel_err);

	// Account for accel magnitude
	accel_mag = accelsData.x*accelsData.x + accelsData.y*accelsData.y + accelsData.z*accelsData.z;
	accel_mag = sqrtf(accel_mag);
	accel_err[0] /= accel_mag;
	accel_err[1] /= accel_mag;
	accel_err[2] /= accel_mag;	

	if ( xQueueReceive(magQueue, &ev, 0) != pdTRUE )
	{
		// Rotate gravity to body frame and cross with accels
		float brot[3];
		float Rbe[3][3];
		MagnetometerData mag;
		
		Quaternion2R(q, Rbe);
		MagnetometerGet(&mag);

		// If the mag is producing bad data don't use it (normally bad calibration)
		if  (!isnan(mag.x) && !isinf(mag.x) && !isnan(mag.y) && !isinf(mag.y) && !isnan(mag.z) && !isinf(mag.z)) {
			rot_mult(Rbe, homeLocation.Be, brot);

			float mag_len = sqrtf(mag.x * mag.x + mag.y * mag.y + mag.z * mag.z);
			mag.x /= mag_len;
			mag.y /= mag_len;
			mag.z /= mag_len;

			float bmag = sqrtf(brot[0] * brot[0] + brot[1] * brot[1] + brot[2] * brot[2]);
			brot[0] /= bmag;
			brot[1] /= bmag;
			brot[2] /= bmag;

			// Only compute if neither vector is null
			if (bmag < 1.0f || mag_len < 1.0f)
				mag_err[0] = mag_err[1] = mag_err[2] = 0.0f;
			else
				CrossProduct((const float *) &mag.x, (const float *) brot, mag_err);
		}
	} else {
		mag_err[0] = mag_err[1] = mag_err[2] = 0.0f;
	}

	// Accumulate integral of error.  Scale here so that units are (deg/s) but Ki has units of s
	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x -= accel_err[0] * attitudeSettings.AccelKi;
	gyrosBias.y -= accel_err[1] * attitudeSettings.AccelKi;
	gyrosBias.z -= mag_err[2] * magKi;
	GyrosBiasSet(&gyrosBias);

	if (revoCalibration.BiasCorrectedRaw != REVOCALIBRATION_BIASCORRECTEDRAW_TRUE) {
		// if the raw values are not adjusted, we need to adjust here.
		gyrosData.x -= gyrosBias.x;
		gyrosData.y -= gyrosBias.y;
		gyrosData.z -= gyrosBias.z;
	}

	// Correct rates based on error, integral component dealt with in updateSensors
	gyrosData.x += accel_err[0] * attitudeSettings.AccelKp / dT;
	gyrosData.y += accel_err[1] * attitudeSettings.AccelKp / dT;
	gyrosData.z += accel_err[2] * attitudeSettings.AccelKp / dT + mag_err[2] * magKp / dT;

	// Work out time derivative from INSAlgo writeup
	// Also accounts for the fact that gyros are in deg/s
	float qdot[4];
	qdot[0] = DEG2RAD(-q[1] * gyrosData.x - q[2] * gyrosData.y - q[3] * gyrosData.z) * dT / 2;
	qdot[1] = DEG2RAD(q[0] * gyrosData.x - q[3] * gyrosData.y + q[2] * gyrosData.z) * dT / 2;
	qdot[2] = DEG2RAD(q[3] * gyrosData.x + q[0] * gyrosData.y - q[1] * gyrosData.z) * dT / 2;
	qdot[3] = DEG2RAD(-q[2] * gyrosData.x + q[1] * gyrosData.y + q[0] * gyrosData.z) * dT / 2;

	// Take a time step
	q[0] = q[0] + qdot[0];
	q[1] = q[1] + qdot[1];
	q[2] = q[2] + qdot[2];
	q[3] = q[3] + qdot[3];

	if(q[0] < 0.0f) {
		q[0] = -q[0];
		q[1] = -q[1];
		q[2] = -q[2];
		q[3] = -q[3];
	}

	// Renomalize
	qmag = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	q[0] = q[0] / qmag;
	q[1] = q[1] / qmag;
	q[2] = q[2] / qmag;
	q[3] = q[3] / qmag;

	// If quaternion has become inappropriately short or is nan reinit.
	// THIS SHOULD NEVER ACTUALLY HAPPEN
	if((fabsf(qmag) < 1.0e-3f) || isnan(qmag)) {
		q[0] = 1.0f;
		q[1] = 0.0f;
		q[2] = 0.0f;
		q[3] = 0.0f;
	}

	quat_copy(q, &attitudeActual.q1);

	// Convert into eueler degrees (makes assumptions about RPY order)
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);

	AttitudeActualSet(&attitudeActual);

	// Flush these queues for avoid errors
	xQueueReceive(baroQueue, &ev, 0);
	if ( xQueueReceive(gpsQueue, &ev, 0) == pdTRUE && homeLocation.Set == HOMELOCATION_SET_TRUE ) {
		float NED[3];
		// Transform the GPS position into NED coordinates
		GPSPositionData gpsPosition;
		GPSPositionGet(&gpsPosition);
		getNED(&gpsPosition, NED);
		
		PositionActualData positionActual;
		PositionActualGet(&positionActual);
		positionActual.North = NED[0];
		positionActual.East = NED[1];
		positionActual.Down = NED[2];
		PositionActualSet(&positionActual);
	}

	if ( xQueueReceive(gpsVelQueue, &ev, 0) == pdTRUE ) {
		// Transform the GPS position into NED coordinates
		GPSVelocityData gpsVelocity;
		GPSVelocityGet(&gpsVelocity);

		VelocityActualData velocityActual;
		VelocityActualGet(&velocityActual);
		velocityActual.North = gpsVelocity.North;
		velocityActual.East = gpsVelocity.East;
		velocityActual.Down = gpsVelocity.Down;
		VelocityActualSet(&velocityActual);
	}
	
	if ( xQueueReceive(airspeedQueue, &ev, 0) == pdTRUE ) {
		// Calculate true airspeed from indicated airspeed
		AirspeedSensorData airspeedSensor;
		AirspeedSensorGet(&airspeedSensor);

		AirspeedActualData airspeed;
		AirspeedActualGet(&airspeed);

		PositionActualData positionActual;
		PositionActualGet(&positionActual);

		if (airspeedSensor.SensorConnected==AIRSPEEDSENSOR_SENSORCONNECTED_TRUE) {
			// we have airspeed available
			airspeed.CalibratedAirspeed = airspeedSensor.CalibratedAirspeed;
			airspeed.TrueAirspeed = airspeed.CalibratedAirspeed * IAS2TAS( homeLocation.Altitude - positionActual.Down );
			AirspeedActualSet(&airspeed);
		}
	}


	if ( variance_error ) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_CRITICAL);
	} else {
		AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
	}


	return 0;
}

#include "insgps.h"
int32_t ins_failed = 0;
extern struct NavStruct Nav;
int32_t init_stage = 0;

/**
 * @brief Use the INSGPS fusion algorithm in either indoor or outdoor mode (use GPS)
 * @params[in] first_run This is the first run so trigger reinitialization
 * @params[in] outdoor_mode If true use the GPS for position, if false weakly pull to (0,0)
 * @return 0 for success, -1 for failure
 */
static int32_t updateAttitudeINSGPS(bool first_run, bool outdoor_mode)
{
	UAVObjEvent ev;
	GyrosData gyrosData;
	AccelsData accelsData;
	MagnetometerData magData;
	AirspeedSensorData airspeedData;
	BaroAltitudeData baroData;
	GPSPositionData gpsData;
	GPSVelocityData gpsVelData;
	GyrosBiasData gyrosBias;

	static bool mag_updated = false;
	static bool baro_updated;
	static bool airspeed_updated;
	static bool gps_updated;
	static bool gps_vel_updated;

	static bool value_error = false;

	static float baroOffset = 0.0f;

	static uint32_t ins_last_time = 0;
	static bool inited;

	float NED[3] = {0.0f, 0.0f, 0.0f};
	float vel[3] = {0.0f, 0.0f, 0.0f};
	float zeros[3] = {0.0f, 0.0f, 0.0f};

	// Perform the update
	uint16_t sensors = 0;
	float dT;

	// Wait until the gyro and accel object is updated, if a timeout then go to failsafe
	if ( (xQueueReceive(gyroQueue, &ev, FAILSAFE_TIMEOUT_MS / portTICK_RATE_MS) != pdTRUE) ||
	     (xQueueReceive(accelQueue, &ev, 1 / portTICK_RATE_MS) != pdTRUE) )
	{
		// Do not set attitude timeout warnings in simulation mode
		if (!AttitudeActualReadOnly()){
			AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
			return -1;
		}
	}

	if (inited) {
		mag_updated = 0;
		baro_updated = 0;
		airspeed_updated = 0;
		gps_updated = 0;
		gps_vel_updated = 0;
	}

	if (first_run) {
		inited = false;
		init_stage = 0;

		mag_updated = 0;
		baro_updated = 0;
		airspeed_updated = 0;
		gps_updated = 0;
		gps_vel_updated = 0;

		ins_last_time = PIOS_DELAY_GetRaw();

		return 0;
	}

	mag_updated |= (xQueueReceive(magQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE);
	baro_updated |= xQueueReceive(baroQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE;
	airspeed_updated |= xQueueReceive(airspeedQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE;

	// Check if we are running simulation
	if (!GPSPositionReadOnly()) {
	    gps_updated |= (xQueueReceive(gpsQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE) && outdoor_mode;
	} else {
	    gps_updated |= pdTRUE && outdoor_mode;
	}

    if (!GPSVelocityReadOnly()) {
        gps_vel_updated |= (xQueueReceive(gpsVelQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE) && outdoor_mode;
    } else {
        gps_vel_updated |= pdTRUE && outdoor_mode;
    }

	// Get most recent data
	GyrosGet(&gyrosData);
	AccelsGet(&accelsData);
	MagnetometerGet(&magData);
	BaroAltitudeGet(&baroData);
	AirspeedSensorGet(&airspeedData);
	GPSPositionGet(&gpsData);
	GPSVelocityGet(&gpsVelData);
	GyrosBiasGet(&gyrosBias);

	value_error = false;
	// safety checks
	if ( invalid(gyrosData.x) ||
	     invalid(gyrosData.y) ||
	     invalid(gyrosData.z) ||
	     invalid(accelsData.x) ||
	     invalid(accelsData.y) ||
	     invalid(accelsData.z) ) {
		// cannot run process update, raise error!
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_ERROR);
		return 0;
	}
	if ( invalid(gyrosBias.x) ||
	     invalid(gyrosBias.y) ||
	     invalid(gyrosBias.z) ) {
		gyrosBias.x = 0.0f;
		gyrosBias.y = 0.0f;
		gyrosBias.z = 0.0f;
	}

	if ( invalid(magData.x) ||
	     invalid(magData.y) ||
	     invalid(magData.z) ) {

		// magnetometers can be ignored for a while
		mag_updated = false;
		value_error = true;
	}

	// Don't require HomeLocation.Set to be true but at least require a mag configuration (allows easily
	// switching between indoor and outdoor mode with Set = false)
	if ( (homeLocation.Be[0] * homeLocation.Be[0] + homeLocation.Be[1] * homeLocation.Be[1] + homeLocation.Be[2] * homeLocation.Be[2] < 1e-5f) ) {
		mag_updated = false;
		value_error = true;
	}

	if ( invalid(baroData.Altitude) ) {
		baro_updated = false;
		value_error = true;
	}

	if ( invalid(airspeedData.CalibratedAirspeed) ) {
		airspeed_updated = false;
		value_error = true;
	}

	if ( invalid(gpsData.Altitude) ) {
		gps_updated = false;
		value_error = true;
	}

	if ( invalid_var(ekfConfiguration.R[EKFCONFIGURATION_R_GPSPOSNORTH]) ||
	     invalid_var(ekfConfiguration.R[EKFCONFIGURATION_R_GPSPOSEAST]) ||
	     invalid_var(ekfConfiguration.R[EKFCONFIGURATION_R_GPSPOSDOWN]) ||
	     invalid_var(ekfConfiguration.R[EKFCONFIGURATION_R_GPSVELNORTH]) ||
	     invalid_var(ekfConfiguration.R[EKFCONFIGURATION_R_GPSVELEAST]) ||
	     invalid_var(ekfConfiguration.R[EKFCONFIGURATION_R_GPSVELDOWN]) ) {
		gps_updated = false;
		value_error = true;
	}

	if ( invalid(gpsVelData.North) ||
	     invalid(gpsVelData.East) ||
	     invalid(gpsVelData.Down) ) {
		gps_vel_updated = false;
		value_error = true;
	}

	// Discard airspeed if sensor not connected
	if ( airspeedData.SensorConnected != AIRSPEEDSENSOR_SENSORCONNECTED_TRUE ) {
		airspeed_updated = false;
	}

	// Have a minimum requirement for gps usage
	if ( ( gpsData.Satellites < 7 ) ||
	     ( gpsData.PDOP > 4.0f ) ||
	     ( gpsData.Latitude==0 && gpsData.Longitude==0 ) ||
	     ( homeLocation.Set != HOMELOCATION_SET_TRUE ) ) {
		gps_updated = false;
		gps_vel_updated = false;
	}

	if ( !inited ) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_ERROR);
	} else if ( value_error ) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_CRITICAL);
	} else if ( variance_error ) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_CRITICAL);
	} else if (outdoor_mode && gpsData.Satellites < 7) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_ERROR);
	} else {
		AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
	}
			
	dT = PIOS_DELAY_DiffuS(ins_last_time) / 1.0e6f;
	ins_last_time = PIOS_DELAY_GetRaw();

	// This should only happen at start up or at mode switches
	if(dT > 0.01f) {
		dT = 0.01f;
	} else if(dT <= 0.001f) {
		dT = 0.001f;
	}

	if (!inited && mag_updated && baro_updated && (gps_updated || !outdoor_mode) && !variance_error) {

		// Don't initialize until all sensors are read
		if (init_stage == 0) {

			// Reset the INS algorithm
			INSGPSInit();
			INSSetMagVar( (float[3]){ ekfConfiguration.R[EKFCONFIGURATION_R_MAGX],
			                          ekfConfiguration.R[EKFCONFIGURATION_R_MAGY],
			                          ekfConfiguration.R[EKFCONFIGURATION_R_MAGZ] } );
			INSSetAccelVar( (float[3]){ ekfConfiguration.Q[EKFCONFIGURATION_Q_ACCELX],
			                            ekfConfiguration.Q[EKFCONFIGURATION_Q_ACCELY],
			                            ekfConfiguration.Q[EKFCONFIGURATION_Q_ACCELZ] } );
			INSSetGyroVar( (float[3]){ ekfConfiguration.Q[EKFCONFIGURATION_Q_GYROX],
			                            ekfConfiguration.Q[EKFCONFIGURATION_Q_GYROY],
			                            ekfConfiguration.Q[EKFCONFIGURATION_Q_GYROZ] } );
			INSSetGyroBiasVar( (float[3]){ ekfConfiguration.Q[EKFCONFIGURATION_Q_GYRODRIFTX],
			                               ekfConfiguration.Q[EKFCONFIGURATION_Q_GYRODRIFTY],
			                               ekfConfiguration.Q[EKFCONFIGURATION_Q_GYRODRIFTZ] } );
			INSSetBaroVar(ekfConfiguration.R[EKFCONFIGURATION_R_BAROZ]);

			// Initialize the gyro bias
			float gyro_bias[3] = {0.0f, 0.0f, 0.0f};
			INSSetGyroBias(gyro_bias);

			float pos[3] = {0.0f, 0.0f, 0.0f};

			if (outdoor_mode) {

				GPSPositionData gpsPosition;
				GPSPositionGet(&gpsPosition);

				// Transform the GPS position into NED coordinates
				getNED(&gpsPosition, pos);

				// Initialize barometric offset to current GPS NED coordinate
				baroOffset = -pos[2] - baroData.Altitude;

			} else {

				// Initialize barometric offset to homelocation altitude
				baroOffset = -baroData.Altitude;
				pos[2] = -(baroData.Altitude + baroOffset);

			}

			xQueueReceive(magQueue, &ev, 100 / portTICK_RATE_MS);
			MagnetometerGet(&magData);

			AttitudeActualData attitudeActual;
			AttitudeActualGet (&attitudeActual);

			// Set initial attitude. Use accels to determine roll and pitch, rotate magnetic measurement accordingly,
			// so pseudo "north" vector can be estimated even if the board is not level
			attitudeActual.Roll = atan2f(-accelsData.y, -accelsData.z);
			float zn = cosf(attitudeActual.Roll) * magData.z + sinf(attitudeActual.Roll) * magData.y;
			float yn = cosf(attitudeActual.Roll) * magData.y - sinf(attitudeActual.Roll) * magData.z;

			// rotate accels z vector according to roll
			float azn = cosf(attitudeActual.Roll) * accelsData.z + sinf(attitudeActual.Roll) * accelsData.y;
			attitudeActual.Pitch = atan2f(accelsData.x, -azn);

			float xn = cosf(attitudeActual.Pitch) * magData.x + sinf(attitudeActual.Pitch) * zn;

			attitudeActual.Yaw = atan2f(-yn, xn);
			// TODO: This is still a hack
			// Put this in a proper generic function in CoordinateConversion.c
			// should take 4 vectors: g (0,0,-9.81), accels, Be (or 1,0,0 if no home loc) and magnetometers (or 1,0,0 if no mags)
			// should calculate the rotation in 3d space using proper cross product math
			// SUBTODO: formulate the math required

			attitudeActual.Roll = RAD2DEG(attitudeActual.Roll);
			attitudeActual.Pitch = RAD2DEG(attitudeActual.Pitch);
			attitudeActual.Yaw = RAD2DEG(attitudeActual.Yaw);

			RPY2Quaternion(&attitudeActual.Roll,&attitudeActual.q1);
			AttitudeActualSet(&attitudeActual);

			float q[4] = { attitudeActual.q1, attitudeActual.q2, attitudeActual.q3, attitudeActual.q4 };
			INSSetState(pos, zeros, q, zeros, zeros);
			
			INSResetP(ekfConfiguration.P);

		} else {
			// Run prediction a bit before any corrections

			// Because the sensor module remove the bias we need to add it
			// back in here so that the INS algorithm can track it correctly
			float gyros[3] = { DEG2RAD(gyrosData.x), DEG2RAD(gyrosData.y), DEG2RAD(gyrosData.z) };
			if (revoCalibration.BiasCorrectedRaw == REVOCALIBRATION_BIASCORRECTEDRAW_TRUE) {
				gyros[0] += DEG2RAD(gyrosBias.x);
				gyros[1] += DEG2RAD(gyrosBias.y);
				gyros[2] += DEG2RAD(gyrosBias.z);
			}
			INSStatePrediction(gyros, &accelsData.x, dT);

			AttitudeActualData attitude;
			AttitudeActualGet(&attitude);
			attitude.q1 = Nav.q[0];
			attitude.q2 = Nav.q[1];
			attitude.q3 = Nav.q[2];
			attitude.q4 = Nav.q[3];
			Quaternion2RPY(&attitude.q1,&attitude.Roll);
			AttitudeActualSet(&attitude);
		}

		init_stage++;
		if(init_stage > 10)
			inited = true;

		return 0;
	}

	if (!inited)
		return 0;

	// Because the sensor module remove the bias we need to add it
	// back in here so that the INS algorithm can track it correctly
	float gyros[3] = { DEG2RAD(gyrosData.x), DEG2RAD(gyrosData.y), DEG2RAD(gyrosData.z) };
	if (revoCalibration.BiasCorrectedRaw == REVOCALIBRATION_BIASCORRECTEDRAW_TRUE) {
		gyros[0] += DEG2RAD(gyrosBias.x);
		gyros[1] += DEG2RAD(gyrosBias.y);
		gyros[2] += DEG2RAD(gyrosBias.z);
	}

	// Advance the state estimate
	INSStatePrediction(gyros, &accelsData.x, dT);

	// Copy the attitude into the UAVO
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = Nav.q[0];
	attitude.q2 = Nav.q[1];
	attitude.q3 = Nav.q[2];
	attitude.q4 = Nav.q[3];
	Quaternion2RPY(&attitude.q1,&attitude.Roll);
	AttitudeActualSet(&attitude);

	// Advance the covariance estimate
	INSCovariancePrediction(dT);

	if(mag_updated)
		sensors |= MAG_SENSORS;

	if(baro_updated)
		sensors |= BARO_SENSOR;

	INSSetMagNorth(homeLocation.Be);
	
	if (gps_updated && outdoor_mode)
	{
		INSSetPosVelVar((float[3]){ ekfConfiguration.R[EKFCONFIGURATION_R_GPSPOSNORTH],
		                            ekfConfiguration.R[EKFCONFIGURATION_R_GPSPOSEAST],
		                            ekfConfiguration.R[EKFCONFIGURATION_R_GPSPOSDOWN] },
		                (float[3]){ ekfConfiguration.R[EKFCONFIGURATION_R_GPSVELNORTH],
		                            ekfConfiguration.R[EKFCONFIGURATION_R_GPSVELEAST],
		                            ekfConfiguration.R[EKFCONFIGURATION_R_GPSVELDOWN] });
		sensors |= POS_SENSORS;

		if (0) { // Old code to take horizontal velocity from GPS Position update
			sensors |= HORIZ_SENSORS;
			vel[0] = gpsData.Groundspeed * cosf(DEG2RAD(gpsData.Heading));
			vel[1] = gpsData.Groundspeed * sinf(DEG2RAD(gpsData.Heading));
			vel[2] = 0.0f;
		}
		// Transform the GPS position into NED coordinates
		getNED(&gpsData, NED);

		// Track barometric altitude offset with a low pass filter
		baroOffset = BARO_OFFSET_LOWPASS_ALPHA * baroOffset +
		    (1.0f - BARO_OFFSET_LOWPASS_ALPHA )
		    * ( -NED[2] - baroData.Altitude );

	} else if (!outdoor_mode) {
		INSSetPosVelVar((float[3]){ ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSPOSINDOOR],
		                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSPOSINDOOR],
		                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSPOSINDOOR] },
		                (float[3]){ ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSVELINDOOR],
		                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSVELINDOOR],
		                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSVELINDOOR] });
		vel[0] = vel[1] = vel[2] = 0.0f;
		NED[0] = NED[1] = 0.0f;
		NED[2] = -(baroData.Altitude + baroOffset);
		sensors |= HORIZ_SENSORS | HORIZ_POS_SENSORS;
		sensors |= POS_SENSORS |VERT_SENSORS;
	}

	if (gps_vel_updated && outdoor_mode) {
		sensors |= HORIZ_SENSORS | VERT_SENSORS;
		vel[0] = gpsVelData.North;
		vel[1] = gpsVelData.East;
		vel[2] = gpsVelData.Down;
	}

	if (airspeed_updated) {
		// we have airspeed available
		AirspeedActualData airspeed;
		AirspeedActualGet(&airspeed);

		airspeed.CalibratedAirspeed = airspeedData.CalibratedAirspeed;
		airspeed.TrueAirspeed = airspeed.CalibratedAirspeed * IAS2TAS( homeLocation.Altitude - Nav.Pos[2] );
		AirspeedActualSet(&airspeed);
		
		if ( !gps_vel_updated && !gps_updated) {
			// feed airspeed into EKF, treat wind as 1e2 variance
			sensors |= HORIZ_SENSORS | VERT_SENSORS;
			INSSetPosVelVar((float[3]){ ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSPOSINDOOR],
			                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSPOSINDOOR],
			                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSPOSINDOOR] },
			                (float[3]){ ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSVELAIRSPEED],
			                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSVELAIRSPEED],
			                            ekfConfiguration.FakeR[EKFCONFIGURATION_FAKER_FAKEGPSVELAIRSPEED] });
			// rotate airspeed vector into NED frame - airspeed is measured in X axis only
			float R[3][3];
			Quaternion2R(Nav.q,R);
			float vtas[3] = {airspeed.TrueAirspeed,0.0f,0.0f};
			rot_mult(R,vtas,vel);
		}
	}
	
	/*
	 * TODO: Need to add a general sanity check for all the inputs to make sure their kosher
	 * although probably should occur within INS itself
	 */
	if (sensors)
		INSCorrection(&magData.x, NED, vel, ( baroData.Altitude + baroOffset ), sensors);

	// Copy the position and velocity into the UAVO
	PositionActualData positionActual;
	PositionActualGet(&positionActual);
	positionActual.North = Nav.Pos[0];
	positionActual.East = Nav.Pos[1];
	positionActual.Down = Nav.Pos[2];
	PositionActualSet(&positionActual);
	
	VelocityActualData velocityActual;
	VelocityActualGet(&velocityActual);
	velocityActual.North = Nav.Vel[0];
	velocityActual.East = Nav.Vel[1];
	velocityActual.Down = Nav.Vel[2];
	VelocityActualSet(&velocityActual);

	gyrosBias.x = RAD2DEG(Nav.gyro_bias[0]);
	gyrosBias.y = RAD2DEG(Nav.gyro_bias[1]);
	gyrosBias.z = RAD2DEG(Nav.gyro_bias[2]);
	GyrosBiasSet(&gyrosBias);

	EKFStateVarianceData vardata;
	EKFStateVarianceGet(&vardata);
	INSGetP(vardata.P);
	EKFStateVarianceSet(&vardata);

	return 0;
}

/**
 * @brief Convert the GPS LLA position into NED coordinates
 * @note this method uses a taylor expansion around the home coordinates
 * to convert to NED which allows it to be done with all floating
 * calculations
 * @param[in] Current GPS coordinates
 * @param[out] NED frame coordinates
 * @returns 0 for success, -1 for failure
 */
float T[3];
static int32_t getNED(GPSPositionData * gpsPosition, float * NED)
{
	float dL[3] = { DEG2RAD((gpsPosition->Latitude - homeLocation.Latitude) / 10.0e6f),
	    DEG2RAD((gpsPosition->Longitude - homeLocation.Longitude) / 10.0e6f),
		(gpsPosition->Altitude + gpsPosition->GeoidSeparation - homeLocation.Altitude) };

	NED[0] = T[0] * dL[0];
	NED[1] = T[1] * dL[1];
	NED[2] = T[2] * dL[2];

	return 0;
}

static void settingsUpdatedCb(UAVObjEvent * ev) 
{
	if (ev == NULL || ev->obj == FlightStatusHandle()) {
		FlightStatusGet(&flightStatus);
	}
	if (ev == NULL || ev->obj == RevoCalibrationHandle()) {
		RevoCalibrationGet(&revoCalibration);
	}
	// change of these settings require reinitialization of the EKF
	// when an error flag has been risen, we also listen to flightStatus updates,
	// since we are waiting for the system to get disarmed so we can reinitialize safely.
	if (ev == NULL ||
			ev->obj == EKFConfigurationHandle() ||
			ev->obj == RevoSettingsHandle() ||
			( variance_error==true && ev->obj == FlightStatusHandle() )
			) {

		bool error = false;

		EKFConfigurationGet(&ekfConfiguration);
		int t;
		for (t=0; t < EKFCONFIGURATION_P_NUMELEM; t++) {
			if (invalid_var(ekfConfiguration.P[t])) {
				error = true;
			}
		}
		for (t=0; t < EKFCONFIGURATION_Q_NUMELEM; t++) {
			if (invalid_var(ekfConfiguration.Q[t])) {
				error = true;
			}
		}
		for (t=0; t < EKFCONFIGURATION_R_NUMELEM; t++) {
			if (invalid_var(ekfConfiguration.R[t])) {
				error = true;
			}
		}

		RevoSettingsGet(&revoSettings);

		// Reinitialization of the EKF is not desired during flight.
		// It will be delayed until the board is disarmed by raising the error flag.
		// We will not prevent the initial initialization though, since the board could be in always armed mode.
		if (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMED && !initialization_required ) {
			error = true;
		}

		if (error) {
			variance_error = true;
		} else {
			// trigger reinitialization - possibly with new algorithm
			running_algorithm = revoSettings.FusionAlgorithm;
			variance_error = false;
			initialization_required = true;
		}
	}
	if(ev == NULL || ev->obj == HomeLocationHandle()) {
		HomeLocationGet(&homeLocation);
		// Compute matrix to convert deltaLLA to NED
		float lat, alt;
		lat = DEG2RAD(homeLocation.Latitude / 10.0e6f);
		alt = homeLocation.Altitude;

		T[0] = alt+6.378137E6f;
		T[1] = cosf(lat)*(alt+6.378137E6f);
		T[2] = -1.0f;

		// TODO: convert positionActual to new reference frame and gracefully update EKF state!
		// needed for long range flights where the reference coordinate is adjusted in flight
	}
	if (ev == NULL || ev->obj == AttitudeSettingsHandle())
		AttitudeSettingsGet(&attitudeSettings);
}
/**
 * @}
 * @}
 */
