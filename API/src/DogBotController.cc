

#include "dogbot/DogBotController.hh"
#include "dogbot/Util.hh"
#include "dogbot/LineABC2d.hh"

namespace DogBotN {

  //! Class to manage the positioning of a single leg.

  DogBotControllerC::DogBotControllerC(std::shared_ptr<DogBotAPIC> &api)
   : m_api(api)
  {
    Init();
  }

  //! Destructor
  DogBotControllerC::~DogBotControllerC()
  {}

  bool DogBotControllerC::Init()
  {
    const std::vector<std::string> &legNames = DogBotAPIC::LegNames();
    for(int i = 0;i < legNames.size();i++) {
      for(int j = 0;j < 3;j++) {
        m_joints[PoseAnglesC::JointId(i,j)] = m_api->GetJointByName(PoseAnglesC::JointName(i,j));
        assert(m_joints[PoseAnglesC::JointId(i,j)] && "Joint not found");
      }
    }
    return true;
  }


  //! Setup trajectory
  bool DogBotControllerC::SetupTrajectory(float updatePeriod,float torqueLimit)
  {
    bool ok = true;
    for(int i = 0;i < 12;i++) {
      if(!m_joints[i]->SetupTrajectory(updatePeriod,torqueLimit)) {
        ok = false;
      }
    }
    return ok;
  }

  //! Send next trajectory position, this should be called at 'updatePeriod' intervals as setup
  //! with SetupTrajectory.
  bool DogBotControllerC::NextTrajectory(const PoseAnglesC &pose)
  {
    bool ok = true;
    for(int i = 0;i < 12;i++) {
      const JointAngleC &ja = pose.JointAngle(i);
      if(m_joints[i] && !m_joints[i]->DemandTrajectory(ja.Position(),ja.Torque()))
        ok = false;
    }
    return ok;
  }






}


