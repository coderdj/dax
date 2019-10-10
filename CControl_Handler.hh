#ifndef _CCONTROL_HANDLER_HH_
#define _CCONTROL_HANDLER_HH_

#include "Options.hh"
#include "MongoLog.hh"
#include "V2718.hh"
#include "DDC10.hh"
#include <thread>

class CControl_Handler{
  
public:
  CControl_Handler(MongoLog *log, std::string procname);
  ~CControl_Handler();

  bsoncxx::document::value GetStatusDoc(std::string hostname);
  int DeviceArm(int run, Options *opts);
  int DeviceStart();
  int DeviceStop();

private:

  V2718 *fV2718;
  DDC10 *fDDC10;

  int fStatus;
  int fCurrentRun;
  std::string fProcname;
  Options *fOptions;
  MongoLog *fLog;
};

#endif
