//
//  main.c
//  esp-homekit-le7-bulb//
//  Created by David B Brown on 16/06/2018.
//  Copyright © 2018 maccoylton. All rights reserved.
//
/*
 * This is an example of an RGBW led lightbulb
 *
 * Debugging printf statements and UART are disabled below because it interfere with mutipwm
 * you can uncomment them for debug purposes
 *
 *
 * Based on code Contributed April 2018 by https://github.com/PCSaito
 */

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_common.h>
#include <espressif/esp_system.h>
#include <esp/uart.h>
#include <etstimer.h>
#include <esplibs/libmain.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <math.h>
#include <sysparam.h>


#include <homekit/homekit.h>
#include <homekit/characteristics.h>

#include <multipwm/multipwm.h>

#include <custom_characteristics.h>
#include <udplogger.h>
#include <shared_functions.h>
#include <rgbw_lights.h>


#define LPF_SHIFT 4  // divide by 16
#define LPF_INTERVAL 10  // in milliseconds

#define WHITE_PWM_PIN 14
#define BLUE_PWM_PIN 12
#define RED_PWM_PIN 15
#define GREEN_PWM_PIN 5
#define PWM_SCALE 255
#define LED_RGB_SCALE 255       // this is the scaling factor used for color conversion
#define LED_STRIP_SET_DELAY 500


// add this section to make your device OTA capable
// create the extra characteristic &ota_trigger, at the end of the primary service (before the NULL)
// it can be used in Eve, which will show it, where Home does not
// and apply the four other parameters in the accessories_information section

#include "ota-api.h"
#define DEVICE_MANUFACTURER "David B Brown"
#define DEVICE_NAME "LINGANZH"
#define DEVICE_MODEL "LE7"
#define DEVICE_SERIAL "12345678"
#define FW_VERSION "1.1"

void on_update(homekit_characteristic_t *ch, homekit_value_t value, void *context);


homekit_characteristic_t wifi_reset   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_RESET, false, .setter=wifi_reset_set);
homekit_characteristic_t ota_beta     = HOMEKIT_CHARACTERISTIC_(CUSTOM_OTA_BETA, false, .setter=ota_beta_set);
homekit_characteristic_t lcm_beta    = HOMEKIT_CHARACTERISTIC_(CUSTOM_LCM_BETA, false, .setter=lcm_beta_set);

homekit_characteristic_t wifi_check_interval   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_CHECK_INTERVAL, 10, .setter=wifi_check_interval_set);
/* checks the wifi is connected and flashes status led to indicated connected */
homekit_characteristic_t task_stats   = HOMEKIT_CHARACTERISTIC_(CUSTOM_TASK_STATS, false , .setter=task_stats_set);

homekit_characteristic_t ota_trigger  = API_OTA_TRIGGER;
homekit_characteristic_t name         = HOMEKIT_CHARACTERISTIC_(NAME, DEVICE_NAME);
homekit_characteristic_t manufacturer = HOMEKIT_CHARACTERISTIC_(MANUFACTURER,  DEVICE_MANUFACTURER);
homekit_characteristic_t serial       = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, DEVICE_SERIAL);
homekit_characteristic_t model        = HOMEKIT_CHARACTERISTIC_(MODEL,         DEVICE_MODEL);
homekit_characteristic_t revision     = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION,  FW_VERSION);

