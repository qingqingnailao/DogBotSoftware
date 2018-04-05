
#include <string>
#include "dogbot/JointRelative.hh"
#include "dogbot/DogBotAPI.hh"
#include "dogbot/Util.hh"

namespace DogBotN {

  //! Default constructor
  JointRelativeC::JointRelativeC()
  {}

  //! Constructor
  JointRelativeC::JointRelativeC(std::shared_ptr<JointC> &jointDrive,std::shared_ptr<JointC> &jointRef)
    : m_jointDrive(jointDrive),
      m_jointRef(jointRef)
  {}

  //! Destructor
  JointRelativeC::~JointRelativeC()
  {
    m_driveCallback.Remove();
  }

  //! Type of joint
  std::string JointRelativeC::JointType() const
  {
    return "relative";
  }

  bool JointRelativeC::Raw2Simple(
      float refPosition,float refVelocity,float refTorque,
      float drivePosition,float driveVelocity,float driveTorque,
      double &position,double &velocity,double &torque
  ) const
  {
    position = (drivePosition - (refPosition * m_refGain + m_refOffset)) * m_gain;
    velocity = driveVelocity - refVelocity;
    torque = driveTorque;
    return true;
  }

  bool JointRelativeC::Simple2Raw(
       float refPosition,float refTorque,
       float position,float torque,
       double &drivePosition,double &driveTorque
  ) const
  {
    drivePosition = (position + (refPosition * m_refGain + m_refOffset)) / m_gain;
    driveTorque = torque;
    return true;
  }

  //! Configure from JSON
  bool JointRelativeC::ConfigureFromJSON(
      DogBotAPIC &api,
      const Json::Value &value
      )
  {
    if(!JointC::ConfigureFromJSON(api,value))
      return false;
    std::string jointRefName = value.get("jointRef","").asString();
    if(!jointRefName.empty()) {
      m_jointRef = api.GetJointByName(jointRefName);
      if(!m_jointRef) {
        api.Log().error("Failed to find servo '{}' ",jointRefName);
        return false;
      }
      m_demandCallback.Remove();
      m_demandCallback = m_jointRef->AddDemandUpdateCallback(
          [this](double position,double torqueLimit) mutable
          { UpdateDemand(); }
      );
    }
    std::string jointDriveName = value.get("jointDrive","").asString();
    if(!jointDriveName.empty()) {
      m_jointDrive = api.GetJointByName(jointDriveName);
      if(!m_jointDrive) {
        api.Log().error("Failed to find servo '{}' ",jointDriveName);
        return false;
      }
    }
    m_refGain = value.get("refGain",1.0).asFloat();
    m_refOffset = value.get("refOffset",0.0).asFloat();
    return true;
  }

  //! Get the servo configuration as JSON
  Json::Value JointRelativeC::ConfigAsJSON() const
  {
    Json::Value ret = JointC::ConfigAsJSON();

    if(m_jointRef)
      ret["jointRef"] = m_jointRef->Name();
    if(m_jointDrive)
      ret["jointDrive"] = m_jointDrive->Name();
    ret["refGain"] = m_refGain;
    ret["refOffset"] = m_refOffset;
    return ret;
  }

  //! Get last reported state of the servo and the time it was taken.
  bool JointRelativeC::GetState(TimePointT &tick,double &position,double &velocity,double &torque) const
  {
    double drivePosition = 0;
    double driveVelocity = 0;
    double driveTorque = 0;

    if(!m_jointDrive->GetState(tick,drivePosition,driveVelocity,driveTorque))
      return false;

    double refPosition = 0;
    double refVelocity = 0;
    double refTorque = 0;

    if(!m_jointRef->GetStateAt(tick,refPosition,refVelocity,refTorque))
      return false;

    Raw2Simple(refPosition,refVelocity,refTorque,
               drivePosition,driveVelocity,driveTorque,
               position,velocity,torque
               );

    return true;
  }

