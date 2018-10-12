/**
 ******************************************************************************
 * @addtogroup Modules Modules
 * @{ 
 * @addtogroup UAVOTaranis UAVO to Taranis S.PORT converter
 * @{ 
 *
 * @file       uavoFrSKYSensorHubBridge.c
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2015
 * @brief      Bridges selected UAVObjects to Taranis S.PORT
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
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
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

/*
 * This module is derived from UAVOFrSKYSportPort but differs
 * in a number of ways. Specifically the SPort code is expected
 * to listen for sensor requests and respond appropriately. This
 * code simply can spew data to the Taranis (since it is talking
 * to different harware). The scheduling system is reused between
 * the two, but duplicated because it isn't really part of the 
 * packing, and touches on the private state of the state machine
 * code in the module 
 */

#include "frsky_packing.h"
#include "pios_thread.h"
#include "pios_modules.h"
#include "modulesettings.h"
#include "flightbatterysettings.h"
#include "flightbatterystate.h"
#include "gpsposition.h"
#include "baroaltitude.h"

#if defined(PIOS_INCLUDE_TARANIS_SPORT)

static void uavoTaranisTask(void *parameters);

#define FRSKY_POLL_REQUEST                 0x7e
#define FRSKY_MINIMUM_POLL_INTERVAL        10000

static const struct frsky_value_item frsky_value_items[] = {
	{FRSKY_CURR_ID,        300,   frsky_encode_current,    0}, // battery current
	{FRSKY_FUEL_ID,        600,   frsky_encode_fuel,       0}, // consumed battery energy
	{FRSKY_RSSI_ID,        100,   frsky_encode_rssi,       0}, // send RSSI information
	{FRSKY_ALT_ID,         100,   frsky_encode_altitude,   0}, // altitude estimate
	{FRSKY_VARIO_ID,       100,   frsky_encode_vario,      0}, // vertical speed
	{FRSKY_RPM_ID,         1500,  frsky_encode_rpm,        0}, // encodes flight status!
	{FRSKY_CELLS_ID,       850,   frsky_encode_cells,      0}, // battery cells 1-2
	{FRSKY_CELLS_ID,       850,   frsky_encode_cells,      1}, // battery cells 3-4
	{FRSKY_CELLS_ID,       850,   frsky_encode_cells,      2}, // battery cells 5-6
	{FRSKY_CELLS_ID,       850,   frsky_encode_cells,      3}, // battery cells 7-8
	{FRSKY_CELLS_ID,       850,   frsky_encode_cells,      4}, // battery cells 9-10
	{FRSKY_CELLS_ID,       850,   frsky_encode_cells,      5}, // battery cells 11-12
};

struct frsky_sport_telemetry {
	int32_t scheduled_item;
	uint32_t last_poll_time;
	uintptr_t com;
	struct frsky_settings frsky_settings;
	uint32_t item_last_triggered[NELEMENTS(frsky_value_items)];
};

#define FRSKY_SPORT_BAUDRATE                    57600

#if defined(PIOS_FRSKY_SPORT_TELEMETRY_STACK_SIZE)
#define STACK_SIZE_BYTES PIOS_FRSKY_SPORT_TELEMETRY_STACK_SIZE
#else
#define STACK_SIZE_BYTES 672
#endif
#define TASK_PRIORITY               PIOS_THREAD_PRIO_LOW

static struct pios_thread *uavoTaranisTaskHandle;
static bool module_enabled;
static struct frsky_sport_telemetry *frsky;

/**
 * Scan for value item with the longest expired time and schedule it to send in next poll turn
 *
 */
