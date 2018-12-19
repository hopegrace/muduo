// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/Timer.h>

using namespace muduo;
using namespace muduo::net;

AtomicInt64 Timer::s_numCreated_;

void Timer::restart(Timestamp now)
{
  if (repeat_)  //如果设置的是重复,则重新添加
  {
    expiration_ = addTime(now, interval_);//将now和interval_想加
  }
  else
  {
    expiration_ = Timestamp::invalid();
  }
}
 