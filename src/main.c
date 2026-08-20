/* ==========================================================================
 * Digital Signal Analyzer - TM4C123GH6PM (Tiva C LaunchPad)
 * ==========================================================================
 *
 * Measures frequency and peak voltage of an analog input signal.
 *
 * Peripherals:
 *   - ADC0 (Sequencer 3) : Samples signal on PE0 (AIN3)
 *   - Timer0A            : Triggers ADC at 100 kHz sample rate
 *   - I2C0 (PB2/PB3)     : Drives 16x2 LCD via PCF8574 backpack
 *   - GPIO Port F        : SW1 (PF4) trigger, LEDs (PF1 red, PF3 green)
 *
 * Operation:
 *   1. Press SW1 to start measurement
 *   2. Red LED ON while sampling
 *   3. Results displayed on LCD, green LED ON when done
 *   4. Press SW1 again for a new measurement
 *
 * Signal Input: PE0 (0 to 3.3 V analog)
 * LCD Connections: PB2 = SCL, PB3 = SDA
 * ==========================================================================
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"

#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/adc.h"
#include "driverlib/timer.h"
#include "driverlib/interrupt.h"
#include "driverlib/i2c.h"
#include "driverlib/pin_map.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

#define SAMPLE_RATE       100000    /* 100 kHz ADC sample rate */
#define BUFFER_SIZE       2048      /* Samples per measurement */
#define ADC_REF_VOLTAGE   3.3f      /* ADC reference voltage */
#define ADC_MAX_VALUE     4095      /* 12-bit ADC maximum */

/* I2C LCD (PCF8574 backpack) - change to 0x3F if using PCF8574A variant */
#define LCD_I2C_ADDR      0x27
#define LCD_BACKLIGHT     0x08
#define LCD_ENABLE        0x04
#define LCD_RS            0x01

/* GPIO defines */
#define LED_RED           GPIO_PIN_1
#define LED_GREEN         GPIO_PIN_3
#define SWITCH_1           GPIO_PIN_4

/* =========================================================================
 * Global Variables
 * ========================================================================= */

static volatile uint32_t g_adcBuffer[BUFFER_SIZE];
static volatile uint32_t g_sampleIndex = 0;
static volatile bool     g_samplingDone = false;
static volatile bool     g_startRequest = false;
static uint32_t          g_sysClock;

/* =========================================================================
 * Function Prototypes
 * ========================================================================= */

/* Initialisation */
void System_Init(void);
void GPIO_Init(void);
void ADC_Init(void);
void Timer_Init(void);
void I2C_LCD_Init(void);

/* LCD primitives */
void LCD_WriteI2C(uint8_t data);
void LCD_PulseEnable(uint8_t data);
void LCD_SendNibble(uint8_t nibble, uint8_t mode);
void LCD_SendByte(uint8_t byte, uint8_t mode);
void LCD_Cmd(uint8_t cmd);
void LCD_Char(uint8_t ch);
void LCD_Print(const char *s);
void LCD_SetCursor(uint8_t row, uint8_t col);
void LCD_Clear(void);

/* Signal processing */
void ProcessSignal(float *peakV, float *freq);

/* Helpers */
void DelayMs(uint32_t ms);
void DelayUs(uint32_t us);
void FloatToStr(float v, char *buf, uint8_t dec);

