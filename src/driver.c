#include "driver.h"

static unsigned char* image_buffer;
static SemaphoreHandle_t buffer_mutex;
static TaskHandle_t task_handle;

static uint16_t *bitplane[2][BITPLANE_CNT];
    
uint32_t getpixel(unsigned char *pix, int x, int y) {
    unsigned char *p=pix+((x+y*64)*3);
    return (p[0]<<16)|(p[1]<<8)|(p[2]);
}

void driver_set_buffer(unsigned char* buffer) {
    BaseType_t err = xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    assert(err == pdPASS && "xSemaphoreTake() failed");
    image_buffer = buffer;
    xSemaphoreGive(buffer_mutex);
}

void driver_init() {
    i2s_parallel_buffer_desc_t bufdesc[2][1<<BITPLANE_CNT];
    i2s_parallel_config_t cfg={
        .gpio_bus={2, 15, 4, 16, 27, 17, -1, -1, 5, 18, 19, 21, 26, 25, -1, -1},
        .gpio_clk=22,
        .bits=I2S_PARALLEL_BITS_16,
        .clkspeed_hz=20*1000*1000,
        .bufa=bufdesc[0],
        .bufb=bufdesc[1],
    };

    for (int i=0; i<BITPLANE_CNT; i++) {
        for (int j=0; j<2; j++) {
            //http://esp-idf.readthedocs.io/en/latest/api-reference/system/mem_alloc.html
            bitplane[j][i]=heap_caps_malloc(BITPLANE_SZ*2, MALLOC_CAP_DMA);
            assert(bitplane[j][i] && "Can't allocate bitplane memory");
        }
    }

    //Do binary time division setup. Essentially, we need n of plane 0, 2n of plane 1, 4n of plane 2 etc, but that
    //needs to be divided evenly over time to stop flicker from happening. This little bit of code tries to do that
    //more-or-less elegantly.
    int times[BITPLANE_CNT]={0};
    printf("Bitplane order: ");
    for (int i=0; i<((1<<BITPLANE_CNT)-1); i++) {
        int ch=0;
        //Find plane that needs insertion the most
        for (int j=0; j<BITPLANE_CNT; j++) {
            if (times[j]<=times[ch]) ch=j;
        }
        printf("%d ", ch);
        //Insert the plane
        for (int j=0; j<2; j++) {
            bufdesc[j][i].memory=bitplane[j][ch];
            bufdesc[j][i].size=BITPLANE_SZ*2;
        }
        //Magic to make sure we choose this bitplane an appropriate time later next time
        times[ch]+=(1<<(BITPLANE_CNT-ch));
    }
    printf("\n");

    //End markers
    bufdesc[0][((1<<BITPLANE_CNT)-1)].memory=NULL;
    bufdesc[1][((1<<BITPLANE_CNT)-1)].memory=NULL;

    //Setup I2S
    i2s_parallel_setup(&I2S1, &cfg);
    printf("I2S setup done.\n");

    buffer_mutex = xSemaphoreCreateMutex();
}

void driver_cleanup() {
    vTaskDelete(task_handle);
    vSemaphoreDelete(buffer_mutex);
}

void driver_update() {

    int backbuf_id=0; //which buffer is the backbuffer, as in, which one is not active so we can write to it
    for(;;) {
        for (int pl=0; pl<BITPLANE_CNT; pl++) {
            int mask=(1<<(8-BITPLANE_CNT+pl)); //bitmask for pixel data in input for this bitplane
            uint16_t *p=bitplane[backbuf_id][pl]; //bitplane location to write to
            for (unsigned int y=0; y<16; y++) {
                int lbits=0;                //Precalculate line bits of the *previous* line, which is the one we're displaying now
                if ((y-1)&1) lbits|=BIT_A;
                if ((y-1)&2) lbits|=BIT_B;
                if ((y-1)&4) lbits|=BIT_C;
                if ((y-1)&8) lbits|=BIT_D;
                for (int fx=0; fx<64; fx++) {
#if DISPLAY_ROWS_SWAPPED
                    int x=fx^1; //to correct for the fact that the stupid LED screen I have has each row swapped...
#else
                    int x=fx;
#endif

                    int v=lbits;
                    //Do not show image while the line bits are changing
                    if (fx<1 || fx>=BRIGHTNESS) v|=BIT_OE;
                    if (fx==62) v|=BIT_LAT; //latch on second-to-last bit... why not last bit? Dunno, probably a timing thing.

                    BaseType_t err = xSemaphoreTake(buffer_mutex, portMAX_DELAY);
                    assert(err == pdPASS && "xSemaphoreTake() failed.");

                    assert(image_buffer && "image buffer is null!");
                    int c1=getpixel(image_buffer, x, y);
                    int c2=getpixel(image_buffer, x, y+16);
                    xSemaphoreGive(buffer_mutex);

                    if (c1 & (mask<<16)) v|=BIT_R1;
                    if (c1 & (mask<<8)) v|=BIT_G1;
                    if (c1 & (mask<<0)) v|=BIT_B1;
                    if (c2 & (mask<<16)) v|=BIT_R2;
                    if (c2 & (mask<<8)) v|=BIT_G2;
                    if (c2 & (mask<<0)) v|=BIT_B2;

                    //Save the calculated value to the bitplane memory
                    *p++=v;

                    //debug
                    printf("%d\n", lbits);
                }
            }
        }

        //Present image and swap buffers
        i2s_parallel_flip_to_buffer(&I2S1, backbuf_id);
        backbuf_id^=1;

        vTaskDelay(REFRESH_RATE / portTICK_PERIOD_MS);
    }
}

void driver_run() {
    //BaseType_t err = xTaskCreate(driver_update, "driver_update", 300, NULL, 4, &task_handle);
    //assert(err == pdPASS && "xTaskCreate() failed.");
    driver_update();
}