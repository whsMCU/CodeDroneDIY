#include <math.h>
#include <avr/wdt.h>
#include "Settings.h"
#include "GetPosition.h"
#include "ESC.h"
#include "Reception.h"
#include "Time.h"
#include "PID.h"
#include "SetPWM.h"

typedef enum { _timer1, _Nbr_16timers } timer16_Sequence_t;

Time time;
MPU6050 accelgyro;
Reception Rx;
PID rollPID;
PID pitchPID;
PID yawPID;
GetPosition Position;

void idleESC(){
    ESC0.write( MIN_POWER); 
    ESC1.write( MIN_POWER); 
    ESC2.write( MIN_POWER); 
    ESC3.write( MIN_POWER); 
    ESCList[0] = ESC0;
    ESCList[1] = ESC1;
    ESCList[2] = ESC2;
    ESCList[3] = ESC3;
    g_NewVal = true;
}

static inline void handle_interrupts(timer16_Sequence_t timer, volatile uint16_t *TCNTn, volatile uint16_t* OCRnA){
 // SetPWM_f4(TCNTn, OCRnA);
  SetPWM_f5(TCNTn, OCRnA);
}

SIGNAL (TIMER1_COMPA_vect){
  handle_interrupts(_timer1, &TCNT1, &OCR1A);
}

void RxInterrupt(){
  Rx.GetWidth();
}

void InitTimer1(){
  // Timer
  TCCR1A = 0;             // normal counting mode
  TCCR1B = _BV(CS10);     // no prescaler
  TCNT1 = 0;              // clear the timer count
  OCR1A = usToTicks(MIN_POWER);
  
  TIFR1 |= _BV(OCF1A);      // clear any pending interrupts;
  TIMSK1 |=  _BV(OCIE1A) ;  // enable the output compare interrupt
}

int g_Throttle[10] = { MIN_POWER, MIN_POWER, MIN_POWER, MIN_POWER, MIN_POWER, MIN_POWER, MIN_POWER, MIN_POWER, MIN_POWER, MIN_POWER};
int g_FlyingMode = FLYING_MODE_ACCRO;
int g_Kp = 0;
bool g_YawPIDActivated = false;

void setup() {
  // Set watchdog reset
  wdt_enable(WDTO_250MS);

  ESC0.attach(8);
  ESC1.attach(9);
  ESC2.attach(10);
  ESC3.attach(11);
  idleESC();
  
  InitTimer1();
    
  // Receiver
  attachInterrupt(0, RxInterrupt, RISING);
 
  // join I2C bus (I2Cdev library doesn't do this automatically)
  Wire.begin();
  Wire.setClock(400000L); // Communication with MPU-6050 at 500KHz
  
 // initialize serial communication
  Serial.begin(250000);

  // initialize device
  accelgyro.initialize();
  accelgyro.setFullScaleGyroRange( MPU6050_GYRO_FS_500); //  +-500°s max  /!\ Be carrefull when changing this parameter: "GyroSensitivity" must be updated accordingly !!!
  accelgyro.setFullScaleAccelRange( MPU6050_ACCEL_FS_2 );//  +-2g max
  
 if( !accelgyro.testConnection())
  Serial.println("Test failed");

  Serial.println("Computing offsets...");
  Position.ComputeOffsets(accelgyro);

  while( !Rx.IsReady() ){
    idleESC();
    delay(10);
  }
  
  g_FlyingMode = Rx.GetFlyingMode(); // Forced to accro
  if( g_FlyingMode == FLYING_MODE_ANGLE){
    rollPID.SetPIDCoef(ANGLE_GAIN, ANGLE_ROLLPITCH_KP, ANGLE_ROLLPITCH_KD, ANGLE_ROLLPITCH_KI); // G, Kp, Kd, Ki
    pitchPID.SetPIDCoef(ANGLE_GAIN, ANGLE_ROLLPITCH_KP, ANGLE_ROLLPITCH_KD, ANGLE_ROLLPITCH_KI);
  }else{
    rollPID.SetPIDCoef(ACCRO_GAIN, ACCRO_ROLLPITCH_KP, ACCRO_ROLLPITCH_KD, ACCRO_ROLLPITCH_KI); // G, Kp, Kd, Ki
    pitchPID.SetPIDCoef(ACCRO_GAIN, ACCRO_ROLLPITCH_KP, ACCRO_ROLLPITCH_KD, ACCRO_ROLLPITCH_KI); 

    g_Kp =  map(analogRead(2), 0, 1023, 0, 300);
    yawPID.SetPIDCoef(ACCRO_GAIN, g_Kp, 0, 0); // G, Kp, Kd, Ki
  }

  time.Init();

  g_YawPIDActivated = Rx.GetSwitchH();

  // Print setup
  Serial.print("MAX_POWER:\t");Serial.println(MAX_POWER);
  if( g_FlyingMode == FLYING_MODE_ANGLE){
    Serial.println("FLYING_MODE_ANGLE");
    Serial.print("Angle PID:\t");Serial.print(ANGLE_GAIN, 4);Serial.print("\t");Serial.print(ANGLE_ROLLPITCH_KP);Serial.print("\t");Serial.print(ANGLE_ROLLPITCH_KD);Serial.print("\t"); Serial.println(ANGLE_ROLLPITCH_KI);
  }else{
    Serial.println("FLYING_MODE_ACCRO");
    Serial.print("Accro PID:\t");Serial.print(ACCRO_GAIN, 2);Serial.print("\t");Serial.print(ACCRO_ROLLPITCH_KP);Serial.print("\t");Serial.print(ACCRO_ROLLPITCH_KD);Serial.print("\t"); Serial.println(ACCRO_ROLLPITCH_KI);
    Serial.println("Speed commands received:\t");
    Serial.print("Aile:\t"); Serial.print(Rx.GetAileronsSpeed());Serial.print("\tElev:\t"); Serial.print(Rx.GetElevatorSpeed());Serial.print("\tThrot:\t"); Serial.print(Rx.GetThrottle());Serial.print("\tRudd:\t"); Serial.println(Rx.GetRudder());
  }
  Serial.print("Yaw PID activation:\t"); Serial.println(g_YawPIDActivated);
  
  Serial.print("MIXING:\t"); Serial.println(MIXING, 2);
  
  Serial.println("Setup Finished");
}

