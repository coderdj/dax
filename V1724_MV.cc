#include "V1724_MV.hh"
#include "MongoLog.hh"
#include "Options.hh"

V1724_MV::V1724_MV(std::shared_ptr<ThreadPool>& tp, std::shared_ptr<Processor>& next,
    std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log)
  :V1724(tp, next, opts, log){
  // MV boards seem to have reg 0x1n80 for channel n threshold
  fChTrigRegister = 0x1080;
  fArtificialDeadtimeChannel = 791;
}

V1724_MV::~V1724_MV(){}

std::tuple<int64_t, int, uint16_t, std::u32string_view> 
V1724_MV::UnpackChannelHeader(std::u32string_view sv, long rollovers,
    uint32_t header_time, uint32_t event_time, int event_words, int n_channels) {
  int words = (event_words-4)/n_channels;
  // returns {timestamp (ns), baseline, waveform}
  // More rollover logic here, because processing is multithreaded.
  // We leverage the fact that readout windows are
  // short and polled frequently compared to the rollover timescale, so there
  // will never be a large difference in timestamps in one data packet
  if (event_time > 15e8 && header_time < 5e8 && rollovers != 0) rollovers--;
  else if (event_time < 5e8 && header_time > 15e8) rollovers++;
  return {((rollovers<<31)+event_time)*fClockCycle,
          words,
          0,
          sv.substr(0, words)};
}
