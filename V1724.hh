#ifndef _V1724_HH_
#define _V1724_HH_

#include <cstdint>
#include <vector>
#include <map>
#include <chrono>

class MongoLog;
class Options;
class data_packet;
class ThreadPool;

class V1724{

 public:
  V1724(MongoLog *log, Options *options);
  virtual ~V1724();

  virtual int Init(int link, int crate, int bid, unsigned int address=0);
  virtual int ReadMBLT(u_int32_t* &buffer);
  virtual int WriteRegister(unsigned int reg, unsigned int value);
  virtual unsigned int ReadRegister(unsigned int reg);
  virtual int GetClockCounter(u_int32_t timestamp);
  virtual int End();

  int bid() {return fBID;}

  virtual int LoadDAC(std::vector<u_int16_t> &dac_values);
  void ClampDACValues(std::vector<u_int16_t>&, std::map<std::string, std::vector<double>>&);
  unsigned GetNumChannels() {return fNChannels;}
  int SetThresholds(std::vector<u_int16_t> vals);

  // Acquisition Control

  virtual int SINStart();
  virtual int SoftwareStart();
  virtual int AcquisitionStop(bool=false);
  virtual int SWTrigger();
  virtual int Reset();
  virtual bool EnsureReady(int ntries, int sleep);
  virtual bool EnsureStarted(int ntries, int sleep);
  virtual bool EnsureStopped(int ntries, int sleep);
  virtual int CheckErrors();
  virtual u_int32_t GetAcquisitionStatus();
  u_int32_t GetHeaderTime(u_int32_t *buff, u_int32_t size);

  std::map<std::string, int> DataFormatDefinition;

protected:
  // Some values for base classes to override 
  unsigned int fAqCtrlRegister;
  unsigned int fAqStatusRegister;
  unsigned int fSwTrigRegister;
  unsigned int fResetRegister;
  unsigned int fChStatusRegister;
  unsigned int fChDACRegister;
  unsigned int fChTrigRegister;
  unsigned int fNChannels;
  unsigned int fSNRegisterMSB;
  unsigned int fSNRegisterLSB;
  unsigned int fBoardFailStatRegister;
  unsigned int fReadoutStatusRegister;
  unsigned int fVMEAlignmentRegister;
  unsigned int fBoardErrRegister;

  int BLT_SIZE;
  std::map<int, long> fBLTCounter;

  bool MonitorRegister(u_int32_t reg, u_int32_t mask, int ntries, int sleep, u_int32_t val=1);
  Options *fOptions;
  int fBoardHandle;
  int fLink, fCrate, fBID;
  unsigned int fBaseAddress;

  // Stuff for clock reset tracking
<<<<<<< HEAD
  u_int32_t fRolloverCounter;
  u_int32_t fLastClock;
  std::chrono::high_resolution_clock::time_point fLastClockTime;
  std::chrono::nanoseconds fClockPeriod;
=======
  u_int32_t clock_counter;
  u_int32_t last_time;
  bool seen_under_5;
  bool seen_over_15;
>>>>>>> Work

  MongoLog *fLog;

  float fBLTSafety, fBufferSafety;

};


#endif