/* ISRs */
void SwitchISR(void);
void ADC0_SS3_ISR(void);

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void)
{
    float peakVoltage, frequency;
    char line1[17], line2[17];

    /* --- Initialise all peripherals --- */
    System_Init();
    GPIO_Init();
    I2C_LCD_Init();
    ADC_Init();
    Timer_Init();

    /* --- Welcome screen --- */
    LCD_Clear();
    LCD_SetCursor(0, 0);
    LCD_Print("Signal Analyzer");
    LCD_SetCursor(1, 0);
    LCD_Print("Press SW1 Start");

    /* Green LED = ready */
    GPIOPinWrite(GPIO_PORTF_BASE, LED_GREEN | LED_RED, LED_GREEN);

    /* --- Super-loop --- */
    while (1)
    {
        if (g_startRequest)
        {
            /* ---- Indicate "measuring" ---- */
            GPIOPinWrite(GPIO_PORTF_BASE, LED_GREEN | LED_RED, LED_RED);
            LCD_Clear();
            LCD_SetCursor(0, 0);
            LCD_Print("Measuring...");

            /* ---- Begin sampling ---- */
            g_sampleIndex = 0;
            g_samplingDone = false;

            ADCIntClear(ADC0_BASE, 3);
            ADCIntEnable(ADC0_BASE, 3);
            IntEnable(INT_ADC0SS3);
            TimerEnable(TIMER0_BASE, TIMER_A);

            /* Wait until buffer is full */
            while (!g_samplingDone) { }

            /* Stop timer */
            TimerDisable(TIMER0_BASE, TIMER_A);
            ADCIntDisable(ADC0_BASE, 3);

            /* ---- Process samples ---- */
            ProcessSignal(&peakVoltage, &frequency);

            /* ---- Display results ---- */
            LCD_Clear();

            /* Line 1 : Frequency */
            if (frequency >= 1000.0f)
            {
                char tmp[10];
                FloatToStr(frequency / 1000.0f, tmp, 2);
                strcpy(line1, "F:");
                strcat(line1, tmp);
                strcat(line1, " kHz");
            }
            else
            {
                char tmp[10];
                FloatToStr(frequency, tmp, 1);
                strcpy(line1, "F:");
                strcat(line1, tmp);
                strcat(line1, " Hz");
            }
            LCD_SetCursor(0, 0);
            LCD_Print(line1);

            /* Line 2 : Peak Voltage */
            {
                char tmp[10];
                FloatToStr(peakVoltage, tmp, 2);
                strcpy(line2, "Vpk:");
                strcat(line2, tmp);
                strcat(line2, " V");
            }
            LCD_SetCursor(1, 0);
            LCD_Print(line2);

            /* Green LED = done */
            GPIOPinWrite(GPIO_PORTF_BASE, LED_GREEN | LED_RED, LED_GREEN);

            g_startRequest = false;
        }
    }
}

/* =========================================================================
 * System Clock (80 MHz via PLL)
 * ========================================================================= */

void System_Init(void)
{
    SysCtlClockSet(SYSCTL_SYSDIV_2_5 | SYSCTL_USE_PLL |
                   SYSCTL_OSC_MAIN | SYSCTL_XTAL_16MHZ);
    g_sysClock = SysCtlClockGet();
}

/* =========================================================================
 * GPIO (LEDs + Switch with interrupt)
 * ========================================================================= */

void GPIO_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF)) {}

    /* LEDs */
    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, LED_RED | LED_GREEN);

    /* SW1 - input with internal pull-up */
    GPIOPinTypeGPIOInput(GPIO_PORTF_BASE, SWITCH_1);
    GPIOPadConfigSet(GPIO_PORTF_BASE, SWITCH_1,
                     GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

    /* Falling-edge interrupt on SW1 */
    GPIOIntTypeSet(GPIO_PORTF_BASE, SWITCH_1, GPIO_FALLING_EDGE);
    GPIOIntEnable(GPIO_PORTF_BASE, SWITCH_1);

    IntRegister(INT_GPIOF, SwitchISR);
    IntEnable(INT_GPIOF);
    IntMasterEnable();
}

/* =========================================================================
 * ADC0 Sequencer 3 (PE0 / AIN3, Timer-triggered)
 * ========================================================================= */

