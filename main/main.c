#include "ds18b20.h"
#include "led_panel.h"
#include "ds3231.h"
#include "logo.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#define MENU_TIMEOUT_US   (10 * 1000000)  // 10 seconds

static int menu_active = 0;
static int64_t last_button_time = 0;


bool stop_flag = false;




// Return 1=Sunday ... 7=Saturday to match DS3231
static int calculate_weekday(int day, int month, int year)
{
    if (month < 3) {
        month += 12;
        year--;
    }
    int K = year % 100;
    int J = year / 100;
    int h = (day + 13*(month + 1)/5 + K + K/4 + J/4 + 5*J) % 7;
    // Zeller's: 0=Saturday, 1=Sunday, ..., 6=Friday
    int d = ((h + 6) % 7) + 1; // 1=Sunday ... 7=Saturday
    return d;
}





typedef enum {
    BTN_MENU,
    BTN_UP,
    BTN_DOWN
} button_t;

static QueueHandle_t button_queue;

static void IRAM_ATTR button_isr_handler(void* arg)
{
    last_button_time = esp_timer_get_time();  // refresh timeout
	button_t btn = (button_t)(uint32_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(button_queue, &btn, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}


static void init_buttons(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << PIN_MENU) | (1ULL << PIN_UP) | (1ULL << PIN_DOWN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // enable internal pull-up
    io_conf.intr_type = GPIO_INTR_NEGEDGE;    // trigger on press (falling edge)
    gpio_config(&io_conf);

    // Create queue
    button_queue = xQueueCreate(10, sizeof(button_t));

    // Install ISR service
    gpio_install_isr_service(0);

    // Add ISR handlers
    gpio_isr_handler_add(PIN_MENU, button_isr_handler, (void*)(uint32_t)BTN_MENU);
    gpio_isr_handler_add(PIN_UP,   button_isr_handler, (void*)(uint32_t)BTN_UP);
    gpio_isr_handler_add(PIN_DOWN, button_isr_handler, (void*)(uint32_t)BTN_DOWN);
}




typedef enum {
    MENU_IDLE,
    MENU_BRIGHTNESS,
    MENU_HOUR,
    MENU_MINUTE,
    MENU_DAY,
    MENU_MONTH,
    MENU_YEAR
} menu_state_t;

static menu_state_t menu_state = MENU_IDLE;

static int brightness_level = 5; // 1–10
static ds3231_time_t tmp_time;   // temporary time editing

static void handle_menu_button(button_t btn, ds3231_dev_t *rtc)
{
    switch (menu_state)
    {
        case MENU_IDLE:
            if (btn == BTN_MENU) {
				menu_state = MENU_BRIGHTNESS;
				stop_flag = true;
			    menu_active = 1;
			    last_button_time = esp_timer_get_time();  // start inactivity timer
			    printf("Menu entered\n");
			}
            break;

        case MENU_BRIGHTNESS:
            if (btn == BTN_UP && brightness_level < 10) brightness_level++;
            if (btn == BTN_DOWN && brightness_level > 1) brightness_level--;
            set_global_brightness(brightness_level * 10);
            if (btn == BTN_MENU) menu_state = MENU_HOUR;
            break;

        case MENU_HOUR:
            if (btn == BTN_UP) tmp_time.hour = (tmp_time.hour + 1) % 24;
            if (btn == BTN_DOWN) tmp_time.hour = (tmp_time.hour + 23) % 24;
            if (btn == BTN_MENU) menu_state = MENU_MINUTE;
            break;

        case MENU_MINUTE:
            if (btn == BTN_UP) tmp_time.minute = (tmp_time.minute + 1) % 60;
            if (btn == BTN_DOWN) tmp_time.minute = (tmp_time.minute + 59) % 60;
            if (btn == BTN_MENU) menu_state = MENU_DAY;
            break;

		case MENU_DAY:
		    if (btn == BTN_UP) tmp_time.day = (tmp_time.day % 31) + 1;
		    if (btn == BTN_DOWN) tmp_time.day = ((tmp_time.day + 29) % 31) + 1;
		    tmp_time.day_of_week = calculate_weekday(tmp_time.day, tmp_time.month, tmp_time.year);
		    if (btn == BTN_MENU) menu_state = MENU_MONTH;
		    break;
		
		case MENU_MONTH:
		    if (btn == BTN_UP) tmp_time.month = (tmp_time.month % 12) + 1;
		    if (btn == BTN_DOWN) tmp_time.month = ((tmp_time.month + 10) % 12) + 1;
		    tmp_time.day_of_week = calculate_weekday(tmp_time.day, tmp_time.month, tmp_time.year);
		    if (btn == BTN_MENU) menu_state = MENU_YEAR;
		    break;
		
		case MENU_YEAR:
		    if (btn == BTN_UP) tmp_time.year++;
		    if (btn == BTN_DOWN && tmp_time.year > 2000) tmp_time.year--;
		    tmp_time.day_of_week = calculate_weekday(tmp_time.day, tmp_time.month, tmp_time.year);
		    if (btn == BTN_MENU) {
		        tmp_time.second = 0;
				ESP_ERROR_CHECK(ds3231_set_time(rtc, &tmp_time));
				save_brightness(brightness_level);  // <--- save here
		        menu_state = MENU_IDLE;
				stop_flag = false;
		    }
		    break;
    }
}






static void menu_task(void *arg)
{
    ds3231_dev_t *rtc = (ds3231_dev_t *)arg;
	ESP_ERROR_CHECK(ds3231_get_time(rtc, &tmp_time));

    static TickType_t last_press_time[3] = {0,0,0}; // BTN_MENU, BTN_UP, BTN_DOWN
    static button_t last_btn = -1;
    static TickType_t repeat_time = 0;

    while(1)
    {
        button_t btn;
        TickType_t now = xTaskGetTickCount();

        if (xQueueReceive(button_queue, &btn, pdMS_TO_TICKS(10))) 
        {
            // Debounce per button
            if ((now - last_press_time[btn]) < pdMS_TO_TICKS(DEBOUNCE_MS))
                continue;

            last_press_time[btn] = now;
            last_btn = btn;
            repeat_time = now + pdMS_TO_TICKS(REPEAT_DELAY);

            handle_menu_button(btn, rtc);
        }
        else
        {
            // Auto-repeat
            if (last_btn != -1 && (now - last_press_time[last_btn]) >= pdMS_TO_TICKS(REPEAT_DELAY))
            {
                if ((now - repeat_time) >= pdMS_TO_TICKS(REPEAT_RATE))
                {
                    handle_menu_button(last_btn, rtc);
                    repeat_time = now;
                }
            }
        }

        // Reset last_btn if all buttons released
        if (gpio_get_level(PIN_MENU) && gpio_get_level(PIN_UP) && gpio_get_level(PIN_DOWN))
            last_btn = -1;

        //vTaskDelay(pdMS_TO_TICKS(10));

        if (menu_active) {
            int64_t now = esp_timer_get_time();
            if (now - last_button_time > MENU_TIMEOUT_US) {
                menu_active = 0;  // auto-exit
                printf("Menu timeout -> exiting\n");
				menu_state = MENU_IDLE;
				stop_flag = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));  // check every 200 ms
    }
}








static ds18b20_t sensor;

static int16_t current_temp = 0;
static bool temp_valid = false;

void temp_task(void *arg)
{
    while (1) {
        int16_t t;
        if (ds18b20_read_temperature_int(&sensor, &t) == ESP_OK) {
            current_temp = t;
            temp_valid = true;
        } else {
            temp_valid = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // read every 5s
    }
}

typedef enum {
    DISPLAY_TIME = 0,
    DISPLAY_DATE,
	DISPLAY_TEMPERATURE,
	DISPLAY_LOGO,
	//DISPLAY_LOGO2,
	//DISPLAY_LOGO3,
    // Add more modes here later, e.g., DISPLAY_TEMPERATURE
} display_mode_t;

const char *dias_semana[] = {
    "DOMINGO", "LUNES", "MARTES", "MIERCOLES", "JUEVES", "VIERNES", "SABADO"
};

const char *meses[] = {
    "ENERO", "FEBRERO", "MARZO", "ABRIL", "MAYO", "JUNIO",
    "JULIO", "AGOSTO", "SEPTIEMBRE", "OCTUBRE", "NOVIEMBRE", "DICIEMBRE"
};

void draw_display(display_mode_t mode, ds3231_time_t *time)
{
    clear_back_buffer();

    switch (mode) {
        case DISPLAY_TIME: {
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			

            int weekday_index = (time->day_of_week - 1) % 7;
            char buf3[32];
            snprintf(buf3, sizeof(buf3), "%s",
                     dias_semana[weekday_index]);

            draw_text(1, 1, buf3, 0, 255, 0);
            
            
            
            
            
            char buf4[32];
            snprintf(buf4, sizeof(buf4), "%02d-%02d-%02d",
                     time->day,time->month,
                     time->year-2000);

            draw_text(4, 11, buf4, 0, 0, 255);//x=6            
            
            
            
            		
			
            int hour12 = time->hour % 12;
            if (hour12 == 0) hour12 = 12;

            // Colon visible only when seconds are even
            bool colon_on = (time->second % 2) == 0;

            char buf[16];
            if (hour12 < 10) {
                snprintf(buf, sizeof(buf), colon_on ? " %1d:%02d" : " %1d %02d", hour12, time->minute);
            } else {
                snprintf(buf, sizeof(buf), colon_on ? "%02d:%02d" : "%02d %02d", hour12, time->minute);
            }

            draw_text(0, 22, buf, 255, 255, 255); // green
                                                                              
            
               char buf2[32];
            if (temp_valid) {
                snprintf(buf2, sizeof(buf2), "%d*", current_temp);
            } else {
                snprintf(buf2, sizeof(buf2), "TEMP ERROR");
            }
            draw_text(40, 22, buf2, 255, 0, 0);         
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            break;
        }
        case DISPLAY_DATE: {
            int weekday_index = (time->day_of_week - 1) % 7;
            char buf[32];
            snprintf(buf, sizeof(buf), "%s %02d %s %04d",
                     dias_semana[weekday_index],
                     time->day,
                     meses[time->month - 1],
                     time->year);

            scroll_text(buf, 0, 255, 0, 255, 15);
            //draw_text(1, 1, "123456789", 0, 255, 0);
            break;
        }
        case DISPLAY_TEMPERATURE: {
            char buf[32];
            if (temp_valid) {
                snprintf(buf, sizeof(buf), "%d*C", current_temp);
            } else {
                snprintf(buf, sizeof(buf), "TEMP ERROR");
            }
            draw_text(0, 0, buf, 0, 255, 255);
            break;
        }
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
   case DISPLAY_LOGO: {
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			
			

            int weekday_index = (time->day_of_week - 1) % 7;
            char buf4[32];
            snprintf(buf4, sizeof(buf4), "%s %02d %s %04d",
                     dias_semana[weekday_index],
                     time->day,
                     meses[time->month - 1],
                     time->year);

            scroll_text_2(buf4, 1, 255, 0, 255, 15);
            //draw_text_2(1, 1, buf4, 0, 255, 0);           
            
            
            
            		
			
/*
            int weekday_index = (time->day_of_week - 1) % 7;
            char buf[32];
            snprintf(buf, sizeof(buf), "%s %02d %s %04d",
                     dias_semana[weekday_index],
                     time->day,
                     meses[time->month - 1],
                     time->year);

            //scroll_text(buf, 0, 255, 0, 255, 15);
            draw_text(1, 1, "123456789", 0, 255, 0);
*/
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            break;
        }
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
/*
        case DISPLAY_LOGO2:
            draw_bitmap_rgb(24,16,logo2_bitmap, LOGO_WIDTH, LOGO_HEIGHT);
            break;
        case DISPLAY_LOGO3:
            draw_bitmap_rgb(24,16,logo3_bitmap, LOGO_WIDTH, LOGO_HEIGHT);
            break;
*/
    }

    swap_buffers();
}






void drawing_task(void *arg)
{
    ds3231_dev_t *rtc = (ds3231_dev_t *)arg;
    display_mode_t mode = DISPLAY_TIME;
    const int mode_interval_s = 10;

    while (1) 
    {
        clear_back_buffer();

        if (menu_state != MENU_IDLE)
        {
            // Draw the menu
            char buf[32];
            switch (menu_state)
            {
                case MENU_BRIGHTNESS:
                    snprintf(buf, sizeof(buf), "BRILLO:%d", brightness_level);
                    draw_text(1, 8, buf, 255, 0, 0);
                    break;
                case MENU_HOUR:
                    snprintf(buf, sizeof(buf), " HORA:%02d", tmp_time.hour);
                    draw_text(0, 8, buf, 255, 0, 0);
                    break;
                case MENU_MINUTE:
                    snprintf(buf, sizeof(buf), "MINUTO:%02d", tmp_time.minute);
                    draw_text(1, 8, buf, 255, 0, 0);
                    break;
                case MENU_DAY:
                    snprintf(buf, sizeof(buf), " DIA:%02d", tmp_time.day);
                    draw_text(0, 8, buf, 255, 0, 0);
                    break;
                case MENU_MONTH:
                    snprintf(buf, sizeof(buf), " MES:%02d", tmp_time.month);
                    draw_text(0, 8, buf, 255, 0, 0);
                    break;
                case MENU_YEAR:
                    snprintf(buf, sizeof(buf), " A|O:%02d", tmp_time.year-2000);
                    draw_text(0, 8, buf, 255, 0, 0);
                    break;
                default: break;
            }

            swap_buffers();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue; // skip normal display
        }

        // Menu not active → normal display
        ds3231_time_t now;
        ESP_ERROR_CHECK(ds3231_get_time(rtc, &now));

        switch (DISPLAY_TIME) 
        {
			case DISPLAY_TIME: {
			    TickType_t start_tick = xTaskGetTickCount();
			    TickType_t duration_ticks = pdMS_TO_TICKS(mode_interval_s * 1000);
			    ds3231_time_t now;
			
			    while (xTaskGetTickCount() - start_tick < duration_ticks) {
			        ESP_ERROR_CHECK(ds3231_get_time(rtc, &now));
			        draw_display(DISPLAY_TIME, &now);  // called frequently for smooth blinking
			
			        // Small delay to avoid blocking CPU and allow colon toggle
			        TickType_t delay_ms = 50; // update every 50ms (20Hz)
			        TickType_t elapsed = 0;
			        while (elapsed < delay_ms) {
			            if (stop_flag) break;  // early exit condition
			            vTaskDelay(pdMS_TO_TICKS(10));
			            elapsed += 10;
			        }
			
			        if (stop_flag) break;
			    }
			    break;
			}
            case DISPLAY_DATE:
                draw_display(DISPLAY_DATE, &now);
                break;

            case DISPLAY_TEMPERATURE:
                draw_display(DISPLAY_TEMPERATURE, &now);
                //vTaskDelay(pdMS_TO_TICKS(mode_interval_s * 500));
                for (int i = 0; i < mode_interval_s; i++) 
                {
                    //vTaskDelay(pdMS_TO_TICKS(1000));

					TickType_t delay_ms = 400;
					TickType_t elapsed = 0;
					while (elapsed < delay_ms) {
					    if (stop_flag) break;   // condition to exit early
					    vTaskDelay(pdMS_TO_TICKS(10));
					    elapsed += 10;
					}
                }
                break;

            case DISPLAY_LOGO:
                draw_display(DISPLAY_LOGO, &now);
                //vTaskDelay(pdMS_TO_TICKS(mode_interval_s * 200));
                for (int i = 0; i < mode_interval_s; i++) 
                {
                    //vTaskDelay(pdMS_TO_TICKS(1000));

					TickType_t delay_ms = 200;
					TickType_t elapsed = 0;
					while (elapsed < delay_ms) {
					    if (stop_flag) break;   // condition to exit early
					    vTaskDelay(pdMS_TO_TICKS(10));
					    elapsed += 10;
					}
                }
                break;
/*

            case DISPLAY_LOGO2:
                draw_display(DISPLAY_LOGO2, &now);
                //vTaskDelay(pdMS_TO_TICKS(mode_interval_s * 200));
                for (int i = 0; i < mode_interval_s; i++) 
                {
                    //vTaskDelay(pdMS_TO_TICKS(1000));

					TickType_t delay_ms = 200;
					TickType_t elapsed = 0;
					while (elapsed < delay_ms) {
					    if (stop_flag) break;   // condition to exit early
					    vTaskDelay(pdMS_TO_TICKS(10));
					    elapsed += 10;
					}
                }
                break;

            case DISPLAY_LOGO3:
                draw_display(DISPLAY_LOGO3, &now);
                //vTaskDelay(pdMS_TO_TICKS(mode_interval_s * 300));
                for (int i = 0; i < mode_interval_s; i++) 
                {
                    //vTaskDelay(pdMS_TO_TICKS(1000));

					TickType_t delay_ms = 200;
					TickType_t elapsed = 0;
					while (elapsed < delay_ms) {
					    if (stop_flag) break;   // condition to exit early
					    vTaskDelay(pdMS_TO_TICKS(10));
					    elapsed += 10;
					}
                }
                break;
*/
        }

        mode++;
        if (mode > DISPLAY_LOGO) mode = DISPLAY_TIME;
    }
}


// ---------------- example usage in app_main ----------------
void app_main(void)
{
    init_pins();

	init_oe_pwm();           // initialize OE PWM
	//set_global_brightness(100);  // 50% brightness

	init_nvs_brightness();
	brightness_level = load_brightness();
	set_global_brightness(brightness_level * 10);


    ds3231_dev_t rtc;
    ESP_ERROR_CHECK(init_ds3231(&rtc));

    ds18b20_init(&sensor, GPIO_NUM_27); // Use GPIO4 with 4.7kΩ pull-up resistor

	ds3231_time_t now;
	ds3231_get_time(&rtc, &now);

	if(now.year < 2025)
	{
		ds3231_time_t set_time = {2025, 8, 18, 13, 52, 0, 2};
    	ESP_ERROR_CHECK(ds3231_set_time(&rtc, &set_time));
	}

	init_planes();

    // Clear both buffers first time
    memset((void*)fbA, 0, sizeof(fbA));
    memset((void*)fbB, 0, sizeof(fbB));

	init_buttons();

    // Start refresh task (pin-driving) on core 0
	xTaskCreatePinnedToCore(refresh_task, "refresh_task", 2048, NULL, 1, NULL, 0);

	xTaskCreatePinnedToCore(drawing_task, "DrawTime", 4096, &rtc, 1, NULL, 1);
	
	xTaskCreatePinnedToCore(temp_task,      "TempTask",      1024, NULL, 2, NULL, 1);

	xTaskCreatePinnedToCore(menu_task, "MenuTask", 4096, &rtc, 2, NULL, 1);


    while (true) 
	{
        vTaskDelay(pdMS_TO_TICKS(1));

    }
}

