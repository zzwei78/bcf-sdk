#include <bc_module_infra_grid.h>


// Reference registers, commands
// https://na.industrial.panasonic.com/sites/default/pidsa/files/downloads/files/grid-eye-high-performance-specifications.pdf

// Adafruit Lib
// https://github.com/adafruit/Adafruit_AMG88xx/blob/master/Adafruit_AMG88xx.cpp

// interrupt flags
// https://github.com/adafruit/Adafruit_AMG88xx/blob/master/examples/amg88xx_interrupt/amg88xx_interrupt.ino

#define BC_AMG88xx_PCLT 0x00
#define BC_AMG88xx_RST 0x01
#define BC_AMG88xx_FPSC 0x02
#define BC_AMG88xx_INTC 0x03
#define BC_AMG88xx_STAT 0x04
#define BC_AMG88xx_SCLR 0x05
#define BC_AMG88xx_AVE 0x07
#define BC_AMG88xx_INTHL 0x08
#define BC_AMG88xx_TTHL 0x0E
#define BC_AMG88xx_TTHH 0x0F
#define BC_AMG88xx_INT0 0x10
#define BC_AMG88xx_AVG 0x1F
#define BC_AMG88xx_T01L 0x80

#define BC_AMG88xx_ADDR 0x68 // in 7bit

#define _BC_MODULE_INFRA_GRID_DELAY_RUN 50
#define _BC_MODULE_INFRA_GRID_DELAY_INITIALIZATION 50
#define _BC_MODULE_INFRA_GRID_DELAY_MEASUREMENT 5

#define _BC_MODULE_INFRA_GRID_DELAY_MODE_CHANGE 50
#define _BC_MODULE_INFRA_GRID_DELAY_INITIAL_RESET 10
#define _BC_MODULE_INFRA_GRID_DELAY_FLAG_RESET 10

static void _bc_module_infra_grid_task_interval(void *param);
static void _bc_module_infra_grid_task_measure(void *param);

void bc_module_infra_grid_init(bc_module_infra_grid_t *self)
{
    memset(self, 0, sizeof(*self));

    self->_i2c_channel = BC_I2C_I2C0;
    self->_i2c_address = BC_AMG88xx_ADDR;

    self->_task_id_interval = bc_scheduler_register(_bc_module_infra_grid_task_interval, self, BC_TICK_INFINITY);
    self->_task_id_measure = bc_scheduler_register(_bc_module_infra_grid_task_measure, self, _BC_MODULE_INFRA_GRID_DELAY_RUN);

    self->_tick_ready = _BC_MODULE_INFRA_GRID_DELAY_RUN;

    bc_i2c_init(self->_i2c_channel, BC_I2C_SPEED_100_KHZ);

}

void bc_module_infra_grid_set_event_handler(bc_module_infra_grid_t *self, void (*event_handler)(bc_module_infra_grid_t *, bc_module_infra_grid_event_t, void *), void *event_param)
{
    self->_event_handler = event_handler;
    self->_event_param = event_param;
}

void bc_module_infra_grid_set_update_interval(bc_module_infra_grid_t *self, bc_tick_t interval)
{
    self->_update_interval = interval;

    if (self->_update_interval > 1000)
    {
        self->_cmd_sleep = true;
    }

    if (self->_update_interval == BC_TICK_INFINITY)
    {
        bc_scheduler_plan_absolute(self->_task_id_interval, BC_TICK_INFINITY);
    }
    else
    {
        bc_scheduler_plan_relative(self->_task_id_interval, self->_update_interval);

        bc_module_infra_grid_measure(self);
    }
}

bool bc_module_infra_grid_measure(bc_module_infra_grid_t *self)
{
    if (self->_measurement_active)
    {
        return false;
    }

    self->_measurement_active = true;

    bc_scheduler_plan_absolute(self->_task_id_measure, self->_tick_ready);

    return true;
}

float bc_module_ifra_grid_read_thermistor(bc_module_infra_grid_t *self)
{
    (void) self;
    int8_t temperature[2];

    bc_i2c_memory_read_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_TTHL, (uint8_t*)&temperature[0]);
    bc_i2c_memory_read_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_TTHH, (uint8_t*)&temperature[1]);

    return (temperature[1]*256 + temperature[0]) * 0.0625;
}


bool bc_module_ifra_grid_read_values(bc_module_infra_grid_t *self)
{
    bc_i2c_memory_transfer_t transfer;

    transfer.device_address = self->_i2c_address;
    transfer.memory_address = BC_AMG88xx_T01L;
    transfer.buffer = self->_sensor_data;
    transfer.length = 64 * 2;

    return bc_i2c_memory_read(self->_i2c_channel, &transfer);
}

void bc_module_ifra_grid_get_temperatures_celsius(bc_module_infra_grid_t *self, float *values)
{
    if (!self->_temperature_valid)
    {
        return false;
    }

    for (int i = 0; i < 64 ;i++)
    {
        float temperature;
        int16_t temporary_data = self->_sensor_data[i];

        if (temporary_data > 0x200)
        {
            temperature = (-temporary_data +  0xfff) * -0.25;
        }
        else
        {
            temperature = temporary_data * 0.25;
        }

        values[i] = temperature;
    }

    return true;
}


static void _bc_module_infra_grid_task_interval(void *param)
{
    bc_module_infra_grid_t *self = param;

    bc_module_infra_grid_measure(self);

    bc_scheduler_plan_current_relative(self->_update_interval);
}

