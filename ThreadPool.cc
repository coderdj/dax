#include "ThreadPool.hh"


ThreadPool::ThreadPool(int num_threads) {
  fFinishNow = false;
  fWaitingTasks = fRunningTasks = 0;
  fThreads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) fThreads.emplace_back(std::thread(&ThreadPool::Run, this));
}

ThreadPool::~ThreadPool() {
  Kill();
  std::unique_lock<std::mutex> lk(fMutex);
  fQueue.clear();
  fWaitingTasks = 0;
  for (auto& t : fThreads) t.join();
  fThreads.clear();
}

void AddTask(void (*func)(std::string&), std::string&& input) {
  {
    std::lock_guard<std::mutex> lg(fMutex);
    fQueue.emplace_back(task_t{func, input});
    fWaitingTasks++;
  }
  fCV.notify_one();
}

void ThreadPool::Run() {
  while (!fFinishNow) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, [&]{return fWaitingTasks > 0 || fFinishNow;});
    if (fQueue.size() > 0 && !fFinishNow) {
      auto task = fQueue.front();
      fQueue.pop_font();
      fWaitingTasks--;
      fRunningTasks++;
      lk.unlock();
      task.func(task.input);
      fRunningTasks--;
    } else {
      lk.unlock();
    }
  }
}
