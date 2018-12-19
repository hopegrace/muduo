// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_EVENTLOOPTHREAD_H
#define MUDUO_NET_EVENTLOOPTHREAD_H

#include <muduo/base/Condition.h>
#include <muduo/base/Mutex.h>
#include <muduo/base/Thread.h>

namespace muduo
{
namespace net
{

class EventLoop;

class EventLoopThread : noncopyable
{
 public:
  typedef std::function<void(EventLoop*)> ThreadInitCallback;

  EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                  const string& name = string());
  ~EventLoopThread();
  EventLoop* startLoop(); //启动线程,该线程成为IO线程
 
 private:
  void threadFunc();  //线程函数

  EventLoop* loop_ GUARDED_BY(mutex_);  //指向一个EventLoop对象
  bool exiting_;
  Thread thread_; //Thread是对线程的一些操作进行封装
  MutexLock mutex_;
  Condition cond_ GUARDED_BY(mutex_);
  ThreadInitCallback callback_;
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_EVENTLOOPTHREAD_H