float speedCurr[3] = { 0.0, 0.0, 0.0 }; // Teta speed (°/s) (only use gyro)
float posCurr[3] = { 0.0, 0.0, 0.0 }; // Teta position (°) (use gyro + accelero)
int g_iloop = 0;

float g_MeanLoop = 0;

//    + configuration:
//         ESC0(CCW)
//          ^
//          |
//  ESC3 <-- --  ESC1
//          |
//         ESC2(CCW)
//   
void PlusConfig(int _throttle, int _pitchPIDOutput, int _YawPIDOutput, int _rollPIDOutput){
  // Pitch correction
  ESC0.write( _throttle - _pitchPIDOutput - _YawPIDOutput);
  //ESC0.write(MIN_POWER);
  ESC2.write( _throttle + _pitchPIDOutput - _YawPIDOutput); 
  //ESC2.write(MIN_POWER);
  
  // Roll correction
  ESC1.write( _throttle - _rollPIDOutput + _YawPIDOutput); 
  ESC3.write( _throttle + _rollPIDOutput + _YawPIDOutput);
}

//    X configuration:
//  ESC0(CCW)  ESC1
//         \  /
//         /  \
//     ESC3   ESC2(CCW)
//

/*void XConfig(int _throttle, int _pitchPIDOutput, int _YawPIDOutput, int _rollPIDOutput){

  ESC0.write( _throttle - _pitchPIDOutput*MIXING +_rollPIDOutput*MIXING - _YawPIDOutput); 
 // ESC0.write(MIN_POWER);
    
 // ESC1.write( _throttle - _pitchPIDOutput*MIXING - _rollPIDOutput*MIXING + _YawPIDOutput);
  ESC1.write(MIN_POWER);
   
  ESC2.write( _throttle + _pitchPIDOutput*MIXING - _rollPIDOutput*MIXING - _YawPIDOutput); 
  //ESC2.write(MIN_POWER);
    
 // ESC3.write( _throttle + _pitchPIDOutput*MIXING +_rollPIDOutput*MIXING  + _YawPIDOutput);
  ESC3.write(MIN_POWER);
}*/

int loopNb = 0;
float meanLoopTime =  0;
  
void loop() {

  int throttle, rudder= 0;
  float loop_time = time.GetloopTime();
  int rollPIDOutput, pitchPIDOutput, YawPIDOutput = 0;
    
    // Get throttle and current position
    throttle = Rx.GetThrottle();

   if( g_FlyingMode == FLYING_MODE_ANGLE ){
      Position.GetCurrPos(accelgyro, posCurr, loop_time);
      if( throttle > 1100 ){
        rollPIDOutput = rollPID.GetPIDOutput( Rx.GetAileronsAngle(), posCurr[0], loop_time );
        pitchPIDOutput = pitchPID.GetPIDOutput( Rx.GetElevatorAngle(), posCurr[1], loop_time );
        YawPIDOutput = Rx.GetRudder();
        
      }else{
        pitchPIDOutput = rollPIDOutput = YawPIDOutput = 0; // No correction if throttle put to min
        rollPID.Reset();
        pitchPID.Reset();
       // yawPID.Reset();
      }
    }else{ // FLYING_MODE_ACCRO*/
      Position.GetCurrSpeed(accelgyro, speedCurr);
      if( throttle > 1100 ){
        rollPIDOutput = rollPID.GetPIDOutput( Rx.GetAileronsSpeed(), speedCurr[0], loop_time );
        pitchPIDOutput = pitchPID.GetPIDOutput( Rx.GetElevatorSpeed(), speedCurr[1], loop_time );
        
        if( g_YawPIDActivated ){ // Activate PID on yaw axis
          YawPIDOutput = yawPID.GetPIDOutput( Rx.GetRudder(), speedCurr[2], loop_time );
        }else{
           YawPIDOutput = Rx.GetRudder();
        }
       //Serial.println(YawPIDOutput);
       //Serial.print(speedCurr[1]);Serial.print("\t"); Serial.println(speedCurr[0]);
      }else{
       pitchPIDOutput = rollPIDOutput = YawPIDOutput = 0; // No correction if throttle put to min
       rollPID.Reset();
       yawPID.Reset();
     }
   }

  PlusConfig(throttle, pitchPIDOutput, YawPIDOutput, rollPIDOutput);
 
  if( loopNb > 1000)
  {
    meanLoopTime = meanLoopTime/loopNb;
    Serial.println(meanLoopTime*1000, 2);
    meanLoopTime = 0;
    loopNb = 0;    
 }else{
    meanLoopTime += loop_time;
    loopNb++;   
  }
  
  wdt_reset();
}

// Notes:
// Inter pos 0: 1900; Inter pos 1: 1496; Inter pos 3: 1088 



