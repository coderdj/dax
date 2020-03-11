#include "DAQController.hh"
#include <functional>
#include "V1724.hh"
#include "V1724_MV.hh"
#include "V1730.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "StraxInserter.hh"
#include "MongoLog.hh"
#include <unistd.h>
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <numeric>

// Status:
// 0-idle
// 1-arming
// 2-armed
// 3-running
// 4-error

const int MaxBoardsPerLink(8);

DAQController::DAQController(MongoLog *log, std::string hostname){
  fLog=log;
  fOptions = NULL;
  fStatus = DAXHelpers::Idle;
  fReadLoop = false;
  fNProcessingThreads=8;
  fDataRate=0.;
  fHostname = hostname;
}

DAQController::~DAQController(){
  if(fProcessingThreads.size()!=0)
    CloseProcessingThreads();
}

std::string DAQController::run_mode(){
  if(fOptions == NULL)
    return "None";
  try{
    return fOptions->GetString("name", "None");
  }
  catch(const std::exception &e){
    return "None";
  }
}

int DAQController::InitializeElectronics(Options *options, std::vector<int>&keys){

  End();
  
  fOptions = options;
  fNProcessingThreads = fOptions->GetNestedInt("processing_threads."+fHostname, 8);  
  fLog->Entry(MongoLog::Local, "Beginning electronics initialization with %i threads",
	      fNProcessingThreads);

  // Initialize digitizers
  fStatus = DAXHelpers::Arming;
  std::vector<int> BIDs;
  for(auto d : fOptions->GetBoards("V17XX", fHostname)){
    fLog->Entry(MongoLog::Local, "Arming new digitizer %i", d.board);

    V1724 *digi;
    if(d.type == "V1724_MV")
      digi = new V1724_MV(fLog, fOptions);
    else if(d.type == "V1730")
      digi = new V1730(fLog, fOptions);
    else
      digi = new V1724(fLog, fOptions);


    if(digi->Init(d.link, d.crate, d.board, d.vme_address)==0){
	fDigitizers[d.link].push_back(digi);
        BIDs.push_back(digi->bid());
        fBoardMap[digi->bid()] = digi;
        fCheckFails[digi->bid()] = false;

	if(std::find(keys.begin(), keys.end(), d.link) == keys.end()){
	  fLog->Entry(MongoLog::Local, "Defining a new optical link at %i", d.link);
	  keys.push_back(d.link);
	}
	fLog->Entry(MongoLog::Debug, "Initialized digitizer %i", d.board);
	fBufferSize[digi->bid()] = 0;
        fBufferLength[digi->bid()] = 0;
        fBuffer[digi->bid()] = std::queue<data_packet*>();
        //fBufferMutex[digi->bid()] = std::mutex(); // no operator=
    }
    else{
      delete digi;
      fLog->Entry(MongoLog::Warning, "Failed to initialize digitizer %i", d.board);
      fStatus = DAXHelpers::Idle;
      return -1;
    }
  }
  fLog->Entry(MongoLog::Local, "This host has %i boards", BIDs.size());
  fLog->Entry(MongoLog::Local, "Sleeping for two seconds");
  // For the sake of sanity and sleeping through the night,
  // do not remove this statement.
  sleep(2); // <-- this one. Leave it here.
  // Seriously. This sleep statement is absolutely vital.
  fLog->Entry(MongoLog::Local, "That felt great, thanks.");
  std::map<int, std::map<std::string, std::vector<double>>> dac_values;
  if (fOptions->GetString("baseline_dac_mode") == "cached")
    fOptions->GetDAC(dac_values, BIDs);
  std::vector<std::thread*> init_threads;

  std::map<int,int> rets;
  // Parallel digitizer programming to speed baselining
  for( auto& link : fDigitizers ) {
    rets[link.first] = 1;
    init_threads.push_back(new std::thread(&DAQController::InitLink, this,
	  std::ref(link.second), std::ref(dac_values), std::ref(rets[link.first])));
  }
  std::for_each(init_threads.begin(), init_threads.end(),
      [](std::thread* t) {t->join(); delete t;});

  if (std::any_of(rets.begin(), rets.end(), [](auto& p) {return p.second != 0;})) {
    fLog->Entry(MongoLog::Warning, "Encountered errors during digitizer programming");
    if (std::any_of(rets.begin(), rets.end(), [](auto& p) {return p.second == -2;}))
      fStatus = DAXHelpers::Error;
    else
      fStatus = DAXHelpers::Idle;
    return -1;
  } else
    fLog->Entry(MongoLog::Debug, "Digitizer programming successful");
  if (fOptions->GetString("baseline_dac_mode") == "fit") fOptions->UpdateDAC(dac_values);

  for(auto const& link : fDigitizers ) {
    for(auto digi : link.second){
      if(fOptions->GetInt("run_start", 0) == 1)
	digi->SINStart();
      else
	digi->AcquisitionStop();
    }
  }
  sleep(1);
  fStatus = DAXHelpers::Armed;

  fLog->Entry(MongoLog::Local, "Arm command finished, returning to main loop");


  return 0;
}

