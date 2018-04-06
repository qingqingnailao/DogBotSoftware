
#include "motion.h"
#include "pwm.h"
#include "mathfunc.h"
#include "coms.h"
#include "canbus.h"
#include "storedconf.h"

float g_homeAngleOffset = 0;      // Offset from phase position to actuator position.
float g_motorPhase2RotationRatio = 7.0;
float g_actuatorRatio = g_motorPhase2RotationRatio * 21.0; // Gear ratio

float g_absoluteMaxCurrent = 20.0; // Maximum torque allowed
float g_uncalibratedCurrentLimit = 4.0; // Maximum torque allowed in un-calibrated mode.

const int g_positionIndexCount = 4;
bool g_haveIndexPositionSample[g_positionIndexCount];
float g_measuredIndexPosition[g_positionIndexCount]; // Measured position
float g_homeIndexPosition = 0;

uint8_t g_motionTime = 0;

enum MotionHomedStateT g_motionHomedState = MHS_Lost;
enum PositionReferenceT g_motionPositionReference = PR_Absolute;
enum ControlStateT g_controlState = CS_LowPower;
enum FaultCodeT g_lastFaultCode = FC_Ok;
enum JointRelativeT g_motionJointRelative = JR_Isolated;

bool g_indicatorState = false;
bool g_newCalibrationData = false;

uint8_t g_otherJointId = 0;
float g_relativePositionGain = 1.0;
float g_relativePositionOffset = 0.0;

bool MotionSyncTime(void)
{
  g_motionTime = 0;
  return true;
}


void MotionResetCalibration(enum MotionHomedStateT defaultCalibrationState) {
  for(int i = 0;i < g_positionIndexCount;i++) {
    g_haveIndexPositionSample[i] = false;
  }

  bool changed = g_motionHomedState != defaultCalibrationState;

  g_motionHomedState = defaultCalibrationState;
  if(changed) {
    SendParamUpdate(CPI_HomedState);
  }
}

static int HomeIndexUpdateOffset(bool newState,bool velocityPositive)
{
  return (newState ? 1 : 0) + ((velocityPositive ? 2 : 0));
}

void MotionUpdateIndex(bool state,float position,float velocity)
{
  if(g_motionHomedState == MHS_Lost)
    return ;

  // FIXME:- Check we're moving with a velocity greater than
  // a given value to avoid triggering off noise.
  int entry = HomeIndexUpdateOffset(state,velocity > 0);
  if(!g_haveIndexPositionSample[entry]) {
    g_haveIndexPositionSample[entry] = true;
  }
  g_newCalibrationData = true;
  g_measuredIndexPosition[entry] = position;
}

bool MotionEstimateOffset(float &value)
{
  int points = 0;

  float meanS = 0;
  float meanC = 0;

  int off1,off2;
  for(int i = 0;i < 2;i++) {
    if(i == 0) {
      off1 = HomeIndexUpdateOffset(true,true);
      off2 = HomeIndexUpdateOffset(false,false);
    } else {
      off1 = HomeIndexUpdateOffset(false,true);
      off2 = HomeIndexUpdateOffset(true,false);
    }
    if(g_haveIndexPositionSample[off1] && g_haveIndexPositionSample[off2]) {

      float pos1 = g_measuredIndexPosition[off1] / g_actuatorRatio;
      float pos2 = g_measuredIndexPosition[off2] / g_actuatorRatio;
      meanS += sin(pos1);
      meanC += cos(pos1);
      meanS += sin(pos2);
      meanC += cos(pos2);
      points += 2;
    }
  }
  if(points <= 2)
    return false;
  value = (atan2(meanS,meanC) - g_homeIndexPosition) * g_actuatorRatio;
  return true;
}

uint8_t g_requestedJointMode = 0;
int16_t g_requestedJointPosition = 0;
uint8_t g_otherJointMode = 0;
int16_t g_otherJointPosition = 0;
float g_otherPhaseOffset = 0; // Last reported position of other joint.

static float DemandToPhasePosition(int16_t position)
{
  return (((float) position) * g_actuatorRatio * M_PI * 4.0/ 32767.0);
}

static int16_t PhasePositionToDemand(float angle)
{
  return (angle * 32767.0) / (g_actuatorRatio * M_PI * 4.0);
}


// This is converts the absolute coordinates into phase coordinates which
// the motor control loop understands.