  //! Estimate state at the given time.
  //! This will linearly extrapolate position, and assume velocity and torque are
  //! the same as the last reading.
  //! If the data is more than 5 ticks away from the
  bool JointRelativeC::GetStateAt(TimePointT tick,double &position,double &velocity,double &torque) const
  {
    double drivePosition = 0;
    double driveVelocity = 0;
    double driveTorque = 0;

    if(!m_jointDrive->GetState(tick,drivePosition,driveVelocity,driveTorque)) {
      return false;
    }

    double refPosition = 0;
    double refVelocity = 0;
    double refTorque = 0;

    if(!m_jointRef->GetStateAt(tick,refPosition,refVelocity,refTorque)) {
      return false;
    }

    return Raw2Simple(refPosition,refVelocity,refTorque,
               drivePosition,driveVelocity,driveTorque,
               position,velocity,torque
               );
  }

  //! Update torque for the servo.
  bool JointRelativeC::DemandTorque(float torque)
  {
    return m_jointDrive->DemandTorque(torque);
  }

  //! Add a update callback for motor position

  CallbackHandleC JointRelativeC::AddPositionUpdateCallback(const PositionUpdateFuncT &callback)
  {
    assert(m_jointDrive);
    {
      std::lock_guard<std::mutex> lock(m_mutexJointAdmin);
      if(!m_driveCallback.IsActive()) {
        m_driveCallback = m_jointDrive->AddPositionUpdateCallback(
            [this](TimePointT theTime,double drivePosition,double driveVelocity,double driveTorque) mutable
            {
              double refPosition = 0;
              double refVelocity = 0;
              double refTorque = 0;
              double position;
              double velocity;
              double torque;
              if(!m_jointRef->GetStateAt(theTime,refPosition,refVelocity,refTorque)) {
                return ;
              }

              if(!Raw2Simple(refPosition,refVelocity,refTorque,
                         drivePosition,driveVelocity,driveTorque,
                         position,velocity,torque
                         )) {
                // Report error?
                return ;
              }
              m_position = position;
              m_velocity = velocity;
              m_torque = torque;
              for(auto &a : m_positionCallbacks.Calls()) {
                if(a) a(theTime,m_position,m_velocity,m_torque);
              }
            }
        );
      }
    }
    return JointC::AddPositionUpdateCallback(callback);
  }

  //! Send an updated demand position
  bool JointRelativeC::UpdateDemand()
  {
    double refPosition = 0;
    double refVelocity = 0;
    double refTorque = 0;

    // Is there any demand set ?
    if(isnanf(m_demandPosition) || isnanf(m_demandTorqueLimit))
      return false;

    //! Do we have a demand from the reference joint ?
    if(!m_jointRef->GetDemand(refPosition,refTorque)) {
      return false;
    }

    double drivePosition = 0;
    double driveTorqueLimit = 0;
    if(!Simple2Raw(
        refPosition,refTorque,
        m_demandPosition,m_demandTorqueLimit,
        drivePosition,driveTorqueLimit)) {
      return false;
    }
    //driveTorqueLimit = Max(1.0,fabs(driveTorqueLimit));
    driveTorqueLimit = m_demandTorqueLimit;

    m_log->info("Setting demand angle for {} to {} with torque {} ",m_jointDrive->Name(),Rad2Deg(drivePosition),driveTorqueLimit);

    // Only send on if something has changed.
    if(drivePosition != m_lastDrivePosition || driveTorqueLimit != m_lastDriveTorque) {
      m_lastDrivePosition = drivePosition;
      m_lastDriveTorque = driveTorqueLimit;
      return m_jointDrive->DemandPosition(drivePosition,driveTorqueLimit);
    }
    return true;
  }


  //! Demand a position for the servo
  bool JointRelativeC::DemandPosition(float position,float torqueLimit)
  {
    JointC::DemandPosition(position,torqueLimit);
    return UpdateDemand();
  }

}