static void _bc_module_infra_grid_task_measure(void *param)
{
    bc_module_infra_grid_t *self = param;

start:

    switch (self->_state)
    {
        case BC_MODULE_INFRA_GRID_STATE_ERROR:
        {
            self->_temperature_valid = false;

            self->_measurement_active = false;

            if (self->_event_handler != NULL)
            {
                self->_event_handler(self, BC_MODULE_INFRA_GRID_EVENT_ERROR, self->_event_param);
            }

            self->_state = BC_MODULE_INFRA_GRID_STATE_INITIALIZE;

            return;
        }
        case BC_MODULE_INFRA_GRID_STATE_INITIALIZE:
        {
            self->_state = BC_MODULE_INFRA_GRID_STATE_ERROR;

            if (self->_enable_sleep)
            {
                // Sleep mode
                if (!bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_PCLT, 0x10))
                {
                    goto start;
                }
            }
            else
            {
                // Set 10 FPS
                if (!bc_i2c_memory_write_8b (self->_i2c_channel, self->_i2c_address, BC_AMG88xx_FPSC, 0x00))
                {
                    goto start;
                }

                // Diff interrpt mode, INT output reactive
                bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_INTC, 0x00);
                // Moving average output mode active
                bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x50);
                bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x45);
                bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x57);
                bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVE, 0x20);
                bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x00);
            }

            self->_state = BC_MODULE_INFRA_GRID_STATE_MODE_CHANGE; //BC_MODULE_INFRA_GRID_STATE_MEASURE;

            self->_tick_ready = bc_tick_get() + _BC_MODULE_INFRA_GRID_DELAY_INITIALIZATION;

            if (self->_measurement_active)
            {
                bc_scheduler_plan_current_absolute(self->_tick_ready);
            }

            return;
        }
        case BC_MODULE_INFRA_GRID_STATE_MODE_CHANGE:
        {
            // Skip wakeup commands in case of fast reading
            if (!self->_enable_sleep)
            {
                self->_state = BC_MODULE_INFRA_GRID_STATE_READ;
                goto start;
            }

            // Normal Mode
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_PCLT, 0x00);
            self->_state = BC_MODULE_INFRA_GRID_STATE_INITIAL_RESET;
            bc_scheduler_plan_current_from_now(_BC_MODULE_INFRA_GRID_DELAY_MODE_CHANGE);

            return;
        }
        case BC_MODULE_INFRA_GRID_STATE_INITIAL_RESET:
        {
            // Write initial reset
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_RST, 0x3F);

            self->_state = BC_MODULE_INFRA_GRID_STATE_FLAG_RESET;

            bc_scheduler_plan_current_from_now(_BC_MODULE_INFRA_GRID_DELAY_INITIAL_RESET);

            return;
        }
        case BC_MODULE_INFRA_GRID_STATE_FLAG_RESET:
        {
            // Write initial reset
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_RST, 0x30);


            self->_state = BC_MODULE_INFRA_GRID_STATE_MEASURE;

            bc_scheduler_plan_current_from_now(20);

            return;

        }
        case BC_MODULE_INFRA_GRID_STATE_MEASURE:
        {
            self->_state = BC_MODULE_INFRA_GRID_STATE_ERROR;

            if (!bc_i2c_memory_write_8b (self->_i2c_channel, self->_i2c_address, BC_AMG88xx_FPSC, 0x01))
            {
                goto start;
            }

            // Diff interrpt mode, INT output reactive
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_INTC, 0x00);
            // Moving average output mode active
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x50);
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x45);
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x57);
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVE, 0x20);
            bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_AVG, 0x00);

            self->_state = BC_MODULE_INFRA_GRID_STATE_READ;
            //goto start;

            bc_scheduler_plan_current_from_now(_BC_MODULE_INFRA_GRID_DELAY_MEASUREMENT);

            return;
        }
        case BC_MODULE_INFRA_GRID_STATE_READ:
        {
            self->_state = BC_MODULE_INFRA_GRID_STATE_ERROR;

            bc_i2c_memory_transfer_t transfer;

            transfer.device_address = self->_i2c_address;
            transfer.memory_address = BC_AMG88xx_T01L;
            transfer.buffer = self->_sensor_data;
            transfer.length = 64 * 2;

            if (!bc_i2c_memory_read(self->_i2c_channel, &transfer))
            {
                goto start;
            }

            // Enable sleep mode
            if (self->_enable_sleep)
            {
                // Sleep Mode
                bc_i2c_memory_write_8b(self->_i2c_channel, self->_i2c_address, BC_AMG88xx_PCLT, 0x10);
            }

            self->_temperature_valid = true;

            self->_state = BC_MODULE_INFRA_GRID_STATE_UPDATE;

            goto start;
        }
        case BC_MODULE_INFRA_GRID_STATE_UPDATE:
        {
            self->_measurement_active = false;

            if (self->_event_handler != NULL)
            {
                self->_event_handler(self, BC_MODULE_INFRA_GRID_EVENT_UPDATE, self->_event_param);
            }

            // Update sleep flag
            if (self->_enable_sleep != self->_cmd_sleep)
            {
                self->_enable_sleep = self->_cmd_sleep;
                self->_state = BC_MODULE_INFRA_GRID_STATE_INITIALIZE;
            }

            self->_state = BC_MODULE_INFRA_GRID_STATE_MODE_CHANGE;

            return;
        }
        default:
        {
            self->_state = BC_MODULE_INFRA_GRID_STATE_ERROR;

            goto start;
        }
    }
}