void SetupEndStops()
{
  if(g_motionHomedState != MHS_Homed) {
    g_endStopPhaseMin = 0;
    g_endStopPhaseMax = 0;
    g_endStopTargetAcceleration = -1; // Disable acceleration based end-stops for the moment.
    return ;
  }
  g_endStopPhaseMin = g_endStopMin * g_actuatorRatio + g_homeAngleOffset;
  g_endStopPhaseMax = g_endStopMax * g_actuatorRatio + g_homeAngleOffset;
  //g_endStopTargetAcceleration = g_endStopTargetBreakCurrent / g_jointInertia;
  g_endStopTargetAcceleration = g_jointInertia;
}

void EnterHomedState()
{
  g_motionHomedState = MHS_Homed;
  SetupEndStops();
  SendParamUpdate(CPI_CalibrationOffset);
  SendParamUpdate(CPI_HomedState);
}


bool UpdateRequestedPosition()
{
  enum PWMControlDynamicT mode = static_cast<enum PWMControlDynamicT>(g_requestedJointMode >> 2);
  switch(mode)
  {
    case CM_Position:
      {
        float positionAsPhaseAngle = DemandToPhasePosition(g_requestedJointPosition);

        if((g_requestedJointMode & 0x1) != 0) { // Calibrated position ?
          // Yes we're dealing with a calibrated position request,
          // this can only be processed if we're homed.
          if(g_motionHomedState != MHS_Homed) return false;
          positionAsPhaseAngle += g_homeAngleOffset;
        }
        if((g_motionPositionReference & 0x2) != 0) { // Relative position to another joint?
          // If we want a calibrated position and the other joint is uncalibrated then
          // give up.
          if((g_otherJointMode & 0x1) == 0
              && (g_motionPositionReference & 0x1) != 0) {
            return false;
          }
          g_otherPhaseOffset = DemandToPhasePosition(g_otherJointPosition)
              * g_relativePositionGain + g_relativePositionOffset;
          positionAsPhaseAngle += g_otherPhaseOffset;
        }

        // Check the demand position doesn't leave the soft end stops. The soft end stops are only
        // relative to the local homed position.
        if(g_motionHomedState == MHS_Homed && g_endStopEnable) {
          // Check end stops.
          if(positionAsPhaseAngle < g_endStopPhaseMin)
            positionAsPhaseAngle = g_endStopPhaseMin;
          if(positionAsPhaseAngle > g_endStopPhaseMax)
            positionAsPhaseAngle = g_endStopPhaseMax;
        }

        g_demandPhasePosition = positionAsPhaseAngle;
      }
      break;
    case CM_Velocity:
      // Uck!
      g_demandPhaseVelocity = PhasePositionToDemand(g_requestedJointPosition);
      break;
    case CM_Torque:
      g_demandTorque = (((float)g_requestedJointPosition) * g_absoluteMaxCurrent) / 32767.0f;
      //g_
      break;
    default:
      FaultDetected(FC_InvalidCommand);
      break;
  }
  return true;
}


bool MotionSetPosition(uint8_t mode,int16_t position,uint16_t torqueLimit)
{
  g_requestedJointMode = mode;
  float newCurrentLimit = ((float) torqueLimit) * g_absoluteMaxCurrent / (65536.0);

  // If we're not homed put a limit on the maximum current we'll accept.
  if(g_motionHomedState != MHS_Homed) {
    if(newCurrentLimit > g_uncalibratedCurrentLimit) {
      newCurrentLimit = g_uncalibratedCurrentLimit;
    }
  }

  g_currentLimit = newCurrentLimit;
  g_requestedJointPosition = position;

  return UpdateRequestedPosition();
}

bool MotionOtherJointUpdate(int16_t position,int16_t torque,uint8_t mode,uint8_t timestamp)
{
  // The following are not used at the moment.
  (void) timestamp;
  (void) torque;

  g_otherJointPosition = position;
  g_otherJointMode = mode;
  if((g_motionPositionReference & 0x2) != 0)
    return UpdateRequestedPosition();
  return true;
}


