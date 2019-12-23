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
#include <colour_conversion.h>


#define LPF_SHIFT 4  // divide by 16
#define LPF_INTERVAL 10  // in milliseconds

#define WHITE_PWM_PIN 15
#define BLUE_PWM_PIN 15
#define RED_PWM_PIN 12
#define GREEN_PWM_PIN 5
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
#define FW_VERSION "1.0"

void on_update(homekit_characteristic_t *ch, homekit_value_t value, void *context);
static pwm_info_t pwm_info;
ETSTimer led_strip_timer;


homekit_characteristic_t wifi_reset   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_RESET, false, .setter=wifi_reset_set);
homekit_characteristic_t wifi_check_interval   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_CHECK_INTERVAL, 10, .setter=wifi_check_interval_set);
/* checks the wifi is connected and flashes status led to indicated connected */
homekit_characteristic_t ota_trigger  = API_OTA_TRIGGER;
homekit_characteristic_t name         = HOMEKIT_CHARACTERISTIC_(NAME, DEVICE_NAME);
homekit_characteristic_t manufacturer = HOMEKIT_CHARACTERISTIC_(MANUFACTURER,  DEVICE_MANUFACTURER);
homekit_characteristic_t serial       = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, DEVICE_SERIAL);
homekit_characteristic_t model        = HOMEKIT_CHARACTERISTIC_(MODEL,         DEVICE_MODEL);
homekit_characteristic_t revision     = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION,  FW_VERSION);
homekit_characteristic_t red_gpio     = HOMEKIT_CHARACTERISTIC_( CUSTOM_RED_GPIO, 12, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t green_gpio   = HOMEKIT_CHARACTERISTIC_( CUSTOM_GREEN_GPIO, 5, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t blue_gpio    = HOMEKIT_CHARACTERISTIC_( CUSTOM_BLUE_GPIO, 13, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t white_gpio   = HOMEKIT_CHARACTERISTIC_( CUSTOM_WHITE_GPIO, 15, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );
homekit_characteristic_t led_boost    = HOMEKIT_CHARACTERISTIC_( CUSTOM_LED_BOOST, 0, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update) );

const int status_led_gpio = 13; /*set the gloabl variable for the led to be sued for showing status */
int led_off_value=1; /* global varibale to support LEDs set to 0 where the LED is connected to GND, 1 where +3.3v */


typedef union {
    struct {
        uint16_t white;
        uint16_t blue;
        uint16_t green;
        uint16_t red;
    };
    uint64_t color;
} rgb_color_t;

// Color smoothing variables
rgb_color_t current_color = { { 0, 0, 0, 0 } };
rgb_color_t target_color = { { 0, 0, 0, 0 } };

// Global variables
float led_hue = 0;              // hue is scaled 0 to 360
float led_saturation = 59;      // saturation is scaled 0 to 100
float led_brightness = 100;     // brightness is scaled 0 to 100
bool led_on = false;            // on is boolean on or off



void default_rgbw_pins() {
    red_gpio.value.int_value = RED_PWM_PIN;
    green_gpio.value.int_value = GREEN_PWM_PIN;
    blue_gpio.value.int_value = BLUE_PWM_PIN;
    white_gpio.value.int_value = WHITE_PWM_PIN;
    
}


void on_update (homekit_characteristic_t *ch, homekit_value_t value, void *context	){
    if (red_gpio.value.int_value == green_gpio.value.int_value ||
        red_gpio.value.int_value == blue_gpio.value.int_value ||
        red_gpio.value.int_value == white_gpio.value.int_value ||
        green_gpio.value.int_value == blue_gpio.value.int_value ||
        green_gpio.value.int_value == white_gpio.value.int_value ||
        blue_gpio.value.int_value == white_gpio.value.int_value)
    {
        default_rgbw_pins();
    }
}

double __ieee754_remainder(double x, double y) {
    return x - y * floor(x/y);
}


void led_strip_set (){
    
    printf("%s: Current colour before set r=%d,g=%d, b=%d, w=%d\n",__func__, current_color.red,current_color.green, current_color.blue, current_color.white );
    
    if (led_on) {
        // convert HSI to RGBW
        HSVtoRGB(led_hue, led_saturation, led_brightness, &target_color);
        printf("%s: h=%d,s=%d,b=%d => r=%d,g=%d, b=%d\n",__func__, (int)led_hue,(int)led_saturation,(int)led_brightness, target_color.red,target_color.green, target_color.blue );
        RGBtoRGBW(&target_color);
        printf("%s: h=%d,s=%d,b=%d => r=%d,g=%d, b=%d, w=%d\n",__func__, (int)led_hue,(int)led_saturation,(int)led_brightness, target_color.red,target_color.green, target_color.blue, target_color.white);
    } else {
        // printf("led srtip off\n");
        target_color.red = 0;
        target_color.green = 0;
        target_color.blue = 0;
        target_color.white = 0;
    }
    
    
    current_color.red = target_color.red * PWM_SCALE;
    current_color.green target_color.green * PWM_SCALE;
    current_color.blue = target_color.blue * PWM_SCALE;
    current_color.white = target_color.white * PWM_SCALE;
    
    printf("%s: Current colour r=%d,g=%d, b=%d, w=%d\n",__func__, current_color.red,current_color.green, current_color.blue, current_color.white );
    
    printf ("Stopping multipwm \n");
    multipwm_stop(&pwm_info);
    multipwm_set_duty(&pwm_info, 0, current_color.white);
    multipwm_set_duty(&pwm_info, 1, current_color.blue);
    multipwm_set_duty(&pwm_info, 2, current_color.green);
    multipwm_set_duty(&pwm_info, 3, current_color.red);
    multipwm_start(&pwm_info);
    printf ("Starting multipwm \n");
    
}

void led_strip_init (){
    
    uint8_t pins[] = {WHITE_PWM_PIN, BLUE_PWM_PIN,GREEN_PWM_PIN, RED_PWM_PIN};
    pwm_info.channels = 4;
    
    multipwm_init(&pwm_info);
    multipwm_set_freq(&pwm_info, 65280);
    for (uint8_t i=0; i<pwm_info.channels; i++) {
        multipwm_set_pin(&pwm_info, i, pins[i]);
        printf ("Set pin %d to %d\n",i, pins[i]);
    }
    sdk_os_timer_setfn(&led_strip_timer, led_strip_set, NULL);
    printf ("Sdk_os_timer_Setfn called\n");
    
}


homekit_value_t led_on_get() {
    return HOMEKIT_BOOL(led_on);
}

void led_on_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        // printf("Invalid on-value format: %d\n", value.format);
        return;
    }
    
    led_on = value.bool_value;
}

