#include <ms5611.h>

#include <Arduino.h>
#include <I2Cdev.h>

/* to store measures generated by interrupts */
volatile uint32_t d1i;
volatile uint32_t d2i;

/* measure status */
volatile static int measureStep;
volatile static boolean newData;

/* mutex flags */
static boolean locked =  false;
static boolean interruptWait = false;

/* compensated values */
static boolean deviceReset = false;
static uint16_t c1, c2, c3, c4, c5, c6; //PROM factors
static double compensatedTemperature;
static double compensatedPressure;


/*********************/
/* measure functions */
/*********************/

/* reset */
void ms5611_resetCommand() {
  I2Cdev::writeBytes(MS5611_ADDRESS, MS5611_CMD_RESET, 0, NULL, false);
}

/* read 16 bit PROM register */
/* address from 0 to 7 */
uint16_t ms5611_getPROMValue(int address){

  uint8_t data[2];
  I2Cdev::readBytes(MS5611_ADDRESS, MS5611_CMD_READ_PROM + (address*2), 2, data, I2Cdev::readTimeout, false);
  uint16_t v = data[0];
  v <<= 8;
  v += data[1];

  return v;
}

/* read 24 bit value */
void ms5611_getDigitalValue(volatile uint32_t& value) {

  uint8_t data[3];
  I2Cdev::readBytes(MS5611_ADDRESS, MS5611_CMD_ADC_READ, 3, data, I2Cdev::readTimeout, false);
  value = (uint32_t)data[0];
  value <<= 8;
  value += data[1];
  value <<= 8;
  value += data[2];
}

/* convert D1 */
void ms5611_convertD1() {
  I2Cdev::writeBytes(MS5611_ADDRESS, MS5611_CMD_CONV_D1, 0, NULL, false);
}

/* convert D2 */
void ms5611_convertD2() {
  I2Cdev::writeBytes(MS5611_ADDRESS, MS5611_CMD_CONV_D2, 0, NULL, false);
}

/*  read temp step : read temp and convert pressure */
void ms5611_readTempStep() {

  /* read raw temp */
  ms5611_getDigitalValue(d1i);

  /* convert d2 */
  ms5611_convertD2();
}

/*  read pressure step : read pressure and convert temp */
void ms5611_readPressureStep() {

  /* read raw pressure */
  ms5611_getDigitalValue(d2i);

  /* convert d1 */
  ms5611_convertD1();

  /* now we have new data */
  newData = true;
}


/* altimeter read step */
/* MUST BE DONE AT STABLE FREQUENCY */
/* here using interrupts */
void ms5611_readStep() {

  if( measureStep == MS5611_STEP_READ_TEMP ) {
    ms5611_readTempStep();
    measureStep = MS5611_STEP_READ_PRESSURE;
  } else {
    ms5611_readPressureStep();
    measureStep = MS5611_STEP_READ_TEMP;
  }
}

/*********************************/
/* interrupt and mutex functions */
/*********************************/

/* lock mutex */
void ms5611_lock() {
  locked = true;
}

/* release mutex */
void ms5611_release() {
  locked = false;

  /* check if and interrupt was done between lock and release */ 
  if( interruptWait ) {
    ms5611_readStep(); //the interrupt can't read, do it for it

#ifdef TIMER2_COMPA_vect
    TCNT2 = 0; //reset timer
#else
    TCNT3 = 0; //reset timer
#endif
    
    interruptWait = false;
  }
}