bool MotionReport(int16_t position,int16_t torque,PositionReferenceT posRef,uint8_t timestamp)
{
  uint8_t mode = posRef & DOGBOT_SERVOREPORTMODE_POSITIONREF;
  if(g_safetyMode == SM_MasterEmergencyStop) {
    if(!IsEmergencyStopButtonSetToSafe()) {
      ChangeControlState(CS_EmergencyStop,SCS_Internal);
    } else {
      // Set safe flag.
      mode |= DOGBOT_SERVOREPORTMODE_EMERGENCYSTOP;
    }
  }
  if(g_hitLimitVelocity) {
    g_hitLimitVelocity = false;
    mode |= DOGBOT_SERVOREPORTMODE_LIMITVELOCITY;
  }
  if(g_hitLimitTorque) {
    g_hitLimitTorque = false;
    mode |= DOGBOT_SERVOREPORTMODE_LIMITTORQUE;
  }
  if(g_hitLimitPosition) {
    g_hitLimitPosition = false;
    mode |= DOGBOT_SERVOREPORTMODE_LIMITPOSITION;
  }

  // Report endstop switch.
  if(palReadPad(GPIOC, GPIOC_PIN8))
    mode |= DOGBOT_SERVOREPORTMODE_INDEXSENSOR;

  if(g_canBridgeMode) {
    PacketServoReportC servo;
    servo.m_packetType = CPT_ServoReport;
    servo.m_deviceId = g_deviceId;
    servo.m_mode = mode;
    servo.m_position = position;
    servo.m_torque = torque;
    servo.m_timestamp = timestamp;
    USBSendPacket(reinterpret_cast<uint8_t *>(&servo),sizeof(PacketServoReportC));
  }
  if(g_deviceId != 0) {
    CANSendServoReport(g_deviceId,position,torque,mode,timestamp);
  }
  return true;
}

void CalibrationCheckFailed() {
  if(g_motionHomedState == MHS_Lost)
    return ;
  g_motionHomedState = MHS_Lost;
  if(g_motionPositionReference == PR_Absolute) {
    FaultDetected(FC_PositionLost);
  }
  MotionResetCalibration(MHS_Lost);
  SendParamUpdate(CPI_HomedState);
}

bool MotionCalZero()
{
  // Can only calibrate if pwm loop is running and we are updating our position.
  if(!g_pwmRun)
    return false;

  // Make sure we don't fly off somewhere.
  g_requestedJointMode = 0;
  g_requestedJointPosition = g_currentPhasePosition;

  g_homeAngleOffset = g_currentPhasePosition;
  EnterHomedState();
  return true;
}


void MotionStep()
{
#if 1
  int16_t torque = g_torqueAverage * (32767.0/g_absoluteMaxCurrent);

  g_motionTime++;
  EmergencyStopTick();

  // Should we
  switch(g_motionHomedState)
  {
    case MHS_Lost:
      break;
    case MHS_Measuring:
      // Establishing a new calibration
      if(g_newCalibrationData) {
        g_newCalibrationData = false;
        if(MotionEstimateOffset(g_homeAngleOffset)) {
          EnterHomedState();
        }
      }
      break;
    case MHS_Homed: {
      // If we're calibrated, we're just going to check all is fine.
      if(g_newCalibrationData) {
        g_newCalibrationData = false;
        // If this is manual set, this will disagree and cause an error.
#if 0
        MotionEstimateOffset(g_homeAngleOffset);
        SendParamUpdate(CPI_CalibrationOffset);
#else
#if 0
        float estimate = 0;
        if(MotionEstimateOffset(estimate)) {
          if(fabs(estimate - g_homeAngleOffset) > M_PI/2.0) {
            CalibrationCheckFailed();
          }
        }
#endif
#endif
      }
    } break;
  }

  float position = g_currentPhasePosition;

  enum PositionReferenceT posRef = g_motionPositionReference;

  switch(g_motionPositionReference)
  {
    case PR_Absolute: {
      if(g_motionHomedState == MHS_Homed) {
        position -= g_homeAngleOffset;
      } else {
        // Without calibration we can only send relative positions.
        position = g_currentPhasePosition;
        posRef = PR_Relative;
      }
      if(g_motionJointRelative == JR_Offset)
        position -= g_otherPhaseOffset;
      int16_t reportPosition = PhasePositionToDemand(position);
      MotionReport(reportPosition,torque,posRef,g_motionTime);
    } break;
    case PR_Relative: {
      if(g_motionJointRelative == JR_Offset)
        position -= g_otherPhaseOffset;
      uint16_t reportPosition = PhasePositionToDemand(position);
      MotionReport(reportPosition,torque,posRef,g_motionTime);
    } break;
  }
#endif
}


enum FaultCodeT LoadSetup(void) {
  if(!g_eeInitDone) {
    StoredConf_Init();
    g_eeInitDone = true;
  }
  StoredConf_Load(&g_storedConfig);