homekit_characteristic_t red_gpio     = HOMEKIT_CHARACTERISTIC_( CUSTOM_RED_GPIO, RED_PWM_PIN, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t green_gpio   = HOMEKIT_CHARACTERISTIC_( CUSTOM_GREEN_GPIO, GREEN_PWM_PIN, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t blue_gpio    = HOMEKIT_CHARACTERISTIC_( CUSTOM_BLUE_GPIO, BLUE_PWM_PIN, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t white_gpio   = HOMEKIT_CHARACTERISTIC_( CUSTOM_WHITE_GPIO, WHITE_PWM_PIN, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t colours_gpio_test   = HOMEKIT_CHARACTERISTIC_(CUSTOM_COLOURS_GPIO_TEST, false , .setter=colours_gpio_test_set, .getter=colours_gpio_test_get);
homekit_characteristic_t pure_white   = HOMEKIT_CHARACTERISTIC_(CUSTOM_COLOURS_PURE_WHITE, false , .setter=colours_pure_white_set);


homekit_characteristic_t on = HOMEKIT_CHARACTERISTIC_(ON, true,
                                                      .getter = led_on_get,
                                                      .setter = led_on_set);

homekit_characteristic_t brightness = HOMEKIT_CHARACTERISTIC_(BRIGHTNESS, 100,
                                                              .getter = led_brightness_get,
                                                              .setter = led_brightness_set);

homekit_characteristic_t hue = HOMEKIT_CHARACTERISTIC_(HUE, 0,
                                                       .getter = led_hue_get,
                                                       .setter = led_hue_set);

homekit_characteristic_t saturation = HOMEKIT_CHARACTERISTIC_(SATURATION, 0,
                                                              .getter = led_saturation_get,
                                                              .setter = led_saturation_set);


homekit_characteristic_t colours_strobe = HOMEKIT_CHARACTERISTIC_(CUSTOM_COLOURS_STROBE, false , .setter=colours_strobe_set, .getter=colours_strobe_get);
homekit_characteristic_t colours_flash = HOMEKIT_CHARACTERISTIC_(CUSTOM_COLOURS_FLASH, false , .setter=colours_flash_set, .getter=colours_flash_get);
homekit_characteristic_t colours_fade = HOMEKIT_CHARACTERISTIC_(CUSTOM_COLOURS_FADE, false , .setter=colours_fade_set, .getter=colours_smooth_get);
homekit_characteristic_t colours_smooth = HOMEKIT_CHARACTERISTIC_(CUSTOM_COLOURS_SMOOTH, false ,.setter=colours_smooth_set, .getter=colours_smooth_get);


const int status_led_gpio = 2; /*set the gloabl variable for the led to be sued for showing status */
int led_off_value=1; /* global varibale to support LEDs set to 0 where the LED is connected to GND, 1 where +3.3v */

int white_default_gpio = WHITE_PWM_PIN;
int red_default_gpio = RED_PWM_PIN;
int green_default_gpio = GREEN_PWM_PIN;
int blue_default_gpio = BLUE_PWM_PIN;

// Color smoothing variables
rgb_color_t current_color = { { 0, 0, 0, 0 } };
rgb_color_t target_color = { { 0, 0, 0, 0 } };

// Global variables
float led_hue = 0;              // hue is scaled 0 to 360
float led_saturation = 59;      // saturation is scaled 0 to 100
float led_brightness = 100;     // brightness is scaled 0 to 100
bool led_on = false;            // on is boolean on or off



void le7_buld_init (){
    
    pwm_info.channels = 4;
    rgbw_lights_init();
    
}



homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id = 1, .category = homekit_accessory_category_lightbulb, .services = (homekit_service_t*[]) {
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics = (homekit_characteristic_t*[]) {
            &name,
            &manufacturer,
            &serial,
            &model,
            &revision,
            HOMEKIT_CHARACTERISTIC(IDENTIFY, identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary = true, .characteristics = (homekit_characteristic_t*[]) {
            HOMEKIT_CHARACTERISTIC(NAME, "LED Strip"),
            &on,
            &brightness,
            &hue,
            &saturation,
            &red_gpio,
            &green_gpio,
            &blue_gpio,
            &white_gpio,
            &ota_trigger,
            &wifi_reset,
            &ota_beta,
            &lcm_beta,
            &wifi_check_interval,
            &task_stats,
            &colours_gpio_test,
            &colours_strobe,
            &colours_flash,
            &colours_fade,
            &colours_smooth,
            &pure_white,
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111" ,
    .setupId = "1234",
    .on_event = on_homekit_event
};


void recover_from_reset (int reason){
    /* called if we restarted abnormally */
    printf ("%s: reason %d\n", __func__, reason);
    load_characteristic_from_flash(&on);
}

void accessory_init_not_paired (void) {
    /* initalise anything you don't want started until wifi and homekit imitialisation is confirmed, but not paired */
    printf ("%s:\n", __func__);
}

void accessory_init (void ){
    /* initalise anything you don't want started until wifi and pairing is confirmed */
    get_sysparam_info();

    printf ("%s: GPIOS are set as follows : W=%d, R=%d, G=%d, B=%d\n",__func__, white_gpio.value.int_value,red_gpio.value.int_value, green_gpio.value.int_value, blue_gpio.value.int_value );

    le7_buld_init ();

    /* sent out values loded from flash, if nothing was loaded from flash then this will be default values */
    homekit_characteristic_notify(&hue,hue.value);
    homekit_characteristic_notify(&saturation,saturation.value );
    homekit_characteristic_notify(&brightness,brightness.value );
    homekit_characteristic_notify(&pure_white,pure_white.value );

}


void user_init(void) {
    
    standard_init (&name, &manufacturer, &model, &serial, &revision);
    
    /*    xTaskCreate(led_strip_send_task, "led_strip_send_task", 256, NULL, 2, NULL);*/
    
    
    wifi_config_init(DEVICE_NAME, NULL, on_wifi_ready);
}