void ADC_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_ADC0)) {}
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOE)) {}

    /* PE0 as analog input */
    GPIOPinTypeADC(GPIO_PORTE_BASE, GPIO_PIN_0);

    /* SS3 = single-sample sequencer, triggered by timer */
    ADCSequenceConfigure(ADC0_BASE, 3, ADC_TRIGGER_TIMER, 0);
    ADCSequenceStepConfigure(ADC0_BASE, 3, 0,
                             ADC_CTL_CH3 | ADC_CTL_IE | ADC_CTL_END);
    ADCSequenceEnable(ADC0_BASE, 3);
    ADCIntClear(ADC0_BASE, 3);

    /* Register ISR (will be enabled/disabled per measurement) */
    IntRegister(INT_ADC0SS3, ADC0_SS3_ISR);
}

/* =========================================================================
 * Timer0A (periodic, generates ADC trigger)
 * ========================================================================= */

void Timer_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_TIMER0)) {}

    TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);
    TimerLoadSet(TIMER0_BASE, TIMER_A, (g_sysClock / SAMPLE_RATE) - 1);

    /* Let Timer0A output trigger the ADC */
    TimerControlTrigger(TIMER0_BASE, TIMER_A, true);

    /* Timer starts disabled; enabled when a measurement begins */
}

/* =========================================================================
 * I2C0 + LCD Initialisation
 * ========================================================================= */

void I2C_LCD_Init(void)
{
    /* Enable I2C0 & Port B */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_I2C0)) {}
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB)) {}

    /* PB2 = SCL, PB3 = SDA */
    GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);

    /* 100 kbps standard mode */
    I2CMasterInitExpClk(I2C0_BASE, g_sysClock, false);
    DelayMs(50);

    /* HD44780 init sequence - switch to 4-bit mode */
    LCD_SendNibble(0x03, 0); DelayMs(5);
    LCD_SendNibble(0x03, 0); DelayMs(5);
    LCD_SendNibble(0x03, 0); DelayMs(1);
    LCD_SendNibble(0x02, 0); DelayMs(1);

    /* 4-bit, 2 lines, 5x8 */
    LCD_Cmd(0x28);
    /* Display ON, cursor OFF */
    LCD_Cmd(0x0C);
    /* Entry mode: increment */
    LCD_Cmd(0x06);
    /* Clear */
    LCD_Cmd(0x01);
    DelayMs(2);
}

/* =========================================================================
 * LCD Low-Level I2C Helpers
 * ========================================================================= */

void LCD_WriteI2C(uint8_t data)
{
    I2CMasterSlaveAddrSet(I2C0_BASE, LCD_I2C_ADDR, false);
    I2CMasterDataPut(I2C0_BASE, data);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_SEND);
    while (I2CMasterBusy(I2C0_BASE)) {}
}

void LCD_PulseEnable(uint8_t data)
{
    LCD_WriteI2C(data | LCD_ENABLE);
    DelayUs(1);
    LCD_WriteI2C(data & ~LCD_ENABLE);
    DelayUs(50);
}

void LCD_SendNibble(uint8_t nibble, uint8_t mode)
{
    uint8_t data = (nibble << 4) | mode | LCD_BACKLIGHT;
    LCD_PulseEnable(data);
}

void LCD_SendByte(uint8_t byte, uint8_t mode)
{
    LCD_SendNibble(byte >> 4, mode);
    LCD_SendNibble(byte & 0x0F, mode);
}

void LCD_Cmd(uint8_t cmd)  { LCD_SendByte(cmd, 0); DelayMs(2); }
void LCD_Char(uint8_t ch)  { LCD_SendByte(ch, LCD_RS); DelayUs(50); }

void LCD_Print(const char *s)
{
    while (*s) { LCD_Char(*s++); }
}

void LCD_SetCursor(uint8_t row, uint8_t col)
{
    LCD_Cmd(0x80 | ((row == 0) ? col : (0x40 + col)));
}

void LCD_Clear(void)
{
    LCD_Cmd(0x01);
    DelayMs(2);
}

/* =========================================================================
 * Signal Processing
 * ========================================================================= */