int DAQController::Start(){
  if(fOptions->GetInt("run_start", 0) == 0){
    for( auto const& link : fDigitizers ){
      for(auto digi : link.second){

	// Ensure digitizer is ready to start
	if(digi->EnsureReady(1000, 1000)!= true){
	  fLog->Entry(MongoLog::Warning, "Digitizer not ready to start after sw command sent");
	  return -1;
	}

	// Send start command
	digi->SoftwareStart();

	// Ensure digitizer is started
	if(digi->EnsureStarted(1000, 1000)!=true){
	  fLog->Entry(MongoLog::Warning,
		      "Timed out waiting for acquisition to start after SW start sent");
	  return -1;
	}
      }
    }
  }
  fStatus = DAXHelpers::Running;
  return 0;
}

int DAQController::Stop(){

  fReadLoop = false; // at some point.
  int counter = 0;
  bool one_still_running = false;
  do{
    one_still_running = false;
    for (auto& p : fRunning) one_still_running |= p.second;
    if (one_still_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }while(one_still_running && counter++ < 10);
  if (counter >= 10) fLog->Entry(MongoLog::Local, "Boards taking a while to clear");
  std::cout<<"Deactivating boards"<<std::endl;
  for( auto const& link : fDigitizers ){
    for(auto digi : link.second){
      digi->AcquisitionStop();

      // Ensure digitizer is stopped
      if(digi->EnsureStopped(1000, 1000) != true){
	fLog->Entry(MongoLog::Warning,
		    "Timed out waiting for acquisition to stop after SW stop sent");
          return -1;
      }
    }
  }
  fLog->Entry(MongoLog::Debug, "Stopped digitizers");

  fStatus = DAXHelpers::Idle;
  return 0;
}

void DAQController::End(){
  Stop();
  fLog->Entry(MongoLog::Local, "Closing Digitizers");
  for( auto const& link : fDigitizers ){
    for(auto digi : link.second){
      digi->End();
      delete digi;
    }
  }
  fLog->Entry(MongoLog::Local, "Closing Processing Threads");
  CloseProcessingThreads();
  fDigitizers.clear();
  fStatus = DAXHelpers::Idle;

  for (auto& p : fBuffer) {
    if(p.second.size() != 0){
      fLog->Entry(MongoLog::Warning, "Deleting uncleared buffer of size %i from board %i",
		p.second.size(), p.first);
      std::for_each(p.second.begin(), p.second.end(), [](auto dp){delete dp;});
      p.second.clear();
    }
  }

  std::cout<<"Finished end"<<std::endl;
}

void DAQController::ReadData(int link){
  fReadLoop = true;
  
  // Raw data buffer should be NULL. If not then maybe it was not cleared since last time

  for (auto digi : fDigitizers[link]) {
    fBufferMutex[digi->bid()].lock();
    if(fBuffer[digi->bid()].size() != 0){
      fLog->Entry(MongoLog::Debug, "Raw data buffer being brute force cleared.");
      std::for_each(fBuffer[digi->bid()].begin(), fBuffer[digi->bid()].end(),
          [](auto dp){delete dp;});
      fBuffer[digi->bid()].clear();
      fDataRate = 0;
    }
    fBufferMutex[digi->bid()].unlock();
  }
  
  u_int32_t board_status = 0;
  int readcycler = 0;
  int err_val = 0;
  data_packet* dp = nullptr;
  fRunning[link] = true;
  while(fReadLoop){
    
    for(auto digi : fDigitizers[link]) {

      // Every 1k reads check board status
      if(readcycler%10000==0){
	readcycler=0;
        board_status = digi->GetAcquisitionStatus();
        fLog->Entry(MongoLog::Local, "Board %i has status 0x%04x",
            digi->bid(), board_status);
      }
      if (fCheckFails[digi->bid()]) {
        fCheckFails[digi->bid()] = false;
        err_val = fBoardMap[digi->bid()]->CheckErrors();
	fLog->Entry(MongoLog::Local, "Error %i from board %i", err_val, digi->bid());
        if (err_val == -1) {

        } else {
          if (err_val & 0x1) fLog->Entry(MongoLog::Local, "Board %i has PLL unlock",
                                         digi->bid());
          if (err_val & 0x2) fLog->Entry(MongoLog::Local, "Board %i has VME bus error",
                                         digi->bid());
        }
      }
      if (dp == nullptr) dp = new data_packet;
      if((dp->size = digi->ReadMBLT(dp->buff, &dp->vBLT))<0){
        if (dp->buff != nullptr) {
	  delete[] dp->buff; // possible leak, catch here
	  dp->buff = nullptr;
          delete dp;
          dp = nullptr;
	}
	break;
      }
      if(dp->size>0){
        dp->bid = digi->bid();
        fBufferMutex[dp->bid].lock();
        fBufferLength[dp->bid]++;
        fBuffer[dp->bid].push(dp);
        fBufferSize[dp->bid] += dp->size;
        fDataRate += dp->size;
        fBufferMutex.unlock();
        dp = nullptr;
      }
    } // for digi in digitizers
    readcycler++;
    usleep(1);
  } // while run
  fRunning[link] = false;
  fLog->Entry(MongoLog::Local, "RO thread %i returning", link);
}

std::map<int, int> DAQController::GetDataPerChan(){
  // Return a map of data transferred per channel since last update
  // Clears the private maps in the StraxInserters
  std::map <int, int> retmap;
  for (const auto& pt : fProcessingThreads)
    pt.inserter->GetDataPerChan(retmap);
  return retmap;
}

long DAQController::GetStraxBufferSize() {
  return std::accumulate(fProcessingThreads.begin(), fProcessingThreads.end(), 0,
      [=](long tot, processingThread pt) {return tot + pt.inserter->GetBufferSize();});
}

int DAQController::GetBufferLength() {
  return fBufferLength.load() + std::accumulate(fProcessingThreads.begin(),
      fProcessingThreads.end(), 0,
      [](int tot, auto pt){return tot + pt.inserter->GetBufferLength();});
}

std::map<std::string, int> DAQController::GetDataFormat(int bid){
  return fBoardMap[bid]->DataFormatDefinition;
}

int DAQController::GetData(std::queue<data_packet*> &retQ, int bid){
  if (fBufferLength[bid] == 0) return 0;
  int ret = 0;
  // let's use a fancy raii lock guard that unlocks when it goes out of scope
  const std::lock_guard<std::mutex> lock(fBufferMutex[bid]);
  if (fBuffer[bid].size() == 0) return 0;
  fBuffer[bid].swap(retQ);
  fBufferLength[bid] = 0;
  ret = fBufferSize[bid];
  fBufferSize[bid] = 0;
  return ret;
}

int DAQController::GetData(data_packet* &dp, int bid) {
  if (fBufferLength[bid] == 0) return 0;
  const std::lock_guard<std::mutex> lock(fBufferMutex[bid]);
  if (fBuffer[bid].size() == 0) return 0;
  dp = fBuffer[bid].front();
  fBufferSize[bid] -= dp->size;
  fBufferLength[bid]--;
  fBuffer[bid].pop();
  return 1;
}

bool DAQController::CheckErrors(){

  // This checks for errors from the threads by checking the
  // error flag in each object. It's appropriate to poll this
  // on the order of ~second(s) and initialize a STOP in case
  // the function returns "true"

  for(unsigned int i=0; i<fProcessingThreads.size(); i++){
    if(fProcessingThreads[i].inserter->CheckError()){
      fLog->Entry(MongoLog::Error, "Error found in processing thread.");
      fStatus=DAXHelpers::Error;
      return true;
    }
  }
  return false;
}

int DAQController::OpenProcessingThreads(){
  int ret = 0;
  for(auto& p : fBoardMap){
    processingThread pt;
    pt.inserter = new StraxInserter();
    if (pt.inserter->Initialize(fOptions, fLog, p.first, this, fHostname)) {
      pt.pthread = new std::thread(); // something to delete later
      ret++;
    } else
      pt.pthread = new std::thread(&StraxInserter::ReadAndInsertData, pt.inserter);
    fProcessingThreads.push_back(pt);
  }
  return ret;
}

void DAQController::CloseProcessingThreads(){
  std::map<int,int> board_fails;
  for(unsigned int i=0; i<fProcessingThreads.size(); i++){
    fProcessingThreads[i].inserter->Close(board_fails);
    // two stage process so there's time to clear data
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  for(unsigned int i=0; i<fProcessingThreads.size(); i++){
    delete fProcessingThreads[i].inserter;
    fProcessingThreads[i].pthread->join();
    delete fProcessingThreads[i].pthread;
  }

  fProcessingThreads.clear();
  if (std::accumulate(board_fails.begin(), board_fails.end(), 0,
	[=](int tot, std::pair<int,int> iter) {return tot + iter.second;})) {
    std::stringstream msg;
    msg << "Found board failures: ";
    for (auto& iter : board_fails) msg << iter.first << ":" << iter.second << " | ";
    fLog->Entry(MongoLog::Warning, msg.str());
  }
}

void DAQController::InitLink(std::vector<V1724*>& digis,
    std::map<int, std::map<std::string, std::vector<double>>>& cal_values, int& ret) {
  std::string BL_MODE = fOptions->GetString("baseline_dac_mode", "fixed");
  std::map<int, std::vector<u_int16_t>> dac_values;
  int nominal_baseline = fOptions->GetInt("baseline_value", 16000);
  if (BL_MODE == "fit") {
    if ((ret = FitBaselines(digis, dac_values, nominal_baseline, cal_values))) {
      fLog->Entry(MongoLog::Warning, "Errors during baseline fitting");
      return;
    }
  }

  for(auto digi : digis){
    fLog->Entry(MongoLog::Local, "Board %i beginning specific init", digi->bid());

    // Multiple options here
    int bid = digi->bid(), success(0);
    if(BL_MODE == "cached") {
      fMapMutex.lock();
      auto board_dac_cal = cal_values.count(bid) ? cal_values[bid] : cal_values[-1];
      fMapMutex.unlock();
      dac_values[bid] = std::vector<u_int16_t>(digi->GetNumChannels());
      fLog->Entry(MongoLog::Local, "Board %i using cached baselines", bid);
      for (unsigned ch = 0; ch < digi->GetNumChannels(); ch++)
	dac_values[bid][ch] = nominal_baseline*board_dac_cal["slope"][ch] + board_dac_cal["yint"][ch];
      digi->ClampDACValues(dac_values[bid], board_dac_cal);
    }
    else if(BL_MODE != "fixed" && BL_MODE != "fit"){
      fLog->Entry(MongoLog::Warning, "Received unknown baseline mode '%s', fallback to fixed", BL_MODE.c_str());
      BL_MODE = "fixed";
    }
    if(BL_MODE == "fixed"){
      int BLVal = fOptions->GetInt("baseline_fixed_value", 4000);
      fLog->Entry(MongoLog::Local, "Loading fixed baselines with value 0x%04x", BLVal);
      dac_values[bid] = std::vector<u_int16_t>(digi->GetNumChannels(), BLVal);
    }

    //int success = 0;
    fLog->Entry(MongoLog::Local, "Board %i finished baselines", bid);
    if(success==-2){
      fLog->Entry(MongoLog::Warning, "Board %i Baselines failed with digi error");
      ret = -2;
      return;
    }
    else if(success!=0){
      fLog->Entry(MongoLog::Warning, "Board %i failed baselines with timeout", digi->bid());
      ret = -1;
      return;
    }

    fLog->Entry(MongoLog::Local, "Board %i survived baseline mode. Going into register setting",
		bid);

    for(auto regi : fOptions->GetRegisters(bid)){
      unsigned int reg = DAXHelpers::StringToHex(regi.reg);
      unsigned int val = DAXHelpers::StringToHex(regi.val);
      success+=digi->WriteRegister(reg, val);
    }
    fLog->Entry(MongoLog::Local, "Board %i loaded user registers, loading DAC.", bid);

    // Load the baselines you just configured
    success += digi->LoadDAC(dac_values[bid]);
    // Load all the other fancy stuff
    success += digi->SetThresholds(fOptions->GetThresholds(bid));

    fLog->Entry(MongoLog::Local,
	"Board %i programmed", digi->bid());
    if(success!=0){
	//LOG
      fLog->Entry(MongoLog::Warning, "Failed to configure digitizers.");
      ret = -1;
      return;
    }
  } // loop over digis per link

  ret = 0;
  return;
}

int DAQController::FitBaselines(std::vector<V1724*> &digis,
    std::map<int, std::vector<u_int16_t>> &dac_values, int target_baseline,
    std::map<int, std::map<std::string, std::vector<double>>> &cal_values) {
  using std::vector;
  using namespace std::chrono_literals;
  int max_iter(2);
  unsigned max_steps(20), ch_this_digi(0);
  int adjustment_threshold(10), convergence_threshold(3), min_adjustment(0xA);
  int rebin_factor(1); // log base 2
  int nbins(1 << (14-rebin_factor)), bins_around_max(3), bid(0);
  int triggers_per_step(3), steps_repeated(0), max_repeated_steps(10);
  std::chrono::milliseconds ms_between_triggers(10);
  vector<int> hist(nbins);
  vector<long> DAC_cal_points = {60000, 30000, 6000}; // arithmetic overflow
  std::map<int, vector<int>> channel_finished;
  std::map<int, u_int32_t*> buffers;
  std::map<int, int> bytes_read;
  std::map<int, vector<vector<double>>> bl_per_channel;
  std::map<int, vector<int>> diff;

  for (auto digi : digis) { // alloc ALL the things!
    bid = digi->bid();
    ch_this_digi = digi->GetNumChannels();
    dac_values[bid] = vector<u_int16_t>(ch_this_digi, 0);
    channel_finished[bid] = vector<int>(ch_this_digi, 0);
    bl_per_channel[bid] = vector<vector<double>>(ch_this_digi, vector<double>(max_steps,0));
    diff[bid] = vector<int>(ch_this_digi, 0);
  }

  bool done(false), redo_iter(false), fail(false), calibrate(true);
  double counts_total(0), counts_around_max(0), B,C,D,E,F, slope, yint, baseline;
  double fraction_around_max(0.8);
  u_int32_t words_in_event, channel_mask, words_per_channel, idx;
  u_int16_t val0, val1;
  int channels_in_event;
  auto beg_it = hist.begin(), max_it = hist.begin(), end_it = hist.end();
  auto max_start = max_it, max_end = max_it;

  for (int iter = 0; iter < max_iter; iter++) {
    if (done || fail) break;
    for (auto& vv : bl_per_channel) // vv = pair(int, vector<vector<double>>)
      for (auto& v : vv.second) // v = vector<double>
        v.assign(v.size(), 0);
    for (auto& v : channel_finished) v.second.assign(v.second.size(), 0);
    steps_repeated = 0;
    fLog->Entry(MongoLog::Local, "Beginning baseline iteration %i/%i", iter, max_iter);

    for (unsigned step = 0; step < max_steps; step++) {
      fLog->Entry(MongoLog::Local, "Beginning baseline step %i/%i", step, max_steps);
      if (std::all_of(channel_finished.begin(), channel_finished.end(),
            [&](auto& p) {
              return std::all_of(p.second.begin(), p.second.end(), [=](int i)
                  {return i >= convergence_threshold;});})) {
        fLog->Entry(MongoLog::Local, "All boards on this link finished baselining");
        done = true;
        break;
      }
      if (steps_repeated >= max_repeated_steps) {
        fLog->Entry(MongoLog::Debug, "Repeating a lot of steps here");
        break;
      }
      // prep
      if (step < DAC_cal_points.size()) {
	if (!calibrate) continue;
        for (auto d : digis)
          dac_values[d->bid()].assign(d->GetNumChannels(), (int)DAC_cal_points[step]);
      }
      for (auto d : digis) {
        if (d->LoadDAC(dac_values[d->bid()])) {
          fLog->Entry(MongoLog::Warning, "Board %i failed to load DAC", d->bid());
          return -2;
        }
      }
      // "After writing, the user is recommended to wait for a few seconds before
      // a new RUN to let the DAC output get stabilized" - CAEN documentation
      std::this_thread::sleep_for(1s);
      // sleep(2) seems unnecessary after preliminary testing

      // start board
      for (auto d : digis) {
        if (d->EnsureReady(1000,1000))
          d->SoftwareStart();
        else
          fail = true;
      }
      std::this_thread::sleep_for(5ms);
      for (auto d : digis) {
        if (!d->EnsureStarted(1000,1000)) {
          d->AcquisitionStop();
          fail = true;
        }
      }

      // send triggers
      for (int trig = 0; trig < triggers_per_step; trig++) {
        for (auto d : digis) d->SWTrigger();
        std::this_thread::sleep_for(ms_between_triggers);
      }
      // stop
      for (auto d : digis) {
        d->AcquisitionStop();
        if (!d->EnsureStopped(1000,1000)) {
          fail = true;
        }
      }
      if (fail) {
        for (auto d : digis) d->AcquisitionStop();
        fLog->Entry(MongoLog::Warning, "Error in baseline digi control");
        return -2;
      }
      std::this_thread::sleep_for(1ms);

      // readout
      for (auto d : digis) {
        bytes_read[d->bid()] = d->ReadMBLT(buffers[d->bid()]);
      }

      // decode
      if (std::any_of(bytes_read.begin(), bytes_read.end(),
            [=](auto p) {return p.second < 0;})) {
        for (auto d : digis) {
          if (bytes_read[d->bid()] < 0)
            fLog->Entry(MongoLog::Error, "Board %i has readout error in baselines",
                d->bid());
        }
        std::for_each(buffers.begin(), buffers.end(), [](auto p){delete[] p.second;});
        return -2;
      }
      if (std::any_of(bytes_read.begin(), bytes_read.end(), [=](auto p) {
            return (0 <= p.second) && (p.second <= 16);})) { // header-only readouts???
        fLog->Entry(MongoLog::Local, "Undersized readout");
        step--;
        steps_repeated++;
        std::for_each(buffers.begin(), buffers.end(), [](auto p){delete[] p.second;});
        continue;
      }

      // analyze
      for (auto d : digis) {
        bid = d->bid();
        idx = 0;
        while ((idx * sizeof(u_int32_t) < bytes_read[bid])) {
          if ((buffers[bid][idx]>>28) == 0xA) {
            words_in_event = buffers[bid][idx]&0xFFFFFFF;
            if (words_in_event == 4) {
              idx += 4;
              continue;
            }
            channel_mask = buffers[bid][idx+1]&0xFF;
	    if (d->DataFormatDefinition["channel_mask_msb_idx"] != -1) {
	      channel_mask = ( ((buffers[bid][idx+2]>>24)&0xFF)<<8 ) | (buffers[bid][idx+1]&0xFF); 
	    }
            if (channel_mask == 0) { // should be impossible?
              idx += 4;
              continue;
            }
            channels_in_event = std::bitset<16>(channel_mask).count();
	    words_per_channel = (words_in_event - 4)/channels_in_event;
            words_per_channel -= d->DataFormatDefinition["channel_header_words"];

            idx += 4;
            for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
              if (!(channel_mask & (1 << ch))) continue;
              idx += d->DataFormatDefinition["channel_header_words"];
              hist.assign(hist.size(), 0);
              for (unsigned w = 0; w < words_per_channel; w++) {
                val0 = buffers[bid][idx+w]&0xFFFF;
                val1 = (buffers[bid][idx+w]>>16)&0xFFFF;
                if (val0*val1 == 0) continue;
                hist[val0 >> rebin_factor]++;
                hist[val1 >> rebin_factor]++;
              }
              idx += words_per_channel;
              max_it = std::max_element(beg_it, end_it);
              max_start = std::max(max_it - bins_around_max, beg_it);
              max_end = std::min(max_it + bins_around_max+1, end_it);
              counts_total = std::accumulate(beg_it, end_it, 0.);
              counts_around_max = std::accumulate(max_start, max_end, 0.);
              if (counts_around_max/counts_total < fraction_around_max) {
                fLog->Entry(MongoLog::Local,
                    "Bd %i ch %i: %d out of %d counts around max %i",
                    bid, ch, counts_around_max, counts_total,
                    (max_it - beg_it)<<rebin_factor);
                redo_iter = true;
              }
              if (counts_total/words_per_channel < 1.5) //25% zeros
                redo_iter = true;
              baseline = 0;
              // calculated weighted average
              for (auto it = max_start; it < max_end; it++)
                baseline += ((it - beg_it)<<rebin_factor)*(*it);
              baseline /= counts_around_max;
              bl_per_channel[bid][ch][step] = baseline;
            } // for each channel

          } else { // if header
            idx++;
          }
        } // end of while in buffer
      } // process per digi
      // cleanup buffers
      for (auto p : buffers) delete[] p.second;
      if (redo_iter) {
        redo_iter = false;
        step--;
        steps_repeated++;
        continue;
      }
      if (step+1 < DAC_cal_points.size()) continue;
      if (step+1 == DAC_cal_points.size() && calibrate) {
        // ****************************
        // Determine calibration values
        // ****************************
        for (auto d : digis) {
        //for (unsigned d = 0; d < digis_this_link; d++) {
          bid = d->bid();
          fMapMutex.lock();
          cal_values[bid] = std::map<std::string, vector<double>>(
              {{"slope", vector<double>(d->GetNumChannels())},
               {"yint", vector<double>(d->GetNumChannels())}});
          fMapMutex.unlock();
          for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
            B = C = D = E = F = 0;
            for (unsigned i = 0; i < DAC_cal_points.size(); i++) {
              B += DAC_cal_points[i]*DAC_cal_points[i];
              C += 1;
              D += DAC_cal_points[i]*bl_per_channel[bid][ch][i];
              E += bl_per_channel[bid][ch][i];
              F += DAC_cal_points[i];
            }
            cal_values[bid]["slope"][ch] = slope = (C*D - E*F)/(B*C - F*F);
            cal_values[bid]["yint"][ch] = yint = (B*E - D*F)/(B*C - F*F);
            //fLog->Entry(MongoLog::Debug, "Bd %i ch %i calibration %.3f/%.1f",
            //    bid, ch, slope, yint);
	    dac_values[bid][ch] = (target_baseline-yint)/slope;
          }
        }
        calibrate = false;
      } else {
        // ******************
        // Do fitting process
        // ******************
        for (auto d : digis) {
        //for (unsigned d = 0; d < digis_this_link; d++) {
          bid = d->bid();
          for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
            if (channel_finished[bid][ch] >= convergence_threshold) continue;

            float off_by = target_baseline - bl_per_channel[bid][ch][step];
            if (abs(off_by) < adjustment_threshold) {
              channel_finished[bid][ch]++;
              continue;
            }
	    channel_finished[bid][ch] = std::max(0, channel_finished[bid][ch]-1);
            int adjustment = off_by * cal_values[bid]["slope"][ch];
            if (abs(adjustment) < min_adjustment)
              adjustment = std::copysign(min_adjustment, adjustment);
            fLog->Entry(MongoLog::Local,
                "Bd %i ch %i dac %04x bl %.1f adjust %i step %i", bid, ch,
                dac_values[bid][ch], bl_per_channel[bid][ch][step], adjustment, step);
            dac_values[bid][ch] += adjustment;
          } // for channels
        } // for digis
      } // fit/calibrate
      for (auto d : digis)
        d->ClampDACValues(dac_values[d->bid()], cal_values[d->bid()]);

    } // end steps
    if (std::all_of(channel_finished.begin(), channel_finished.end(),
          [&](auto& p){return std::all_of(p.second.begin(), p.second.end(), [=](int i){
            return i >= convergence_threshold;});})) {
      fLog->Entry(MongoLog::Local, "All baselines for boards on this link converged");
      break;
    }
  } // end iterations
  //for (unsigned d = 0; d < digis_this_link; d++) {
  for (auto d : digis) {
    for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
      bid = d->bid();
      fLog->Entry(MongoLog::Local, "Bd %i ch %i diff %i", bid, ch,
	(target_baseline-cal_values[bid]["yint"][ch])/cal_values[bid]["slope"][ch] - dac_values[bid][ch]);
    }
  }
  if (fail) return -2;
  if (std::any_of(channel_finished.begin(), channel_finished.end(),
    	[&](auto& p){return std::any_of(p.second.begin(), p.second.end(), [=](int i){
      	  return i < convergence_threshold;});})) return -1;
  return 0;
}