static void frsky_schedule_next_item(void)
{
	uint32_t i = 0;
	int32_t max_exp_time = INT32_MIN;
	int32_t most_exp_item = -1;
	for (i = 0; i < NELEMENTS(frsky_value_items); i++) {
		if (frsky_value_items[i].encode_value(&frsky->frsky_settings, 0, true, frsky_value_items[i].fn_arg)) {
			int32_t exp_time = PIOS_DELAY_GetuSSince(frsky->item_last_triggered[i]) -
					(frsky_value_items[i].period_ms * 1000);
			if (exp_time > max_exp_time) {
				max_exp_time = exp_time;
				most_exp_item = i;
			}
		}
	}
	frsky->scheduled_item = most_exp_item;
}
/**
 * Send value item previously scheduled by frsky_schedule_next_itme()
 * @returns true when item value was sended
 */
static bool frsky_send_scheduled_item(void)
{
	int32_t item = frsky->scheduled_item;
	if ((item >= 0) && (item < NELEMENTS(frsky_value_items))) {
		frsky->item_last_triggered[item] = PIOS_DELAY_GetuS();
		uint32_t value = 0;
		if (frsky_value_items[item].encode_value(&frsky->frsky_settings, &value, false,
				frsky_value_items[item].fn_arg)) {
			frsky_send_frame(frsky->com, (uint16_t)(frsky_value_items[item].id), value, true);
			return true;
		}
	}

	return false;
}

/**
 * Start the module
 * \return -1 if start failed
 * \return 0 on success
 */
static int32_t uavoTaranisStart(void)
{
	if (!module_enabled)
		return -1;

	if (FlightBatterySettingsHandle() != NULL
			&& FlightBatteryStateHandle() != NULL) {
		uint8_t currentPin;
		FlightBatterySettingsCurrentPinGet(&currentPin);
		if (currentPin != FLIGHTBATTERYSETTINGS_CURRENTPIN_NONE)
			frsky->frsky_settings.use_current_sensor = true;
		FlightBatterySettingsGet(&frsky->frsky_settings.battery_settings);
		frsky->frsky_settings.batt_cell_count = frsky->frsky_settings.battery_settings.NbCells;
	}
	if (BaroAltitudeHandle() != NULL
			&& PIOS_SENSORS_IsRegistered(PIOS_SENSOR_BARO))
		frsky->frsky_settings.use_baro_sensor = true;

	// Start tasks
	uavoTaranisTaskHandle = PIOS_Thread_Create(
			uavoTaranisTask, "uavoFrSKYSensorHubBridge",
			STACK_SIZE_BYTES, NULL, TASK_PRIORITY);
	TaskMonitorAdd(TASKINFO_RUNNING_UAVOFRSKYSENSORHUBBRIDGE,
			uavoTaranisTaskHandle);
	return 0;
}

/**
 * Initialize the module
 * \return -1 if initialization failed
 * \return 0 on success
 */
static int32_t uavoTaranisInitialize(void)
{
	uintptr_t sport_com = PIOS_COM_FRSKY_SPORT;

	if (sport_com) {
		frsky = PIOS_malloc(sizeof(struct frsky_sport_telemetry));
		if (frsky != NULL) {
			memset(frsky, 0x00, sizeof(struct frsky_sport_telemetry));

			frsky->frsky_settings.use_current_sensor = true;
			frsky->frsky_settings.batt_cell_count = 0;
			frsky->frsky_settings.use_baro_sensor = true;
			frsky->last_poll_time = PIOS_DELAY_GetuS();
			frsky->scheduled_item = -1;
			frsky->com = sport_com;

			uint8_t i;
			for (i = 0; i < NELEMENTS(frsky_value_items); i++)
				frsky->item_last_triggered[i] = PIOS_DELAY_GetuS();
			PIOS_COM_ChangeBaud(frsky->com, FRSKY_SPORT_BAUDRATE);
			module_enabled = true;
			return 0;
		}
	}

	return -1;
}

MODULE_INITCALL(uavoTaranisInitialize, uavoTaranisStart)

/**
 * Main task. It does not return.
 */
static void uavoTaranisTask(void *parameters)
{
	while (1) {
		frsky_send_scheduled_item();
		frsky_schedule_next_item();
		PIOS_Thread_Sleep(10);

	}
}

#endif /* PIOS_INCLUDE_TARANIS_SPORT */
/**
 * @}
 * @}
 */