void ProcessSignal(float *peakV, float *freq)
{
    uint32_t i;
    uint32_t maxVal = 0;
    uint32_t sum = 0;
    uint32_t risingCrossings = 0;
    float dcOffset;

    /* --- Pass 1: find max and mean (DC offset) --- */
    for (i = 0; i < BUFFER_SIZE; i++)
    {
        uint32_t s = g_adcBuffer[i];
        if (s > maxVal) maxVal = s;
        sum += s;
    }
    dcOffset = (float)sum / (float)BUFFER_SIZE;

    /* Peak voltage = max ADC reading in volts */
    *peakV = (float)maxVal * ADC_REF_VOLTAGE / (float)ADC_MAX_VALUE;

    /* --- Pass 2: count rising zero-crossings --- */
    {
        bool prevAbove = ((float)g_adcBuffer[0] > dcOffset);

        for (i = 1; i < BUFFER_SIZE; i++)
        {
            bool currAbove = ((float)g_adcBuffer[i] > dcOffset);

            if (!prevAbove && currAbove)
                risingCrossings++;

            prevAbove = currAbove;
        }
    }

    /* Frequency = crossings / total sample time */
    if (risingCrossings > 0)
    {
        float totalTime = (float)BUFFER_SIZE / (float)SAMPLE_RATE;
        *freq = (float)risingCrossings / totalTime;
    }
    else
    {
        *freq = 0.0f; /* DC or no signal */
    }
}

/* =========================================================================
 * Utility Functions
 * ========================================================================= */

void DelayMs(uint32_t ms)
{
    SysCtlDelay((g_sysClock / 3000) * ms);
}

void DelayUs(uint32_t us)
{
    SysCtlDelay((g_sysClock / 3000000) * us);
}

/**
 * Simple float-to-string without pulling in sprintf / stdio.
 * Handles values 0-99999 with up to 3 decimal places.
 */
void FloatToStr(float v, char *buf, uint8_t dec)
{
    uint32_t mult = 1;
    uint8_t  d;
    int      intPart, fracPart;
    char     tmp[12];
    int      idx = 0, i;

    for (d = 0; d < dec; d++) mult *= 10;

    if (v < 0.0f) { *buf++ = '-'; v = -v; }

    intPart  = (int)v;
    fracPart = (int)((v - (float)intPart) * (float)mult + 0.5f);

    /* Integer part string (reverse) */
    if (intPart == 0)
    {
        tmp[idx++] = '0';
    }
    else
    {
        while (intPart > 0)
        {
            tmp[idx++] = '0' + (intPart % 10);
            intPart /= 10;
        }
    }
    for (i = idx - 1; i >= 0; i--) *buf++ = tmp[i];

    if (dec > 0)
    {
        *buf++ = '.';
        /* Fractional part with leading zeros */
        idx = 0;
        for (d = 0; d < dec; d++)
        {
            tmp[d] = '0' + (fracPart % 10);
            fracPart /= 10;
        }
        for (i = dec - 1; i >= 0; i--) *buf++ = tmp[i];
    }
    *buf = '\0';
}

/* =========================================================================
 * Interrupt Service Routines
 * ========================================================================= */

/**
 * GPIO Port F ISR - SW1 pressed (falling edge on PF4)
 */
void SwitchISR(void)
{
    GPIOIntClear(GPIO_PORTF_BASE, SWITCH_1);

    /* Only accept if not already sampling */
    if (!g_startRequest && g_samplingDone)
    {
        /* Intentionally left empty on first boot;
           g_samplingDone is false initially, handled below */
    }

    g_startRequest = true;

    /* Debounce delay */
    SysCtlDelay(200000);
}

/**
 * ADC0 Sequencer 3 ISR - called each time a sample is ready
 */
void ADC0_SS3_ISR(void)
{
    uint32_t value;

    ADCIntClear(ADC0_BASE, 3);
    ADCSequenceDataGet(ADC0_BASE, 3, &value);

    if (g_sampleIndex < BUFFER_SIZE)
    {
        g_adcBuffer[g_sampleIndex++] = value;
    }

    if (g_sampleIndex >= BUFFER_SIZE)
    {
        g_samplingDone = true;
    }
}