  //g_deviceId = g_storedConfig.deviceId; //!< This can cause Id conflicts, better to let controller give id again
  g_otherJointId = g_storedConfig.otherJointId;
  g_relativePositionGain = g_storedConfig.m_relativePositionGain;
  g_relativePositionOffset = g_storedConfig.m_relativePositionOffset;
  g_motionPositionReference = (enum PositionReferenceT) g_storedConfig.m_motionPositionReference;

  g_phaseResistance = g_storedConfig.m_phaseResistance;
  g_phaseInductance = g_storedConfig.m_phaseInductance;
  g_phaseOffsetVoltage = g_storedConfig.m_phaseOffsetVoltage;

  // g_velocityLimit = g_storedConfig.m_velocityLimit;
  g_velocityLimit = 100.0; //!< Load a low default limit, it is up to the control software to raise it when ready

  g_absoluteMaxCurrent = g_storedConfig.m_absoluteMaxCurrent;
  g_homeIndexPosition = g_storedConfig.m_homeIndexPosition;
  g_minSupplyVoltage = g_storedConfig.m_minSupplyVoltage;

  // Setup angles.
  for(int i = 0;i < g_calibrationPointCount;i++) {
    g_phaseAngles[i][0] = g_storedConfig.phaseAngles[i][0];
    g_phaseAngles[i][1] = g_storedConfig.phaseAngles[i][1];
    g_phaseAngles[i][2] = g_storedConfig.phaseAngles[i][2];
  }

  g_jointRole = g_storedConfig.m_jointRole;
  g_endStopEnable = g_storedConfig.m_endStopEnable;
  g_endStopMin = g_storedConfig.m_endStopMin;
  g_endStopStartBounce = g_storedConfig.m_endStopStartBounce;
  g_endStopMax = g_storedConfig.m_endStopMax;
  g_endStopEndBounce = g_storedConfig.m_endStopEndBounce;
  g_endStopTargetBreakCurrent = g_storedConfig.m_endStopTargetBreakCurrent;
  g_endStopMaxBreakCurrent = g_storedConfig.m_endStopMaxBreakCurrent;
  g_jointInertia = g_storedConfig.m_jointInertia;
  g_safetyMode = g_storedConfig.m_safetyMode;

  SetupEndStops();

  return FC_Ok;
}

enum FaultCodeT SaveSetup(void) {
  if(!g_eeInitDone) {
    StoredConf_Init();
    g_eeInitDone = true;
  }

  g_storedConfig.configState = 1;
  g_storedConfig.deviceId = g_deviceId;
  g_storedConfig.otherJointId = g_otherJointId;

  g_storedConfig.m_relativePositionGain = g_relativePositionGain;
  g_storedConfig.m_relativePositionOffset =  g_relativePositionOffset;
  g_storedConfig.m_motionPositionReference = (int) g_motionPositionReference;
  g_storedConfig.m_phaseResistance = g_phaseResistance;
  g_storedConfig.m_phaseInductance = g_phaseInductance;
  g_storedConfig.m_phaseOffsetVoltage = g_phaseOffsetVoltage;
  g_storedConfig.m_velocityLimit = g_velocityLimit;
  g_storedConfig.m_absoluteMaxCurrent = g_absoluteMaxCurrent;
  g_storedConfig.m_homeIndexPosition = g_homeIndexPosition;
  g_storedConfig.m_minSupplyVoltage = g_minSupplyVoltage;

  for(int i = 0;i < g_calibrationPointCount;i++) {
    g_storedConfig.phaseAngles[i][0] = g_phaseAngles[i][0];
    g_storedConfig.phaseAngles[i][1] = g_phaseAngles[i][1];
    g_storedConfig.phaseAngles[i][2] = g_phaseAngles[i][2];
  }

  g_storedConfig.m_jointRole = g_jointRole;
  g_storedConfig.m_endStopEnable = g_endStopEnable;
  g_storedConfig.m_endStopMin = g_endStopMin;
  g_storedConfig.m_endStopStartBounce = g_endStopStartBounce;
  g_storedConfig.m_endStopMax = g_endStopMax;
  g_storedConfig.m_endStopEndBounce = g_endStopEndBounce;
  g_storedConfig.m_endStopTargetBreakCurrent = g_endStopTargetBreakCurrent;
  g_storedConfig.m_endStopMaxBreakCurrent = g_endStopMaxBreakCurrent;
  g_storedConfig.m_jointInertia = g_jointInertia;
  g_storedConfig.m_safetyMode = g_safetyMode;

  if(!StoredConf_Save(&g_storedConfig)) {
    return FC_InternalStoreFailed;
  }
  return FC_Ok;
}
