
#ifndef __RCPP_PARALLEL__
#define __RCPP_PARALLEL__

// determine whether to use TBB
#ifndef RCPP_PARALLEL_USE_TBB
#ifdef _WIN32
  #define RCPP_PARALLEL_USE_TBB 0
#else
  #define RCPP_PARALLEL_USE_TBB 1
#endif
#endif

namespace RcppParallel {

////// Shared Types ///////////////////////////////////////////////////

// Code which can be executed within a worker thread. We implement
// dynamic dispatch using vtables so we can have a stable type to
// cast the void* to within the worker thread.

struct Worker {
  virtual ~Worker() {}
  virtual void operator()(std::size_t begin, std::size_t end) = 0;
};

template <typename T> 
struct ReduceWorker : public Worker {
  virtual void split(const T& source) = 0;
  virtual void join(const T& rhs) = 0;
};

} // namespace RcppParallel

////// TinyThreads Implementation /////////////////////////////////////

// tinythread library 
#include <tthread/tinythread.h>

#include <vector>

namespace RcppParallel {

namespace {

// Class which represents a range of indexes to perform work on
// (worker functions are passed this range so they know which
// elements are safe to read/write to)
class IndexRange {
public:

  // Initizlize with a begin and (exclusive) end index
  IndexRange(std::size_t begin, std::size_t end)
    : begin_(begin), end_(end)
  {
  }
  
  // Access begin() and end()
  std::size_t begin() const { return begin_; }
  std::size_t end() const { return end_; }
  
private:
  std::size_t begin_;
  std::size_t end_;
};


// Because tinythread allows us to pass only a plain C function
// we need to pass our worker and range within a struct that we 
// can cast to/from void*
struct Work {
  Work(IndexRange range, Worker& worker) 
    :  range(range), worker(worker)
  {
  }
  IndexRange range;
  Worker& worker;
};

// Thread which performs work (then deletes the work object
// when it's done)
extern "C" inline void workerThread(void* data) {
  try
  {
    Work* pWork = static_cast<Work*>(data);
    pWork->worker(pWork->range.begin(), pWork->range.end());
    delete pWork;
  }
  catch(...)
  {
  }
}

// Function to calculate the ranges for a given input
std::vector<IndexRange> splitInputRange(const IndexRange& range) {
  
  // max threads is based on hardware concurrency
  std::size_t threads = tthread::thread::hardware_concurrency();
  
  // determine the chunk size
  std::size_t length = range.end() - range.begin();
  std::size_t chunkSize = length / threads;
  
  // allocate ranges
  std::vector<IndexRange> ranges;
  std::size_t nextIndex = range.begin();
  for (std::size_t i = 0; i<threads; i++) {
    std::size_t begin = nextIndex;
    std::size_t end = std::min(begin + chunkSize, range.end());
    ranges.push_back(IndexRange(begin, end));
    nextIndex = end;
  }

  // return ranges  
  return ranges;
}

} // anonymous namespace

// Execute the IWorker over the IndexRange in parallel
inline void ttParallelFor(std::size_t begin, std::size_t end, Worker& worker) {
  
  using namespace tthread;
  
  // split the work
  std::vector<IndexRange> ranges = splitInputRange(IndexRange(begin, end));
  
  // create threads
  std::vector<thread*> threads;
  for (std::size_t i = 0; i<ranges.size(); ++i) {
    threads.push_back(new thread(workerThread, new Work(ranges[i], worker)));   
  }
  
  // join and delete them
  for (std::size_t i = 0; i<threads.size(); ++i) {
    threads[i]->join();
    delete threads[i];
  }
}

// Execute the IWorker over the range in parallel then join results
template <typename T>
inline void ttParallelReduce(
  std::size_t begin, std::size_t end, ReduceWorker<T>& worker) {
  
  using namespace tthread;
  
  // split the work
  std::vector<IndexRange> ranges = splitInputRange(IndexRange(begin, end));
  
  // create threads (split for each thread and track the allocated workers)
  std::vector<thread*> threads;
  std::vector<ReduceWorker<T>*> workers;
  for (std::size_t i = 0; i<ranges.size(); ++i) {
    T* pWorker = new T();
    pWorker->split(static_cast<T&>(worker));
    workers.push_back(pWorker);
    threads.push_back(new thread(workerThread, new Work(ranges[i], *pWorker)));  
  }
  
  // wait for each thread, join it's results, then delete the worker & thread
  for (std::size_t i = 0; i<threads.size(); ++i) {
    
    // wait for thread
    threads[i]->join();
   
    // join the results
    worker.join(static_cast<T&>(*workers[i]));
    
    // delete the worker (which we split above) and the thread
    delete workers[i];
    delete threads[i];
  }
}

} // namespace RcppParallel

////// TBB Implementation /////////////////////////////////////////////

#if RCPP_PARALLEL_USE_TBB

#include <tbb/tbb.h>

namespace RcppParallel {

struct TBBWorker
{
  explicit TBBWorker(Worker& worker) : worker_(worker) {}
  
  void operator()(const tbb::blocked_range<size_t>& r) const {
    worker_(r.begin(), r.end());
  }
  
private:
  Worker& worker_;
};

inline void tbbParallelFor(std::size_t begin, std::size_t end, Worker& worker) {
   TBBWorker tbbWorker(worker);
   tbb::parallel_for(tbb::blocked_range<size_t>(begin, end), tbbWorker);
}

template <typename T>
struct TBBReduce 
{  
  explicit TBBReduce(ReduceWorker<T>& reduce) 
    : pReduce_(static_cast<T*>(&reduce)), wasSplit_(false)
  {
  }
   
  virtual ~TBBReduce() {
    try
    {
      if (wasSplit_)
        delete pReduce_;
    }
    catch(...)
    {
    }
  }
   
  void operator()(const tbb::blocked_range<size_t>& r) {
    pReduce_->operator()(r.begin(), r.end());
  }
   
  TBBReduce(TBBReduce& reduce, tbb::split) 
    : pReduce_(new T()), wasSplit_(true) 
  {  
    pReduce_->split(*reduce.pReduce_);   
  }
  
  void join(const TBBReduce& reduce) { 
    pReduce_->join(*reduce.pReduce_); 
  }
   
private:
   T* pReduce_;
   bool wasSplit_;
};

template <typename T>
inline void tbbParallelReduce(
  std::size_t begin, std::size_t end, ReduceWorker<T>& worker) {
    
   TBBReduce<T> tbbReduce(worker);
   tbb::parallel_reduce(tbb::blocked_range<size_t>(begin, end), tbbReduce);
}

} // namespace RcppParallel

#endif

////// Dispatch to Implementation /////////////////////////////////////

namespace RcppParallel {

inline void parallelFor(std::size_t begin, std::size_t end, Worker& worker) {
  
#if RCPP_PARALLEL_USE_TBB
  tbbParallelFor(begin, end, worker);
#else
  ttParallelFor(begin, end, worker);
#endif

}

template <typename T>
inline void parallelReduce(
  std::size_t begin, std::size_t end, ReduceWorker<T>& worker) {

#if RCPP_PARALLEL_USE_TBB
  tbbParallelReduce(begin, end, worker);
#else
  ttParallelReduce(begin, end, worker);
#endif

}

} // namespace RcppParallel

#endif // __RCPP_PARALLEL__