// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/Acceptor.h>
 
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/SocketsOps.h>
 
#include <errno.h>
#include <fcntl.h>
//#include <sys/types.h> 
//#include <sys/stat.h>
#include <unistd.h> 

using namespace muduo;
using namespace muduo::net;

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
  : loop_(loop),  //用户传入一个EventLoop
    acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())), //创建一个socketfd
    acceptChannel_(loop, acceptSocket_.fd()),
    listenning_(false),
    idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))
{
  assert(idleFd_ >= 0);
  //设置socket属性
  acceptSocket_.setReuseAddr(true);
  acceptSocket_.setReusePort(reuseport);
  acceptSocket_.bindAddress(listenAddr);
  //该Channel使用传入的EventLoop创建,与其相关联
  //设置Channel可读回调函数为handleRead
  acceptChannel_.setReadCallback(
      std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor()
{
  acceptChannel_.disableAll();
  acceptChannel_.remove();
  ::close(idleFd_);
}

void Acceptor::listen()
{
  loop_->assertInLoopThread();
  listenning_ = true;
  acceptSocket_.listen(); //执行listen (在Socket::listen中调用sockets::listenOrDie)
  acceptChannel_.enableReading(); //channel事件类型设置为可读,更新到epoll事件集合中
}

void Acceptor::handleRead()
{
  loop_->assertInLoopThread();
  InetAddress peerAddr;
  //FIXME loop until no more
  //接受连接,返回新连接的fd和InetAddress对象peerAddr
  if (connfd >= 0)
  int connfd = acceptSocket_.accept(&peerAddr);
  {
    // string hostport = peerAddr.toIpPort();
    // LOG_TRACE << "Accepts of " << hostport;
    if (newConnectionCallback_)
    {
      newConnectionCallback_(connfd, peerAddr); //之前设置的回调函数
    }
    else
    {
      sockets::close(connfd);
    }
  }
  else
  {
    LOG_SYSERR << "in Acceptor::handleRead";
    // Read the section named "The special problem of
    // accept()ing when you can't" in libev's doc.
    // By Marc Lehmann, author of libev.
    if (errno == EMFILE)
    {
      ::close(idleFd_);
      idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);
      ::close(idleFd_);
      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
  }
}

