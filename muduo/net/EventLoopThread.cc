// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoopThread.h>

#include <muduo/net/EventLoop.h>

using namespace muduo;
using namespace muduo::net;
 
EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                                 const string& name)
  : loop_(NULL),
    exiting_(false),
    //将线程函数绑定为threadFunc
    thread_(std::bind(&EventLoopThread::threadFunc, this), name), 
    mutex_(),
    cond_(mutex_),
    callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
  exiting_ = true;
  if (loop_ != NULL) // not 100% race-free, eg. threadFunc could be running callback_.
  {
    // still a tiny chance to call destructed object, if threadFunc exits just now.
    // but when EventLoopThread destructs, usually programming is exiting anyway.
    loop_->quit();
    thread_.join();
  }
}


EventLoop* EventLoopThread::startLoop()
{
  assert(!thread_.started());
  //在Thread::start中执行指定的线程函数,即EventLoopThread::threadFunc,
  //创建EventLoop对象
  thread_.start();

  EventLoop* loop = NULL;
  {
    MutexLockGuard lock(mutex_);
    while (loop_ == NULL)
    {
      //需要等待直到指定的EventLoop对象被创建
      cond_.wait();
    }
    loop = loop_;
  }

  //返回一个指向栈上对象的EventLoop指针
  return loop;
}

void EventLoopThread::threadFunc()
{
  EventLoop loop;

  if (callback_)
  {
    //该回调函数callback_由TcpServer::setThreadInitCallback()指定
    ///需要由用户手动指定
    //没有就不执行
    callback_(&loop);
  }

  {
    MutexLockGuard lock(mutex_);
    //loop_指向一个栈上的局部变量,因为该函数退出,意味着线程结束了,那么EventLoopThread对象也就没有存在价值了
    loop_ = &loop;
    //创建好了,发送通知
    cond_.notify();
  }

  //在这里循环,知道EventLoopThread析构,然后也不会再用loop_访问EventLoop了
  loop.loop();
  //assert(exiting_);
  MutexLockGuard lock(mutex_);
  loop_ = NULL;
}