homekit_value_t led_brightness_get() {
    return HOMEKIT_INT(led_brightness);
}

void led_brightness_set(homekit_value_t value) {
    if (value.format != homekit_format_int) {
        // printf("Invalid brightness-value format: %d\n", value.format);
        return;
    }
    led_brightness = value.int_value;
    sdk_os_timer_arm (&led_strip_timer, LED_STRIP_SET_DELAY, 0 );
    printf ("led_brigthness_set timer armed\n");
}

homekit_value_t led_hue_get() {
    return HOMEKIT_FLOAT(led_hue);
}

void led_hue_set(homekit_value_t value) {
    if (value.format != homekit_format_float) {
        // printf("Invalid hue-value format: %d\n", value.format);
        return;
    }
    led_hue = value.float_value;
    sdk_os_timer_arm (&led_strip_timer, LED_STRIP_SET_DELAY, 0 );
    printf ("led_hugh_set timer armed\n");
    
}

homekit_value_t led_saturation_get() {
    return HOMEKIT_FLOAT(led_saturation);
}

void led_saturation_set(homekit_value_t value) {
    if (value.format != homekit_format_float) {
        // printf("Invalid sat-value format: %d\n", value.format);
        return;
    }
    led_saturation = value.float_value;
    sdk_os_timer_arm (&led_strip_timer, LED_STRIP_SET_DELAY, 0 );
    printf ("led_saturation_set timer armed\n");
    
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
            HOMEKIT_CHARACTERISTIC(
                                   ON, true,
                                   .getter = led_on_get,
                                   .setter = led_on_set
                                   ),
            HOMEKIT_CHARACTERISTIC(
                                   BRIGHTNESS, 100,
                                   .getter = led_brightness_get,
                                   .setter = led_brightness_set
                                   ),
            HOMEKIT_CHARACTERISTIC(
                                   HUE, 0,
                                   .getter = led_hue_get,
                                   .setter = led_hue_set
                                   ),
            HOMEKIT_CHARACTERISTIC(
                                   SATURATION, 0,
                                   .getter = led_saturation_get,
                                   .setter = led_saturation_set
                                   ),
            &red_gpio,
            &green_gpio,
            &blue_gpio,
            &white_gpio,
            &led_boost,
            &ota_trigger,
            &wifi_reset,
            &wifi_check_interval,
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


void accessory_init_not_paired (void) {
    /* initalise anything you don't want started until wifi and homekit imitialisation is confirmed, but not paired */
}

void accessory_init (void ){
    /* initalise anything you don't want started until wifi and pairing is confirmed */
    
}

void user_init(void) {
    
    standard_init (&name, &manufacturer, &model, &serial, &revision);
    
    led_strip_init ();
    
    /*    xTaskCreate(led_strip_send_task, "led_strip_send_task", 256, NULL, 2, NULL);*/
    
    
    wifi_config_init(DEVICE_NAME, NULL, on_wifi_ready);
    
    
}
