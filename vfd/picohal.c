/*
  picohal.c - PICOHAL Modbus spindle support

  Part of grblHAL

  Copyright (c) 2024 Mitchell Grams
  Copyright (c) 2023-2024 Terje Io

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  grblHAL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with grblHAL. If not, see <http://www.gnu.org/licenses/>.

*/

#include "../shared.h"

#if SPINDLE_ENABLE & (1<<SPINDLE_PICOHAL)

#include <math.h>
#include <string.h>

#include "spindle.h"

static uint32_t modbus_address;
static spindle_id_t spindle_id;
static uint32_t freq_min = 0, freq_max = 0;
static spindle_ptrs_t *spindle_hal = NULL;
static spindle_data_t spindle_data = {0};
static spindle_state_t vfd_state = {0};
static on_report_options_ptr on_report_options;
static on_spindle_selected_ptr on_spindle_selected;
static settings_changed_ptr settings_changed;

static void rx_packet (modbus_message_t *msg);
static void rx_exception (uint8_t code, void *context);

static const modbus_callbacks_t callbacks = {
    .on_rx_packet = rx_packet,
    .on_rx_exception = rx_exception
};

static bool spindleConfig (spindle_ptrs_t *spindle)
{
    return modbus_isup();
}


static void spindleSetRPM (float rpm, bool block)
{
    if(rpm != spindle_data.rpm_programmed ) {

        uint16_t power = (uint16_t)(rpm);

        modbus_message_t rpm_cmd = {
            .context = (void *)VFD_SetRPM,
            .crc_check = false,
            .adu[0] = modbus_address,
            .adu[1] = ModBus_WriteRegister,
            .adu[2] = 0x09, //register address MSB
            .adu[3] = 0x01, //register address LSB
            .adu[4] = power >> 8,
            .adu[5] = power & 0xFF,
            .tx_length = 8,
            .rx_length = 8
        };

        modbus_send(&rpm_cmd, &callbacks, block);

        spindle_set_at_speed_range(spindle_hal, &spindle_data, rpm); //does this need to be here?
    }
}

static void spindleUpdateRPM (spindle_ptrs_t *spindle, float rpm)
{
    spindleSetRPM(rpm, false);
}

// Start or stop spindle
static void spindleSetState (spindle_ptrs_t *spindle, spindle_state_t state, float rpm)
{
    modbus_message_t mode_cmd = {
        .context = (void *)VFD_SetStatus,
        .crc_check = false,
        .adu[0] = modbus_address,
        .adu[1] = ModBus_WriteRegister,
        .adu[2] = 0x09,
        .adu[3] = 0x00,
        .adu[4] = 0x00,
        .adu[5] = (!state.on || rpm == 0.0f) ? 0x00 : (state.ccw ? 0x03 : 0x01),
        .tx_length = 8,
        .rx_length = 8
    };

    if(vfd_state.ccw != state.ccw)
        spindle_data.rpm_programmed = 0.0f;

    vfd_state.on = state.on;
    vfd_state.ccw = state.ccw;

    if(modbus_send(&mode_cmd, &callbacks, true))
        spindleSetRPM(rpm, true);
}

// Returns spindle state in a spindle_state_t variable
static spindle_state_t spindleGetState (spindle_ptrs_t *spindle)
{
    modbus_message_t mode_cmd = {
        .context = (void *)VFD_GetRPM,
        .crc_check = false,
        .adu[0] = modbus_address,
        .adu[1] = ModBus_ReadHoldingRegisters,
        .adu[2] = 0x05,
        .adu[3] = 0x00,
        .adu[4] = 0x00,
        .adu[5] = 0x01,
        .tx_length = 8,
        .rx_length = 7
    };

    modbus_send(&mode_cmd, &callbacks, false); // TODO: add flag for not raising alarm?

    vfd_state.at_speed = spindle->get_data(SpindleData_AtSpeed)->state_programmed.at_speed;

    return vfd_state; // return previous state as we do not want to wait for the response
}

static spindle_data_t *spindleGetData (spindle_data_request_t request)
{
    return &spindle_data;
}

static void rx_packet (modbus_message_t *msg)
{
    if(!(msg->adu[0] & 0x80)) {

        switch((vfd_response_t)msg->context) {

            case VFD_GetRPM:
                break;

            default:
                break;
        }
    }
}

static void raise_alarm (void *data)
{
    system_raise_alarm(Alarm_Spindle);
}

static void rx_exception (uint8_t code, void *context)
{
    // Alarm needs to be raised directly to correctly handle an error during reset (the rt command queue is
    // emptied on a warm reset). Exception is during cold start, where alarms need to be queued.
    if(sys.cold_start)
        protocol_enqueue_foreground_task(raise_alarm, NULL);
    else
        system_raise_alarm(Alarm_Spindle);
}

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(!newopt)
        report_plugin("PicoHAL Modbus Spindle", "0.01");
}

static void onSpindleSelected (spindle_ptrs_t *spindle)
{
    if(spindle->id == spindle_id) {

        spindle_hal = spindle;
        spindle_data.rpm_programmed = -1.0f;

        modbus_set_silence(NULL);
        modbus_address = vfd_get_modbus_address(spindle_id);

        spindleGetRPMRange();

    } else
        spindle_hal = NULL;

    if(on_spindle_selected)
        on_spindle_selected(spindle);
}

static void settingsChanged (settings_t *settings, settings_changed_flags_t changed)
{
    settings_changed(settings, changed);

    if(changed.spindle) {

        spindle_ptrs_t *spindle = spindle_get_hal(spindle_id, SpindleHAL_Configured);

        spindle->at_speed_tolerance = settings->spindle.at_speed_tolerance;
        spindle_data.at_speed_enabled = settings->spindle.at_speed_tolerance >= 0.0f;
    }
}

void vfd_picohal_init (void)
{
    static const vfd_spindle_ptrs_t vfd = {
        .spindle = {
            .type = SpindleType_VFD,
            .ref_id = SPINDLE_PICOHAL_VFD,
            .cap = {
                .variable = On,
                .at_speed = On,
                .direction = On,
                .cmd_controlled = On,
                .laser = On
            },
            .config = spindleConfig,
            .set_state = spindleSetState,
            .get_state = spindleGetState,
            .update_rpm = spindleUpdateRPM,
            .get_data = spindleGetData
        }
    };

    if((spindle_id = vfd_register(&vfd, "PicoHAL")) != -1) {

        on_spindle_selected = grbl.on_spindle_selected;
        grbl.on_spindle_selected = onSpindleSelected;

        settings_changed = hal.settings_changed;
        hal.settings_changed = settingsChanged;

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = onReportOptions;
    }
}

#endif