/* the main interrupt function */
/* read at stable frequency */
#ifdef TIMER2_COMPA_vect
ISR(TIMER2_COMPA_vect) {
#else
ISR(TIMER3_COMPA_vect) {
#endif
  
  /* if mutex locked, let the main loop do the job when release */
  if( locked ) {
    interruptWait = true;
    return;
  }
    
  /* reenable interrupts for i2c */
  interrupts();

  /* read at stable frequency */
  ms5611_readStep();
}


/* setting timer */
void ms5611_setTimer() {
  noInterrupts();   // disable all interrupts

#ifdef TIMER2_COMPA_vect
  TCCR2A = 0b00000010; //CTC MODE
  TCCR2B = 0b00000111; //1024 prescale
  TIMSK2 = 0b00000010; //enable CompA
  
  TCNT2  = 0;
  OCR2A = MS5611_INTERRUPT_COMPARE;
#else
  TCCR3A = 0b00000000; //CTC MODE
  TCCR3B = 0b00001101; //1024 prescale
  TIMSK3 = 0b00000010; //enable CompA
  
  TCNT3  = 0;
  OCR3A = MS5611_INTERRUPT_COMPARE;
#endif
  
  interrupts();
}


/******************************/
/* altimeter public functions */
/******************************/

/* init */
void ms5611_init() {
  
  /* reset the device if needed */
  if( ! deviceReset ){
    deviceReset = true;
    ms5611_resetCommand();
    delay(MS5611_RESET_DELAY);
    
    c1 = ms5611_getPROMValue(0);
    c2 = ms5611_getPROMValue(1);
    c3 = ms5611_getPROMValue(2);
    c4 = ms5611_getPROMValue(3);
    c5 = ms5611_getPROMValue(4);
    c6 = ms5611_getPROMValue(5);
  }
  
  /* initialize interrupt variables */
  measureStep = MS5611_STEP_READ_TEMP;
  newData = false;

  /* convert D1 before starting interrupts */
  ms5611_convertD1();
  delay(MS5611_CONV_DELAY);

  /* start interrupts */
  ms5611_setTimer();

}


/* check if we have new data */
boolean ms5611_dataReady(void) {
  return newData;
}

/* update data */
void ms5611_updateData(void) {

  /* lock the mutex to get the values */
  uint32_t d1;
  uint32_t d2;
  ms5611_lock();
  d1 = d1i;
  d2 = d2i;
  newData = false;
  ms5611_release();

  /* compute temperature */
  int32_t dt, temp;
  
  int32_t c5s = c5;
  c5s <<= 8;
  dt = d2 - c5s;

  int32_t c6s = c6;
  c6s *= dt;
  c6s >>= 23;
  
  temp = 2000 + c6s;

  /* compute compensation */
  int64_t off, sens;
  
  /* offset */
  int64_t c2d = c2;
  c2d <<=  16;
  
  int64_t c4d = c4;
  c4d *= dt;
  c4d >>= 7;

  off = c2d + c4d;
 
  /* sens */
  int64_t c1d = c1;
  c1d <<= 15;

  int64_t c3d = c3;
  c3d *= dt;
  c3d >>= 8;
 
  sens = c1d + c3d;

  /* second order compensation */
  int64_t t2, off2, sens2;
 
  if( temp < 2000 ) {
    t2 = dt;
    t2 *= t2;
    t2 >>= 31;
    
    off2 = temp-2000;
    off2 *= off2;
    off2 *= 5;
    sens2 = off2;
    off2 >>= 1;
    sens2 >>= 2;
      
    if( temp < -1500 ){
      int64_t dtemp = temp + 1500;
      dtemp *= dtemp;
      off2 += 7*dtemp;
      dtemp *= 11;
      dtemp >>= 1;
      sens2 += dtemp;
    }
    temp = temp - t2;
    off = off - off2;
    sens = sens - sens2;
  }
  
  /* compute pressure */
  int64_t p;
 
  p = d1 * sens;
  p >>= 21;
  p -= off;
  //p >>= 15 !!! done with doubles, see below

  /* save result */
  compensatedTemperature = (double)temp/100;
  compensatedPressure = ((double)p / (double)32768)/(double)100;  //32768 = 2^15
}

/* return temperature */  
double ms5611_getTemperature() {
  return compensatedTemperature;
}

/* return pressure */
double ms5611_getPressure() {
  return compensatedPressure;
}

/* compute altitude */
double ms5611_getAltitude() {
  double alti;
  alti = pow((compensatedPressure/(MS5611_BASE_SEA_PRESSURE)), 0.1902949572); //0.1902949572 = 1/5.255
  alti = (1-alti)*(288.15/0.0065);
  return alti;
}
    